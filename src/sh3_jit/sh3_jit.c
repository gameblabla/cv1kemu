/*
 * Self-contained C23 SH-3 JIT frontend.
 *
 * This provides a compact C block cache, SH-3 subset decoder, and
 * pluggable backend table.  The interpreter is
 * still the exact fallback and remains the default execution backend.
 */

#include "sh3_jit/cv1k_sh3_c23_jit.h"
#include "sh3_jit/sh3_jit_backend.h"
#include "cv1k_config.h"
#include "emu.h"
#include <stdlib.h>
#include <string.h>

#define SH_T   0x00000001U
#define SH_S   0x00000002U
#define SH_I   0x000000f0U
#define SH_Q   0x00000100U
#define SH_M   0x00000200U
#define SH_FD  0x00008000U
#define SH_BL  0x10000000U
#define SH_RB  0x20000000U
#define SH_MD  0x40000000U
#define SH34_FLAGS_JIT (SH_MD | SH_RB | SH_BL | SH_FD | SH_M | SH_Q | SH_I | SH_S | SH_T)

#define JIT_BUCKETS 16384U
#define JIT_MAX_OPS 48U

static int g_c23jit_enabled = 0;
static cv1k_u32 g_stat_blocks = 0;
static cv1k_u32 g_stat_hits = 0;
static cv1k_u32 g_stat_fallbacks = 0;
static cv1k_u32 g_stat_invalidations = 0;
static struct cv1k_sh3_jit_block *g_blocks[JIT_BUCKETS];
static const struct cv1k_sh3_jit_backend *g_backend;
static cv1k_u32 g_executing_blocks = 0;
static int g_reset_pending = 0;
#ifdef CV1K_JIT_FALLBACK_PROFILE
#include <stdio.h>
static unsigned long long g_fallback_op_hist[65536];
#endif

static cv1k_u32 phys_addr(cv1k_u32 addr) { return (addr >= 0x80000000U && addr <= 0xbfffffffU) ? (addr & 0x1fffffffU) : addr; }

static inline cv1k_u32 hash_pc(cv1k_u32 pc)
{
    return (pc ^ (pc >> 4) ^ (pc >> 13)) & (JIT_BUCKETS - 1U);
}

cv1k_u32 cv1k_sh3_jit_linear_cycles(cv1k_u16 op)
{
    if ((op & 0xff00U) == 0xcb00U) return 3U;
    if ((op & 0xf0ffU) == 0x4017U || (op & 0xf0ffU) == 0x4027U) return 3U;
    if ((op & 0xf0ffU) == 0x4013U || (op & 0xf0ffU) == 0x4023U) return 2U;
    if ((op & 0xf00fU) == 0x0007U) return 2U; /* MUL.L is 2 cycles */
    return 1U;
}

