#include "sh3_jit/cv1k_sh3_c23_jit.h"
#include "sh3_jit/cv1k_ir.h"
#include "cpu_sh7709s.h"
#include "bus.h"
#include "cv1k_config.h"
#include "emu.h"
#include <stdlib.h>
#include <string.h>

/* Browser wasm cannot execute bytes stored in linear memory.  This backend is
 * therefore a wasm-safe block JIT: guest SH-3 instruction fetch/decode is done
 * once into a cached straight-line opcode span, and execution reuses the exact
 * MAME-derived C23 opcode semantics through sh7709s_run_linear_ops().  It is a
 * real translated block cache for wasm, but not a late-linked generated-wasm
 * backend; see docs/wasm_jit_backend.md for the async module-link route. */

#define SH_T   0x00000001U
#define SH_I   0x000000f0U
#define SH_BL  0x10000000U
#define WASM_BLOCK_BUCKETS 8192U
#define WASM_BLOCK_FAST_SLOTS 65536U
#define WASM_BLOCK_MAX_OPS 64U
#define WASM_HOT_NONE 0U
#define WASM_HOT_DTNOP 1U
#define WASM_HOT_BRCOND 2U
#define WASM_HOT_BRCOND_NOP 3U
#define WASM_HOT_BRA_NOP 4U
#define WASM_HOT_BSR_NOP 5U
#define WASM_HOT_RTS_NOP 6U
#define WASM_HOT_BRCOND_SLOT 7U
#define WASM_HOT_RTS_SLOT 8U
#define WASM_HOT_JMP_SLOT 9U
#define WASM_HOT_JSR_SLOT 10U
#define WASM_HOT_BRA_SLOT 11U
#define WASM_HOT_BSR_SLOT 12U
#define WASM_HOT_BRAF_SLOT 13U
#define WASM_HOT_BSRF_SLOT 14U

struct wasm_block {
    cv1k_u32 pc;
    cv1k_u16 op_count;
    cv1k_u16 cycle_bound;
    cv1k_u8 hot_kind;
    cv1k_u8 hot_r0;
    cv1k_u16 hot_op;
    cv1k_u16 ops[WASM_BLOCK_MAX_OPS];
    struct wasm_block *next;
};
struct wasm_block_page {
    struct wasm_block item[2048];
    size_t used;
    struct wasm_block_page *next;
};

static int g_wasm_ir_enabled = 0;
static struct wasm_block *g_blocks[WASM_BLOCK_BUCKETS];
static struct wasm_block *g_fast[WASM_BLOCK_FAST_SLOTS];
static struct wasm_block_page *g_pages;
static cv1k_u32 g_stat_blocks, g_stat_hits, g_stat_fallbacks, g_stat_invalidations;

int sh7709s_c23jit_available(void) { return 0; }
const char *sh7709s_c23jit_backend_name(void) { return "none"; }
void sh7709s_c23jit_enable(int enabled) { (void)enabled; }
int sh7709s_c23jit_enabled(void) { return 0; }
void sh7709s_c23jit_reset(void) {}
cv1k_u32 sh7709s_c23jit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                  cv1k_u32 cycle_budget, cv1k_u32 tmu_interval)
{ (void)cpu; (void)bus; (void)cycle_budget; (void)tmu_interval; return 0u; }
void sh7709s_c23jit_stats(cv1k_u32 *blocks, cv1k_u32 *hits, cv1k_u32 *fallbacks,
                          cv1k_u32 *invalidations)
{ if (blocks) *blocks = 0u; if (hits) *hits = 0u; if (fallbacks) *fallbacks = 0u; if (invalidations) *invalidations = 0u; }
cv1k_u32 cv1k_sh3_jit_test_run_block(struct sh7709s_cpu *cpu, struct cv1k_bus *bus)
{ (void)cpu; (void)bus; return 0u; }

static cv1k_u32 hash_pc(cv1k_u32 pc) { return (pc ^ (pc >> 4) ^ (pc >> 13)) & (WASM_BLOCK_BUCKETS - 1U); }
static cv1k_u32 fast_idx(cv1k_u32 pc) { return (pc >> 1U) & (WASM_BLOCK_FAST_SLOTS - 1U); }

static struct wasm_block *alloc_block(void)
{
    if (g_pages == NULL || g_pages->used >= (sizeof(g_pages->item) / sizeof(g_pages->item[0]))) {
        struct wasm_block_page *p = (struct wasm_block_page *)calloc(1, sizeof(*p));
        if (p == NULL) return NULL;
        p->next = g_pages;
        g_pages = p;
    }
    return &g_pages->item[g_pages->used++];
}