cv1k_u32 cv1k_sh3_jit_block_cycles_max(const cv1k_u16 *ops, size_t count)
{
    cv1k_u32 cycles = 0U;
    for (size_t i = 0; i < count; i++) {
        cv1k_u16 op = ops[i];
        if ((op & 0xff00U) == 0x8900U || (op & 0xff00U) == 0x8b00U) return cycles + 3U;
        if ((op & 0xff00U) == 0x8d00U || (op & 0xff00U) == 0x8f00U) return cycles + 2U;
        if ((op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U) return cycles + 2U;
        if ((op & 0xf0ffU) == 0x0023U || (op & 0xf0ffU) == 0x0003U) return cycles + 2U;
        if ((op & 0xf0ffU) == 0x400bU || op == 0x000bU || op == 0x002bU) return cycles + 2U;
        if ((op & 0xf0ffU) == 0x402bU) return cycles + 1U;
        cycles += cv1k_sh3_jit_linear_cycles(op);
    }
    return cycles;
}

static cv1k_u8 *hot_read_ptr(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 need)
{
    struct cv1k_machine *m = bus ? bus->machine : NULL;
    cv1k_u32 phys = phys_addr(addr);
    if (m == NULL) return NULL;
    if (phys >= CV1K_ADDR_WORK_RAM && phys - CV1K_ADDR_WORK_RAM + need <= m->main_ram_size)
        return m->main_ram + (phys - CV1K_ADDR_WORK_RAM);
    if (m->boot_rom != NULL && phys + need <= m->boot_rom_size)
        return m->boot_rom + phys;
    return NULL;
}

static cv1k_u8 *hot_write_ptr(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 need)
{
    struct cv1k_machine *m = bus ? bus->machine : NULL;
    cv1k_u32 phys = phys_addr(addr);
    if (m == NULL) return NULL;
    if (phys >= CV1K_ADDR_WORK_RAM && phys - CV1K_ADDR_WORK_RAM + need <= m->main_ram_size)
        return m->main_ram + (phys - CV1K_ADDR_WORK_RAM);
    return NULL;
}

/* SH7709S cache-access hook for the JIT's hot memory helpers.  Compiles to
 * nothing in the fast build (CV1K_CACHE_ACCURATE == 0); in the accurate build it
 * gates on the runtime flags so the common case is a predictable branch rather
 * than a call on every JIT'd memory access. */
static CV1K_ALWAYS_INLINE void jit_cache_note(struct cv1k_bus *bus, cv1k_u32 addr, int write, int fetch)
{
#if CV1K_CACHE_ACCURATE
    struct cv1k_machine *mm = bus ? bus->machine : NULL;
    if (CV1K_UNLIKELY(mm != NULL && (mm->mame_cache_meta || mm->sh7709s_cache_timing)))
        cv1k_bus_cache_access(bus, addr, write, fetch);
#else
    (void)bus; (void)addr; (void)write; (void)fetch;
#endif
}

cv1k_u8 cv1k_sh3_jit_read8(struct cv1k_bus *bus, cv1k_u32 addr)
{
    cv1k_u8 *p = hot_read_ptr(bus, addr, 1U);
    if (p) { jit_cache_note(bus, addr, 0, 0); return p[0]; }
    return cv1k_bus_read8(bus, addr);
}

cv1k_u16 cv1k_sh3_jit_read16(struct cv1k_bus *bus, cv1k_u32 addr)
{
    cv1k_u8 *p = hot_read_ptr(bus, addr, 2U);
    if (p) { jit_cache_note(bus, addr, 0, 0); return (cv1k_u16)(((cv1k_u16)p[0] << 8) | p[1]); }
    return cv1k_bus_read16(bus, addr);
}

cv1k_u32 cv1k_sh3_jit_read32(struct cv1k_bus *bus, cv1k_u32 addr)
{
    cv1k_u8 *p = hot_read_ptr(bus, addr, 4U);
    if (p) { jit_cache_note(bus, addr, 0, 0); return (((cv1k_u32)p[0] << 24) | ((cv1k_u32)p[1] << 16) | ((cv1k_u32)p[2] << 8) | p[3]); }
    return cv1k_bus_read32(bus, addr);
}

void cv1k_sh3_jit_write8(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data)
{
    cv1k_u8 *p = hot_write_ptr(bus, addr, 1U);
    if (p) { jit_cache_note(bus, addr, 1, 0); p[0] = (cv1k_u8)data; }
    else cv1k_bus_write8(bus, addr, (cv1k_u8)data);
}

void cv1k_sh3_jit_write16(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data)
{
    cv1k_u8 *p = hot_write_ptr(bus, addr, 2U);
    if (p) { jit_cache_note(bus, addr, 1, 0); p[0] = (cv1k_u8)(data >> 8); p[1] = (cv1k_u8)data; }
    else cv1k_bus_write16(bus, addr, (cv1k_u16)data);
}

void cv1k_sh3_jit_write32(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data)
{
    cv1k_u8 *p = hot_write_ptr(bus, addr, 4U);
    if (p) { jit_cache_note(bus, addr, 1, 0); p[0] = (cv1k_u8)(data >> 24); p[1] = (cv1k_u8)(data >> 16); p[2] = (cv1k_u8)(data >> 8); p[3] = (cv1k_u8)data; }
    else cv1k_bus_write32(bus, addr, data);
}

void cv1k_sh3_jit_ldc_sr(struct sh7709s_cpu *cpu, cv1k_u32 n)
{
    cv1k_u32 reg = cpu->r[n & 15U];
    if ((reg & SH_RB) != (cpu->sr & SH_RB)) {
        for (int i = 0; i < 8; i++) {
            cv1k_u32 t = cpu->r[i];
            cpu->r[i] = cpu->rb[i];
            cpu->rb[i] = t;
        }
    }
    cpu->sr = reg & SH34_FLAGS_JIT;
}

void cv1k_sh3_jit_rte(struct sh7709s_cpu *cpu)
{
    if (cpu == NULL) return;
    cpu->m_delay = cpu->ea = cpu->spc;
    if ((cpu->ssr & SH_RB) != (cpu->sr & SH_RB)) {
        for (int i = 0; i < 8; i++) {
            cv1k_u32 t = cpu->r[i];
            cpu->r[i] = cpu->rb[i];
            cpu->rb[i] = t;
        }
    }
    cpu->sr = cpu->ssr;
}

/* Verbatim copies of the MAME SH interpreter ops (src/sh_common_ops.inc) so the
 * JIT can emit a focused call instead of failing the whole block.  Keep these
 * bit-exact with the interpreter. */
void cv1k_sh3_jit_rotcl(struct sh7709s_cpu *cpu, cv1k_u32 n)
{
    cv1k_u32 temp = (cpu->r[n] >> 31) & SH_T;
    cpu->r[n] = (cpu->r[n] << 1) | (cpu->sr & SH_T);
    cpu->sr = (cpu->sr & ~SH_T) | temp;
}

void cv1k_sh3_jit_rotcr(struct sh7709s_cpu *cpu, cv1k_u32 n)
{
    cv1k_u32 temp = (cpu->sr & SH_T) << 31;
    if (cpu->r[n] & SH_T) cpu->sr |= SH_T; else cpu->sr &= ~SH_T;
    cpu->r[n] = (cpu->r[n] >> 1) | temp;
}

void cv1k_sh3_jit_div0s(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n)
{
    if (!((cpu->r[n] >> 31) & 1U)) cpu->sr &= ~SH_Q; else cpu->sr |= SH_Q;
    if (!((cpu->r[m] >> 31) & 1U)) cpu->sr &= ~SH_M; else cpu->sr |= SH_M;
    if (((cpu->r[m] ^ cpu->r[n]) >> 31) & 1U) cpu->sr |= SH_T; else cpu->sr &= ~SH_T;
}

void cv1k_sh3_jit_div0u(struct sh7709s_cpu *cpu)
{
    cpu->sr &= ~(SH_M | SH_Q | SH_T);
}

void cv1k_sh3_jit_div1(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n)
{
    cv1k_u32 old_q = cpu->sr & SH_Q;
    if (0x80000000U & cpu->r[n]) cpu->sr |= SH_Q; else cpu->sr &= ~SH_Q;

    cpu->r[n] = (cpu->r[n] << 1) | (cpu->sr & SH_T);

    if (!old_q) {
        if (!(cpu->sr & SH_M)) {
            cv1k_u32 tmp = cpu->r[n];
            cpu->r[n] -= cpu->r[m];
            if (!(cpu->sr & SH_Q)) { if (cpu->r[n] > tmp) cpu->sr |= SH_Q; else cpu->sr &= ~SH_Q; }
            else                   { if (cpu->r[n] > tmp) cpu->sr &= ~SH_Q; else cpu->sr |= SH_Q; }
        } else {
            cv1k_u32 tmp = cpu->r[n];
            cpu->r[n] += cpu->r[m];
            if (!(cpu->sr & SH_Q)) { if (cpu->r[n] < tmp) cpu->sr &= ~SH_Q; else cpu->sr |= SH_Q; }
            else                   { if (cpu->r[n] < tmp) cpu->sr |= SH_Q; else cpu->sr &= ~SH_Q; }
        }
    } else {
        if (!(cpu->sr & SH_M)) {
            cv1k_u32 tmp = cpu->r[n];
            cpu->r[n] += cpu->r[m];
            if (!(cpu->sr & SH_Q)) { if (cpu->r[n] < tmp) cpu->sr |= SH_Q; else cpu->sr &= ~SH_Q; }
            else                   { if (cpu->r[n] < tmp) cpu->sr &= ~SH_Q; else cpu->sr |= SH_Q; }
        } else {
            cv1k_u32 tmp = cpu->r[n];
            cpu->r[n] -= cpu->r[m];
            if (!(cpu->sr & SH_Q)) { if (cpu->r[n] > tmp) cpu->sr &= ~SH_Q; else cpu->sr |= SH_Q; }
            else                   { if (cpu->r[n] > tmp) cpu->sr |= SH_Q; else cpu->sr &= ~SH_Q; }
        }
    }

    {
        cv1k_u32 tmp = (cpu->sr & (SH_Q | SH_M));
        if (tmp == 0U || tmp == 0x300U) cpu->sr |= SH_T; else cpu->sr &= ~SH_T;
    }
}

void cv1k_sh3_jit_mull(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n)
{
    cpu->macl = cpu->r[n] * cpu->r[m];
}

void cv1k_sh3_jit_negc(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n)
{
    cv1k_u32 temp = cpu->r[m];
    cpu->r[n] = (cv1k_u32)(0U - temp - (cpu->sr & SH_T));
    if (temp || (cpu->sr & SH_T)) cpu->sr |= SH_T; else cpu->sr &= ~SH_T;
}

void cv1k_sh3_jit_swapb(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n)
{
    cv1k_u32 temp = cpu->r[m] & 0xffff0000U;
    temp |= (cpu->r[m] & 0x000000ffU) << 8;
    cpu->r[n] = (cv1k_u8)(cpu->r[m] >> 8);
    cpu->r[n] = cpu->r[n] | temp;
}

void cv1k_sh3_jit_swapw(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n)
{
    cv1k_u32 temp = cpu->r[m] >> 16;
    cpu->r[n] = (cpu->r[m] << 16) | temp;
}

void cv1k_sh3_jit_rotl(struct sh7709s_cpu *cpu, cv1k_u32 n)
{
    cpu->sr = (cpu->sr & ~SH_T) | ((cpu->r[n] >> 31) & SH_T);
    cpu->r[n] = (cpu->r[n] << 1) | (cpu->r[n] >> 31);
}

void cv1k_sh3_jit_rotr(struct sh7709s_cpu *cpu, cv1k_u32 n)
{
    cpu->sr = (cpu->sr & ~SH_T) | (cpu->r[n] & SH_T);
    cpu->r[n] = (cpu->r[n] >> 1) | (cpu->r[n] << 31);
}

int cv1k_sh3_jit_is_terminal_branch(cv1k_u16 op)
{
    return ((op & 0xff00U) == 0x8900U || (op & 0xff00U) == 0x8b00U ||
            (op & 0xff00U) == 0x8d00U || (op & 0xff00U) == 0x8f00U ||
            (op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U ||
            (op & 0xf0ffU) == 0x0023U || (op & 0xf0ffU) == 0x0003U ||
            (op & 0xf0ffU) == 0x402bU || (op & 0xf0ffU) == 0x400bU ||
            op == 0x000bU || op == 0x002bU || (op & 0xf0ffU) == 0x400eU);
}

int cv1k_sh3_jit_branch_has_delay_slot(cv1k_u16 op)
{
    return ((op & 0xff00U) == 0x8d00U || (op & 0xff00U) == 0x8f00U ||
            (op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U ||
            (op & 0xf0ffU) == 0x0023U || (op & 0xf0ffU) == 0x0003U ||
            (op & 0xf0ffU) == 0x402bU || (op & 0xf0ffU) == 0x400bU ||
            op == 0x000bU || op == 0x002bU || (op & 0xf0ffU) == 0x400eU);
}

static int unsupported_system(cv1k_u16 op)
{
    /* RTE is a terminal delayed branch in the JIT. */
    if ((op & 0xff00U) == 0xc300U) return 1; /* TRAPA */
    if ((op & 0xf00fU) == 0x400eU || (op & 0xf00fU) == 0x4007U) return 1; /* LDC/LDC.L SR etc */
    return 0;
}

int cv1k_sh3_jit_supported_linear(cv1k_u16 op)
{
    if (cv1k_sh3_jit_is_terminal_branch(op) || unsupported_system(op)) return 0;
    if (op == 0x0009U || op == 0x0008U || op == 0x0018U) return 1;
    if ((op & 0xf000U) == 0xe000U || (op & 0xf000U) == 0x7000U ||
        (op & 0xf000U) == 0x9000U || (op & 0xf000U) == 0xd000U ||
        (op & 0xf000U) == 0x5000U || (op & 0xf000U) == 0x1000U) return 1;
    if ((op & 0xff00U) == 0x8000U || (op & 0xff00U) == 0x8100U ||
        (op & 0xff00U) == 0x8400U || (op & 0xff00U) == 0x8500U ||
        (op & 0xff00U) == 0x8800U || (op & 0xff00U) == 0xc000U ||
        (op & 0xff00U) == 0xc100U || (op & 0xff00U) == 0xc200U ||
        (op & 0xff00U) == 0xc400U || (op & 0xff00U) == 0xc500U ||
        (op & 0xff00U) == 0xc600U || (op & 0xff00U) == 0xc700U ||
        (op & 0xff00U) == 0xc800U || (op & 0xff00U) == 0xc900U ||
        (op & 0xff00U) == 0xca00U || (op & 0xff00U) == 0xcb00U) return 1;
    switch (op & 0xf00fU) {
    case 0x6003U: case 0x300cU: case 0x3008U: case 0x2008U: case 0x2009U: case 0x200bU: case 0x200aU:
    case 0x6007U: case 0x600bU: case 0x600cU: case 0x600dU: case 0x600eU: case 0x600fU: case 0x3000U: case 0x3002U: case 0x3003U: case 0x3006U: case 0x3007U:
    case 0x6000U: case 0x6001U: case 0x6002U: case 0x2000U: case 0x2001U: case 0x2002U:
    case 0x6004U: case 0x6005U: case 0x6006U: case 0x2004U: case 0x2005U: case 0x2006U:
    case 0x000cU: case 0x000dU: case 0x000eU: case 0x0004U: case 0x0005U: case 0x0006U:
    case 0x400cU: case 0x400dU:
    case 0x3004U: case 0x2007U: /* DIV1, DIV0S via focused C helper */
    case 0x0007U:               /* MUL.L via focused C helper */
    case 0x6008U: case 0x6009U: case 0x600aU: /* SWAP.B, SWAP.W, NEGC via helper */
    case 0x4004U: case 0x4005U: /* ROTL, ROTR via focused C helper */
        return 1;
    default:
        break;
    }
    switch (op & 0xf0ffU) {
    case 0x000aU: case 0x0012U: case 0x001aU: case 0x0022U: case 0x0029U: case 0x002aU:
    case 0x4002U: case 0x4012U: case 0x4022U:
    case 0x4006U: case 0x4016U: case 0x4026U:
    case 0x4013U: case 0x4023U: case 0x4017U: case 0x4027U:
    case 0x400aU: case 0x401aU:
    case 0x4010U: case 0x4011U: case 0x4015U: case 0x4000U: case 0x4001U: case 0x4020U: case 0x4021U:
    case 0x4008U: case 0x4009U: case 0x4018U: case 0x4019U: case 0x4028U: case 0x4029U:
    case 0x401eU: case 0x402eU:
    case 0x4024U: case 0x4025U: /* ROTCL, ROTCR via focused C helper */
    case 0x0019U:               /* DIV0U via focused C helper */
        return 1;
    default:
        return 0;
    }
}

int cv1k_sh3_jit_delay_can_inline(cv1k_u16 op)
{
    if (!cv1k_sh3_jit_supported_linear(op)) return 0;
    if ((op & 0xf000U) == 0x9000U || (op & 0xf000U) == 0xd000U || (op & 0xff00U) == 0xc700U) return 0;
    return 1;
}

static struct cv1k_sh3_jit_block *lookup_block(cv1k_u32 pc)
{
    for (struct cv1k_sh3_jit_block *b = g_blocks[hash_pc(pc)]; b != NULL; b = b->next) {
        if (b->pc == pc) return b;
    }
    return NULL;
}

static struct cv1k_sh3_jit_block *compile_block(struct cv1k_bus *bus, cv1k_u32 pc)
{
    struct cv1k_machine *m = bus ? bus->machine : NULL;
    struct cv1k_sh3_jit_block *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    if (m != NULL) m->sh7709s_cache_timing_suppress++;
    b->pc = pc;
    for (size_t i = 0; i < JIT_MAX_OPS; i++) {
        cv1k_u32 opc = pc + (cv1k_u32)i * 2U;
        cv1k_u16 op = cv1k_bus_fetch16(bus, opc);
        if (cv1k_sh3_jit_is_terminal_branch(op)) {
            if (cv1k_sh3_jit_branch_has_delay_slot(op)) {
                cv1k_u16 delay = cv1k_bus_fetch16(bus, opc + 2U);
                b->ops[b->op_count++] = op;
                if (cv1k_sh3_jit_delay_can_inline(delay) && b->op_count < JIT_MAX_OPS)
                    b->ops[b->op_count++] = delay;
            } else {
                b->ops[b->op_count++] = op;
            }
            break;
        }
        if (!cv1k_sh3_jit_supported_linear(op)) break;
        b->ops[b->op_count++] = op;
    }

    if (b->op_count == 0 || g_backend == NULL || !g_backend->compile || g_backend->compile(b) != 0) {
        b->negative = 1;
        b->fn = NULL;
        b->cycles = 0;
    } else {
        b->cycles = cv1k_sh3_jit_block_cycles_max(b->ops, b->op_count);
        g_stat_blocks++;
    }

    cv1k_u32 h = hash_pc(pc);
    b->next = g_blocks[h];
    g_blocks[h] = b;
    if (m != NULL && m->sh7709s_cache_timing_suppress != 0) m->sh7709s_cache_timing_suppress--;
    return b;
}

static struct cv1k_sh3_jit_block *get_block(struct cv1k_bus *bus, cv1k_u32 pc)
{
    struct cv1k_sh3_jit_block *b = lookup_block(pc);
    return b ? b : compile_block(bus, pc);
}

/* TEST-ONLY differential-tester entry: compile a fresh block at cpu->pc and run
 * it once.  Returns the number of guest ops the block covered, or 0 if the block
 * could not be JIT-compiled.  Used by tools/jit_difftest to assert the JIT block
 * produces the same architectural state as interpreting the same ops. */
cv1k_u32 cv1k_sh3_jit_test_run_block(struct sh7709s_cpu *cpu, struct cv1k_bus *bus)
{
    struct cv1k_sh3_jit_block *b;
    if (cpu == NULL || bus == NULL) return 0;
    if (g_backend == NULL) g_backend = cv1k_sh3_jit_select_backend();
    b = compile_block(bus, cpu->pc);
    if (b == NULL || b->negative || b->fn == NULL) return 0;
    b->fn(cpu, bus);
    return b->op_count;
}

static inline int jit_state_ok(const struct sh7709s_cpu *cpu)
{
    return cpu != NULL && cpu->sleep_mode == 0U && cpu->m_delay == 0U && cpu->halted == 0U;
}

static inline int jit_irq_can_accept_now(const struct sh7709s_cpu *cpu)
{
    if (cpu == NULL || cpu->m_delay != 0U || cpu->pend_mask == 0U || (cpu->sr & SH_BL) != 0U) return 0;
    const cv1k_u32 mask = (cpu->sr & SH_I) >> 4;
    cv1k_u32 pm = cpu->pend_mask;
    while (pm != 0U) {
        const int i = __builtin_ctz(pm);
        if ((cpu->pend_pri[i] & 0x0fU) > mask) return 1;
        pm &= pm - 1U;
    }
    return 0;
}

int sh7709s_c23jit_available(void)
{
    if (g_backend == NULL) g_backend = cv1k_sh3_jit_select_backend();
    return g_backend != NULL && g_backend->available != NULL && g_backend->available();
}

const char *sh7709s_c23jit_backend_name(void)
{
    if (g_backend == NULL) g_backend = cv1k_sh3_jit_select_backend();
    return g_backend ? g_backend->name : "none";
}

void sh7709s_c23jit_enable(int enabled)
{
    g_backend = cv1k_sh3_jit_select_backend();
    g_c23jit_enabled = enabled && sh7709s_c23jit_available();
}

int sh7709s_c23jit_enabled(void)
{
    return g_c23jit_enabled;
}

void sh7709s_c23jit_stats(cv1k_u32 *blocks, cv1k_u32 *hits, cv1k_u32 *fallbacks, cv1k_u32 *invalidations)
{
    if (blocks) *blocks = g_stat_blocks;
    if (hits) *hits = g_stat_hits;
    if (fallbacks) *fallbacks = g_stat_fallbacks;
    if (invalidations) *invalidations = g_stat_invalidations;
#ifdef CV1K_JIT_FALLBACK_PROFILE
    for (int rank = 0; rank < 20; rank++) {
        unsigned best = 0; unsigned long long bc = 0;
        for (unsigned i = 0; i < 65536; i++) if (g_fallback_op_hist[i] > bc) { bc = g_fallback_op_hist[i]; best = i; }
        if (bc == 0) break;
        fprintf(stderr, "jit-fallback-op[%02d] op=%04x count=%llu\n", rank, best, bc);
        g_fallback_op_hist[best] = 0;
    }
#endif
}

static void sh7709s_c23jit_reset_now(void)
{
    if (g_backend == NULL) g_backend = cv1k_sh3_jit_select_backend();
    for (size_t i = 0; i < JIT_BUCKETS; i++) {
        struct cv1k_sh3_jit_block *b = g_blocks[i];
        while (b) {
            struct cv1k_sh3_jit_block *next = b->next;
            if (g_backend && g_backend->free_code) g_backend->free_code(b);
            free(b);
            b = next;
        }
        g_blocks[i] = NULL;
    }
}

void sh7709s_c23jit_reset(void)
{
    if (g_executing_blocks != 0U) {
        g_reset_pending = 1;
        g_stat_invalidations++;
        return;
    }
    sh7709s_c23jit_reset_now();
    g_reset_pending = 0;
    g_stat_blocks = 0;
    g_stat_hits = 0;
    g_stat_fallbacks = 0;
    g_stat_invalidations = 0;
}

cv1k_u32 sh7709s_c23jit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                  cv1k_u32 cycle_budget, cv1k_u32 tmu_interval)
{
    if (!g_c23jit_enabled || cpu == NULL || bus == NULL) return 0;
    if (tmu_interval == 0UL) tmu_interval = 2048UL;

    const cv1k_u32 start = cpu->cycles;
    const cv1k_u32 frame_end = start + cycle_budget;
    cv1k_u32 next_tmu = cpu->cycles + tmu_interval;
    cv1k_u32 local_hits = 0U;
    cv1k_u32 local_fallbacks = 0U;

    while ((cv1k_s32)(cpu->cycles - frame_end) < 0) {
        if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) {
            cv1k_bus_tmu_tick(bus);
            next_tmu = cpu->cycles + tmu_interval;
        }

        int used_jit = 0;
        if (jit_irq_can_accept_now(cpu)) sh7709s_accept_pending_irq(cpu, bus);

        if (jit_state_ok(cpu)) {
            struct cv1k_sh3_jit_block *b = get_block(bus, cpu->pc);
            const cv1k_u32 to_tmu = next_tmu - cpu->cycles;
            if (b != NULL && !b->negative && b->fn != NULL && b->cycles != 0U && b->cycles < to_tmu) {
                cv1k_u32 ran;
#if CV1K_CACHE_ACCURATE
                {
                    struct cv1k_machine *mm = bus ? bus->machine : NULL;
                    if (mm != NULL && (mm->mame_cache_meta || mm->sh7709s_cache_timing)) {
                        size_t oi;
                        for (oi = 0; oi < b->op_count; oi++) {
                            cv1k_bus_cache_access(bus, b->pc + (cv1k_u32)(oi * 2U), 0, 1);
                        }
                    }
                }
#endif
                g_executing_blocks++;
                ran = b->fn(cpu, bus);
                if (g_executing_blocks != 0U) g_executing_blocks--;
                if (g_executing_blocks == 0U && g_reset_pending) {
                    sh7709s_c23jit_reset_now();
                    g_reset_pending = 0;
                }
                if (ran != 0U) {
                    if (jit_irq_can_accept_now(cpu)) sh7709s_accept_pending_irq(cpu, bus);
                    local_hits++;
                    used_jit = 1;
                }
            }
        }

        if (!used_jit) {
#ifdef CV1K_JIT_FALLBACK_PROFILE
            g_fallback_op_hist[cv1k_bus_fetch16(bus, cpu->pc)]++;
#endif
            sh7709s_step(cpu, bus);
            local_fallbacks++;
        }

        if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) {
            cv1k_bus_tmu_tick(bus);
            next_tmu = cpu->cycles + tmu_interval;
        }
        if (cpu->pc == cpu->idle_pc0 || cpu->pc == cpu->idle_pc1) {
            cv1k_u32 jump = ((cv1k_s32)(next_tmu - frame_end) < 0) ? next_tmu : frame_end;
            if ((cv1k_s32)(jump - cpu->cycles) > 0) cpu->cycles = jump;
            if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) {
                cv1k_bus_tmu_tick(bus);
                next_tmu = cpu->cycles + tmu_interval;
            }
        }
    }

    g_stat_hits += local_hits;
    g_stat_fallbacks += local_fallbacks;
    return cpu->cycles - start;
}