static int is_terminal_or_delay(cv1k_u16 op)
{
    if (op == 0x000bU || op == 0x001bU || op == 0x002bU) return 1;                 /* RTS/SLEEP/RTE */
    if ((op & 0xf0ffU) == 0x0003U || (op & 0xf0ffU) == 0x0023U) return 1;          /* BSRF/BRAF */
    if ((op & 0xf0ffU) == 0x400bU || (op & 0xf0ffU) == 0x402bU) return 1;          /* JSR/JMP */
    if ((op & 0xff00U) == 0x8900U || (op & 0xff00U) == 0x8b00U ||
        (op & 0xff00U) == 0x8d00U || (op & 0xff00U) == 0x8f00U) return 1;          /* BT/BF/BTS/BFS */
    if ((op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U) return 1;          /* BRA/BSR */
    if ((op & 0xff00U) == 0xc300U) return 1;                                      /* TRAPA */
    return 0;
}

static cv1k_u16 conservative_cycle_bound(cv1k_u16 op)
{
    if ((op & 0xff00U) == 0xcb00U || (op & 0xff00U) == 0xcd00U ||
        (op & 0xff00U) == 0xce00U || (op & 0xff00U) == 0xcf00U) return 4U;         /* GBR byte RMW */
    if ((op & 0xf0ffU) == 0x4007U || (op & 0xf0ffU) == 0x4013U ||
        (op & 0xf0ffU) == 0x4023U || (op & 0xf0ffU) == 0x4027U) return 4U;         /* SR/VBR mem/control */
    if ((op & 0xf00fU) == 0x000fU || (op & 0xf00fU) == 0x400fU) return 4U;         /* MAC */
    if ((op & 0xf00fU) == 0x0007U || (op & 0xf00fU) == 0x300eU || (op & 0xf00fU) == 0x300fU) return 2U;
    return 1U;
}

static struct wasm_block *find_block(cv1k_u32 pc)
{
    cv1k_u32 fi = fast_idx(pc);
    struct wasm_block *f = g_fast[fi];
    if (f != NULL && f->pc == pc) return f;
    cv1k_u32 h = hash_pc(pc);
    for (struct wasm_block *b = g_blocks[h]; b != NULL; b = b->next) {
        if (b->pc == pc) { g_fast[fi] = b; return b; }
    }
    return NULL;
}


static void classify_hot_block(struct wasm_block *b, struct cv1k_bus *bus, cv1k_u32 pc)
{
    cv1k_u16 op0 = cv1k_bus_fetch16(bus, pc + 0U);
    cv1k_u16 op1 = cv1k_bus_fetch16(bus, pc + 2U);
    cv1k_u16 op2 = cv1k_bus_fetch16(bus, pc + 4U);
    cv1k_u16 op3 = cv1k_bus_fetch16(bus, pc + 6U);
    b->hot_kind = WASM_HOT_NONE;
    b->hot_r0 = 0U;
    b->hot_op = op0;
    /* Canonical SH countdown busy loop used heavily during boot/audio waits:
     *   NOP; DT Rn; BF/S loop; NOP
     * Running whole loop chunks avoids millions of separate fallback BF/S +
     * delay-slot NOP interpreter calls while preserving the MAME-derived cycle
     * counts used by the native IR helper. */
    if (op0 == 0x0009U && (op1 & 0xf0ffU) == 0x4010U && op2 == 0x8ffcU && op3 == 0x0009U) {
        b->hot_kind = WASM_HOT_DTNOP;
        b->hot_r0 = (cv1k_u8)((op1 >> 8U) & 15U);
    }
    else if ((op0 & 0xff00U) == 0x8900U || (op0 & 0xff00U) == 0x8b00U) {
        b->hot_kind = WASM_HOT_BRCOND;
    }
    else if (((op0 & 0xff00U) == 0x8d00U || (op0 & 0xff00U) == 0x8f00U) && op1 == 0x0009U) {
        b->hot_kind = WASM_HOT_BRCOND_NOP;
    }
    else if ((op0 & 0xff00U) == 0x8d00U || (op0 & 0xff00U) == 0x8f00U) {
        b->hot_kind = WASM_HOT_BRCOND_SLOT;
    }
    else if ((op0 & 0xf000U) == 0xa000U && op1 == 0x0009U) {
        b->hot_kind = WASM_HOT_BRA_NOP;
    }
    else if ((op0 & 0xf000U) == 0xb000U && op1 == 0x0009U) {
        b->hot_kind = WASM_HOT_BSR_NOP;
    }
    else if (op0 == 0x000bU && op1 == 0x0009U) {
        b->hot_kind = WASM_HOT_RTS_NOP;
    }
    else if (op0 == 0x000bU) {
        b->hot_kind = WASM_HOT_RTS_SLOT;
    }
    else if ((op0 & 0xf0ffU) == 0x402bU) {
        b->hot_kind = WASM_HOT_JMP_SLOT;
        b->hot_r0 = (cv1k_u8)((op0 >> 8U) & 15U);
    }
    else if ((op0 & 0xf0ffU) == 0x400bU) {
        b->hot_kind = WASM_HOT_JSR_SLOT;
        b->hot_r0 = (cv1k_u8)((op0 >> 8U) & 15U);
    }
    else if ((op0 & 0xf000U) == 0xa000U) {
        b->hot_kind = WASM_HOT_BRA_SLOT;
    }
    else if ((op0 & 0xf000U) == 0xb000U) {
        b->hot_kind = WASM_HOT_BSR_SLOT;
    }
    else if ((op0 & 0xf0ffU) == 0x0023U) {
        b->hot_kind = WASM_HOT_BRAF_SLOT;
        b->hot_r0 = (cv1k_u8)((op0 >> 8U) & 15U);
    }
    else if ((op0 & 0xf0ffU) == 0x0003U) {
        b->hot_kind = WASM_HOT_BSRF_SLOT;
        b->hot_r0 = (cv1k_u8)((op0 >> 8U) & 15U);
    }
}

static int run_hot_dtnop(struct sh7709s_cpu *cpu, cv1k_u32 pc, cv1k_u8 rn,
                         cv1k_u32 next_tmu, cv1k_u32 frame_end)
{
    cv1k_u32 to_tmu = next_tmu - cpu->cycles;
    cv1k_u32 to_frame = frame_end - cpu->cycles;
    cv1k_u32 limit = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
    if (limit <= 1U) return 0;
    cv1k_u32 remain = limit;
    cv1k_u32 r = cpu->r[rn & 15U];
    uint64_t taken_before_zero = (r == 0U) ? UINT64_C(0xffffffff) : (uint64_t)(r - 1U);
    uint64_t can_take = remain / 5U;
    uint64_t take = (taken_before_zero < can_take) ? taken_before_zero : can_take;
    if (take != 0U) {
        cpu->r[rn & 15U] = r - (cv1k_u32)take;
        cpu->sr &= ~1U;
        cpu->ppc = pc + 6U;
        cpu->pc = pc;
        cpu->cycles += (cv1k_u32)(take * 5U);
        remain -= (cv1k_u32)(take * 5U);
        r = cpu->r[rn & 15U];
    }
    if (r == 1U && remain >= 3U) {
        cpu->r[rn & 15U] = 0U;
        cpu->sr = (cpu->sr & ~1U) | 1U;
        cpu->ppc = pc + 4U;
        cpu->pc = pc + 6U;
        cpu->cycles += 3U;
        return 1;
    }
    return take != 0U;
}


static CV1K_ALWAYS_INLINE cv1k_s32 sext12_wasm(cv1k_u32 v)
{ v &= 0xfffU; return (cv1k_s32)((v ^ 0x800U) - 0x800U); }

static CV1K_ALWAYS_INLINE cv1k_s32 sext8_wasm(cv1k_u32 v)
{ return (cv1k_s32)(int8_t)(v & 0xffU); }

static int run_hot_branch(struct sh7709s_cpu *cpu, const struct wasm_block *b,
                          cv1k_u32 next_tmu, cv1k_u32 frame_end)
{
    cv1k_u16 op = b->hot_op;
    cv1k_u32 pc = b->pc;
    cv1k_u32 to_tmu = next_tmu - cpu->cycles;
    cv1k_u32 to_frame = frame_end - cpu->cycles;
    cv1k_u32 lim = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
    cv1k_s32 disp;
    switch (b->hot_kind) {
    case WASM_HOT_BRCOND:
        if ((op & 0xff00U) == 0x8900U) { /* BT */
            if (cpu->sr & 1U) { if (lim < 3U) return 0; disp = sext8_wasm(op); cpu->ea = pc + 4U + (cv1k_u32)(disp * 2); cpu->pc = cpu->ea; cpu->cycles += 3U; }
            else { if (lim < 1U) return 0; cpu->pc = pc + 2U; cpu->cycles += 1U; }
            cpu->ppc = pc;
            return 1;
        }
        if ((op & 0xff00U) == 0x8b00U) { /* BF */
            if (!(cpu->sr & 1U)) { if (lim < 3U) return 0; disp = sext8_wasm(op); cpu->ea = pc + 4U + (cv1k_u32)(disp * 2); cpu->pc = cpu->ea; cpu->cycles += 3U; }
            else { if (lim < 1U) return 0; cpu->pc = pc + 2U; cpu->cycles += 1U; }
            cpu->ppc = pc;
            return 1;
        }
        return 0;
    case WASM_HOT_BRCOND_NOP:
        if ((op & 0xff00U) == 0x8d00U) { /* BT/S + NOP */
            if (cpu->sr & 1U) { if (lim < 3U) return 0; disp = sext8_wasm(op); cpu->ea = pc + 4U + (cv1k_u32)(disp * 2); cpu->ppc = pc + 2U; cpu->pc = cpu->ea; cpu->cycles += 3U; }
            else { if (lim < 1U) return 0; cpu->ppc = pc; cpu->pc = pc + 2U; cpu->cycles += 1U; }
            return 1;
        }
        if ((op & 0xff00U) == 0x8f00U) { /* BF/S + NOP */
            if (!(cpu->sr & 1U)) { if (lim < 3U) return 0; disp = sext8_wasm(op); cpu->ea = pc + 4U + (cv1k_u32)(disp * 2); cpu->ppc = pc + 2U; cpu->pc = cpu->ea; cpu->cycles += 3U; }
            else { if (lim < 1U) return 0; cpu->ppc = pc; cpu->pc = pc + 2U; cpu->cycles += 1U; }
            return 1;
        }
        return 0;
    case WASM_HOT_BRA_NOP:
        if (lim < 3U) return 0;
        disp = sext12_wasm(op);
        cpu->ea = pc + 4U + (cv1k_u32)(disp * 2);
        cpu->ppc = pc + 2U;
        cpu->pc = cpu->ea;
        cpu->cycles += 3U;
        return 1;
    case WASM_HOT_BSR_NOP:
        if (lim < 3U) return 0;
        disp = sext12_wasm(op);
        cpu->pr = pc + 4U;
        cpu->ea = pc + 4U + (cv1k_u32)(disp * 2);
        cpu->ppc = pc + 2U;
        cpu->pc = cpu->ea;
        cpu->cycles += 3U;
        return 1;
    case WASM_HOT_RTS_NOP:
        if (lim < 3U) return 0;
        cpu->ea = cpu->pr;
        cpu->ppc = pc + 2U;
        cpu->pc = cpu->pr;
        cpu->cycles += 3U;
        return 1;
    case WASM_HOT_BRCOND_SLOT:
        if ((op & 0xff00U) == 0x8d00U) { /* BT/S: execute branch only; delay slot remains next */
            if (cpu->sr & 1U) { if (lim < 2U) return 0; disp = sext8_wasm(op); cpu->m_delay = cpu->ea = pc + 4U + (cv1k_u32)(disp * 2); cpu->pc = pc + 2U; cpu->cycles += 2U; }
            else { if (lim < 1U) return 0; cpu->pc = pc + 2U; cpu->cycles += 1U; }
            cpu->ppc = pc;
            return 1;
        }
        if ((op & 0xff00U) == 0x8f00U) { /* BF/S */
            if (!(cpu->sr & 1U)) { if (lim < 2U) return 0; disp = sext8_wasm(op); cpu->m_delay = cpu->ea = pc + 4U + (cv1k_u32)(disp * 2); cpu->pc = pc + 2U; cpu->cycles += 2U; }
            else { if (lim < 1U) return 0; cpu->pc = pc + 2U; cpu->cycles += 1U; }
            cpu->ppc = pc;
            return 1;
        }
        return 0;
    case WASM_HOT_RTS_SLOT:
        if (lim < 2U) return 0;
        cpu->m_delay = cpu->ea = cpu->pr;
        cpu->ppc = pc;
        cpu->pc = pc + 2U;
        cpu->cycles += 2U;
        return 1;
    case WASM_HOT_JMP_SLOT:
        if (lim < 1U) return 0;
        cpu->m_delay = cpu->ea = cpu->r[b->hot_r0 & 15U];
        cpu->ppc = pc;
        cpu->pc = pc + 2U;
        cpu->cycles += 1U;
        return 1;
    case WASM_HOT_JSR_SLOT:
        if (lim < 2U) return 0;
        cpu->pr = pc + 4U;
        cpu->m_delay = cpu->ea = cpu->r[b->hot_r0 & 15U];
        cpu->ppc = pc;
        cpu->pc = pc + 2U;
        cpu->cycles += 2U;
        return 1;
    case WASM_HOT_BRA_SLOT:
        if (lim < 2U) return 0;
        disp = sext12_wasm(op);
        cpu->m_delay = cpu->ea = pc + 4U + (cv1k_u32)(disp * 2);
        cpu->ppc = pc;
        cpu->pc = pc + 2U;
        cpu->cycles += 2U;
        return 1;
    case WASM_HOT_BSR_SLOT:
        if (lim < 2U) return 0;
        disp = sext12_wasm(op);
        cpu->pr = pc + 4U;
        cpu->m_delay = cpu->ea = pc + 4U + (cv1k_u32)(disp * 2);
        cpu->ppc = pc;
        cpu->pc = pc + 2U;
        cpu->cycles += 2U;
        return 1;
    case WASM_HOT_BRAF_SLOT:
        if (lim < 2U) return 0;
        cpu->m_delay = cpu->ea = pc + 4U + cpu->r[b->hot_r0 & 15U];
        cpu->ppc = pc;
        cpu->pc = pc + 2U;
        cpu->cycles += 2U;
        return 1;
    case WASM_HOT_BSRF_SLOT:
        if (lim < 2U) return 0;
        cpu->pr = pc + 4U;
        cpu->m_delay = cpu->ea = pc + 4U + cpu->r[b->hot_r0 & 15U];
        cpu->ppc = pc;
        cpu->pc = pc + 2U;
        cpu->cycles += 2U;
        return 1;
    default:
        return 0;
    }
}

static struct wasm_block *compile_block(struct cv1k_bus *bus, cv1k_u32 pc)
{
    struct wasm_block *hit = find_block(pc);
    if (hit != NULL) return hit;
    struct wasm_block *b = alloc_block();
    if (b == NULL) return NULL;
    b->pc = pc;
    classify_hot_block(b, bus, pc);
    cv1k_u32 p = pc;
    cv1k_u32 bound = 0U;
    for (cv1k_u32 i = 0; i < WASM_BLOCK_MAX_OPS; i++) {
        cv1k_u16 op = cv1k_bus_fetch16(bus, p);
        if (is_terminal_or_delay(op)) break;
        b->ops[b->op_count++] = op;
        cv1k_u16 c = conservative_cycle_bound(op);
        bound += c;
        p += 2U;
        if (c > 1U && i >= 15U) break;
    }
    if (bound > 65535U) bound = 65535U;
    b->cycle_bound = (cv1k_u16)bound;
    cv1k_u32 h = hash_pc(pc);
    b->next = g_blocks[h];
    g_blocks[h] = b;
    g_fast[fast_idx(pc)] = b;
    g_stat_blocks++;
    return b;
}

static int irq_can_accept(const struct sh7709s_cpu *cpu)
{
    if (cpu == NULL || cpu->pend_mask == 0U) return 0;
    if ((cpu->sr & SH_BL) != 0U) return 0;
    cv1k_u32 mask = (cpu->sr & SH_I) >> 4U;
    cv1k_u32 pm = cpu->pend_mask;
    while (pm != 0U) {
        cv1k_u32 bit = pm & (0U - pm);
        int i = __builtin_ctz(pm);
        if ((cpu->pend_pri[i] & 0x0fU) > mask) return 1;
        pm ^= bit;
    }
    return 0;
}


static cv1k_u32 phys_addr(cv1k_u32 addr) { return (addr >= 0x80000000U && addr <= 0xbfffffffU) ? (addr & 0x1fffffffU) : addr; }
static CV1K_ALWAYS_INLINE cv1k_u8 *wasm_hot_read_ptr(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 need)
{
    struct cv1k_machine *m = bus ? bus->machine : NULL;
    cv1k_u32 phys = phys_addr(addr);
    if (m == NULL) return NULL;
    if (phys >= CV1K_ADDR_WORK_RAM && phys - CV1K_ADDR_WORK_RAM + need <= m->main_ram_size) return m->main_ram + (phys - CV1K_ADDR_WORK_RAM);
    if (m->boot_rom != NULL && phys + need <= m->boot_rom_size) return m->boot_rom + phys;
    return NULL;
}
static CV1K_ALWAYS_INLINE cv1k_u8 *wasm_hot_write_ptr(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 need)
{
    struct cv1k_machine *m = bus ? bus->machine : NULL;
    cv1k_u32 phys = phys_addr(addr);
    if (m == NULL) return NULL;
    if (phys >= CV1K_ADDR_WORK_RAM && phys - CV1K_ADDR_WORK_RAM + need <= m->main_ram_size) return m->main_ram + (phys - CV1K_ADDR_WORK_RAM);
    return NULL;
}
cv1k_u8 cv1k_sh3_jit_read8(struct cv1k_bus *bus, cv1k_u32 addr)
{ cv1k_u8 *p=wasm_hot_read_ptr(bus,addr,1U); return p?*p:cv1k_bus_read8(bus,addr); }
cv1k_u16 cv1k_sh3_jit_read16(struct cv1k_bus *bus, cv1k_u32 addr)
{ cv1k_u8 *p=wasm_hot_read_ptr(bus,addr,2U); return p?(cv1k_u16)(((cv1k_u16)p[0]<<8)|p[1]):cv1k_bus_read16(bus,addr); }
cv1k_u32 cv1k_sh3_jit_read32(struct cv1k_bus *bus, cv1k_u32 addr)
{ cv1k_u8 *p=wasm_hot_read_ptr(bus,addr,4U); return p?(((cv1k_u32)p[0]<<24)|((cv1k_u32)p[1]<<16)|((cv1k_u32)p[2]<<8)|p[3]):cv1k_bus_read32(bus,addr); }
void cv1k_sh3_jit_write8(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data)
{ cv1k_u8 *p=wasm_hot_write_ptr(bus,addr,1U); if(p)*p=(cv1k_u8)data; else cv1k_bus_write8(bus,addr,data); }
void cv1k_sh3_jit_write16(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data)
{ cv1k_u8 *p=wasm_hot_write_ptr(bus,addr,2U); if(p){p[0]=(cv1k_u8)(data>>8);p[1]=(cv1k_u8)data;} else cv1k_bus_write16(bus,addr,data); }
void cv1k_sh3_jit_write32(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data)
{ cv1k_u8 *p=wasm_hot_write_ptr(bus,addr,4U); if(p){p[0]=(cv1k_u8)(data>>24);p[1]=(cv1k_u8)(data>>16);p[2]=(cv1k_u8)(data>>8);p[3]=(cv1k_u8)data;} else cv1k_bus_write32(bus,addr,data); }
static cv1k_s32 sext12(cv1k_u32 v) { v &= 0xfffU; return (cv1k_s32)((v ^ 0x800U) - 0x800U); }

static int __attribute__((unused)) ir_try_hot_fallback_step(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u16 op)
{
    cv1k_u32 pc, tmp;
    unsigned n, m;
    if (cpu == NULL || bus == NULL || cpu->sleep_mode != 0U || cpu->m_delay != 0U || cpu->halted != 0U) return 0;
    pc = cpu->pc & 0xfffffffeU;
    cpu->ppc = pc;
    n = (unsigned)((op >> 8U) & 15U);
    m = (unsigned)((op >> 4U) & 15U);
#define IR_FALL_DONE(cyc) do { cpu->pc = pc + 2U; cpu->cycles += (cyc); return 1; } while (0)
#define IR_SET_T(v) do { cpu->sr = (cpu->sr & ~1U) | ((v) ? 1U : 0U); } while (0)
    if (op == 0x0009U) IR_FALL_DONE(1U); /* NOP */
    if ((op & 0xf0ffU) == 0x4010U) { cpu->r[n]--; IR_SET_T(cpu->r[n] == 0U); IR_FALL_DONE(1U); } /* DT */
    if ((op & 0xf00fU) == 0x6003U) { cpu->r[n] = cpu->r[m]; IR_FALL_DONE(1U); } /* MOV Rm,Rn */
    if ((op & 0xf000U) == 0x7000U) { cpu->r[n] += (cv1k_u32)(int8_t)(op & 0xffU); IR_FALL_DONE(1U); } /* ADD #imm,Rn */
    if ((op & 0xf000U) == 0xe000U) { cpu->r[n] = (cv1k_u32)(int8_t)(op & 0xffU); IR_FALL_DONE(1U); } /* MOV #imm,Rn */
    if ((op & 0xf00fU) == 0x300cU) { cpu->r[n] += cpu->r[m]; IR_FALL_DONE(1U); }
    if ((op & 0xf00fU) == 0x3008U) { cpu->r[n] -= cpu->r[m]; IR_FALL_DONE(1U); }

    /* Hot delayed-control fallbacks.  These set m_delay like the MAME-derived
     * core; the slot executes on the next step so IRQ/timer observation stays
     * equivalent to the fallback interpreter path. */
    if (op == 0x000bU) { cpu->m_delay = cpu->ea = cpu->pr; cpu->pc = pc + 2U; cpu->cycles += 2U; return 1; } /* RTS */
    if ((op & 0xf000U) == 0xa000U) { /* BRA */
        cpu->m_delay = cpu->ea = pc + 4U + ((cv1k_u32)sext12(op) * 2U);
        cpu->pc = pc + 2U; cpu->cycles += 2U; return 1;
    }
    if ((op & 0xf000U) == 0xb000U) { /* BSR */
        cpu->pr = pc + 4U;
        cpu->m_delay = cpu->ea = pc + 4U + ((cv1k_u32)sext12(op) * 2U);
        cpu->pc = pc + 2U; cpu->cycles += 2U; return 1;
    }
    if ((op & 0xf0ffU) == 0x402bU) { cpu->m_delay = cpu->ea = cpu->r[n]; cpu->pc = pc + 2U; cpu->cycles += 1U; return 1; } /* JMP @Rn */
    if ((op & 0xf0ffU) == 0x400bU) { cpu->pr = pc + 4U; cpu->m_delay = cpu->ea = cpu->r[n]; cpu->pc = pc + 2U; cpu->cycles += 2U; return 1; } /* JSR @Rn */
    if ((op & 0xf0ffU) == 0x0023U) { cpu->m_delay = cpu->ea = pc + 4U + cpu->r[n]; cpu->pc = pc + 2U; cpu->cycles += 2U; return 1; } /* BRAF */
    if ((op & 0xf0ffU) == 0x0003U) { cpu->pr = pc + 4U; cpu->m_delay = cpu->ea = pc + 4U + cpu->r[n]; cpu->pc = pc + 2U; cpu->cycles += 2U; return 1; } /* BSRF */

    /* Exact hot branch fallbacks for TMU-boundary cases.  Mirror the active
     * MAME-derived sh7709s_step() semantics: taken BT/BF cost 3 cycles,
     * untaken cost 1; taken BT/S/BF/S sets m_delay and costs 2 cycles, while
     * untaken falls through to the slot as a normal next instruction. */
    if ((op & 0xff00U) == 0x8900U) { /* BT disp */
        if (cpu->sr & 1U) {
            cpu->ea = pc + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(op & 0xffU) * 2);
            cpu->pc = cpu->ea; cpu->cycles += 3U;
        } else { cpu->pc = pc + 2U; cpu->cycles += 1U; }
        return 1;
    }
    if ((op & 0xff00U) == 0x8b00U) { /* BF disp */
        if (!(cpu->sr & 1U)) {
            cpu->ea = pc + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(op & 0xffU) * 2);
            cpu->pc = cpu->ea; cpu->cycles += 3U;
        } else { cpu->pc = pc + 2U; cpu->cycles += 1U; }
        return 1;
    }
    if ((op & 0xff00U) == 0x8d00U) { /* BT/S disp */
        if (cpu->sr & 1U) {
            cpu->m_delay = cpu->ea = pc + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(op & 0xffU) * 2);
            cpu->pc = pc + 2U; cpu->cycles += 2U;
        } else { cpu->pc = pc + 2U; cpu->cycles += 1U; }
        return 1;
    }
    if ((op & 0xff00U) == 0x8f00U) { /* BF/S disp */
        if (!(cpu->sr & 1U)) {
            cpu->m_delay = cpu->ea = pc + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(op & 0xffU) * 2);
            cpu->pc = pc + 2U; cpu->cycles += 2U;
        } else { cpu->pc = pc + 2U; cpu->cycles += 1U; }
        return 1;
    }
    /* Very common non-memory TMU-boundary fallbacks.  Keep them before the
     * memory-move matrix so tight arithmetic/shift loops do not test dozens
     * of unrelated load/store patterns before returning. */
    if ((op & 0xf00fU) == 0x2009U) { cpu->r[n] &= cpu->r[m]; IR_FALL_DONE(1U); }
    if ((op & 0xf00fU) == 0x200aU) { cpu->r[n] ^= cpu->r[m]; IR_FALL_DONE(1U); }
    if ((op & 0xf00fU) == 0x200bU) { cpu->r[n] |= cpu->r[m]; IR_FALL_DONE(1U); }
    if ((op & 0xf00fU) == 0x600cU) { cpu->r[n] = cpu->r[m] & 0xffU; IR_FALL_DONE(1U); }
    if ((op & 0xf00fU) == 0x600dU) { cpu->r[n] = cpu->r[m] & 0xffffU; IR_FALL_DONE(1U); }
    if ((op & 0xf00fU) == 0x600eU) { cpu->r[n] = (cv1k_u32)(int8_t)(cpu->r[m] & 0xffU); IR_FALL_DONE(1U); }
    if ((op & 0xf00fU) == 0x600fU) { cpu->r[n] = (cv1k_u32)(int16_t)(cpu->r[m] & 0xffffU); IR_FALL_DONE(1U); }
    switch (op & 0xf0ffU) {
    case 0x4008U: cpu->r[n] <<= 2;  IR_FALL_DONE(1U);
    case 0x4018U: cpu->r[n] <<= 8;  IR_FALL_DONE(1U);
    case 0x4028U: cpu->r[n] <<= 16; IR_FALL_DONE(1U);
    case 0x4009U: cpu->r[n] >>= 2;  IR_FALL_DONE(1U);
    case 0x4019U: cpu->r[n] >>= 8;  IR_FALL_DONE(1U);
    case 0x4029U: cpu->r[n] >>= 16; IR_FALL_DONE(1U);
    case 0x4000U: case 0x4020U: IR_SET_T((cpu->r[n] >> 31) & 1U); cpu->r[n] <<= 1; IR_FALL_DONE(1U);
    case 0x4001U: IR_SET_T(cpu->r[n] & 1U); cpu->r[n] >>= 1; IR_FALL_DONE(1U);
    case 0x4021U: IR_SET_T(cpu->r[n] & 1U); cpu->r[n] = (cv1k_u32)((cv1k_s32)cpu->r[n] >> 1); IR_FALL_DONE(1U);
    case 0x4011U: IR_SET_T((cv1k_s32)cpu->r[n] >= 0); IR_FALL_DONE(1U);
    case 0x4015U: IR_SET_T((cv1k_s32)cpu->r[n] > 0); IR_FALL_DONE(1U);
    case 0x4024U: tmp = (cpu->r[n] >> 31) & 1U; cpu->r[n] = (cpu->r[n] << 1) | (cpu->sr & 1U); IR_SET_T(tmp); IR_FALL_DONE(1U);
    case 0x4025U: tmp = cpu->r[n] & 1U; cpu->r[n] = (cpu->r[n] >> 1) | ((cpu->sr & 1U) << 31); IR_SET_T(tmp); IR_FALL_DONE(1U);
    default: break;
    }

    /* Hot TMU-boundary fallback for SH memory moves.  These are normally
     * handled by native IR blocks; they show up here when the remaining cycles
     * before the next TMU tick are too small to run the whole block.  Use the
     * shared JIT memory helpers instead of sh7709s_step(), keeping the same
     * direct work-RAM fast path as generated code and falling back to the bus
     * for MMIO/aliases/cache-accurate cases. */
    if ((op & 0xf00fU) == 0x2000U) { cv1k_sh3_jit_write8(bus, cpu->r[n], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.B Rm,@Rn */
    if ((op & 0xf00fU) == 0x2001U) { cv1k_sh3_jit_write16(bus, cpu->r[n], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.W Rm,@Rn */
    if ((op & 0xf00fU) == 0x2002U) { cv1k_sh3_jit_write32(bus, cpu->r[n], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.L Rm,@Rn */
    if ((op & 0xf00fU) == 0x2004U) { cpu->r[n] -= 1U; cv1k_sh3_jit_write8(bus, cpu->r[n], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.B Rm,@-Rn */
    if ((op & 0xf00fU) == 0x2005U) { cpu->r[n] -= 2U; cv1k_sh3_jit_write16(bus, cpu->r[n], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.W Rm,@-Rn */
    if ((op & 0xf00fU) == 0x2006U) { cpu->r[n] -= 4U; cv1k_sh3_jit_write32(bus, cpu->r[n], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.L Rm,@-Rn */
    if ((op & 0xf00fU) == 0x6000U) { cpu->r[n] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.B @Rm,Rn */
    if ((op & 0xf00fU) == 0x6001U) { cpu->r[n] = (cv1k_u32)(int16_t)cv1k_sh3_jit_read16(bus, cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.W @Rm,Rn */
    if ((op & 0xf00fU) == 0x6002U) { cpu->r[n] = cv1k_sh3_jit_read32(bus, cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.L @Rm,Rn */
    if ((op & 0xf00fU) == 0x6004U) { tmp = cv1k_sh3_jit_read8(bus, cpu->r[m]); cpu->r[n] = (cv1k_u32)(int8_t)(tmp & 0xffU); if (m != n) cpu->r[m] += 1U; IR_FALL_DONE(1U); } /* MOV.B @Rm+,Rn */
    if ((op & 0xf00fU) == 0x6005U) { tmp = cv1k_sh3_jit_read16(bus, cpu->r[m]); cpu->r[n] = (cv1k_u32)(int16_t)(tmp & 0xffffU); if (m != n) cpu->r[m] += 2U; IR_FALL_DONE(1U); } /* MOV.W @Rm+,Rn */
    if ((op & 0xf00fU) == 0x6006U) { cpu->r[n] = cv1k_sh3_jit_read32(bus, cpu->r[m]); if (m != n) cpu->r[m] += 4U; IR_FALL_DONE(1U); } /* MOV.L @Rm+,Rn */
    if ((op & 0xf000U) == 0x1000U) { cv1k_sh3_jit_write32(bus, cpu->r[n] + ((cv1k_u32)(op & 0x0fU) * 4U), cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.L Rm,@(disp,Rn) */
    if ((op & 0xf000U) == 0x5000U) { cpu->r[n] = cv1k_sh3_jit_read32(bus, cpu->r[m] + ((cv1k_u32)(op & 0x0fU) * 4U)); IR_FALL_DONE(1U); } /* MOV.L @(disp,Rm),Rn */
    if ((op & 0xf00fU) == 0x0004U) { cv1k_sh3_jit_write8(bus, cpu->r[n] + cpu->r[0], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.B Rm,@(R0,Rn) */
    if ((op & 0xf00fU) == 0x0005U) { cv1k_sh3_jit_write16(bus, cpu->r[n] + cpu->r[0], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.W Rm,@(R0,Rn) */
    if ((op & 0xf00fU) == 0x0006U) { cv1k_sh3_jit_write32(bus, cpu->r[n] + cpu->r[0], cpu->r[m]); IR_FALL_DONE(1U); } /* MOV.L Rm,@(R0,Rn) */
    if ((op & 0xf00fU) == 0x000cU) { cpu->r[n] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->r[m] + cpu->r[0]); IR_FALL_DONE(1U); } /* MOV.B @(R0,Rm),Rn */
    if ((op & 0xf00fU) == 0x000dU) { cpu->r[n] = (cv1k_u32)(int16_t)cv1k_sh3_jit_read16(bus, cpu->r[m] + cpu->r[0]); IR_FALL_DONE(1U); } /* MOV.W @(R0,Rm),Rn */
    if ((op & 0xf00fU) == 0x000eU) { cpu->r[n] = cv1k_sh3_jit_read32(bus, cpu->r[m] + cpu->r[0]); IR_FALL_DONE(1U); } /* MOV.L @(R0,Rm),Rn */
    switch ((op >> 8U) & 0x0fU) {
    case 0: if ((op & 0xf000U) == 0x8000U) { cv1k_sh3_jit_write8(bus, cpu->r[m] + (cv1k_u32)(op & 0x0fU), cpu->r[0]); IR_FALL_DONE(1U); } break;
    case 1: if ((op & 0xf000U) == 0x8000U) { cv1k_sh3_jit_write16(bus, cpu->r[m] + ((cv1k_u32)(op & 0x0fU) * 2U), cpu->r[0]); IR_FALL_DONE(1U); } break;
    case 4: if ((op & 0xf000U) == 0x8000U) { cpu->r[0] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->r[m] + (cv1k_u32)(op & 0x0fU)); IR_FALL_DONE(1U); } break;
    case 5: if ((op & 0xf000U) == 0x8000U) { cpu->r[0] = (cv1k_u32)(int16_t)cv1k_sh3_jit_read16(bus, cpu->r[m] + ((cv1k_u32)(op & 0x0fU) * 2U)); IR_FALL_DONE(1U); } break;
    default: break;
    }
    if ((op & 0xff00U) == 0xc000U) { cv1k_sh3_jit_write8(bus, cpu->gbr + (cv1k_u32)(op & 0xffU), cpu->r[0]); IR_FALL_DONE(1U); }
    if ((op & 0xff00U) == 0xc100U) { cv1k_sh3_jit_write16(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 2U), cpu->r[0]); IR_FALL_DONE(1U); }
    if ((op & 0xff00U) == 0xc200U) { cv1k_sh3_jit_write32(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 4U), cpu->r[0]); IR_FALL_DONE(1U); }
    if ((op & 0xff00U) == 0xc400U) { cpu->r[0] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->gbr + (cv1k_u32)(op & 0xffU)); IR_FALL_DONE(1U); }
    if ((op & 0xff00U) == 0xc500U) { cpu->r[0] = (cv1k_u32)(int16_t)cv1k_sh3_jit_read16(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 2U)); IR_FALL_DONE(1U); }
    if ((op & 0xff00U) == 0xc600U) { cpu->r[0] = cv1k_sh3_jit_read32(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 4U)); IR_FALL_DONE(1U); }

    if ((op & 0xf00fU) == 0x400dU) { /* SHLD Rm,Rn */
        if ((cpu->r[m] & 0x80000000U) == 0U) cpu->r[n] <<= (cpu->r[m] & 0x1fU);
        else if ((cpu->r[m] & 0x1fU) == 0U) cpu->r[n] = 0U;
        else cpu->r[n] >>= (((~cpu->r[m]) & 0x1fU) + 1U);
        IR_FALL_DONE(1U);
    }
    if ((op & 0xf00fU) == 0x400cU) { /* SHAD Rm,Rn */
        if ((cpu->r[m] & 0x80000000U) == 0U) cpu->r[n] <<= (cpu->r[m] & 0x1fU);
        else if ((cpu->r[m] & 0x1fU) == 0U) cpu->r[n] = (cpu->r[n] & 0x80000000U) ? 0xffffffffU : 0U;
        else cpu->r[n] = (cv1k_u32)((cv1k_s32)cpu->r[n] >> (((~cpu->r[m]) & 0x1fU) + 1U));
        IR_FALL_DONE(1U);
    }
    if ((op & 0xf00fU) == 0x3004U) { /* DIV1 Rm,Rn: exact inline hot fallback */
        cv1k_u32 old_q = cpu->sr & 0x00000100U; /* SH_Q */
        cv1k_u32 tmp;
        if (cpu->r[n] & 0x80000000U) cpu->sr |= 0x00000100U; else cpu->sr &= ~0x00000100U;
        cpu->r[n] = (cpu->r[n] << 1) | (cpu->sr & 1U);
        if (!old_q) {
            if (!(cpu->sr & 0x00000200U)) { /* !M: subtract */
                tmp = cpu->r[n]; cpu->r[n] -= cpu->r[m];
                if (!(cpu->sr & 0x00000100U)) { if (cpu->r[n] > tmp) cpu->sr |= 0x00000100U; else cpu->sr &= ~0x00000100U; }
                else                         { if (cpu->r[n] > tmp) cpu->sr &= ~0x00000100U; else cpu->sr |= 0x00000100U; }
            } else {                         /* M: add */
                tmp = cpu->r[n]; cpu->r[n] += cpu->r[m];
                if (!(cpu->sr & 0x00000100U)) { if (cpu->r[n] < tmp) cpu->sr &= ~0x00000100U; else cpu->sr |= 0x00000100U; }
                else                         { if (cpu->r[n] < tmp) cpu->sr |= 0x00000100U; else cpu->sr &= ~0x00000100U; }
            }
        } else {
            if (!(cpu->sr & 0x00000200U)) { /* !M: add */
                tmp = cpu->r[n]; cpu->r[n] += cpu->r[m];
                if (!(cpu->sr & 0x00000100U)) { if (cpu->r[n] < tmp) cpu->sr |= 0x00000100U; else cpu->sr &= ~0x00000100U; }
                else                         { if (cpu->r[n] < tmp) cpu->sr &= ~0x00000100U; else cpu->sr |= 0x00000100U; }
            } else {                         /* M: subtract */
                tmp = cpu->r[n]; cpu->r[n] -= cpu->r[m];
                if (!(cpu->sr & 0x00000100U)) { if (cpu->r[n] > tmp) cpu->sr &= ~0x00000100U; else cpu->sr |= 0x00000100U; }
                else                         { if (cpu->r[n] > tmp) cpu->sr |= 0x00000100U; else cpu->sr &= ~0x00000100U; }
            }
        }
        tmp = cpu->sr & 0x00000300U;
        if (tmp == 0U || tmp == 0x00000300U) cpu->sr |= 1U; else cpu->sr &= ~1U;
        IR_FALL_DONE(1U);
    }
#undef IR_SET_T
#undef IR_FALL_DONE
    return 0;
}

static void reset_blocks(void)
{
    struct wasm_block_page *p = g_pages;
    while (p != NULL) { struct wasm_block_page *n = p->next; free(p); p = n; }
    g_pages = NULL;
    memset(g_blocks, 0, sizeof(g_blocks));
    memset(g_fast, 0, sizeof(g_fast));
    g_stat_invalidations++;
    g_stat_blocks = 0U;
}

static const struct cv1k_ir_backend g_wasm_backend = { "wasm-blockjit", NULL, NULL, NULL, NULL };
const struct cv1k_ir_backend *cv1k_ir_select_backend(void) { return &g_wasm_backend; }
int cv1k_ir_phase1_supported(cv1k_u16 op) { return !is_terminal_or_delay(op); }
void cv1k_ir_set_cache(int on) { (void)on; }
void cv1k_ir_set_fastram(int on) { (void)on; }
void cv1k_ir_set_internal_loops(int on) { (void)on; }
int cv1k_ir_enabled(void) { return g_wasm_ir_enabled; }
void cv1k_ir_enable(int on) { g_wasm_ir_enabled = on ? 1 : 0; }
void cv1k_ir_reset(void) { reset_blocks(); }
cv1k_u32 cv1k_ir_compile_block(struct cv1k_bus *bus, cv1k_u32 pc,
                               cv1k_ir_block_fn *out_fn, void **out_mem, size_t *out_cap)
{ (void)bus; (void)pc; if (out_fn) *out_fn = NULL; if (out_mem) *out_mem = NULL; if (out_cap) *out_cap = 0U; return 0U; }
void cv1k_ir_free_block(void *mem, size_t cap) { (void)mem; (void)cap; }

cv1k_u32 cv1k_irjit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                              cv1k_u32 cycle_budget, cv1k_u32 tmu_interval)
{
    if (!g_wasm_ir_enabled || cpu == NULL || bus == NULL) return 0U;
    if (tmu_interval == 0U) tmu_interval = 2048U;
    cv1k_u32 start = cpu->cycles;
    cv1k_u32 frame_end = start + cycle_budget;
    cv1k_u32 next_tmu = cpu->cycles + tmu_interval;
    while ((cv1k_s32)(cpu->cycles - frame_end) < 0) {
        if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        int used = 0;
        if (cpu->sleep_mode == 0U && cpu->m_delay == 0U && cpu->halted == 0U && !irq_can_accept(cpu)) {
            struct wasm_block *b = compile_block(bus, cpu->pc);
            cv1k_u32 to_tmu = next_tmu - cpu->cycles;
            cv1k_u32 to_frame = frame_end - cpu->cycles;
            cv1k_u32 lim = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
            if (b != NULL && b->hot_kind == WASM_HOT_DTNOP) {
                used = run_hot_dtnop(cpu, b->pc, b->hot_r0, next_tmu, frame_end);
                if (used) g_stat_hits++;
            }
            if (!used && b != NULL && b->hot_kind >= WASM_HOT_BRCOND) {
                used = run_hot_branch(cpu, b, next_tmu, frame_end);
                if (used) g_stat_hits++;
            }
            if (!used && b != NULL && b->op_count != 0U && b->cycle_bound != 0U && b->cycle_bound <= lim) {
                cv1k_u32 before = cpu->cycles;
                sh7709s_run_linear_ops(cpu, bus, b->ops, b->op_count);
                used = (cpu->cycles != before);
                if (used) g_stat_hits++;
            }
        }
        if (!used) { sh7709s_step(cpu, bus); g_stat_fallbacks++; }
        if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        if (cpu->pc == cpu->idle_pc0 || cpu->pc == cpu->idle_pc1) {
            cv1k_u32 jump = ((cv1k_s32)(next_tmu - frame_end) < 0) ? next_tmu : frame_end;
            if ((cv1k_s32)(jump - cpu->cycles) > 0) cpu->cycles = jump;
            if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        }
    }
    return cpu->cycles - start;
}

const char *cv1k_wasm_jit_backend_name(void) { return g_wasm_backend.name; }
void cv1k_wasm_jit_stats(cv1k_u32 *blocks, cv1k_u32 *hits, cv1k_u32 *fallbacks, cv1k_u32 *invalidations)
{ if (blocks) *blocks = g_stat_blocks; if (hits) *hits = g_stat_hits; if (fallbacks) *fallbacks = g_stat_fallbacks; if (invalidations) *invalidations = g_stat_invalidations; }
