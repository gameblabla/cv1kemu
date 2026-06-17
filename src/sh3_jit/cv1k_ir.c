/*
 * Phase-1 IR DRC pipeline: SH-3 frontend -> arch-independent IR -> x64 emit.
 *
 * This is the first concrete step of docs/DRC_DESIGN.md.  It is built ISOLATED
 * from the production JIT (sh3_jit.c / sh3_jit_x64.c): it has its own block
 * compiler and is exercised only by tools/jit_difftest (--ir), so the shipping
 * emulator is unaffected until this path is proven and switched over.
 *
 * Phase 1 covers the straight-line ALU/move subset with no guest T-bit effects
 * and no memory/branches; register allocation here is trivial (per-guest-op
 * scratch regs).  Phase 2 replaces the allocator with cross-block linear scan
 * keeping guest regs in host registers (the actual perf win).
 *
 * x64 encodings are copied verbatim from the verified sh3_jit_x64.c emitter.
 */
#include "sh3_jit/cv1k_ir.h"
#include "sh3_jit/sh3_jit_backend.h"
#include "cpu_sh7709s.h"
#include "emu.h"   /* struct cv1k_machine: main_ram base/size for fast-RAM inline */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef CV1K_IR_RUNTIME_STATS
#define CV1K_IR_RUNTIME_STATS 0
#endif
#if CV1K_IR_RUNTIME_STATS
#define IR_STAT_INC(x) do { (x)++; } while (0)
#else
#define IR_STAT_INC(x) do { } while (0)
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ---- tiny x64 emitter (subset, identical encodings to sh3_jit_x64.c) ---- */
enum { RAX=0, RCX=1, RDX=2, RBX=3, RSP=4, RBP=5, RSI=6, RDI=7, R8=8, R9=9, R10=10, R11=11, R12=12, R13=13, R14=14, R15=15 };

struct e64 { uint8_t *p; size_t size, cap; int fail; };

static void e8(struct e64 *e, uint8_t v) { if (e->size < e->cap) e->p[e->size++] = v; else e->fail = 1; }
static void e32(struct e64 *e, uint32_t v) { for (unsigned i = 0; i < 4; i++) e8(e, (uint8_t)(v >> (i * 8))); }
static void rex(struct e64 *e, int w, unsigned r, unsigned x, unsigned b)
{ uint8_t p = (uint8_t)(0x40U | (w ? 8U : 0U) | ((r >> 3U) ? 4U : 0U) | ((x >> 3U) ? 2U : 0U) | ((b >> 3U) ? 1U : 0U)); if (p != 0x40U) e8(e, p); }
static void modrm(struct e64 *e, unsigned mod, unsigned reg, unsigned rm) { e8(e, (uint8_t)((mod << 6) | ((reg & 7U) << 3) | (rm & 7U))); }
static void sib(struct e64 *e, unsigned sc, unsigned ix, unsigned base) { e8(e, (uint8_t)((sc << 6) | ((ix & 7U) << 3) | (base & 7U))); }
static void mem_r12(struct e64 *e, unsigned reg, size_t off) { modrm(e, 2, reg, 4); sib(e, 0, 4, 4); e32(e, (uint32_t)off); }

static void mov_r64_r64(struct e64 *e, unsigned d, unsigned s) { rex(e, 1, s, 0, d); e8(e, 0x89); modrm(e, 3, s, d); }
static void mov_r32_r32(struct e64 *e, unsigned d, unsigned s) { rex(e, 0, s, 0, d); e8(e, 0x89); modrm(e, 3, s, d); }
static void mov_r32_imm32(struct e64 *e, unsigned d, uint32_t imm) { rex(e, 0, 0, 0, d); e8(e, (uint8_t)(0xb8U + (d & 7U))); e32(e, imm); }
static void mov_r32_m32r12(struct e64 *e, unsigned d, size_t off) { rex(e, 0, d, 0, R12); e8(e, 0x8b); mem_r12(e, d, off); }
static void mov_m32r12_r32(struct e64 *e, size_t off, unsigned s) { rex(e, 0, s, 0, R12); e8(e, 0x89); mem_r12(e, s, off); }
static void mov_m32r12_imm32(struct e64 *e, size_t off, uint32_t imm) { rex(e, 0, 0, 0, R12); e8(e, 0xc7); mem_r12(e, 0, off); e32(e, imm); }
static int imm8_sext32(uint32_t imm) { return (uint32_t)(int32_t)(int8_t)imm == imm; }
static void alu_m32r12_imm(struct e64 *e, size_t off, unsigned ext, uint32_t imm) {
    rex(e, 0, ext, 0, R12);
    if (imm8_sext32(imm)) { e8(e, 0x83); mem_r12(e, ext, off); e8(e, (uint8_t)imm); }
    else { e8(e, 0x81); mem_r12(e, ext, off); e32(e, imm); }
}
static void add_m32r12_imm32(struct e64 *e, size_t off, uint32_t imm) { alu_m32r12_imm(e, off, 0, imm); }
static void and_m32r12_imm32(struct e64 *e, size_t off, uint32_t imm) { alu_m32r12_imm(e, off, 4, imm); }
static void or_m32r12_imm32(struct e64 *e, size_t off, uint32_t imm) { alu_m32r12_imm(e, off, 1, imm); }
static void or_m32r12_r32(struct e64 *e, size_t off, unsigned s) { rex(e, 0, s, 0, R12); e8(e, 0x09); mem_r12(e, s, off); }
static void add_m32r12_r32(struct e64 *e, size_t off, unsigned s) { rex(e, 0, s, 0, R12); e8(e, 0x01); mem_r12(e, s, off); }
/* dst = dst OP src (op r/m32, r32): 01 add, 29 sub, 21 and, 09 or, 31 xor */
static void alu_rr(struct e64 *e, uint8_t opc, unsigned d, unsigned s) { rex(e, 0, s, 0, d); e8(e, opc); modrm(e, 3, s, d); }
static void adc_rr(struct e64 *e, unsigned d, unsigned s) { rex(e, 0, s, 0, d); e8(e, 0x11); modrm(e, 3, s, d); }
static void sbb_rr(struct e64 *e, unsigned d, unsigned s) { rex(e, 0, s, 0, d); e8(e, 0x19); modrm(e, 3, s, d); }
/* dst = dst OP imm32 (81 /ext): 0 add, 1 or, 4 and, 5 sub, 6 xor */
static void alu_ri(struct e64 *e, unsigned ext, unsigned d, uint32_t imm) {
    rex(e, 0, ext, 0, d);
    if (imm8_sext32(imm)) { e8(e, 0x83); modrm(e, 3, ext, d); e8(e, (uint8_t)imm); }
    else { e8(e, 0x81); modrm(e, 3, ext, d); e32(e, imm); }
}
static void not_r32(struct e64 *e, unsigned r) { rex(e, 0, 2, 0, r); e8(e, 0xf7); modrm(e, 3, 2, r); }
static void neg_r32(struct e64 *e, unsigned r) { rex(e, 0, 3, 0, r); e8(e, 0xf7); modrm(e, 3, 3, r); }
static void mul_r32(struct e64 *e, unsigned r) { rex(e, 0, 4, 0, r); e8(e, 0xf7); modrm(e, 3, 4, r); }
static void imul1_r32(struct e64 *e, unsigned r) { rex(e, 0, 5, 0, r); e8(e, 0xf7); modrm(e, 3, 5, r); }
static void imul_rr(struct e64 *e, unsigned d, unsigned s) { rex(e, 0, d, 0, s); e8(e, 0x0f); e8(e, 0xaf); modrm(e, 3, d, s); }
/* shift r32 by imm8 (C1 /ext): 4 shl, 5 shr, 7 sar */
static void sh_ri(struct e64 *e, unsigned ext, unsigned r, uint8_t imm) { rex(e, 0, ext, 0, r); e8(e, 0xc1); modrm(e, 3, ext, r); e8(e, imm); }
static void sh_cl(struct e64 *e, unsigned ext, unsigned r) { rex(e, 0, ext, 0, r); e8(e, 0xd3); modrm(e, 3, ext, r); }
static void e64imm(struct e64 *e, uint64_t v) { for (unsigned i = 0; i < 8; i++) e8(e, (uint8_t)(v >> (i * 8))); }
static void mov_r64_imm64(struct e64 *e, unsigned d, uint64_t imm) { rex(e, 1, 0, 0, d); e8(e, (uint8_t)(0xb8U + (d & 7U))); e64imm(e, imm); }
static void call_rax(struct e64 *e, const void *fn) { mov_r64_imm64(e, RAX, (uintptr_t)fn); e8(e, 0xff); e8(e, 0xd0); }
static void jmp_r64(struct e64 *e, unsigned r) { rex(e, 0, 0, 0, r); e8(e, 0xff); modrm(e, 3, 4, r); }
static size_t mov_r64_imm64_site(struct e64 *e, unsigned d, uint64_t imm) { rex(e, 1, 0, 0, d); e8(e, (uint8_t)(0xb8U + (d & 7U))); size_t at = e->size; e64imm(e, imm); return at; }
static void movsx_eax_al(struct e64 *e) { e8(e, 0x0f); e8(e, 0xbe); e8(e, 0xc0); }
static void movsx_eax_ax(struct e64 *e) { e8(e, 0x0f); e8(e, 0xbf); e8(e, 0xc0); }
static void sub_rsp_imm8(struct e64 *e, uint8_t n) { e8(e, 0x48); e8(e, 0x83); e8(e, 0xec); e8(e, n); }
static void add_rsp_imm8(struct e64 *e, uint8_t n) { e8(e, 0x48); e8(e, 0x83); e8(e, 0xc4); e8(e, n); }
/* control-flow + fast-RAM helpers (fast-path inlining) */
static size_t jcc32(struct e64 *e, uint8_t cc) { e8(e, 0x0f); e8(e, (uint8_t)(0x80U | cc)); size_t p = e->size; e32(e, 0); return p; }
static size_t jmp32(struct e64 *e) { e8(e, 0xe9); size_t p = e->size; e32(e, 0); return p; }
static void patch_here(struct e64 *e, size_t at) { int32_t rel = (int32_t)(e->size - (at + 4)); if (at + 4 <= e->cap) memcpy(e->p + at, &rel, 4); }
static void patch_to(struct e64 *e, size_t at, size_t target) { int32_t rel = (int32_t)(target - (at + 4)); if (at + 4 <= e->cap) memcpy(e->p + at, &rel, 4); }
static void cmp_r32_imm32(struct e64 *e, unsigned r, uint32_t imm) {
    rex(e, 0, 7, 0, r);
    if (imm8_sext32(imm)) { e8(e, 0x83); modrm(e, 3, 7, r); e8(e, (uint8_t)imm); }
    else { e8(e, 0x81); modrm(e, 3, 7, r); e32(e, imm); }
}
static void add_r64_r64(struct e64 *e, unsigned d, unsigned s) { rex(e, 1, s, 0, d); e8(e, 0x01); modrm(e, 3, s, d); }
static void bswap_r32(struct e64 *e, unsigned r) { if (r >= 8) e8(e, 0x41); e8(e, 0x0f); e8(e, (uint8_t)(0xc8U + (r & 7U))); }
static void ror_r16_imm8(struct e64 *e, unsigned r, uint8_t imm) { e8(e, 0x66); rex(e, 0, 1, 0, r); e8(e, 0xc1); modrm(e, 3, 1, r); e8(e, imm); }
static void movsx_r32_r16(struct e64 *e, unsigned d, unsigned s) { rex(e, 0, d, 0, s); e8(e, 0x0f); e8(e, 0xbf); modrm(e, 3, d, s); }
/* [base] addressing (base must not be rsp/rbp/r12/r13; we use r9) */
static void mov_r32_memb(struct e64 *e, unsigned dst, unsigned base) { rex(e, 0, dst, 0, base); e8(e, 0x8b); modrm(e, 0, dst, base); }
static void movzx_r32_memb16(struct e64 *e, unsigned dst, unsigned base) { rex(e, 0, dst, 0, base); e8(e, 0x0f); e8(e, 0xb7); modrm(e, 0, dst, base); }
static void movsx_r32_memb8(struct e64 *e, unsigned dst, unsigned base) { rex(e, 0, dst, 0, base); e8(e, 0x0f); e8(e, 0xbe); modrm(e, 0, dst, base); }
static void mov_memb_r32(struct e64 *e, unsigned base, unsigned src) { rex(e, 0, src, 0, base); e8(e, 0x89); modrm(e, 0, src, base); }
static void mov_memb_r16(struct e64 *e, unsigned base, unsigned src) { e8(e, 0x66); rex(e, 0, src, 0, base); e8(e, 0x89); modrm(e, 0, src, base); }
static void mov_memb_r8(struct e64 *e, unsigned base, unsigned src) { rex(e, 0, src, 0, base); e8(e, 0x88); modrm(e, 0, src, base); }

/* compare / test / setcc / 1-bit shift primitives (for T-setting guest ops) */
static void cmp_rr(struct e64 *e, unsigned a, unsigned b) { rex(e, 0, b, 0, a); e8(e, 0x39); modrm(e, 3, b, a); }  /* flags of a-b */
static void test_rr(struct e64 *e, unsigned a, unsigned b) { rex(e, 0, b, 0, a); e8(e, 0x85); modrm(e, 3, b, a); } /* flags of a&b */
static void test_ri(struct e64 *e, unsigned a, uint32_t imm) { rex(e, 0, 0, 0, a); e8(e, 0xf7); modrm(e, 3, 0, a); e32(e, imm); }
static void bt_m32r12_imm8(struct e64 *e, size_t off, uint8_t bit) { rex(e, 0, 4, 0, R12); e8(e, 0x0f); e8(e, 0xba); mem_r12(e, 4, off); e8(e, bit); }
static void setcc_reg(struct e64 *e, uint8_t cc, unsigned r) { rex(e, 0, 0, 0, r); e8(e, 0x0f); e8(e, cc); modrm(e, 3, 0, r); } /* setcc r8 (r in RAX/RCX/RDX) */
static void movzx_r32_r8(struct e64 *e, unsigned d, unsigned s) { rex(e, 0, d, 0, s); e8(e, 0x0f); e8(e, 0xb6); modrm(e, 3, d, s); }
/* D1 /ext: shift/rotate r by 1, setting CF.  ext: 0 rol,1 ror,2 rcl,3 rcr,4 shl,5 shr,7 sar */
static void shift1(struct e64 *e, unsigned ext, unsigned r) { rex(e, 0, ext, 0, r); e8(e, 0xd1); modrm(e, 3, ext, r); }

#define OFF_R(n)    (offsetof(struct sh7709s_cpu, r) + (size_t)(n) * sizeof(cv1k_u32))
#define OFF_PC      offsetof(struct sh7709s_cpu, pc)
#define OFF_PPC     offsetof(struct sh7709s_cpu, ppc)
#define OFF_CYCLES  offsetof(struct sh7709s_cpu, cycles)
#define OFF_SR      offsetof(struct sh7709s_cpu, sr)
#define OFF_PR      offsetof(struct sh7709s_cpu, pr)
#define OFF_GBR     offsetof(struct sh7709s_cpu, gbr)
#define OFF_VBR     offsetof(struct sh7709s_cpu, vbr)
#define OFF_MACH    offsetof(struct sh7709s_cpu, mach)
#define OFF_MACL    offsetof(struct sh7709s_cpu, macl)
#define OFF_EA      offsetof(struct sh7709s_cpu, ea)
#define OFF_M_DELAY offsetof(struct sh7709s_cpu, m_delay)
#define OFF_HALTED  offsetof(struct sh7709s_cpu, halted)
#define OFF_PEND_MASK offsetof(struct sh7709s_cpu, pend_mask)
#define OFF_SLEEP_MODE offsetof(struct sh7709s_cpu, sleep_mode)

static void push_reg(struct e64 *e, unsigned r) { if (r >= 8) e8(e, 0x41); e8(e, (uint8_t)(0x50 + (r & 7U))); }
static void pop_reg(struct e64 *e, unsigned r)  { if (r >= 8) e8(e, 0x41); e8(e, (uint8_t)(0x58 + (r & 7U))); }

/* Native-code arena for IR blocks.  The old path mmap()'d one 16 KiB RX/WX
 * region per compiled block; CV1K boots compile tens of thousands of small
 * blocks, so syscall cost and scattered i-cache locality show up in frame
 * benchmarks.  Keep the per-block reservation model (so existing cap checks stay
 * valid) but suballocate those reservations from larger executable chunks. */
#define IR_CODE_SLOT_SIZE 16384U
#define IR_CODE_SLICE_SLOT_SIZE 8192U
#define IR_CODE_CHUNK_SIZE (4U * 1024U * 1024U)
struct ir_code_chunk { uint8_t *mem; size_t cap, used; struct ir_code_chunk *next; };
static struct ir_code_chunk *g_ir_code_chunks = NULL;

static void *ir_code_alloc(size_t cap, size_t *actual_cap)
{
#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
#if defined(_WIN32)
    size_t pagesz = 4096U;
#else
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
#endif
    cap = (cap + pagesz - 1U) & ~(pagesz - 1U);
    if (cap < IR_CODE_SLICE_SLOT_SIZE) cap = IR_CODE_SLICE_SLOT_SIZE;
    struct ir_code_chunk *c = g_ir_code_chunks;
    if (c == NULL || c->used + cap > c->cap) {
        size_t chunk_cap = IR_CODE_CHUNK_SIZE;
        if (chunk_cap < cap) chunk_cap = cap;
        chunk_cap = (chunk_cap + pagesz - 1U) & ~(pagesz - 1U);
#if defined(_WIN32)
        void *mem = VirtualAlloc(NULL, chunk_cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (mem == NULL) return NULL;
#else
        void *mem = mmap(NULL, chunk_cap, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mem == MAP_FAILED) return NULL;
#endif
        c = (struct ir_code_chunk *)calloc(1, sizeof(*c));
        if (c == NULL) {
#if defined(_WIN32)
            VirtualFree(mem, 0, MEM_RELEASE);
#else
            munmap(mem, chunk_cap);
#endif
            return NULL;
        }
        c->mem = (uint8_t *)mem;
        c->cap = chunk_cap;
        c->used = 0;
        c->next = g_ir_code_chunks;
        g_ir_code_chunks = c;
    }
    void *ret = c->mem + c->used;
    c->used += cap;
    if (actual_cap) *actual_cap = cap;
    return ret;
#else
    (void)cap; (void)actual_cap; return NULL;
#endif
}

static int ir_code_ptr_in_arena(void *mem)
{
    uint8_t *p = (uint8_t *)mem;
    for (struct ir_code_chunk *c = g_ir_code_chunks; c; c = c->next)
        if (p >= c->mem && p < c->mem + c->cap) return 1;
    return 0;
}

static void ir_code_arena_reset(void)
{
#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
    struct ir_code_chunk *c = g_ir_code_chunks;
    while (c) {
        struct ir_code_chunk *n = c->next;
        if (c->mem) {
#if defined(_WIN32)
            VirtualFree(c->mem, 0, MEM_RELEASE);
#else
            munmap(c->mem, c->cap);
#endif
        }
        free(c);
        c = n;
    }
    g_ir_code_chunks = NULL;
#endif
}

/* Phase-2 blocks contain no calls, so stack alignment is irrelevant; we save
 * only the callee-saved registers actually used (r12=cpu plus whichever of
 * rbx/r14/r15 the allocator assigned), keeping per-block overhead minimal. */
static void prologue(struct e64 *e, const unsigned *save, int nsave)
{
    for (int i = 0; i < nsave; i++) push_reg(e, save[i]);
    mov_r64_r64(e, R12, RDI);          /* r12 = cpu */
}
/* set_pc: write cpu->pc = next_pc (linear block); 0 = a branch terminator
 * already set cpu->pc. */
static void epilogue_tail(struct e64 *e, const unsigned *save, int nsave, int pad, int set_pc, cv1k_u32 next_pc, cv1k_u32 cycles)
{
    if (set_pc) mov_m32r12_imm32(e, OFF_PC, next_pc);
    add_m32r12_imm32(e, OFF_CYCLES, cycles);
    mov_r32_imm32(e, RAX, cycles);
    if (pad) add_rsp_imm8(e, (uint8_t)pad);
    for (int i = nsave - 1; i >= 0; i--) pop_reg(e, save[i]);
    e8(e, 0xc3);                       /* ret */
}
static void epilogue_tail_rax_cycles(struct e64 *e, const unsigned *save, int nsave, int pad)
{
    add_m32r12_r32(e, OFF_CYCLES, RAX);
    if (pad) add_rsp_imm8(e, (uint8_t)pad);
    for (int i = nsave - 1; i >= 0; i--) pop_reg(e, save[i]);
    e8(e, 0xc3);                       /* ret; eax already holds cycles */
}
static void cmovcc_r32(struct e64 *e, uint8_t cc, unsigned d, unsigned s) { rex(e, 0, d, 0, s); e8(e, 0x0f); e8(e, cc); modrm(e, 3, d, s); }

#define IR_CHAIN_MAX_SITES 2
struct ir_last_chain_site { cv1k_u32 target_pc; cv1k_u32 source_cycles; size_t total_imm_off; size_t body_imm_off; };
static cv1k_u32 g_ir_chain_limit = 0;
static size_t g_ir_last_body_off = 0;
static int g_ir_last_chain_count = 0;
static int g_ir_last_chainable_body = 0;
static struct ir_last_chain_site g_ir_last_chain[IR_CHAIN_MAX_SITES];
static cv1k_u32 g_ir_compile_cycle_limit = 0;
static size_t g_ir_compile_code_slot_size = 0;

static void chain_epilogue_tail(struct e64 *e, cv1k_u32 cycles)
{
    add_m32r12_imm32(e, OFF_CYCLES, cycles);
    mov_r32_imm32(e, RAX, cycles);
    pop_reg(e, R15); pop_reg(e, R14); pop_reg(e, RBX);
    pop_reg(e, R13); pop_reg(e, R12);
    e8(e, 0xc3);
}

static void chain_prologue(struct e64 *e, uint64_t ram_base)
{
    (void)ram_base;
    push_reg(e, R12); push_reg(e, R13);
    push_reg(e, RBX); push_reg(e, R14); push_reg(e, R15);
    mov_r64_r64(e, R12, RDI);          /* r12 = cpu */
    mov_r64_r64(e, R13, RSI);          /* r13 = bus */
}

static size_t emit_body_chain_exit(struct e64 *e, cv1k_u32 target_pc, cv1k_u32 source_cycles)
{
    /* cpu->pc is made current for debugging/fallback visibility and for the
     * guarded no-chain return path.  Chain only when no new event state appeared
     * and both this source plus the target block fit inside g_ir_chain_limit. */
    mov_m32r12_imm32(e, OFF_PC, target_pc);

    mov_r32_m32r12(e, RAX, OFF_PEND_MASK);
    test_rr(e, RAX, RAX);
    size_t fail_pend = jcc32(e, 0x05); /* jnz */
    mov_r32_m32r12(e, RAX, OFF_M_DELAY);
    test_rr(e, RAX, RAX);
    size_t fail_mdelay = jcc32(e, 0x05);
    mov_r32_m32r12(e, RAX, OFF_HALTED);
    test_rr(e, RAX, RAX);
    size_t fail_halt = jcc32(e, 0x05);
    mov_r32_m32r12(e, RAX, OFF_SLEEP_MODE);
    test_rr(e, RAX, RAX);
    size_t fail_sleep = jcc32(e, 0x05);

    mov_r64_imm64(e, RDX, (uintptr_t)&g_ir_chain_limit);
    mov_r32_memb(e, RDX, RDX);         /* edx = run limit */
    test_rr(e, RDX, RDX);
    size_t fail_nolimit = jcc32(e, 0x04); /* jz: standalone compile/difftest or no run budget */
    mov_r32_m32r12(e, RAX, OFF_CYCLES);
    alu_rr(e, 0x29, RDX, RAX);         /* edx = limit - cpu->cycles */
    cmp_r32_imm32(e, RDX, 0xffffffffU);/* patched to source+target cycles; unpatched always fails */
    size_t total_imm = e->size - 4;
    size_t fail_limit = jcc32(e, 0x02); /* jb: remaining < total */

    add_m32r12_imm32(e, OFF_CYCLES, source_cycles);
    mov_r64_imm64_site(e, RDX, 0);
    size_t body_imm = e->size - 8;
    jmp_r64(e, RDX);

    size_t fail = e->size;
    patch_to(e, fail_pend, fail); patch_to(e, fail_mdelay, fail);
    patch_to(e, fail_halt, fail); patch_to(e, fail_sleep, fail); patch_to(e, fail_nolimit, fail); patch_to(e, fail_limit, fail);
    chain_epilogue_tail(e, source_cycles);
    if (g_ir_last_chain_count < IR_CHAIN_MAX_SITES) {
        g_ir_last_chain[g_ir_last_chain_count].target_pc = target_pc;
        g_ir_last_chain[g_ir_last_chain_count].source_cycles = source_cycles;
        g_ir_last_chain[g_ir_last_chain_count].total_imm_off = total_imm;
        g_ir_last_chain[g_ir_last_chain_count].body_imm_off = body_imm;
        g_ir_last_chain_count++;
    }
    return body_imm;
}

/* Local lowering flags (reuse the cv1k_ir_inst.flags byte; disjoint from the
 * shared IMMB/SEXT/DIRTY bits 0x01/0x02/0x04). */
#define LFLAG_CMP_TST  0x08u   /* IR_CMP: use TEST/AND (Rn & Rm) instead of CMP   */
#define LFLAG_CMP_ZERO 0x10u   /* IR_CMP: compare operand a against 0             */
#define LFLAG_SETT     0x20u   /* IR_SHL/SHR/SAR: shift by 1 and set T from carry */

enum { H_DIV0U = 1, H_DIV0S, H_DIV1, H_MULL, H_NEGC, H_SWAPB, H_SWAPW };
enum { S_SR = 1, S_GBR, S_VBR, S_PR, S_MACH, S_MACL };

/* ---- IR builder (per guest op) ---- */
struct iropbuf { struct cv1k_ir_inst in[16]; int n; cv1k_ir_vreg nv; };
static cv1k_ir_vreg ir_newv(struct iropbuf *b) { return b->nv++; }
static void ir_push(struct iropbuf *b, cv1k_u8 op, cv1k_ir_vreg dst, cv1k_ir_vreg a, cv1k_ir_vreg bb, cv1k_u8 flags, cv1k_u32 imm)
{
    if (b->n >= (int)(sizeof b->in / sizeof b->in[0])) return;
    struct cv1k_ir_inst *i = &b->in[b->n++];
    i->op = op; i->dst = dst; i->a = a; i->b = bb; i->flags = flags; i->imm = imm; i->aux = 0;
}

static cv1k_s32 sext8(cv1k_u32 v) { return (cv1k_s32)(int8_t)(v & 0xffU); }
static cv1k_s32 sext12(cv1k_u32 v) { v &= 0xfffU; return (cv1k_s32)((v ^ 0x800U) - 0x800U); }

static cv1k_u32 special_off(cv1k_u8 s)
{
    switch (s) {
    case S_SR:   return (cv1k_u32)OFF_SR;
    case S_GBR:  return (cv1k_u32)OFF_GBR;
    case S_VBR:  return (cv1k_u32)OFF_VBR;
    case S_PR:   return (cv1k_u32)OFF_PR;
    case S_MACH: return (cv1k_u32)OFF_MACH;
    case S_MACL: return (cv1k_u32)OFF_MACL;
    default:     return 0;
    }
}

/* Lower one SH-3 op into the per-op IR buffer.  Returns 1 if this op is part of
 * the phase-1 IR subset, else 0 (block ends here). */
static int ir_lower(cv1k_u16 op, cv1k_u32 pc, struct iropbuf *b)
{
    unsigned n = (op >> 8) & 15U, m = (op >> 4) & 15U;
    cv1k_ir_vreg v0, v1;
    b->n = 0; b->nv = 0;
    if (op == 0x0009U) return 1;             /* NOP: empty block op */
    if (op == 0x0008U) { ir_push(b, IR_TSTORE, 0, 0, 0, CV1K_IR_FLAG_IMMB, 0U); return 1; } /* CLRT */
    if (op == 0x0018U) { ir_push(b, IR_TSTORE, 0, 0, 0, CV1K_IR_FLAG_IMMB, 1U); return 1; } /* SETT */
    if (op == 0x0019U) { ir_push(b, IR_CALLH, 0, 0, 0, 0, 0); b->in[b->n - 1].aux = H_DIV0U; return 1; } /* DIV0U */
    if ((op & 0xf0ffU) == 0x0029U) { /* MOVT Rn */
        v0 = ir_newv(b); ir_push(b, IR_SLOAD, v0, 0, 0, 0, S_SR);
        ir_push(b, IR_AND, v0, v0, 0, CV1K_IR_FLAG_IMMB, 1U);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    if ((op & 0xff00U) == 0xc700U) { /* MOVA @(disp,PC),R0 */
        cv1k_u32 addr = ((pc + 4U) & ~3U) + (cv1k_u32)(op & 0xffU) * 4U;
        v0 = ir_newv(b); ir_push(b, IR_MOVI, v0, 0, 0, 0, addr);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, 0); return 1;
    }
    if ((op & 0xf0ffU) == 0x0002U || (op & 0xf0ffU) == 0x0012U || (op & 0xf0ffU) == 0x0022U ||
        (op & 0xf0ffU) == 0x000aU || (op & 0xf0ffU) == 0x001aU || (op & 0xf0ffU) == 0x002aU) { /* STC/STS special,Rn */
        cv1k_u8 sp = (op & 0xf0ffU) == 0x0002U ? S_SR   : (op & 0xf0ffU) == 0x0012U ? S_GBR :
                    (op & 0xf0ffU) == 0x0022U ? S_VBR  : (op & 0xf0ffU) == 0x000aU ? S_MACH :
                    (op & 0xf0ffU) == 0x001aU ? S_MACL : S_PR;
        v0 = ir_newv(b); ir_push(b, IR_SLOAD, v0, 0, 0, 0, sp);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    if ((op & 0xf0ffU) == 0x401eU || (op & 0xf0ffU) == 0x402eU ||
        (op & 0xf0ffU) == 0x400aU || (op & 0xf0ffU) == 0x401aU || (op & 0xf0ffU) == 0x402aU) { /* LDC/LDS Rn,special (not SR; SR needs bank swap) */
        cv1k_u8 sp = (op & 0xf0ffU) == 0x401eU ? S_GBR  : (op & 0xf0ffU) == 0x402eU ? S_VBR :
                    (op & 0xf0ffU) == 0x400aU ? S_MACH : (op & 0xf0ffU) == 0x401aU ? S_MACL : S_PR;
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n);
        ir_push(b, IR_SSTORE, 0, v0, 0, 0, sp); return 1;
    }
    if ((op & 0xf0ffU) == 0x4002U || (op & 0xf0ffU) == 0x4012U || (op & 0xf0ffU) == 0x4022U ||
        (op & 0xf0ffU) == 0x4003U || (op & 0xf0ffU) == 0x4013U || (op & 0xf0ffU) == 0x4023U) { /* STS.L/STC.L special,@-Rn */
        cv1k_u8 sp = (op & 0xf0ffU) == 0x4002U ? S_MACH : (op & 0xf0ffU) == 0x4012U ? S_MACL :
                    (op & 0xf0ffU) == 0x4022U ? S_PR   : (op & 0xf0ffU) == 0x4003U ? S_SR   :
                    (op & 0xf0ffU) == 0x4013U ? S_GBR  : S_VBR;
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n);
        ir_push(b, IR_SUB, v0, v0, 0, CV1K_IR_FLAG_IMMB, 4U);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n);
        ir_push(b, IR_SLOAD, v1, 0, 0, 0, sp);
        ir_push(b, IR_STORE, 0, v0, v1, 0, 0); b->in[b->n - 1].aux = 4U; return 1;
    }
    if ((op & 0xf0ffU) == 0x4006U || (op & 0xf0ffU) == 0x4016U || (op & 0xf0ffU) == 0x4026U ||
        (op & 0xf0ffU) == 0x4017U || (op & 0xf0ffU) == 0x4027U) { /* LDS.L/LDC.L @Rn+,special (not SR; SR needs bank swap) */
        cv1k_u8 sp = (op & 0xf0ffU) == 0x4006U ? S_MACH : (op & 0xf0ffU) == 0x4016U ? S_MACL :
                    (op & 0xf0ffU) == 0x4026U ? S_PR   : (op & 0xf0ffU) == 0x4017U ? S_GBR  : S_VBR;
        cv1k_ir_vreg v2; v0 = ir_newv(b); v1 = ir_newv(b); v2 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n);
        ir_push(b, IR_LOAD, v1, v0, 0, 0, 0); b->in[b->n - 1].aux = 4U;
        ir_push(b, IR_SSTORE, 0, v1, 0, 0, sp);
        ir_push(b, IR_GLOAD, v2, 0, 0, 0, n);
        ir_push(b, IR_ADD, v2, v2, 0, CV1K_IR_FLAG_IMMB, 4U);
        ir_push(b, IR_GSTORE, 0, v2, 0, 0, n); return 1;
    }
    if ((op & 0xf000U) == 0xe000U) { /* MOV #imm,Rn */
        v0 = ir_newv(b); ir_push(b, IR_MOVI, v0, 0, 0, 0, (cv1k_u32)sext8(op));
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    if ((op & 0xf000U) == 0x7000U) { /* ADD #imm,Rn */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n);
        ir_push(b, IR_ADD, v0, v0, 0, CV1K_IR_FLAG_IMMB, (cv1k_u32)sext8(op));
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    if ((op & 0xf0ffU) == 0x4010U) { /* DT Rn: Rn--; T = (Rn==0) */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n);
        ir_push(b, IR_DECT, v0, v0, 0, 0, 0);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    /* one-register shifts / rotates / sign compares (0x4n**) */
    switch (op & 0xf0ffU) {
    case 0x4000U: case 0x4020U: /* SHLL / SHAL: Rn<<=1; T=MSB */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_SHL, v0, v0, 0, LFLAG_SETT, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x4001U: /* SHLR: Rn>>=1 logical; T=LSB */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_SHR, v0, v0, 0, LFLAG_SETT, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x4021U: /* SHAR: Rn>>=1 arithmetic; T=LSB */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_SAR, v0, v0, 0, LFLAG_SETT, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x4008U: case 0x4018U: case 0x4028U: { /* SHLL2/8/16: no T */
        unsigned sh = (op & 0xffU) == 0x08U ? 2U : (op & 0xffU) == 0x18U ? 8U : 16U;
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_SHL, v0, v0, 0, 0, sh); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1; }
    case 0x4009U: case 0x4019U: case 0x4029U: { /* SHLR2/8/16: no T */
        unsigned sh = (op & 0xffU) == 0x09U ? 2U : (op & 0xffU) == 0x19U ? 8U : 16U;
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_SHR, v0, v0, 0, 0, sh); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1; }
    case 0x4004U: /* ROTL: T=MSB */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_ROL, v0, v0, 0, 0, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x4005U: /* ROTR: T=LSB */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_ROR, v0, v0, 0, 0, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x4024U: /* ROTCL: rotate left through T */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_ROCL, v0, v0, 0, 0, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x4025U: /* ROTCR: rotate right through T */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_ROCR, v0, v0, 0, 0, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x4011U: /* CMP/PZ: T = (Rn >= 0) signed */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_CMP, v0, v0, 0, LFLAG_CMP_ZERO, 0); b->in[b->n - 1].aux = 0x9dU; return 1;
    case 0x4015U: /* CMP/PL: T = (Rn > 0) signed */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_CMP, v0, v0, 0, LFLAG_CMP_ZERO, 0); b->in[b->n - 1].aux = 0x9fU; return 1;
    default: break;
    }
    if ((op & 0xff00U) == 0x8800U) { /* CMP/EQ #imm,R0: T = (R0 == sext imm) */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, 0); ir_push(b, IR_CMP, v0, v0, 0, CV1K_IR_FLAG_IMMB, (cv1k_u32)sext8(op)); b->in[b->n - 1].aux = 0x94U; return 1;
    }
    if ((op & 0xff00U) == 0xc800U) { /* TST #imm,R0: T = ((R0 & imm)==0) */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, 0); ir_push(b, IR_CMP, v0, v0, 0, LFLAG_CMP_TST | CV1K_IR_FLAG_IMMB, op & 0xffU); b->in[b->n - 1].aux = 0x94U; return 1;
    }
    if ((op & 0xff00U) == 0xc900U) { /* AND #imm,R0 (zero-extended) */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, 0); ir_push(b, IR_AND, v0, v0, 0, CV1K_IR_FLAG_IMMB, op & 0xffU); ir_push(b, IR_GSTORE, 0, v0, 0, 0, 0); return 1;
    }
    if ((op & 0xff00U) == 0xca00U) { /* XOR #imm,R0 */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, 0); ir_push(b, IR_XOR, v0, v0, 0, CV1K_IR_FLAG_IMMB, op & 0xffU); ir_push(b, IR_GSTORE, 0, v0, 0, 0, 0); return 1;
    }
    if ((op & 0xff00U) == 0xcb00U) { /* OR #imm,R0 */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, 0); ir_push(b, IR_OR, v0, v0, 0, CV1K_IR_FLAG_IMMB, op & 0xffU); ir_push(b, IR_GSTORE, 0, v0, 0, 0, 0); return 1;
    }
    if ((op & 0xf000U) == 0x5000U) { /* MOV.L @(disp,Rm),Rn */
        cv1k_u32 off = (cv1k_u32)(op & 0xfU) * 4U;
        v0 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, m); ir_push(b, IR_ADD, v0, v0, 0, CV1K_IR_FLAG_IMMB, off);
        ir_push(b, IR_LOAD, v0, v0, 0, 0, 0); b->in[b->n - 1].aux = 4U;
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    if ((op & 0xf000U) == 0x1000U) { /* MOV.L Rm,@(disp,Rn) */
        cv1k_u32 off = (cv1k_u32)(op & 0xfU) * 4U;
        v0 = ir_newv(b); v1 = ir_newv(b); cv1k_ir_vreg vt = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_ADD, vt, v0, 0, CV1K_IR_FLAG_IMMB, off); /* vt = Rn+off (temp, Rn intact) */
        ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);
        ir_push(b, IR_STORE, 0, vt, v1, 0, 0); b->in[b->n - 1].aux = 4U; return 1;
    }
    if ((op & 0xff00U) == 0x8400U || (op & 0xff00U) == 0x8500U) { /* MOV.B/W @(disp,Rm),R0 */
        unsigned sz = (op & 0xff00U) == 0x8400U ? 1U : 2U; unsigned rm = (op >> 4) & 15U;
        cv1k_u32 off = (cv1k_u32)(op & 0xfU) * sz;
        v0 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, rm); ir_push(b, IR_ADD, v0, v0, 0, CV1K_IR_FLAG_IMMB, off);
        ir_push(b, IR_LOAD, v0, v0, 0, CV1K_IR_FLAG_SEXT, 0); b->in[b->n - 1].aux = (cv1k_u8)sz;
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, 0); return 1;
    }
    if ((op & 0xff00U) == 0x8000U || (op & 0xff00U) == 0x8100U) { /* MOV.B/W R0,@(disp,Rm) */
        unsigned sz = (op & 0xff00U) == 0x8000U ? 1U : 2U; unsigned rm = (op >> 4) & 15U;
        cv1k_u32 off = (cv1k_u32)(op & 0xfU) * sz;
        v0 = ir_newv(b); v1 = ir_newv(b); cv1k_ir_vreg vt = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, rm); ir_push(b, IR_ADD, vt, v0, 0, CV1K_IR_FLAG_IMMB, off);
        ir_push(b, IR_GLOAD, v1, 0, 0, 0, 0);
        ir_push(b, IR_STORE, 0, vt, v1, 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz; return 1;
    }
    if ((op & 0xf000U) == 0x0000U) { /* MOV.B/W/L Rm,@(R0,Rn) and @(R0,Rm),Rn */
        unsigned lo = op & 0x000fU;
        if (lo == 0x04U || lo == 0x05U || lo == 0x06U) {
            unsigned sz = lo == 0x04U ? 1U : lo == 0x05U ? 2U : 4U;
            cv1k_ir_vreg vbase = ir_newv(b), vzero = ir_newv(b), vdata = ir_newv(b);
            ir_push(b, IR_GLOAD, vbase, 0, 0, 0, n);
            ir_push(b, IR_GLOAD, vzero, 0, 0, 0, 0);
            ir_push(b, IR_GLOAD, vdata, 0, 0, 0, m);
            ir_push(b, IR_STOREIDX, vdata, vbase, vzero, 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz; return 1;
        }
        if (lo == 0x0cU || lo == 0x0dU || lo == 0x0eU) {
            unsigned sz = lo == 0x0cU ? 1U : lo == 0x0dU ? 2U : 4U;
            cv1k_ir_vreg vbase = ir_newv(b), vzero = ir_newv(b), vdata = ir_newv(b);
            ir_push(b, IR_GLOAD, vbase, 0, 0, 0, m);
            ir_push(b, IR_GLOAD, vzero, 0, 0, 0, 0);
            ir_push(b, IR_LOADIDX, vdata, vbase, vzero, (sz != 4U) ? CV1K_IR_FLAG_SEXT : 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz;
            ir_push(b, IR_GSTORE, 0, vdata, 0, 0, n); return 1;
        }
    }
    if ((op & 0xf000U) == 0x9000U) { /* MOV.W @(disp,PC),Rn (PC-relative constant) */
        cv1k_u32 addr = pc + 4U + (cv1k_u32)(op & 0xffU) * 2U;
        v0 = ir_newv(b);
        ir_push(b, IR_LOAD, v0, 0, 0, CV1K_IR_FLAG_IMMB | CV1K_IR_FLAG_SEXT, addr); b->in[b->n - 1].aux = 2U;
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    if ((op & 0xf000U) == 0xd000U) { /* MOV.L @(disp,PC),Rn (PC-relative constant) */
        cv1k_u32 addr = (pc & ~3U) + 4U + (cv1k_u32)(op & 0xffU) * 4U;
        v0 = ir_newv(b);
        ir_push(b, IR_LOAD, v0, 0, 0, CV1K_IR_FLAG_IMMB, addr); b->in[b->n - 1].aux = 4U;
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    switch (op & 0xf00fU) {
    case 0x6003U: /* MOV Rm,Rn */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, m); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x6000U: case 0x6001U: case 0x6002U: { /* MOV.B/W/L @Rm,Rn (load, B/W sign-extend) */
        unsigned sz = (op & 3U) == 0U ? 1U : (op & 3U) == 1U ? 2U : 4U;
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, m);                 /* addr = Rm */
        ir_push(b, IR_LOAD, v1, v0, 0, (sz != 4U) ? CV1K_IR_FLAG_SEXT : 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz;
        ir_push(b, IR_GSTORE, 0, v1, 0, 0, n); return 1; }
    case 0x2000U: case 0x2001U: case 0x2002U: { /* MOV.B/W/L Rm,@Rn (store) */
        unsigned sz = (op & 3U) == 0U ? 1U : (op & 3U) == 1U ? 2U : 4U;
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n);                 /* addr = Rn */
        ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);                 /* data = Rm */
        ir_push(b, IR_STORE, 0, v0, v1, 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz; return 1; }
    case 0x300cU: case 0x3008U: case 0x2009U: case 0x200bU: case 0x200aU: { /* ADD/SUB/AND/OR/XOR Rm,Rn */
        cv1k_u8 irop = (op & 0xf00fU) == 0x300cU ? IR_ADD : (op & 0xf00fU) == 0x3008U ? IR_SUB :
                       (op & 0xf00fU) == 0x2009U ? IR_AND : (op & 0xf00fU) == 0x200bU ? IR_OR : IR_XOR;
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);
        ir_push(b, irop, v0, v0, v1, 0, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1; }
    case 0x300eU: case 0x300aU: { /* ADDC/SUBC Rm,Rn */
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);
        ir_push(b, (op & 0xf00fU) == 0x300eU ? IR_ADDC : IR_SUBC, v0, v0, v1, 0, 0);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1; }
    case 0x400cU: case 0x400dU: { /* SHAD/SHLD Rm,Rn (dynamic shift) */
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);
        ir_push(b, (op & 0xf00fU) == 0x400cU ? IR_SHAD : IR_SHLD, v0, v0, v1, 0, 0);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1; }
    case 0x0007U: { /* MUL.L Rm,Rn -> MACL = low32((s32)Rn * (s32)Rm) */
        v0 = ir_newv(b);
        ir_push(b, IR_MUL_MACL, v0, 0, 0, 0, (cv1k_u32)((m << 4U) | n)); return 1; }
    case 0x200eU: case 0x200fU: { /* MULU.W/MULS.W Rm,Rn -> MACL */
        v0 = ir_newv(b);
        ir_push(b, IR_MUL_MACL, v0, 0, 0, (op & 0xf00fU) == 0x200fU ? 2U : 1U,
                (cv1k_u32)((m << 4U) | n)); return 1; }
    case 0x3005U: case 0x300dU: { /* DMULU/DMULS.L Rm,Rn -> MACH:MACL */
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);
        ir_push(b, IR_DMUL_MACL, 0, v0, v1, (op & 0xf00fU) == 0x300dU ? 1U : 0U, 0); return 1; }
    case 0x2007U: case 0x3004U: case 0x6008U: case 0x6009U: case 0x600aU: { /* exact helper-call ops */
        cv1k_u8 h = (op & 0xf00fU) == 0x2007U ? H_DIV0S : (op & 0xf00fU) == 0x3004U ? H_DIV1 :
                   (op & 0xf00fU) == 0x6008U ? H_SWAPB : (op & 0xf00fU) == 0x6009U ? H_SWAPW : H_NEGC;
        ir_push(b, IR_CALLH, 0, 0, 0, 0, (cv1k_u32)((m << 4U) | n)); b->in[b->n - 1].aux = h; return 1; }
    case 0x2008U: case 0x3000U: case 0x3002U: case 0x3003U: case 0x3006U: case 0x3007U: { /* TST / CMP/EQ/HS/GE/HI/GT Rm,Rn */
        cv1k_u8 setcc; cv1k_u8 fl = 0;
        switch (op & 0xf00fU) {
        case 0x2008U: setcc = 0x94U; fl = LFLAG_CMP_TST; break;  /* TST   : T=((Rn&Rm)==0) -> setz   */
        case 0x3000U: setcc = 0x94U; break;                      /* CMP/EQ: sete                     */
        case 0x3002U: setcc = 0x93U; break;                      /* CMP/HS: setae (unsigned >=)      */
        case 0x3003U: setcc = 0x9dU; break;                      /* CMP/GE: setge (signed >=)        */
        case 0x3006U: setcc = 0x97U; break;                      /* CMP/HI: seta  (unsigned >)       */
        default:      setcc = 0x9fU; break;                      /* CMP/GT: setg  (signed >)         */
        }
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n); ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);
        ir_push(b, IR_CMP, v0, v0, v1, fl, 0); b->in[b->n - 1].aux = setcc; return 1; }
    case 0x6004U: case 0x6005U: case 0x6006U: { /* MOV.B/W/L @Rm+,Rn (post-increment load) */
        unsigned sz = (op & 3U) == 0U ? 1U : (op & 3U) == 1U ? 2U : 4U;
        if (n == m) {                            /* n==m: load only, no post-increment */
            v0 = ir_newv(b); v1 = ir_newv(b);
            ir_push(b, IR_GLOAD, v0, 0, 0, 0, m);
            ir_push(b, IR_LOAD, v1, v0, 0, (sz != 4U) ? CV1K_IR_FLAG_SEXT : 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz;
            ir_push(b, IR_GSTORE, 0, v1, 0, 0, n); return 1;
        }
        /* The load may emit a helper call that clobbers caller-saved scratch, so
         * the address reg cannot be reused for the increment if Rm is spilled —
         * reload Rm from its home AFTER the load instead. */
        cv1k_ir_vreg v2 = 0;
        v0 = ir_newv(b); v1 = ir_newv(b); v2 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, m);                              /* addr = Rm           */
        ir_push(b, IR_LOAD, v1, v0, 0, (sz != 4U) ? CV1K_IR_FLAG_SEXT : 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz;
        ir_push(b, IR_GLOAD, v2, 0, 0, 0, m);                              /* reload Rm           */
        ir_push(b, IR_ADD, v2, v2, 0, CV1K_IR_FLAG_IMMB, sz); ir_push(b, IR_GSTORE, 0, v2, 0, 0, m); /* Rm += sz */
        ir_push(b, IR_GSTORE, 0, v1, 0, 0, n); return 1; }                 /* Rn = loaded (last)  */
    case 0x2004U: case 0x2005U: case 0x2006U: { /* MOV.B/W/L Rm,@-Rn (pre-decrement store) */
        /* n==m needs the ORIGINAL Rn stored at Rn-sz (SH order: write before the
         * decrement commits) — the coalesced address reg can't also hold it, so
         * defer that rare aliasing case to the interpreter. */
        if (n == m) return 0;
        unsigned sz = (op & 3U) == 0U ? 1U : (op & 3U) == 1U ? 2U : 4U;
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, n);                              /* Rn -= sz; addr = Rn */
        ir_push(b, IR_SUB, v0, v0, 0, CV1K_IR_FLAG_IMMB, sz); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n);
        ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);                              /* data = Rm           */
        ir_push(b, IR_STORE, 0, v0, v1, 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz; return 1; }
    case 0x000cU: case 0x000dU: case 0x000eU: { /* MOV.B/W/L @(R0,Rm),Rn (indexed load) */
        /* address = R0 + Rm is built in Rn's home; if the dest aliases an index
         * register (n==0 or n==m) that home is clobbered before both operands are
         * read, so fall back for those rare cases. */
        if (n == 0U || n == m) return 0;
        unsigned sz = (op & 3U) == 0U ? 1U : (op & 3U) == 1U ? 2U : 4U;
        v0 = ir_newv(b); v1 = ir_newv(b);
        ir_push(b, IR_GLOAD, v0, 0, 0, 0, 0);                             /* v0 = R0 (-> Rn home) */
        ir_push(b, IR_GLOAD, v1, 0, 0, 0, m);                             /* v1 = Rm              */
        ir_push(b, IR_ADD, v0, v0, v1, 0, 0);                             /* addr = R0 + Rm       */
        ir_push(b, IR_LOAD, v0, v0, 0, (sz != 4U) ? CV1K_IR_FLAG_SEXT : 0, 0); b->in[b->n - 1].aux = (cv1k_u8)sz;
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1; }
    case 0x6007U: /* NOT Rm,Rn */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, m); ir_push(b, IR_NOT, v0, v0, 0, 0, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x600bU: /* NEG Rm,Rn */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, m); ir_push(b, IR_NEG, v0, v0, 0, 0, 0); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x600cU: /* EXTU.B */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, m); ir_push(b, IR_AND, v0, v0, 0, CV1K_IR_FLAG_IMMB, 0xffU); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x600dU: /* EXTU.W */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, m); ir_push(b, IR_AND, v0, v0, 0, CV1K_IR_FLAG_IMMB, 0xffffU); ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x600eU: /* EXTS.B = (x<<24)>>24 arithmetic */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, m);
        ir_push(b, IR_SHL, v0, v0, 0, CV1K_IR_FLAG_IMMB, 24); ir_push(b, IR_SAR, v0, v0, 0, CV1K_IR_FLAG_IMMB, 24);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    case 0x600fU: /* EXTS.W */
        v0 = ir_newv(b); ir_push(b, IR_GLOAD, v0, 0, 0, 0, m);
        ir_push(b, IR_SHL, v0, v0, 0, CV1K_IR_FLAG_IMMB, 16); ir_push(b, IR_SAR, v0, v0, 0, CV1K_IR_FLAG_IMMB, 16);
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    default: break;
    }
    return 0;
}

/* vreg temporaries (per guest op, dead after) use a small scratch set; guest
 * regs are cached in the disjoint GPOOL by the allocator.  Both pools avoid
 * RSP/RBP and R12=cpu/R13=bus. */
static const unsigned SCRATCH[] = { RAX, RCX, RDX };
#define NSCRATCH ((cv1k_ir_vreg)(sizeof SCRATCH / sizeof SCRATCH[0]))
static const int GPOOL[] = { RSI, RDI, R8, R9, R10, R11, RBX, R14, R15 };
#define NG ((int)(sizeof GPOOL / sizeof GPOOL[0]))

/* Diagnostic toggle: when 0, guest regs are never cached (all GLOAD/GSTORE go to
 * memory).  Keep production on the conservative memory-backed path; the
 * differential tester enables this explicitly when exercising the allocator. */
static int g_ir_cache_on = 0;
void cv1k_ir_set_cache(int on) { g_ir_cache_on = on ? 1 : 0; }
/* Inline work-RAM access is safe in the default fast build; cache-accurate builds
 * still emit the bus/cache hook before direct RAM access. */
static int g_ir_fastram_on = 1;
void cv1k_ir_set_fastram(int on) { g_ir_fastram_on = on ? 1 : 0; }
/* Internal-loop (back-branch) chaining: ON for the isolated bench/test, but OFF
 * in production (a loop block has no mid-loop IRQ/cycle checks, so an IRQ-waiting
 * spin loop would hang and a long loop would overrun the cycle budget). */
static int g_ir_internal_loops = 0;
void cv1k_ir_set_internal_loops(int on) { g_ir_internal_loops = on ? 1 : 0; }
static cv1k_u32 g_ir_last_compile_cycles = 0;

/* True body-entry chaining.  This is intentionally guarded by a runtime limit
 * written by cv1k_irjit_run_frame() before entering native code.  Chained blocks
 * jump directly to the target body, after one shared standard frame has been
 * established, so the target avoids C dispatch and prologue work. */
static int g_ir_body_chain_init = 0;
static int g_ir_body_chain_on = 0;
static unsigned long long g_ir_body_chain_link_limit = ~0ULL;
static unsigned long long g_ir_chain_links = 0, g_ir_chain_taken_sites = 0, g_ir_chain_site_blocks = 0, g_ir_chainable_blocks = 0, g_ir_chain_patch_attempts = 0, g_ir_chain_patch_bad = 0;
static void ir_body_chain_maybe_init(void)
{
    if (g_ir_body_chain_init) return;
    g_ir_body_chain_init = 1;
    const char *e = getenv("CV1K_IR_BODY_CHAIN");
    if (e && *e && *e != '0' && *e != 'n' && *e != 'N') g_ir_body_chain_on = 1;
    e = getenv("CV1K_IR_BODY_CHAIN_LINK_LIMIT");
    if (e && *e) g_ir_body_chain_link_limit = strtoull(e, NULL, 0);
}

#define X_JA 0x07  /* jcc: unsigned above (>) */

static void emit_cache_note(struct e64 *e, unsigned areg, int write)
{
#if CV1K_CACHE_ACCURATE
    mov_r64_r64(e, RDI, R13);
    if (areg != RSI) mov_r32_r32(e, RSI, areg);
    mov_r32_imm32(e, RDX, (cv1k_u32)(write ? 1U : 0U));
    mov_r32_imm32(e, RCX, 0U);
    call_rax(e, (const void *)cv1k_bus_cache_access);
#else
    (void)e; (void)areg; (void)write;
#endif
}

/* Inline fast-RAM load: direct host access for P0 work-RAM, else the bus helper.
 *
 * LOADIDX/STOREIDX materialise their computed address in R8 before calling this
 * helper.  The original fast path also used R8 as its offset scratch and
 * therefore disabled direct work-RAM access for exactly those indexed forms.
 * Keep the address register intact for the slow path and use R11 as the offset
 * scratch instead; R9 remains the host RAM pointer.  Memory blocks do not cache
 * guest registers in caller-saved R8/R9/R10/R11, so these temporaries are free on
 * the emitted fast path. */
static void emit_load(struct e64 *e, unsigned dreg, unsigned areg, unsigned sz, int sext,
                      uint64_t ram_base, uint32_t ram_size, unsigned ram_base_hreg)
{
    size_t to_slow = 0, to_done = 0;
    int fast = g_ir_fastram_on && areg != R9 && areg != R11 && dreg != R9 && dreg != R11;
    if (fast) {
        emit_cache_note(e, areg, 0);
        mov_r32_r32(e, R11, areg);                  /* r11 = addr              */
        alu_ri(e, 5, R11, CV1K_ADDR_WORK_RAM);      /* r11 -= 0x0c000000 (off) */
        cmp_r32_imm32(e, R11, ram_size - sz);
        to_slow = jcc32(e, X_JA);                   /* off > size-sz -> slow   */
        if (ram_base_hreg) add_r64_r64(e, R11, ram_base_hreg);
        else { mov_r64_imm64(e, R9, ram_base); add_r64_r64(e, R11, R9); }
        if (sz == 4)      { mov_r32_memb(e, dreg, R11); bswap_r32(e, dreg); }
        else if (sz == 2) { movzx_r32_memb16(e, dreg, R11); ror_r16_imm8(e, dreg, 8); if (sext) movsx_r32_r16(e, dreg, dreg); }
        else              { movsx_r32_memb8(e, dreg, R11); }    /* MOV.B sign-extends */
        to_done = jmp32(e);
        patch_here(e, to_slow);
    }
    if (areg != RSI) mov_r32_r32(e, RSI, areg);
    mov_r64_r64(e, RDI, R13);
    call_rax(e, sz == 1 ? (const void *)cv1k_sh3_jit_read8 : sz == 2 ? (const void *)cv1k_sh3_jit_read16 : (const void *)cv1k_sh3_jit_read32);
    if (sext) { if (sz == 1) movsx_eax_al(e); else if (sz == 2) movsx_eax_ax(e); }
    if (dreg != RAX) mov_r32_r32(e, dreg, RAX);
    if (fast) patch_here(e, to_done);
}

/* Compile-time constant load address.  PC-relative MOV.W/MOV.L table reads are
 * common in SH-3 code streams.  Once the target address is known at compile
 * time, the generated fast path can skip the guest-range check and guest->host
 * address arithmetic used by the generic register-address load. */
static void emit_const_load(struct e64 *e, unsigned dreg, cv1k_u32 addr, unsigned sz, int sext,
                            uint64_t ram_base, uint32_t ram_size)
{
    int fast = g_ir_fastram_on && addr >= CV1K_ADDR_WORK_RAM &&
               (addr - CV1K_ADDR_WORK_RAM) <= ram_size - sz && dreg != R11;
    if (fast) {
#if CV1K_CACHE_ACCURATE
        mov_r32_imm32(e, R11, addr);
        emit_cache_note(e, R11, 0);
#endif
        uint64_t host = ram_base + (uint64_t)(addr - CV1K_ADDR_WORK_RAM);
        mov_r64_imm64(e, R11, host);
        if (sz == 4)      { mov_r32_memb(e, dreg, R11); bswap_r32(e, dreg); }
        else if (sz == 2) { movzx_r32_memb16(e, dreg, R11); ror_r16_imm8(e, dreg, 8); if (sext) movsx_r32_r16(e, dreg, dreg); }
        else              { movsx_r32_memb8(e, dreg, R11); }
        return;
    }
    mov_r32_imm32(e, RSI, addr);
    mov_r64_r64(e, RDI, R13);
    call_rax(e, sz == 1 ? (const void *)cv1k_sh3_jit_read8 : sz == 2 ? (const void *)cv1k_sh3_jit_read16 : (const void *)cv1k_sh3_jit_read32);
    if (sext) { if (sz == 1) movsx_eax_al(e); else if (sz == 2) movsx_eax_ax(e); }
    if (dreg != RAX) mov_r32_r32(e, dreg, RAX);
}

/* Inline fast-RAM store (byte-swapped to big-endian), else the bus helper. */
static void emit_store(struct e64 *e, unsigned areg, unsigned datareg, unsigned sz,
                       uint64_t ram_base, uint32_t ram_size, unsigned ram_base_hreg)
{
    size_t to_slow = 0, to_done = 0;
    int fast = g_ir_fastram_on && areg != R9 && areg != R10 && areg != R11 &&
               datareg != R9 && datareg != R10 && datareg != R11;
    if (fast) {
        emit_cache_note(e, areg, 1);
        mov_r32_r32(e, R11, areg);
        alu_ri(e, 5, R11, CV1K_ADDR_WORK_RAM);
        cmp_r32_imm32(e, R11, ram_size - sz);
        to_slow = jcc32(e, X_JA);
        if (ram_base_hreg) add_r64_r64(e, R11, ram_base_hreg);
        else { mov_r64_imm64(e, R9, ram_base); add_r64_r64(e, R11, R9); }
        if (sz == 4)      { mov_r32_r32(e, R10, datareg); bswap_r32(e, R10); mov_memb_r32(e, R11, R10); }
        else if (sz == 2) { mov_r32_r32(e, R10, datareg); ror_r16_imm8(e, R10, 8); mov_memb_r16(e, R11, R10); }
        else              { mov_memb_r8(e, R11, datareg); }     /* store low byte */
        to_done = jmp32(e);
        patch_here(e, to_slow);
    }
    if (areg != RSI) mov_r32_r32(e, RSI, areg);
    if (datareg != RDX) mov_r32_r32(e, RDX, datareg);
    mov_r64_r64(e, RDI, R13);
    call_rax(e, sz == 1 ? (const void *)cv1k_sh3_jit_write8 : sz == 2 ? (const void *)cv1k_sh3_jit_write16 : (const void *)cv1k_sh3_jit_write32);
    if (fast) patch_here(e, to_done);
}

/* SR.T = (the flags just set satisfy `setcc`).  Only one scratch temp is
 * needed: setcc into a non-live scratch byte, zero-extend it, then update SR.T
 * in memory.  This keeps compare/test ops compilable even when both operands
 * already occupy the other scratch registers. */
static int emit_t_from_flags(struct e64 *e, uint8_t setcc, int x1, int x2)
{
    int t = -1; const unsigned cand[3] = { RAX, RCX, RDX };
    for (int i = 0; i < 3; i++) { int c = (int)cand[i]; if (c != x1 && c != x2) { t = c; break; } }
    if (t < 0) return 0;
    setcc_reg(e, setcc, (unsigned)t);                 /* tb = condition (0/1) */
    movzx_r32_r8(e, (unsigned)t, (unsigned)t);
    and_m32r12_imm32(e, OFF_SR, ~1U);                 /* clear SR.T           */
    or_m32r12_r32(e, OFF_SR, (unsigned)t);            /* SR.T |= condition    */
    return 1;
}

static void emit_shadld(struct e64 *e, int arithmetic, unsigned d, unsigned countreg)
{
    if (countreg != RCX) mov_r32_r32(e, RCX, countreg);  /* x64 variable shifts use CL */
    test_rr(e, RCX, RCX);
    size_t j_neg = jcc32(e, 0x08);                       /* js: negative count */
    alu_ri(e, 4, RCX, 31U);
    sh_cl(e, 4, d);                                      /* positive: left */
    size_t j_done = jmp32(e);

    size_t lab_neg = e->size;
    alu_ri(e, 4, RCX, 31U);
    size_t j_zero = jcc32(e, 0x04);                      /* negative multiple of 32 */
    mov_r32_imm32(e, RDX, 32U);
    alu_rr(e, 0x29, RDX, RCX);                           /* rdx = 32 - (count&31) */
    mov_r32_r32(e, RCX, RDX);
    sh_cl(e, arithmetic ? 7U : 5U, d);                   /* negative: right */
    size_t j_done2 = jmp32(e);

    size_t lab_zero = e->size;
    if (arithmetic) sh_ri(e, 7U, d, 31U);                /* all sign bits */
    else mov_r32_imm32(e, d, 0U);

    size_t lab_done = e->size;
    patch_to(e, j_neg, lab_neg); patch_to(e, j_zero, lab_zero);
    patch_to(e, j_done, lab_done); patch_to(e, j_done2, lab_done);
}

static int helper_reads_m(cv1k_u8 h) { return h == H_DIV0S || h == H_DIV1 || h == H_MULL || h == H_NEGC || h == H_SWAPB || h == H_SWAPW; }
static int helper_reads_n(cv1k_u8 h) { return h == H_DIV0S || h == H_DIV1 || h == H_MULL; }
static int helper_writes_n(cv1k_u8 h) { return h == H_DIV1 || h == H_NEGC || h == H_SWAPB || h == H_SWAPW; }
static const void *helper_fn(cv1k_u8 h)
{
    switch (h) {
    case H_DIV0U: return (const void *)cv1k_sh3_jit_div0u;
    case H_DIV0S: return (const void *)cv1k_sh3_jit_div0s;
    case H_DIV1:  return (const void *)cv1k_sh3_jit_div1;
    case H_MULL:  return (const void *)cv1k_sh3_jit_mull;
    case H_NEGC:  return (const void *)cv1k_sh3_jit_negc;
    case H_SWAPB: return (const void *)cv1k_sh3_jit_swapb;
    case H_SWAPW: return (const void *)cv1k_sh3_jit_swapw;
    default: return NULL;
    }
}

/* Emit one guest op from its IR buffer, with copy coalescing.
 *
 * hreg[g] = host reg caching guest reg g (or -1 = spilled to cpu->r[g]).
 * The single result vreg (the one that is GSTORE'd) is mapped onto its
 * destination guest reg's host register, and read-only operand vregs are mapped
 * onto their source guest reg's host register.  This turns "GLOAD;OP;GSTORE"
 * into an in-place op on the cached host reg (e.g. ADD Rm,Rn -> add H[Rn],H[Rm]
 * with no surrounding moves) and makes coalesced GLOAD/GSTORE disappear. */
static int ir_emit_op(struct e64 *e, const struct iropbuf *b, const int *hreg,
                      uint64_t ram_base, uint32_t ram_size, unsigned ram_base_hreg)
{
    int vmap[16];            /* vreg -> host reg */
    int vsrc[16];            /* vreg -> guest reg it is GLOAD'd from, or -1 */
    int rv = -1, gd = -1;    /* result vreg and its GSTORE destination guest reg */
    if (b->nv > 16) return 0;
    for (cv1k_ir_vreg v = 0; v < b->nv; v++) { vmap[v] = -1; vsrc[v] = -1; }
    for (int i = 0; i < b->n; i++) {
        const struct cv1k_ir_inst *in = &b->in[i];
        if (in->op == IR_GLOAD)  vsrc[in->dst] = (int)(in->imm & 15U);
        if (in->op == IR_GSTORE) { rv = in->a; gd = (int)(in->imm & 15U); }
    }
    /* (rv may be -1 for store-only ops, which write memory not a guest reg) */

    int sc = 0;              /* next free scratch reg */
    /* result vreg -> H[gd] (cached) or a scratch reg (spilled dest) */
    if (rv >= 0) vmap[rv] = (gd >= 0 && hreg[gd] >= 0) ? hreg[gd] : (int)SCRATCH[sc++];
    /* operand vregs -> their cached source reg, else a scratch */
    for (cv1k_ir_vreg v = 0; v < b->nv; v++) {
        if ((int)v == rv) continue;
        if (vsrc[v] >= 0 && hreg[vsrc[v]] >= 0) vmap[v] = hreg[vsrc[v]];
        else { if (sc >= (int)NSCRATCH) return 0; vmap[v] = (int)SCRATCH[sc++]; }
    }

    for (int i = 0; i < b->n; i++) {
        const struct cv1k_ir_inst *in = &b->in[i];
        unsigned d = (unsigned)vmap[in->dst];
        switch (in->op) {
        case IR_MOVI: mov_r32_imm32(e, d, in->imm); break;
        case IR_GLOAD: { /* materialise vreg from guest reg (nop if already there) */
            int g = (int)(in->imm & 15U);
            if (hreg[g] >= 0) { if ((unsigned)hreg[g] != d) mov_r32_r32(e, d, (unsigned)hreg[g]); }
            else mov_r32_m32r12(e, d, OFF_R(g));
            break; }
        case IR_GSTORE: { /* write vreg back to guest reg (nop if coalesced) */
            int g = (int)(in->imm & 15U); unsigned sv = (unsigned)vmap[in->a];
            if (hreg[g] >= 0) { if ((unsigned)hreg[g] != sv) mov_r32_r32(e, (unsigned)hreg[g], sv); }
            else mov_m32r12_r32(e, OFF_R(g), sv);
            break; }
        case IR_MOV: if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]); break;
        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: {
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            unsigned ri = in->op == IR_ADD ? 0 : in->op == IR_OR ? 1 : in->op == IR_AND ? 4 : in->op == IR_SUB ? 5 : 6;
            uint8_t rr = in->op == IR_ADD ? 0x01 : in->op == IR_SUB ? 0x29 : in->op == IR_AND ? 0x21 : in->op == IR_OR ? 0x09 : 0x31;
            if (in->flags & CV1K_IR_FLAG_IMMB) alu_ri(e, ri, d, in->imm); else alu_rr(e, rr, d, (unsigned)vmap[in->b]);
            break; }
        case IR_ADDC: case IR_SUBC: {
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            unsigned rb = (unsigned)vmap[in->b];
            bt_m32r12_imm8(e, OFF_SR, 0);
            if (in->op == IR_ADDC) adc_rr(e, d, rb); else sbb_rr(e, d, rb);
            if (!emit_t_from_flags(e, 0x92 /*setc*/, (int)d, (int)rb)) return 0;
            break; }
        case IR_NOT: if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]); not_r32(e, d); break;
        case IR_NEG: if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]); neg_r32(e, d); break;
        case IR_MUL_MACL: {
            unsigned m = (in->imm >> 4U) & 15U, n = in->imm & 15U;
            if (hreg[n] >= 0) mov_r32_r32(e, RAX, (unsigned)hreg[n]); else mov_r32_m32r12(e, RAX, OFF_R(n));
            if (hreg[m] >= 0) mov_r32_r32(e, RCX, (unsigned)hreg[m]); else mov_r32_m32r12(e, RCX, OFF_R(m));
            if (in->flags == 1U) {
                alu_ri(e, 4, RAX, 0xffffU);
                alu_ri(e, 4, RCX, 0xffffU);
            } else if (in->flags == 2U) {
                sh_ri(e, 4U, RAX, 16U); sh_ri(e, 7U, RAX, 16U);
                sh_ri(e, 4U, RCX, 16U); sh_ri(e, 7U, RCX, 16U);
            }
            imul_rr(e, RAX, RCX);
            mov_m32r12_r32(e, OFF_MACL, RAX);
            break; }
        case IR_MULU:
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            imul_rr(e, d, (unsigned)vmap[in->b]);
            break;
        case IR_DMUL_MACL: {
            unsigned ra = (unsigned)vmap[in->a], rb = (unsigned)vmap[in->b];
            unsigned src = rb;
            if (rb == RAX) {
                src = (ra == RCX) ? RDX : RCX;
                mov_r32_r32(e, src, rb);
            }
            if (ra != RAX) mov_r32_r32(e, RAX, ra);
            if (in->flags & 1U) imul1_r32(e, src); else mul_r32(e, src);
            mov_m32r12_r32(e, OFF_MACL, RAX);
            mov_m32r12_r32(e, OFF_MACH, RDX);
            break; }
        case IR_TSTORE: /* SR.T = imm (CLRT/SETT) */
            if (in->imm & 1U) or_m32r12_imm32(e, OFF_SR, 1U);
            else and_m32r12_imm32(e, OFF_SR, ~1U);
            break;
        case IR_DECT: { /* d -= 1; SR.T = (d == 0). */
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            int t = -1; const unsigned cand[3] = { RAX, RCX, RDX };
            for (int ci = 0; ci < 3; ci++) { if ((unsigned)cand[ci] != d) { t = (int)cand[ci]; break; } }
            if (t < 0) return 0;
            alu_ri(e, 5, d, 1);                                  /* sub d, 1 (sets ZF) */
            e8(e, 0x0f); e8(e, 0x94); modrm(e, 3, 0, (unsigned)t); /* sete tb */
            movzx_r32_r8(e, (unsigned)t, (unsigned)t);
            and_m32r12_imm32(e, OFF_SR, ~1U);
            or_m32r12_r32(e, OFF_SR, (unsigned)t);
            break; }
        case IR_SHL: case IR_SHR: case IR_SAR: {
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            unsigned ext = in->op == IR_SHL ? 4 : in->op == IR_SHR ? 5 : 7;
            if (in->flags & LFLAG_SETT) {            /* SHLL/SHLR/SHAL/SHAR: shift 1, T=carry */
                shift1(e, ext, d);
                if (!emit_t_from_flags(e, 0x92 /*setc*/, (int)d, -1)) return 0;
            } else sh_ri(e, ext, d, (uint8_t)in->imm);
            break; }
        case IR_ROL: case IR_ROR: {              /* ROTL/ROTR: rotate 1, T=carry */
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            shift1(e, in->op == IR_ROL ? 0 : 1, d);
            if (!emit_t_from_flags(e, 0x92, (int)d, -1)) return 0;
            break; }
        case IR_ROCL: case IR_ROCR: {            /* ROTCL/ROTCR: rotate 1 through T */
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            int t0 = -1; const unsigned cand[3] = { RAX, RCX, RDX };
            for (int ci = 0; ci < 3; ci++) { if ((unsigned)cand[ci] == d) continue; t0 = (int)cand[ci]; break; }
            if (t0 < 0) return 0;
            mov_r32_m32r12(e, (unsigned)t0, OFF_SR);
            sh_ri(e, 5, (unsigned)t0, 1);         /* CF = old SR.T (bit0)          */
            shift1(e, in->op == IR_ROCL ? 2 : 3, d);  /* rcl/rcr d,1 through CF     */
            if (!emit_t_from_flags(e, 0x92, (int)d, -1)) return 0;
            break; }
        case IR_SHAD: case IR_SHLD: {            /* dynamic arithmetic/logical shift */
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            emit_shadld(e, in->op == IR_SHAD, d, (unsigned)vmap[in->b]);
            break; }
        case IR_CMP: {                           /* SR.T = (a cc b); no reg result */
            unsigned ra = (unsigned)vmap[in->a]; int x2 = -1;
            if (in->flags & LFLAG_CMP_TST) {
                if (in->flags & CV1K_IR_FLAG_IMMB) test_ri(e, ra, in->imm);
                else { unsigned rb = (unsigned)vmap[in->b]; test_rr(e, ra, rb); x2 = (int)rb; }
            } else if (in->flags & LFLAG_CMP_ZERO) {
                cmp_r32_imm32(e, ra, 0);
            } else if (in->flags & CV1K_IR_FLAG_IMMB) {
                cmp_r32_imm32(e, ra, in->imm);
            } else { unsigned rb = (unsigned)vmap[in->b]; cmp_rr(e, ra, rb); x2 = (int)rb; }
            if (!emit_t_from_flags(e, in->aux, (int)ra, x2)) return 0;
            break; }
        case IR_SLOAD: {
            cv1k_u32 off = special_off((cv1k_u8)in->imm); if (off == 0) return 0;
            mov_r32_m32r12(e, d, off);
            break; }
        case IR_SSTORE: {
            cv1k_u32 off = special_off((cv1k_u8)in->imm); if (off == 0) return 0;
            mov_m32r12_r32(e, off, (unsigned)vmap[in->a]);
            break; }
        case IR_CALLH: {
            cv1k_u8 h = in->aux; unsigned m = (in->imm >> 4U) & 15U, n = in->imm & 15U;
            const void *fn = helper_fn(h);
            if (!fn) return 0;
            /* Helpers operate on cpu->r[] memory.  Cached guest regs are kept in
             * callee-saved hosts for call-containing blocks, but their memory homes
             * must be made current before the call and any helper-written result
             * reloaded afterward. */
            if (helper_reads_m(h) && hreg[m] >= 0) mov_m32r12_r32(e, OFF_R(m), (unsigned)hreg[m]);
            if (helper_reads_n(h) && n != m && hreg[n] >= 0) mov_m32r12_r32(e, OFF_R(n), (unsigned)hreg[n]);
            mov_r64_r64(e, RDI, R12);
            if (h != H_DIV0U) { mov_r32_imm32(e, RSI, m); mov_r32_imm32(e, RDX, n); }
            call_rax(e, fn);
            if (helper_writes_n(h) && hreg[n] >= 0) mov_r32_m32r12(e, (unsigned)hreg[n], OFF_R(n));
            break; }
        case IR_LOADIDX:
            mov_r32_r32(e, R8, (unsigned)vmap[in->a]);
            alu_rr(e, 0x01, R8, (unsigned)vmap[in->b]);
            emit_load(e, d, R8, in->aux, (in->flags & CV1K_IR_FLAG_SEXT) != 0, ram_base, ram_size, ram_base_hreg); break;
        case IR_STOREIDX:
            mov_r32_r32(e, R8, (unsigned)vmap[in->a]);
            alu_rr(e, 0x01, R8, (unsigned)vmap[in->b]);
            emit_store(e, R8, d, in->aux, ram_base, ram_size, ram_base_hreg); break;
        case IR_LOAD:
            if (in->flags & CV1K_IR_FLAG_IMMB) emit_const_load(e, d, in->imm, in->aux, (in->flags & CV1K_IR_FLAG_SEXT) != 0, ram_base, ram_size);
            else emit_load(e, d, (unsigned)vmap[in->a], in->aux, (in->flags & CV1K_IR_FLAG_SEXT) != 0, ram_base, ram_size, ram_base_hreg);
            break;
        case IR_STORE: emit_store(e, (unsigned)vmap[in->a], (unsigned)vmap[in->b], in->aux, ram_base, ram_size, ram_base_hreg); break;
        default: return 0;
        }
    }
    return 1;
}

int cv1k_ir_phase1_supported(cv1k_u16 op)
{
    struct iropbuf b = {0}; return ir_lower(op, 0, &b);
}

static void ir_accum_masks(const struct iropbuf *b, cv1k_u32 *rmask, cv1k_u32 *wmask,
                           int *order, int *norder, int *has_mem)
{
    for (int i = 0; i < b->n; i++) {
        const struct cv1k_ir_inst *in = &b->in[i];
        if (in->op == IR_LOAD || in->op == IR_STORE || in->op == IR_LOADIDX || in->op == IR_STOREIDX || in->op == IR_CALLH) *has_mem = 1;
        if (in->op == IR_GLOAD || in->op == IR_GSTORE) {
            int g = (int)(in->imm & 15U);
            if (in->op == IR_GLOAD) *rmask |= 1U << g; else *wmask |= 1U << g;
            int seen = 0; for (int j = 0; j < *norder; j++) if (order[j] == g) { seen = 1; break; }
            if (!seen && *norder < 16) order[(*norder)++] = g;
        } else if (in->op == IR_CALLH) {
            cv1k_u8 h = in->aux; int regs[3]; int nr = 0;
            int m = (int)((in->imm >> 4U) & 15U), n = (int)(in->imm & 15U);
            if (helper_reads_m(h)) { *rmask |= 1U << m; regs[nr++] = m; }
            if (helper_reads_n(h)) { *rmask |= 1U << n; regs[nr++] = n; }
            if (helper_writes_n(h)) { *wmask |= 1U << n; regs[nr++] = n; }
            for (int ri = 0; ri < nr; ri++) {
                int g = regs[ri], seen = 0;
                for (int j = 0; j < *norder; j++) if (order[j] == g) { seen = 1; break; }
                if (!seen && *norder < 16) order[(*norder)++] = g;
            }
        } else if (in->op == IR_MUL_MACL) {
            int regs[2]; int nr = 0;
            regs[nr++] = (int)((in->imm >> 4U) & 15U);
            regs[nr++] = (int)(in->imm & 15U);
            *rmask |= (1U << regs[0]) | (1U << regs[1]);
            for (int ri = 0; ri < nr; ri++) {
                int g = regs[ri], seen = 0;
                for (int j = 0; j < *norder; j++) if (order[j] == g) { seen = 1; break; }
                if (!seen && *norder < 16) order[(*norder)++] = g;
            }
        }
    }
}




static int ir_buf_needs_ppc(const struct iropbuf *b)
{
    int n = b->n;
    if (n > (int)(sizeof b->in / sizeof b->in[0])) n = (int)(sizeof b->in / sizeof b->in[0]);
    for (int i = 0; i < n; i++) {
        switch (b->in[i].op) {
        case IR_LOAD: case IR_STORE: case IR_LOADIDX: case IR_STOREIDX:
            return 1;
        default:
            break;
        }
    }
    return 0;
}

static int ir_buf_mem_ops(const struct iropbuf *b)
{
    int n = b->n, c = 0;
    if (n > (int)(sizeof b->in / sizeof b->in[0])) n = (int)(sizeof b->in / sizeof b->in[0]);
    for (int i = 0; i < n; i++) {
        switch (b->in[i].op) {
        case IR_LOAD: case IR_STORE: case IR_LOADIDX: case IR_STOREIDX:
            c++;
            break;
        default:
            break;
        }
    }
    return c;
}

/* Compile a fresh block at `pc`.  On success returns ops covered and fills
 * *out_fn / *out_mem / *out_cap (caller frees via cv1k_ir_free_block). */
cv1k_u32 cv1k_ir_compile_block(struct cv1k_bus *bus, cv1k_u32 pc,
                               cv1k_ir_block_fn *out_fn, void **out_mem, size_t *out_cap)
{
    *out_fn = NULL; *out_mem = NULL; *out_cap = 0; g_ir_last_compile_cycles = 0; g_ir_last_body_off = 0; g_ir_last_chain_count = 0; g_ir_last_chainable_body = 0; memset(g_ir_last_chain, 0, sizeof(g_ir_last_chain)); ir_body_chain_maybe_init();
#if !((defined(__x86_64__) || defined(_M_X64)) && (defined(__unix__) || defined(__APPLE__)))
    (void)bus; (void)pc; return 0;
#else
    struct cv1k_machine *mm = bus->machine;
    if (mm == NULL || mm->main_ram == NULL) return 0;
    uint64_t ram_base = (uint64_t)(uintptr_t)mm->main_ram;
    uint32_t ram_size = (uint32_t)mm->main_ram_size;

    /* ---- pass 1: decode + analyse (no codegen yet) ---- */
    cv1k_u16 ops[CV1K_IR_MAX_INSTS];
    int brtarget[CV1K_IR_MAX_INSTS];          /* op index for an internal back-branch, else -1 */
    cv1k_u32 count = 0, cycles = 0;
    cv1k_u32 readmask = 0, writemask = 0;     /* guest regs read / written      */
    int order[16]; int norder = 0;            /* guest regs in first-use order  */
    int has_mem = 0;
    int mem_ops = 0;
    int term = 0;                             /* 0 none, 1 cond, 2 cond-delay, 3 static-delay, 4 dynamic-delay, 5 RTE+NOP, 6 LDC SR */
    cv1k_u16 term_op = 0, term_delay = 0;     /* terminator branch + delay-slot op */
    cv1k_u32 term_bpc = 0, term_prefix_cycles = 0, term_delay_cycles = 0;
    for (; count < CV1K_IR_MAX_INSTS; count++) {
        cv1k_u16 op = cv1k_bus_fetch16(bus, pc + count * 2U);
        brtarget[count] = -1;
        unsigned hi = op & 0xff00U;
        /* Conditional branches BT/BF (0x89/0x8b) and delayed BT/S /BF/S (0x8d/
         * 0x8f).  A backward in-block target (non-delayed) becomes an internal
         * loop; otherwise the branch TERMINATES the block and computes the next
         * PC itself (delayed forms execute the delay slot inline first). */
        if (hi == 0x8900U || hi == 0x8b00U || hi == 0x8d00U || hi == 0x8f00U) {
            int delayed = (hi == 0x8d00U || hi == 0x8f00U);
            if (g_ir_internal_loops && !delayed) {
                cv1k_u32 target = pc + count * 2U + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(op & 0xffU) * 2);
                if (target >= pc) {
                    cv1k_u32 ti = (target - pc) / 2U;
                    if (ti < count) { brtarget[count] = (int)ti; ops[count] = op; cycles += 1U; continue; }
                }
            }
            term_bpc = pc + count * 2U; term_op = op;
            if (delayed) {
                cv1k_u16 ds = cv1k_bus_fetch16(bus, term_bpc + 2U);
                struct iropbuf db = {0};
                if (!ir_lower(ds, term_bpc + 2U, &db) || db.nv > NSCRATCH) break; /* can't lower delay slot -> branch is a fallback */
                cv1k_u32 drm = 0, dwm = 0; int dorder[16]; int dnorder = 0, dhas_mem = 0;
                ir_accum_masks(&db, &drm, &dwm, dorder, &dnorder, &dhas_mem);
                mem_ops += ir_buf_mem_ops(&db);
                if (dhas_mem) has_mem = 1;
                term_prefix_cycles = cycles;
                term_delay_cycles = cv1k_sh3_jit_linear_cycles(ds);
                if (g_ir_compile_cycle_limit != 0U && cycles + 2U + term_delay_cycles > g_ir_compile_cycle_limit) break;
                term_delay = ds; term = 2; cycles += 2U + term_delay_cycles;
            } else {
                if (g_ir_compile_cycle_limit != 0U && cycles + 3U > g_ir_compile_cycle_limit) break;
                term_prefix_cycles = cycles; term = 1; cycles += 3U;
            }
            break;
        }
        if ((op & 0xf0ffU) == 0x400eU) { /* LDC Rn,SR: helper handles RB bank swap; terminal because cached regs become stale. */
            int rn = (int)((op >> 8U) & 15U), seen = 0;
            if (g_ir_compile_cycle_limit != 0U && cycles + 1U > g_ir_compile_cycle_limit) break;
            term_bpc = pc + count * 2U; term_op = op; term_prefix_cycles = cycles; term = 6; cycles += 1U; has_mem = 1;
            readmask |= 1U << rn;
            for (int j = 0; j < norder; j++) if (order[j] == rn) { seen = 1; break; }
            if (!seen && norder < 16) order[norder++] = rn;
            break;
        }
        if (op == 0x002bU) { /* RTE: compile the common standalone RTE;NOP return block only. */
            cv1k_u16 ds = cv1k_bus_fetch16(bus, pc + count * 2U + 2U);
            if (count != 0 || ds != 0x0009U) break;
            { cv1k_u32 tc = 2U + cv1k_sh3_jit_linear_cycles(ds); if (g_ir_compile_cycle_limit != 0U && cycles + tc > g_ir_compile_cycle_limit) break;
            term_bpc = pc + count * 2U; term_op = op; term_delay = ds; term = 5; cycles += tc; }
            break;
        }
        if ((op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U ||
            (op & 0xf0ffU) == 0x0003U || (op & 0xf0ffU) == 0x0023U ||
            (op & 0xf0ffU) == 0x400bU || (op & 0xf0ffU) == 0x402bU || op == 0x000bU) {
            cv1k_u16 ds = cv1k_bus_fetch16(bus, pc + count * 2U + 2U);
            struct iropbuf db = {0};
            if (!ir_lower(ds, pc + count * 2U + 2U, &db) || db.nv > NSCRATCH) break;
            ir_accum_masks(&db, &readmask, &writemask, order, &norder, &has_mem);
            mem_ops += ir_buf_mem_ops(&db);
            { cv1k_u32 tc = (((op & 0xf0ffU) == 0x402bU) ? 1U : 2U) + cv1k_sh3_jit_linear_cycles(ds);
            if (g_ir_compile_cycle_limit != 0U && cycles + tc > g_ir_compile_cycle_limit) break;
            term_bpc = pc + count * 2U; term_op = op; term_delay = ds;
            term = ((op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U) ? 3 : 4;
            cycles += tc; }
            break;
        }
        struct iropbuf b = {0};
        if (!ir_lower(op, pc + count * 2U, &b)) break;
        if (b.nv > NSCRATCH) break;
        { cv1k_u32 oc = cv1k_sh3_jit_linear_cycles(op);
        if (g_ir_compile_cycle_limit != 0U && cycles + oc > g_ir_compile_cycle_limit) break;
        ops[count] = op;
        cycles += oc; }
        ir_accum_masks(&b, &readmask, &writemask, order, &norder, &has_mem);
        mem_ops += ir_buf_mem_ops(&b);
    }
    if (count == 0 && term == 0) return 0;   /* nothing lowerable here */

    g_ir_last_chainable_body = (g_ir_body_chain_on && g_ir_compile_cycle_limit == 0U && term == 0 && count > 0);

    /* ---- allocate: cache guest regs in host regs (first-use order) ----
     * Memory-op blocks contain helper calls, so only callee-saved registers may
     * hold guest regs across the block; ALU-only blocks may use all of them. */
    static const int MEMPOOL[] = { RBX, R14, R15 };
    const int *pool = has_mem ? MEMPOOL : GPOOL;
    int np = has_mem ? (int)(sizeof MEMPOOL / sizeof MEMPOOL[0]) : NG;
    int hoist_ram_base = has_mem && g_ir_fastram_on && mem_ops >= 2;
    if (g_ir_body_chain_on) hoist_ram_base = 0;
    int hreg[16]; for (int g = 0; g < 16; g++) hreg[g] = -1;
    if (g_ir_cache_on)
        for (int i = 0; i < norder && i < np; i++) hreg[order[i]] = pool[i];

    size_t cap = g_ir_compile_code_slot_size ? g_ir_compile_code_slot_size : IR_CODE_SLOT_SIZE;
    void *mem = ir_code_alloc(cap, &cap);
    if (mem == NULL) return 0;
    struct e64 e = { .p = (uint8_t *)mem, .size = 0, .cap = cap, .fail = 0 };

    /* save list: normal blocks use a minimal frame.  Body-entry chaining uses a
     * standard frame, because any source body may jump into any target body and
     * the target epilogue must restore the same stack/register layout. */
    unsigned save[7]; int nsave = 0; save[nsave++] = R12;
    if (has_mem) { save[nsave++] = R13; if (hoist_ram_base) save[nsave++] = RBP; }
    { int rbx = 0, r14 = 0, r15 = 0;
      for (int g = 0; g < 16; g++) { if (hreg[g] == RBX) rbx = 1; else if (hreg[g] == R14) r14 = 1; else if (hreg[g] == R15) r15 = 1; }
      if (rbx) save[nsave++] = RBX;
      if (r14) save[nsave++] = R14;
      if (r15) save[nsave++] = R15; }
    int pad = (has_mem && (nsave % 2 == 0)) ? 8 : 0;
    if (g_ir_body_chain_on) {
        nsave = 0; save[nsave++] = R12; save[nsave++] = R13;
        save[nsave++] = RBX; save[nsave++] = R14; save[nsave++] = R15; pad = 0;
    }

    if (g_ir_body_chain_on) {
        chain_prologue(&e, ram_base);
        g_ir_last_body_off = e.size;
    } else {
        prologue(&e, save, nsave);
        if (has_mem) { mov_r64_r64(&e, R13, RSI); if (hoist_ram_base) mov_r64_imm64(&e, RBP, ram_base); }
        if (pad) sub_rsp_imm8(&e, (uint8_t)pad);
        g_ir_last_body_off = e.size;
    }
    /* entry/body: load cached guest regs that are read into their host regs */
    for (int g = 0; g < 16; g++)
        if (hreg[g] >= 0 && (readmask & (1U << g))) mov_r32_m32r12(&e, (unsigned)hreg[g], OFF_R(g));
    /* body.  label[k] = code offset of guest op k, for internal back-branches. */
    size_t label[CV1K_IR_MAX_INSTS];
    for (cv1k_u32 k = 0; k < count; k++) {
        label[k] = e.size;
        if (brtarget[k] >= 0) {
            /* Internal conditional branch (loop): registers stay resident across
             * iterations.  The hot SH countdown idiom is DT Rn; BF loop.  After
             * DT, T is exactly (Rn == 0), so branch directly on the decremented
             * Rn instead of reloading and testing SR.T from memory. */
            int is_bt = (ops[k] & 0xff00U) == 0x8900U;
            if (k > 0 && (ops[k - 1U] & 0xf0ffU) == 0x4010U) {
                unsigned rn = (unsigned)((ops[k - 1U] >> 8U) & 15U);
                if (hreg[rn] >= 0) {
                    test_rr(&e, (unsigned)hreg[rn], (unsigned)hreg[rn]);
                } else {
                    mov_r32_m32r12(&e, RAX, OFF_R(rn));
                    test_rr(&e, RAX, RAX);
                }
                e8(&e, 0x0f); e8(&e, (uint8_t)(is_bt ? 0x84 : 0x85)); /* BT: z, BF: nz */
            } else {
                mov_r32_m32r12(&e, RAX, OFF_SR);
                e8(&e, 0xa8); e8(&e, 0x01);                       /* test al, 1 (T bit) */
                e8(&e, 0x0f); e8(&e, (uint8_t)(is_bt ? 0x85 : 0x84)); /* jnz / jz rel32 */
            }
            int32_t rel = (int32_t)((intptr_t)label[brtarget[k]] - (intptr_t)(e.size + 4));
            e32(&e, (uint32_t)rel);
            if (k > 0 && (ops[k - 1U] & 0xf0ffU) == 0x4010U) {
                if (is_bt) and_m32r12_imm32(&e, OFF_SR, ~1U);
                else      or_m32r12_imm32(&e, OFF_SR, 1U);
            }
            continue;
        }
        if ((ops[k] & 0xf0ffU) == 0x4010U && k + 1U < count && brtarget[k + 1U] >= 0) {
            /* DT immediately feeding an internal BT/BF loop branch.  The branch
             * tests the decremented counter directly, so updating SR.T on every
             * taken iteration is wasted.  Defer the architectural T update to
             * the branch fall-through path below. */
            unsigned rn = (unsigned)((ops[k] >> 8U) & 15U);
            if (hreg[rn] >= 0) {
                alu_ri(&e, 5, (unsigned)hreg[rn], 1U);
            } else {
                mov_r32_m32r12(&e, RAX, OFF_R(rn));
                alu_ri(&e, 5, RAX, 1U);
                mov_m32r12_r32(&e, OFF_R(rn), RAX);
            }
            continue;
        }
        struct iropbuf b = {0};
        ir_lower(ops[k], pc + k * 2U, &b);
        if (ir_buf_needs_ppc(&b)) mov_m32r12_imm32(&e, OFF_PPC, pc + k * 2U);
        if (!ir_emit_op(&e, &b, hreg, ram_base, ram_size, hoist_ram_base ? RBP : 0U)) { e.fail = 1; break; }
    }
    if (term == 1 && !e.fail) {
        cv1k_u32 taken = term_bpc + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(term_op & 0xffU) * 2);
        cv1k_u32 fall = term_bpc + 2U;
        unsigned hib = term_op & 0xff00U;
        int is_t = (hib == 0x8900U);
        for (int g = 0; g < 16; g++)
            if (hreg[g] >= 0 && (writemask & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
        mov_r32_m32r12(&e, RAX, OFF_SR);
        e8(&e, 0xa8); e8(&e, 0x01);
        if (g_ir_body_chain_on) {
            size_t j_taken = jcc32(&e, (uint8_t)(is_t ? 0x85 : 0x84));
            emit_body_chain_exit(&e, fall, term_prefix_cycles + 1U);
            size_t lab_taken = e.size;
            patch_to(&e, j_taken, lab_taken);
            mov_m32r12_imm32(&e, OFF_EA, taken);
            mov_m32r12_imm32(&e, OFF_PC, taken);
            epilogue_tail(&e, save, nsave, pad, 0, 0, term_prefix_cycles + 3U);
        } else {
            uint8_t cc = (uint8_t)(is_t ? 0x45 : 0x44);  /* cmovne/cmove: branch taken? */
            mov_r32_imm32(&e, RCX, fall);
            mov_r32_imm32(&e, RDX, taken);
            cmovcc_r32(&e, cc, RCX, RDX);
            mov_m32r12_r32(&e, OFF_PC, RCX);
            mov_r32_m32r12(&e, RDX, OFF_EA);
            mov_r32_imm32(&e, RCX, taken);
            cmovcc_r32(&e, cc, RDX, RCX);
            mov_m32r12_r32(&e, OFF_EA, RDX);
            mov_r32_imm32(&e, RAX, term_prefix_cycles + 1U);
            mov_r32_imm32(&e, RCX, term_prefix_cycles + 3U);
            cmovcc_r32(&e, cc, RAX, RCX);
            epilogue_tail_rax_cycles(&e, save, nsave, pad);
        }
    }

    if (term == 6 && !e.fail) {
        unsigned rn = (term_op >> 8U) & 15U;
        for (int g = 0; g < 16; g++)
            if (hreg[g] >= 0 && (writemask & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
        mov_r64_r64(&e, RDI, R12);
        mov_r32_imm32(&e, RSI, rn);
        call_rax(&e, (const void *)cv1k_sh3_jit_ldc_sr);
        epilogue_tail(&e, save, nsave, pad, 1, term_bpc + 2U, term_prefix_cycles + 1U);
    }

    if (term == 2 && !e.fail) {
        cv1k_u32 taken = term_bpc + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(term_op & 0xffU) * 2);
        cv1k_u32 fall = term_bpc + 2U;
        unsigned hib = term_op & 0xff00U;
        int is_t = (hib == 0x8d00U);
        mov_r32_m32r12(&e, RAX, OFF_SR);
        e8(&e, 0xa8); e8(&e, 0x01);
        size_t j_taken = jcc32(&e, (uint8_t)(is_t ? 0x85 : 0x84));
        for (int g = 0; g < 16; g++)
            if (hreg[g] >= 0 && (writemask & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
        if (g_ir_body_chain_on) {
            emit_body_chain_exit(&e, fall, term_prefix_cycles + 1U);
        } else {
            mov_m32r12_imm32(&e, OFF_PC, fall);
            epilogue_tail(&e, save, nsave, pad, 0, 0, term_prefix_cycles + 1U);
        }

        size_t lab_taken = e.size;
        patch_to(&e, j_taken, lab_taken);
        mov_m32r12_imm32(&e, OFF_EA, taken);
        struct iropbuf db = {0}; ir_lower(term_delay, term_bpc + 2U, &db);
        if (ir_buf_needs_ppc(&db)) mov_m32r12_imm32(&e, OFF_PPC, term_bpc + 2U);
        if (!ir_emit_op(&e, &db, hreg, ram_base, ram_size, hoist_ram_base ? RBP : 0U)) e.fail = 1;
        cv1k_u32 delay_writemask = 0, delay_readmask = 0; int delay_order[16]; int delay_norder = 0, delay_has_mem = 0;
        ir_accum_masks(&db, &delay_readmask, &delay_writemask, delay_order, &delay_norder, &delay_has_mem);
        for (int g = 0; g < 16; g++)
            if (hreg[g] >= 0 && ((writemask | delay_writemask) & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
        mov_m32r12_imm32(&e, OFF_PC, taken);
        epilogue_tail(&e, save, nsave, pad, 0, 0, term_prefix_cycles + 2U + term_delay_cycles);
    }

    /* Unconditional delayed branches execute the delay slot in the same native block. */
    if ((term == 3 || term == 4) && !e.fail) {
        if (term == 3) {
            if ((term_op & 0xf000U) == 0xb000U) mov_m32r12_imm32(&e, OFF_PR, term_bpc + 4U); /* BSR */
        } else if (term == 4) {
            unsigned tn = (term_op >> 8) & 15U;
            if (term_op == 0x000bU) {
                mov_r32_m32r12(&e, RAX, OFF_PR);
            } else {
                if (hreg[tn] >= 0) mov_r32_r32(&e, RAX, (unsigned)hreg[tn]); else mov_r32_m32r12(&e, RAX, OFF_R(tn));
                if ((term_op & 0xf0ffU) == 0x0003U || (term_op & 0xf0ffU) == 0x0023U) alu_ri(&e, 0, RAX, term_bpc + 4U); /* BSRF/BRAF */
                if ((term_op & 0xf0ffU) == 0x0003U || (term_op & 0xf0ffU) == 0x400bU) mov_m32r12_imm32(&e, OFF_PR, term_bpc + 4U); /* BSRF/JSR */
            }
            mov_m32r12_r32(&e, OFF_EA, RAX);
        }
        struct iropbuf db = {0}; ir_lower(term_delay, term_bpc + 2U, &db);
        if (ir_buf_needs_ppc(&db)) mov_m32r12_imm32(&e, OFF_PPC, term_bpc + 2U);
        if (!ir_emit_op(&e, &db, hreg, ram_base, ram_size, hoist_ram_base ? RBP : 0U)) e.fail = 1;
    }
    /* exit: store back cached guest regs that were written */
    for (int g = 0; g < 16; g++)
        if (hreg[g] >= 0 && (writemask & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
    if (e.fail) { cv1k_ir_free_block(mem, cap); return 0; }
    if (term == 1 || term == 2) {
        /* Conditional terminators are emitted above because their paths need
         * different cycle accounting and, for BT/S/BF/S, different delay-slot
         * execution.  Do not append a second generic terminator here: it is
         * unreachable after those per-path epilogues and only bloats compile
         * time plus the executable code cache. */
    } else if (term == 3) {
        cv1k_u32 target = term_bpc + 4U + ((cv1k_u32)sext12(term_op) * 2U);
        if (g_ir_body_chain_on) {
            emit_body_chain_exit(&e, target, cycles);
        } else {
            mov_m32r12_imm32(&e, OFF_PC, target);
            epilogue_tail(&e, save, nsave, pad, 0, 0, cycles);
        }
    } else if (term == 4) {
        mov_r32_m32r12(&e, RAX, OFF_EA);
        mov_m32r12_r32(&e, OFF_PC, RAX);
        epilogue_tail(&e, save, nsave, pad, 0, 0, cycles);
    } else if (term == 5) {
        mov_r64_r64(&e, RDI, R12);
        call_rax(&e, (const void *)cv1k_sh3_jit_rte);
        mov_r32_m32r12(&e, RAX, OFF_M_DELAY);
        mov_m32r12_r32(&e, OFF_PC, RAX);
        mov_m32r12_imm32(&e, OFF_M_DELAY, 0U);
        epilogue_tail(&e, save, nsave, pad, 0, 0, cycles);
    } else {
        cv1k_u32 next_pc = pc + count * 2U;
        if (g_ir_body_chain_on && term == 0 && count > 0) {
            emit_body_chain_exit(&e, next_pc, cycles);
        } else {
            epilogue_tail(&e, save, nsave, pad, 1, next_pc, cycles);
        }
    }
    if (e.fail) { cv1k_ir_free_block(mem, cap); return 0; }
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)mem, (char *)mem + e.size);
#endif
    *out_fn = (cv1k_ir_block_fn)mem; *out_mem = mem; *out_cap = cap;
    g_ir_last_compile_cycles = cycles;
    return count + (term == 1 || term == 6 ? 1U : (term == 2 || term == 3 || term == 4 || term == 5) ? 2U : 0U);  /* guest insns covered */
#endif
}

void cv1k_ir_free_block(void *mem, size_t cap)
{
#if defined(_WIN32)
    (void)cap;
    if (mem && !ir_code_ptr_in_arena(mem)) VirtualFree(mem, 0, MEM_RELEASE);
#elif defined(__unix__) || defined(__APPLE__)
    if (mem && !ir_code_ptr_in_arena(mem)) munmap(mem, cap);
#else
    (void)mem; (void)cap;
#endif
}

/* ===================== production wiring ============================ *
 * A block cache + run-frame that drives the IR DRC like the legacy JIT:
 * compile-and-cache IR blocks, run them, and fall back to the interpreter for
 * any op the IR frontend does not lower.  The cache is reset on the same
 * icache-invalidation triggers the legacy JIT uses (DMA code loads, etc.). */
#define IRC_BUCKETS 16384U
enum ir_hot_dispatch_kind { IR_HOT_NONE = 1U, IR_HOT_BYTE_COPY = 2U, IR_HOT_STORE_FILL = 3U, IR_HOT_TCOND_NOP = 4U, IR_HOT_DTNOP = 5U, IR_HOT_BITBYTE_COPY = 6U, IR_HOT_WORD_MERGE = 7U };
struct ir_hot_dispatch_cached {
    cv1k_u32 pc, target;
    cv1k_u16 op;
    cv1k_u8 kind, state;
    cv1k_u8 r0, r1, r2, r3, r4, r5, r6, r7;
    cv1k_s8 inc0, inc1;
};
struct ir_chain_patch { cv1k_u32 target_pc; cv1k_u32 source_cycles; size_t total_imm_off; size_t body_imm_off; int linked; };
struct ir_cached { cv1k_u32 pc; cv1k_u32 cycles; cv1k_ir_block_fn fn; void *body; int chainable_body; void *mem; size_t cap; cv1k_u8 nchain; struct ir_chain_patch chain[IR_CHAIN_MAX_SITES]; struct ir_hot_dispatch_cached hot; struct ir_cached *next; };
struct ir_cached_page { struct ir_cached item[4096]; size_t used; struct ir_cached_page *next; };
static struct ir_cached_page *g_irc_pages = NULL;
#define IRC_FAST_SLOTS 262144U
static struct ir_cached *g_irc[IRC_BUCKETS];
static struct ir_cached *g_irc_fast[IRC_FAST_SLOTS];

#define IR_SLICE_BUCKETS 4096U
#define IR_SLICE_FAST_SLOTS 65536U
struct ir_slice_cached { cv1k_u32 pc; cv1k_u16 limit; cv1k_u16 cycles; cv1k_ir_block_fn fn; void *mem; size_t cap; struct ir_slice_cached *next; };
struct ir_slice_page { struct ir_slice_cached item[4096]; size_t used; struct ir_slice_page *next; };
static struct ir_slice_page *g_ir_slice_pages = NULL;
static struct ir_slice_cached *g_ir_slice[IR_SLICE_BUCKETS];
static struct ir_slice_cached *g_ir_slice_fast[IR_SLICE_FAST_SLOTS];
static unsigned long long g_ir_slice_hits = 0, g_ir_slice_misses = 0, g_ir_slice_runs = 0;
static int g_ir_slice_init = 0, g_ir_slice_on = 1;
static void ir_slice_maybe_init(void)
{
    if (g_ir_slice_init) return;
    g_ir_slice_init = 1;
    const char *e = getenv("CV1K_IR_SLICES");
    if (e && (*e == '0' || *e == 'n' || *e == 'N')) g_ir_slice_on = 0;
}
static cv1k_u32 ir_slice_hash(cv1k_u32 pc, cv1k_u32 limit)
{
    return (pc ^ (pc >> 4) ^ (pc >> 13) ^ (limit * 131U)) & (IR_SLICE_BUCKETS - 1U);
}
static CV1K_ALWAYS_INLINE cv1k_u32 ir_slice_fast_idx(cv1k_u32 pc, cv1k_u32 limit)
{
    return ((pc >> 1U) ^ (pc >> 9U) ^ (limit * 131U)) & (IR_SLICE_FAST_SLOTS - 1U);
}
static struct ir_slice_cached *ir_slice_find(cv1k_u32 pc, cv1k_u32 limit)
{
    cv1k_u32 fi = ir_slice_fast_idx(pc, limit);
    struct ir_slice_cached *fs = g_ir_slice_fast[fi];
    if (fs != NULL && fs->pc == pc && fs->limit == (cv1k_u16)limit) return fs;
    cv1k_u32 h = ir_slice_hash(pc, limit);
    for (struct ir_slice_cached *s = g_ir_slice[h]; s; s = s->next) {
        if (s->pc == pc && s->limit == (cv1k_u16)limit) { g_ir_slice_fast[fi] = s; return s; }
    }
    return NULL;
}
static struct ir_slice_cached *ir_slice_alloc_entry(void)
{
    struct ir_slice_page *p = g_ir_slice_pages;
    if (p == NULL || p->used >= (sizeof(p->item) / sizeof(p->item[0]))) {
        p = (struct ir_slice_page *)calloc(1, sizeof(*p));
        if (p == NULL) return NULL;
        p->next = g_ir_slice_pages;
        g_ir_slice_pages = p;
    }
    return &p->item[p->used++];
}

static struct ir_slice_cached *ir_slice_get(struct cv1k_bus *bus, cv1k_u32 pc, cv1k_u32 limit)
{
    if (limit == 0U || limit > 255U) return NULL;
    struct ir_slice_cached *hit = ir_slice_find(pc, limit);
    if (hit) { IR_STAT_INC(g_ir_slice_hits); return hit; }
    IR_STAT_INC(g_ir_slice_misses);
    cv1k_ir_block_fn fn = NULL; void *mem = NULL; size_t cap = 0;
    g_ir_compile_cycle_limit = limit;
    g_ir_compile_code_slot_size = IR_CODE_SLICE_SLOT_SIZE;
    cv1k_ir_compile_block(bus, pc, &fn, &mem, &cap);
    g_ir_compile_code_slot_size = 0;
    g_ir_compile_cycle_limit = 0;
    int ok = (fn != NULL && g_ir_last_compile_cycles != 0U && g_ir_last_compile_cycles <= limit);
    if (!ok) { if (mem) cv1k_ir_free_block(mem, cap); fn = NULL; mem = NULL; cap = 0; }
    struct ir_slice_cached *s = ir_slice_alloc_entry();
    if (!s) { if (mem) cv1k_ir_free_block(mem, cap); return NULL; }
    s->pc = pc; s->limit = (cv1k_u16)limit; s->cycles = ok ? (cv1k_u16)g_ir_last_compile_cycles : 0U; s->fn = fn; s->mem = mem; s->cap = cap;
    cv1k_u32 h = ir_slice_hash(pc, limit); s->next = g_ir_slice[h]; g_ir_slice[h] = s;
    g_ir_slice_fast[ir_slice_fast_idx(pc, limit)] = s;
    return s;
}

static int g_ir_enabled = 0;

int  cv1k_ir_enabled(void) { return g_ir_enabled; }
void cv1k_ir_enable(int on) { g_ir_enabled = on ? 1 : 0; }

/* Optional fallback profiler: set CV1K_IR_FB=1 to log, at exit, the IR hit/
 * fallback counts and the most common ops that fell back to the interpreter. */
static unsigned long long g_ir_hits = 0, g_ir_falls = 0, g_ir_falls_md = 0;
static unsigned long long *g_ir_fb = NULL;
static int g_ir_fb_init = 0;
static void ir_fb_dump(void)
{
    fprintf(stderr, "ir-jit: block-runs=%llu fallback-insns=%llu (%.1f%% fallback; %llu in delay slots)\n",
            g_ir_hits, g_ir_falls, 100.0 * (double)g_ir_falls / (double)(g_ir_hits + g_ir_falls + 1ULL), g_ir_falls_md);
    fprintf(stderr, "ir-jit: body-chain links=%llu taken-sites=%llu site-blocks=%llu chainable=%llu patch-attempts=%llu bad=%llu enabled=%d\n", g_ir_chain_links, g_ir_chain_taken_sites, g_ir_chain_site_blocks, g_ir_chainable_blocks, g_ir_chain_patch_attempts, g_ir_chain_patch_bad, g_ir_body_chain_on);
    fprintf(stderr, "ir-jit: slices runs=%llu hits=%llu misses=%llu enabled=%d\n", g_ir_slice_runs, g_ir_slice_hits, g_ir_slice_misses, g_ir_slice_on);
    if (!g_ir_fb) return;
    for (int rank = 0; rank < 24; rank++) {
        unsigned best = 0; unsigned long long bc = 0;
        for (unsigned i = 0; i < 65536; i++) if (g_ir_fb[i] > bc) { bc = g_ir_fb[i]; best = i; }
        if (bc == 0) break;
        fprintf(stderr, "  ir-fallback[%02d] op=%04x count=%llu\n", rank, best, bc);
        g_ir_fb[best] = 0;
    }
}
static void ir_fb_maybe_init(void)
{
    if (g_ir_fb_init) return;
    g_ir_fb_init = 1;
#ifndef CV1K_WASM
    if (getenv("CV1K_IR_FB")) { g_ir_fb = (unsigned long long *)calloc(65536, sizeof(unsigned long long)); atexit(ir_fb_dump); }
#endif
}

static cv1k_u32 irc_hash(cv1k_u32 pc) { return (pc ^ (pc >> 4) ^ (pc >> 13)) & (IRC_BUCKETS - 1U); }

static struct ir_cached *irc_find_cached(cv1k_u32 pc)
{
    cv1k_u32 fi = (pc >> 1U) & (IRC_FAST_SLOTS - 1U);
    struct ir_cached *fc = g_irc_fast[fi];
    if (fc != NULL && fc->pc == pc) return fc;
    cv1k_u32 h = irc_hash(pc);
    for (struct ir_cached *c = g_irc[h]; c; c = c->next) {
        if (c->pc == pc) { g_irc_fast[fi] = c; return c; }
    }
    return NULL;
}

static void irc_patch_one_chain(struct ir_cached *src, unsigned si, struct ir_cached *dst)
{
    if (!src || !dst || si >= src->nchain) return;
    IR_STAT_INC(g_ir_chain_patch_attempts);
    struct ir_chain_patch *p = &src->chain[si];
    if (src->mem == NULL || dst->body == NULL || dst->cycles == 0U || dst->fn == NULL || !dst->chainable_body) { IR_STAT_INC(g_ir_chain_patch_bad); return; }
    if (g_ir_chain_links >= g_ir_body_chain_link_limit) return;
    uint8_t *base = (uint8_t *)src->mem;
    uint32_t total = p->source_cycles + dst->cycles;
    memcpy(base + p->total_imm_off, &total, sizeof(total));
    uint64_t body = (uint64_t)(uintptr_t)dst->body;
    memcpy(base + p->body_imm_off, &body, sizeof(body));
    if (!p->linked) { p->linked = 1; IR_STAT_INC(g_ir_chain_links); }
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)base + p->total_imm_off, (char *)base + p->body_imm_off + 8);
#endif
}

static void irc_patch_outgoing(struct ir_cached *src)
{
    if (!g_ir_body_chain_on || !src) return;
    for (unsigned i = 0; i < src->nchain; i++) {
        struct ir_cached *dst = irc_find_cached(src->chain[i].target_pc);
        if (dst) irc_patch_one_chain(src, i, dst);
    }
}

static void irc_patch_incoming(struct ir_cached *dst)
{
    if (!g_ir_body_chain_on || !dst) return;
    for (cv1k_u32 h = 0; h < IRC_BUCKETS; h++) {
        for (struct ir_cached *src = g_irc[h]; src; src = src->next) {
            for (unsigned i = 0; i < src->nchain; i++)
                if (src->chain[i].target_pc == dst->pc) irc_patch_one_chain(src, i, dst);
        }
    }
}

static struct ir_cached *irc_alloc_entry(void)
{
    struct ir_cached_page *p = g_irc_pages;
    if (p == NULL || p->used >= (sizeof p->item / sizeof p->item[0])) {
        p = (struct ir_cached_page *)calloc(1, sizeof(*p));
        if (p == NULL) return NULL;
        p->next = g_irc_pages;
        g_irc_pages = p;
    }
    return &p->item[p->used++];
}

static void irc_free_pages(void)
{
    struct ir_cached_page *p = g_irc_pages;
    while (p) {
        struct ir_cached_page *n = p->next;
        free(p);
        p = n;
    }
    g_irc_pages = NULL;
}

void cv1k_ir_reset(void)
{
    for (cv1k_u32 i = 0; i < IRC_BUCKETS; i++) {
        struct ir_cached *c = g_irc[i];
        while (c) { struct ir_cached *n = c->next; if (c->mem) cv1k_ir_free_block(c->mem, c->cap); c = n; }
        g_irc[i] = NULL;
    }
    irc_free_pages();
    for (cv1k_u32 i = 0; i < IR_SLICE_BUCKETS; i++) {
        struct ir_slice_cached *s = g_ir_slice[i];
        while (s) { struct ir_slice_cached *n = s->next; if (s->mem) cv1k_ir_free_block(s->mem, s->cap); s = n; }
        g_ir_slice[i] = NULL;
    }
    while (g_ir_slice_pages) { struct ir_slice_page *n = g_ir_slice_pages->next; free(g_ir_slice_pages); g_ir_slice_pages = n; }
    memset(g_ir_slice_fast, 0, sizeof(g_ir_slice_fast));
    memset(g_irc_fast, 0, sizeof(g_irc_fast));
    ir_code_arena_reset();
}

static void ir_classify_hot_dispatch(struct ir_hot_dispatch_cached *hc, const struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 pc);

/* Look up (or compile+cache) the IR block at pc.  Returns its fn, or NULL when
 * the op at pc is not IR-lowerable (caller uses the interpreter).  Negative
 * results are cached (fn == NULL) so we don't recompile non-IR PCs each time. */
static CV1K_ALWAYS_INLINE struct ir_cached *irc_get(struct cv1k_bus *bus, cv1k_u32 pc)
{
    struct ir_cached *hit = irc_find_cached(pc);
    if (hit) return hit;
    cv1k_u32 h = irc_hash(pc);
    cv1k_u32 fi = (pc >> 1U) & (IRC_FAST_SLOTS - 1U);
    cv1k_ir_block_fn fn = NULL; void *mem = NULL; size_t cap = 0;
    cv1k_ir_compile_block(bus, pc, &fn, &mem, &cap);
    struct ir_cached *c = irc_alloc_entry();
    if (c == NULL) { if (mem) cv1k_ir_free_block(mem, cap); return NULL; }
    memset(c, 0, sizeof(*c));
    c->pc = pc; c->cycles = fn ? g_ir_last_compile_cycles : 0U; c->fn = fn;
    c->body = (fn && mem) ? (void *)((uint8_t *)mem + g_ir_last_body_off) : NULL;
    c->chainable_body = g_ir_last_chainable_body;
    c->mem = mem; c->cap = cap;
    ir_classify_hot_dispatch(&c->hot, NULL, bus, pc);
    c->nchain = (cv1k_u8)g_ir_last_chain_count;
    if (c->nchain) IR_STAT_INC(g_ir_chain_site_blocks);
    if (c->chainable_body) IR_STAT_INC(g_ir_chainable_blocks);
    for (int i = 0; i < g_ir_last_chain_count && i < IR_CHAIN_MAX_SITES; i++) {
        c->chain[i].target_pc = g_ir_last_chain[i].target_pc;
        c->chain[i].source_cycles = g_ir_last_chain[i].source_cycles;
        c->chain[i].total_imm_off = g_ir_last_chain[i].total_imm_off;
        c->chain[i].body_imm_off = g_ir_last_chain[i].body_imm_off;
        c->chain[i].linked = 0;
    }
    c->next = g_irc[h]; g_irc[h] = c; g_irc_fast[fi] = c;
    irc_patch_outgoing(c);
    irc_patch_incoming(c);
    return c;
}

#define IR_SH_I  0x000000f0U
#define IR_SH_BL 0x10000000U
static CV1K_ALWAYS_INLINE int irc_irq_can_accept(const struct sh7709s_cpu *cpu)
{
    if (cpu->m_delay != 0U || cpu->pend_mask == 0U || (cpu->sr & IR_SH_BL) != 0U) return 0;
    cv1k_u32 mask = (cpu->sr & IR_SH_I) >> 4, pm = cpu->pend_mask;
    if ((pm & (pm - 1U)) == 0U) { int i = __builtin_ctz(pm); return (cpu->pend_pri[i] & 0x0fU) > mask; }
    while (pm != 0U) { int i = __builtin_ctz(pm); if ((cpu->pend_pri[i] & 0x0fU) > mask) return 1; pm &= pm - 1U; }
    return 0;
}


/* Hot bounded memory loops ending in BF/S;NOP.  These are not game-specific PC
 * shims: they recognise SH-3 instruction shapes, execute the same bus helpers as
 * the interpreter, and cap work at the next TMU/frame boundary so IRQ/timer
 * observation points remain intact. */
static int ir_tcond_opcode_supported(cv1k_u16 op)
{
    if ((op & 0xf00fU) == 0x2008U) return 1; /* TST Rm,Rn */
    if ((op & 0xf00fU) == 0x3000U) return 1; /* CMP/EQ */
    if ((op & 0xf00fU) == 0x3002U) return 1; /* CMP/HS */
    if ((op & 0xf00fU) == 0x3003U) return 1; /* CMP/GE */
    if ((op & 0xf00fU) == 0x3006U) return 1; /* CMP/HI */
    if ((op & 0xf00fU) == 0x3007U) return 1; /* CMP/GT */
    if ((op & 0xff00U) == 0x8800U) return 1; /* CMP/EQ #imm,R0 */
    if ((op & 0xff00U) == 0xc800U) return 1; /* TST #imm,R0 */
    if ((op & 0xf0ffU) == 0x4011U) return 1; /* CMP/PZ */
    if ((op & 0xf0ffU) == 0x4015U) return 1; /* CMP/PL */
    return 0;
}

static int ir_eval_tcond_no_write(const struct sh7709s_cpu *cpu, cv1k_u16 op, int *out_t)
{
    unsigned n = (unsigned)((op >> 8U) & 15U), m = (unsigned)((op >> 4U) & 15U);
    if ((op & 0xf00fU) == 0x2008U) { *out_t = ((cpu->r[n] & cpu->r[m]) == 0U); return 1; }                 /* TST Rm,Rn */
    if ((op & 0xf00fU) == 0x3000U) { *out_t = (cpu->r[n] == cpu->r[m]); return 1; }                         /* CMP/EQ */
    if ((op & 0xf00fU) == 0x3002U) { *out_t = (cpu->r[n] >= cpu->r[m]); return 1; }                         /* CMP/HS */
    if ((op & 0xf00fU) == 0x3003U) { *out_t = ((cv1k_s32)cpu->r[n] >= (cv1k_s32)cpu->r[m]); return 1; }       /* CMP/GE */
    if ((op & 0xf00fU) == 0x3006U) { *out_t = (cpu->r[n] > cpu->r[m]); return 1; }                          /* CMP/HI */
    if ((op & 0xf00fU) == 0x3007U) { *out_t = ((cv1k_s32)cpu->r[n] > (cv1k_s32)cpu->r[m]); return 1; }        /* CMP/GT */
    if ((op & 0xff00U) == 0x8800U) { *out_t = (cpu->r[0] == (cv1k_u32)(int8_t)(op & 0xffU)); return 1; }      /* CMP/EQ #imm,R0 */
    if ((op & 0xff00U) == 0xc800U) { *out_t = ((cpu->r[0] & (cv1k_u32)(op & 0xffU)) == 0U); return 1; }       /* TST #imm,R0 */
    if ((op & 0xf0ffU) == 0x4011U) { *out_t = ((cv1k_s32)cpu->r[n] >= 0); return 1; }                        /* CMP/PZ */
    if ((op & 0xf0ffU) == 0x4015U) { *out_t = ((cv1k_s32)cpu->r[n] > 0); return 1; }                         /* CMP/PL */
    return 0;
}

static int ir_run_hot_byte_copy_cached(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                       cv1k_u32 next_tmu, cv1k_u32 frame_end,
                                       const struct ir_hot_dispatch_cached *hc)
{
    cv1k_u32 pc = hc->pc;
    cv1k_u32 to_tmu = next_tmu - cpu->cycles;
    cv1k_u32 to_frame = frame_end - cpu->cycles;
    cv1k_u32 remain = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
    if (remain < 5U) return 0;

    unsigned src_r = hc->r0, tmp_r = hc->r1, dst_r = hc->r2, ctr_r = hc->r3;
    uint64_t before_zero = (cpu->r[ctr_r] == 0U) ? UINT64_C(0xffffffff) : (uint64_t)(cpu->r[ctr_r] - 1U);
    uint64_t can_take = remain / 7U;
    uint64_t take = (before_zero < can_take) ? before_zero : can_take;
    int ran = 0;
    while (take-- != 0U) {
        cpu->r[tmp_r] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->r[src_r]);
        if (src_r != tmp_r) cpu->r[src_r] += 1U;
        cv1k_sh3_jit_write8(bus, cpu->r[dst_r], cpu->r[tmp_r]);
        cpu->r[dst_r] += (cv1k_u32)(cv1k_s32)hc->inc0;
        cpu->r[ctr_r] -= 1U;
        cpu->sr &= ~1U;
        cpu->ppc = pc + 10U;
        cpu->pc = pc;
        cpu->cycles += 7U;
        remain -= 7U;
        ran = 1;
    }
    if (cpu->r[ctr_r] == 1U && remain >= 5U) {
        cpu->r[tmp_r] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->r[src_r]);
        if (src_r != tmp_r) cpu->r[src_r] += 1U;
        cv1k_sh3_jit_write8(bus, cpu->r[dst_r], cpu->r[tmp_r]);
        cpu->r[dst_r] += (cv1k_u32)(cv1k_s32)hc->inc0;
        cpu->r[ctr_r] = 0U;
        cpu->sr = (cpu->sr & ~1U) | 1U;
        cpu->ppc = pc + 8U;
        cpu->pc = pc + 10U;
        cpu->cycles += 5U;
        return 1;
    }
    return ran;
}

static int ir_run_hot_store_fill_cached(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                        cv1k_u32 next_tmu, cv1k_u32 frame_end,
                                        const struct ir_hot_dispatch_cached *hc)
{
    cv1k_u32 pc = hc->pc;
    cv1k_u32 to_tmu = next_tmu - cpu->cycles;
    cv1k_u32 to_frame = frame_end - cpu->cycles;
    cv1k_u32 remain = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
    int ran = 0;
    while (remain >= 8U) {
        cv1k_sh3_jit_write32(bus, cpu->r[hc->r0], cpu->r[hc->r1]);
        cpu->r[hc->r2] += (cv1k_u32)(cv1k_s32)hc->inc0;
        cpu->r[hc->r4] = cv1k_sh3_jit_read32(bus, cpu->r[hc->r3]);
        cpu->r[hc->r4] >>= hc->r5;
        if (cpu->r[hc->r2] >= cpu->r[hc->r4]) {
            cpu->sr = (cpu->sr & ~1U) | 1U;
            cpu->ppc = pc + 10U;
            cpu->pc = pc + 12U;
            cpu->cycles += 6U;
            return 1;
        }
        cpu->sr &= ~1U;
        cpu->r[hc->r6] += (cv1k_u32)(cv1k_s32)hc->inc1;
        cpu->ppc = pc + 12U;
        cpu->pc = pc;
        cpu->cycles += 8U;
        remain -= 8U;
        ran = 1;
    }
    return ran;
}

static int ir_run_hot_tcond_cached(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                   cv1k_u32 next_tmu, cv1k_u32 frame_end,
                                   const struct ir_hot_dispatch_cached *hc)
{
    (void)bus;
    cv1k_u32 pc = hc->pc;
    cv1k_u32 to_tmu = next_tmu - cpu->cycles;
    cv1k_u32 to_frame = frame_end - cpu->cycles;
    cv1k_u32 remain = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
    int t = 0;
    if (!ir_eval_tcond_no_write(cpu, hc->op, &t)) return 0;
    int taken = (hc->r0 == 0x8dU) ? t : !t;
    if ((!taken && remain < 2U) || (taken && remain < 3U)) return 0;
    cpu->sr = (cpu->sr & ~1U) | (t ? 1U : 0U);
    if (taken) {
        cpu->ea = hc->target;
        if (remain >= 4U) {
            cpu->ppc = pc + 4U;
            cpu->pc = hc->target;
            cpu->cycles += 4U;
        } else {
            cpu->m_delay = hc->target;
            cpu->ppc = pc + 2U;
            cpu->pc = pc + 4U;
            cpu->cycles += 3U;
        }
    } else {
        cpu->ppc = pc + 2U;
        cpu->pc = pc + 4U;
        cpu->cycles += 2U;
    }
    return 1;
}

static int ir_run_hot_dtnop_cached(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                   cv1k_u32 next_tmu, cv1k_u32 frame_end,
                                   const struct ir_hot_dispatch_cached *hc)
{
    (void)bus;
    cv1k_u32 pc = hc->pc;
    unsigned rn = hc->r0;
    cv1k_u32 to_tmu = next_tmu - cpu->cycles;
    cv1k_u32 to_frame = frame_end - cpu->cycles;
    cv1k_u32 limit = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
    if (limit <= 1U) return 0;
    cv1k_u32 remain = limit;
    cv1k_u32 r = cpu->r[rn];
    uint64_t taken_before_zero = (r == 0U) ? UINT64_C(0xffffffff) : (uint64_t)(r - 1U);
    uint64_t can_take = remain / 5U;
    uint64_t take = (taken_before_zero < can_take) ? taken_before_zero : can_take;
    if (take != 0U) {
        cpu->r[rn] = r - (cv1k_u32)take;
        cpu->sr &= ~1U;
        cpu->ppc = pc + 6U;
        cpu->pc = pc;
        cpu->cycles += (cv1k_u32)(take * 5U);
        remain -= (cv1k_u32)(take * 5U);
        r = cpu->r[rn];
    }
    if (r == 1U && remain >= 3U) {
        cpu->r[rn] = 0U;
        cpu->sr = (cpu->sr & ~1U) | 1U;
        cpu->ppc = pc + 4U;
        cpu->pc = pc + 6U;
        cpu->cycles += 3U;
        return 1;
    }
    return take != 0U;
}

/* Accelerate a general SH-3 bit/byte copy idiom:
 *   CMP/PZ Rb; BF exit
 *   MOV.B @Rs+,Rt; MOV.B Rt,@Rd; ADD #imm,Rd; DT Rc; BT/S exit; MOV Rb,Rw
 *   ADD Rw,Rw; DT Ri; BF/S loop; EXTS.B Rw,Rb
 * The helper only consumes complete back-edge iterations.  Any exit iteration
 * is left for the normal IR/interpreter path, which keeps branch-delay and SR.T
 * edge cases exact without baking in a PC-specific shortcut. */

/* Accelerate a bounded SH-3 16-bit merge/countdown idiom:
 *   MOV.W @Ra,Rt; EXTU.W Rt,Rv; TST Rv,Rv; BT tail
 *   MOV.W @Rb,Rt; EXTU.W Rt,Rc; CMP/GE Rc,Rv; BT tail; MOV.W Rc,@Ra
 *   ADD #2,Ra; DT Rn; BF/S loop; ADD #2,Rb
 * This is a shape recognizer, not a PC shortcut.  The helper uses direct work
 * RAM only when dcache is disabled and addresses are in bounds; otherwise it
 * uses the existing JIT bus helpers.  It stops before TMU/frame boundaries. */
static CV1K_ALWAYS_INLINE cv1k_u16 ir_hot_read16(struct cv1k_bus *bus, cv1k_u32 addr)
{
    struct cv1k_machine *m = bus ? bus->machine : NULL;
    if (m != NULL && m->main_ram != NULL && m->dcache_enabled == 0 &&
        addr >= CV1K_ADDR_WORK_RAM && addr - CV1K_ADDR_WORK_RAM + 1U < m->main_ram_size) {
        cv1k_u8 *p = m->main_ram + (addr - CV1K_ADDR_WORK_RAM);
        return (cv1k_u16)(((cv1k_u16)p[0] << 8) | p[1]);
    }
    return cv1k_sh3_jit_read16(bus, addr);
}
static CV1K_ALWAYS_INLINE void ir_hot_write16(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data)
{
    struct cv1k_machine *m = bus ? bus->machine : NULL;
    if (m != NULL && m->main_ram != NULL && m->dcache_enabled == 0 &&
        addr >= CV1K_ADDR_WORK_RAM && addr - CV1K_ADDR_WORK_RAM + 1U < m->main_ram_size) {
        cv1k_u8 *p = m->main_ram + (addr - CV1K_ADDR_WORK_RAM);
        p[0] = (cv1k_u8)(data >> 8); p[1] = (cv1k_u8)data;
        return;
    }
    cv1k_sh3_jit_write16(bus, addr, data);
}
static int ir_run_hot_word_merge_cached(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                        cv1k_u32 next_tmu, cv1k_u32 frame_end,
                                        const struct ir_hot_dispatch_cached *hc)
{
    cv1k_u32 pc = hc->pc;
    cv1k_u32 to_tmu = next_tmu - cpu->cycles;
    cv1k_u32 to_frame = frame_end - cpu->cycles;
    cv1k_u32 remain = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
    if (remain < 11U) return 0;
    unsigned ra = hc->r0, rb = hc->r1, rt = hc->r2, rv = hc->r3, rc = hc->r4, rn = hc->r5;
    int ran = 0;
    for (;;) {
        cv1k_u32 a = cpu->r[ra];
        cv1k_u32 b = cpu->r[rb];
        cv1k_u32 v = (cv1k_u32)ir_hot_read16(bus, a);
        cpu->r[rt] = v; cpu->r[rv] = v;
        cv1k_u32 cost = 11U;
        if (v != 0U) {
            cv1k_u32 c = (cv1k_u32)ir_hot_read16(bus, b);
            cpu->r[rt] = c; cpu->r[rc] = c;
            cost = (v >= c) ? 17U : 18U;
            if (remain < cost) return ran;
            if (v >= c) cpu->sr = (cpu->sr & ~1U) | 1U;
            else { cpu->sr &= ~1U; ir_hot_write16(bus, a, c); }
        } else {
            if (remain < cost) return ran;
            cpu->sr = (cpu->sr & ~1U) | 1U;
        }
        cpu->r[ra] = a + (cv1k_u32)(cv1k_s32)hc->inc0;
        cv1k_u32 ctr = cpu->r[rn] - 1U;
        cpu->r[rn] = ctr;
        cpu->r[rb] = b + (cv1k_u32)(cv1k_s32)hc->inc1;
        cpu->cycles += cost;
        remain -= cost;
        ran = 1;
        if (ctr == 0U) {
            cpu->sr = (cpu->sr & ~1U) | 1U;
            cpu->ppc = pc + 22U;
            cpu->pc = pc + 26U;
            return 1;
        }
        cpu->sr &= ~1U;
        cpu->ppc = pc + 24U;
        cpu->pc = pc;
        if (remain < 11U) return 1;
    }
}

static int ir_run_hot_bitbyte_copy_cached(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                          cv1k_u32 next_tmu, cv1k_u32 frame_end,
                                          const struct ir_hot_dispatch_cached *hc)
{
    cv1k_u32 pc = hc->pc;
    cv1k_u32 to_tmu = next_tmu - cpu->cycles;
    cv1k_u32 to_frame = frame_end - cpu->cycles;
    cv1k_u32 remain = ((cv1k_s32)(to_tmu - to_frame) < 0) ? to_tmu : to_frame;
    if (remain < 14U) return 0;

    unsigned bit_r = hc->r0, src_r = hc->r1, tmp_r = hc->r2, dst_r = hc->r3;
    unsigned ctr_r = hc->r4, work_r = hc->r5, inner_r = hc->r6;
    cv1k_u32 ctr = cpu->r[ctr_r];
    cv1k_u32 inner = cpu->r[inner_r];
    cv1k_u32 bit = cpu->r[bit_r];
    if ((cv1k_s32)bit < 0) return 0;
    uint64_t max_by_ctr = (ctr == 0U) ? UINT64_C(0xffffffff) : (uint64_t)(ctr - 1U);
    uint64_t max_by_inner = (inner == 0U) ? UINT64_C(0xffffffff) : (uint64_t)(inner - 1U);
    uint64_t max_by_cycles = remain / 14U;
    uint64_t take = max_by_cycles;
    if (take > max_by_ctr) take = max_by_ctr;
    if (take > max_by_inner) take = max_by_inner;
    if (take == 0U) return 0;

    uint64_t bit_take = 0;
    cv1k_u32 b = bit;
    while (bit_take < take && (cv1k_s32)b >= 0) {
        b = (cv1k_u32)(cv1k_s32)(int8_t)((b << 1U) & 0xffU);
        bit_take++;
    }
    take = bit_take;
    if (take == 0U) return 0;

    struct cv1k_machine *mch = bus ? bus->machine : NULL;
    int regs_distinct = (src_r != tmp_r && src_r != dst_r && src_r != ctr_r && src_r != work_r && src_r != inner_r && src_r != bit_r &&
                         tmp_r != dst_r && tmp_r != ctr_r && tmp_r != work_r && tmp_r != inner_r && tmp_r != bit_r &&
                         dst_r != ctr_r && dst_r != work_r && dst_r != inner_r && dst_r != bit_r &&
                         ctr_r != work_r && ctr_r != inner_r && ctr_r != bit_r &&
                         work_r != inner_r && work_r != bit_r && inner_r != bit_r);
    cv1k_u32 src0 = cpu->r[src_r];
    cv1k_u32 dst0 = cpu->r[dst_r];
    int direct = regs_distinct && hc->inc0 == 1 && mch != NULL && mch->main_ram != NULL && mch->dcache_enabled == 0 &&
                 src0 >= CV1K_ADDR_WORK_RAM && dst0 >= CV1K_ADDR_WORK_RAM &&
                 (uint64_t)(src0 - CV1K_ADDR_WORK_RAM) + take <= (uint64_t)mch->main_ram_size &&
                 (uint64_t)(dst0 - CV1K_ADDR_WORK_RAM) + take <= (uint64_t)mch->main_ram_size;
    if (direct) {
        cv1k_u8 *ram = mch->main_ram;
        size_t so = (size_t)(src0 - CV1K_ADDR_WORK_RAM);
        size_t doff = (size_t)(dst0 - CV1K_ADDR_WORK_RAM);
        cv1k_u32 last_v = 0U;
        cv1k_u32 rb = cpu->r[bit_r];
        cv1k_u32 rw = cpu->r[work_r];
        for (uint64_t i = 0; i < take; i++) {
            cv1k_u8 raw = ram[so + (size_t)i];
            ram[doff + (size_t)i] = raw;
            last_v = (cv1k_u32)(cv1k_s32)(int8_t)raw;
            rw = rb + rb;
            rb = (cv1k_u32)(cv1k_s32)(int8_t)(rw & 0xffU);
        }
        cpu->r[tmp_r] = last_v;
        cpu->r[src_r] = src0 + (cv1k_u32)take;
        cpu->r[dst_r] = dst0 + (cv1k_u32)take;
        cpu->r[ctr_r] -= (cv1k_u32)take;
        cpu->r[inner_r] -= (cv1k_u32)take;
        cpu->r[work_r] = rw;
        cpu->r[bit_r] = rb;
    } else {
        for (uint64_t i = 0; i < take; i++) {
            cv1k_u32 v = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->r[src_r]);
            cpu->r[tmp_r] = v;
            if (src_r != tmp_r) cpu->r[src_r] += 1U;
            cv1k_sh3_jit_write8(bus, cpu->r[dst_r], v);
            cpu->r[dst_r] += (cv1k_u32)(cv1k_s32)hc->inc0;
            cpu->r[ctr_r] -= 1U;
            cpu->r[work_r] = cpu->r[bit_r];
            cpu->r[work_r] += cpu->r[work_r];
            cpu->r[inner_r] -= 1U;
            cpu->r[bit_r] = (cv1k_u32)(cv1k_s32)(int8_t)(cpu->r[work_r] & 0xffU);
        }
    }
    cpu->sr &= ~1U;
    cpu->ppc = pc + 22U;
    cpu->pc = pc;
    cpu->cycles += (cv1k_u32)(take * 14U);
    return 1;
}

static void ir_classify_hot_dispatch(struct ir_hot_dispatch_cached *hc, const struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 pc)
{
    (void)cpu;
    memset(hc, 0, sizeof(*hc));
    hc->pc = pc;
    hc->kind = IR_HOT_NONE;
    hc->state = 1U;
    cv1k_u16 op0 = cv1k_bus_fetch16(bus, pc + 0U);
    cv1k_u16 op1 = cv1k_bus_fetch16(bus, pc + 2U);
    cv1k_u16 op2 = cv1k_bus_fetch16(bus, pc + 4U);

    if (op0 == 0x0009U && (op1 & 0xf0ffU) == 0x4010U && op2 == 0x8ffcU && cv1k_bus_fetch16(bus, pc + 6U) == 0x0009U) {
        hc->kind = IR_HOT_DTNOP;
        hc->r0 = (cv1k_u8)((op1 >> 8U) & 15U);
        return;
    }

    if ((op2 == 0x0009U) && ((op1 & 0xff00U) == 0x8d00U || (op1 & 0xff00U) == 0x8f00U) && ir_tcond_opcode_supported(op0)) {
        hc->kind = IR_HOT_TCOND_NOP;
        hc->op = op0;
        hc->r0 = (cv1k_u8)(op1 >> 8U);
        hc->target = pc + 6U + (cv1k_u32)((cv1k_s32)(int8_t)(op1 & 0xffU) * 2);
        return;
    }

    cv1k_u16 op3 = cv1k_bus_fetch16(bus, pc + 6U);
    cv1k_u16 op4 = cv1k_bus_fetch16(bus, pc + 8U);
    cv1k_u16 op5 = cv1k_bus_fetch16(bus, pc + 10U);
    cv1k_u16 op6 = cv1k_bus_fetch16(bus, pc + 12U);
    if ((op0 & 0xf00fU) == 0x6004U && (op1 & 0xf00fU) == 0x2000U && (op2 & 0xf000U) == 0x7000U &&
        (op3 & 0xf0ffU) == 0x4010U && (op4 & 0xff00U) == 0x8f00U && op5 == 0x0009U &&
        pc + 12U + (cv1k_u32)((cv1k_s32)(int8_t)(op4 & 0xffU) * 2) == pc) {
        cv1k_u8 tmp_r = (cv1k_u8)((op0 >> 8U) & 15U);
        cv1k_u8 dst_r = (cv1k_u8)((op1 >> 8U) & 15U);
        if (((op1 >> 4U) & 15U) == tmp_r && ((op2 >> 8U) & 15U) == dst_r) {
            hc->kind = IR_HOT_BYTE_COPY;
            hc->r0 = (cv1k_u8)((op0 >> 4U) & 15U);
            hc->r1 = tmp_r;
            hc->r2 = dst_r;
            hc->r3 = (cv1k_u8)((op3 >> 8U) & 15U);
            hc->inc0 = (cv1k_s8)(int8_t)(op2 & 0xffU);
            return;
        }
    }

    cv1k_u16 op7 = cv1k_bus_fetch16(bus, pc + 14U);
    cv1k_u16 op8 = cv1k_bus_fetch16(bus, pc + 16U);
    cv1k_u16 op9 = cv1k_bus_fetch16(bus, pc + 18U);
    cv1k_u16 op10 = cv1k_bus_fetch16(bus, pc + 20U);
    cv1k_u16 op11 = cv1k_bus_fetch16(bus, pc + 22U);

    if ((op0 & 0xf00fU) == 0x6001U && (op1 & 0xf00fU) == 0x600dU &&
        (op2 & 0xf00fU) == 0x2008U && (op3 & 0xff00U) == 0x8900U &&
        (op4 & 0xf00fU) == 0x6001U && (op5 & 0xf00fU) == 0x600dU &&
        (op6 & 0xf00fU) == 0x3003U && (op7 & 0xff00U) == 0x8900U &&
        (op8 & 0xf00fU) == 0x2001U && (op9 & 0xf000U) == 0x7000U &&
        (op10 & 0xf0ffU) == 0x4010U && (op11 & 0xff00U) == 0x8f00U) {
        cv1k_u16 op12 = cv1k_bus_fetch16(bus, pc + 24U);
        cv1k_u8 rt0 = (cv1k_u8)((op0 >> 8U) & 15U), ra = (cv1k_u8)((op0 >> 4U) & 15U);
        cv1k_u8 rv = (cv1k_u8)((op1 >> 8U) & 15U), rt1 = (cv1k_u8)((op1 >> 4U) & 15U);
        cv1k_u8 tst_n = (cv1k_u8)((op2 >> 8U) & 15U), tst_m = (cv1k_u8)((op2 >> 4U) & 15U);
        cv1k_u8 rt2 = (cv1k_u8)((op4 >> 8U) & 15U), rb = (cv1k_u8)((op4 >> 4U) & 15U);
        cv1k_u8 rc = (cv1k_u8)((op5 >> 8U) & 15U), rt3 = (cv1k_u8)((op5 >> 4U) & 15U);
        cv1k_u8 cmp_n = (cv1k_u8)((op6 >> 8U) & 15U), cmp_m = (cv1k_u8)((op6 >> 4U) & 15U);
        cv1k_u8 st_n = (cv1k_u8)((op8 >> 8U) & 15U), st_m = (cv1k_u8)((op8 >> 4U) & 15U);
        cv1k_u8 add_a = (cv1k_u8)((op9 >> 8U) & 15U), rn = (cv1k_u8)((op10 >> 8U) & 15U);
        if (rt1 == rt0 && rt2 == rt0 && rt3 == rt0 && tst_n == rv && tst_m == rv &&
            cmp_n == rv && cmp_m == rc && st_n == ra && st_m == rc && add_a == ra &&
            op3 == 0x8904U && op7 == 0x8900U && op9 == (cv1k_u16)(0x7000U | (ra << 8U) | 0x02U) &&
            op12 == (cv1k_u16)(0x7000U | (rb << 8U) | 0x02U) &&
            pc + 24U + (cv1k_u32)((cv1k_s32)(int8_t)(op11 & 0xffU) * 2) == pc) {
            hc->kind = IR_HOT_WORD_MERGE;
            hc->r0 = ra; hc->r1 = rb; hc->r2 = rt0; hc->r3 = rv; hc->r4 = rc; hc->r5 = rn;
            hc->inc0 = 2; hc->inc1 = 2;
            return;
        }
    }

    if (0 && (op0 & 0xf0ffU) == 0x4011U && (op1 & 0xff00U) == 0x8b00U &&
        (op2 & 0xf00fU) == 0x6004U && (op3 & 0xf00fU) == 0x2000U &&
        (op4 & 0xf000U) == 0x7000U && (op5 & 0xf0ffU) == 0x4010U &&
        (op6 & 0xff00U) == 0x8d00U && (op7 & 0xf00fU) == 0x6003U &&
        (op8 & 0xf00fU) == 0x300cU && (op9 & 0xf0ffU) == 0x4010U &&
        (op10 & 0xff00U) == 0x8f00U && (op11 & 0xf00fU) == 0x600eU &&
        pc + 24U + (cv1k_u32)((cv1k_s32)(int8_t)(op10 & 0xffU) * 2) == pc) {
        cv1k_u8 bit_r = (cv1k_u8)((op0 >> 8U) & 15U);
        cv1k_u8 src_r = (cv1k_u8)((op2 >> 4U) & 15U);
        cv1k_u8 tmp_r = (cv1k_u8)((op2 >> 8U) & 15U);
        cv1k_u8 dst_r = (cv1k_u8)((op3 >> 8U) & 15U);
        cv1k_u8 mov_data = (cv1k_u8)((op3 >> 4U) & 15U);
        cv1k_u8 add_dst = (cv1k_u8)((op4 >> 8U) & 15U);
        cv1k_u8 ctr_r = (cv1k_u8)((op5 >> 8U) & 15U);
        cv1k_u8 work_r = (cv1k_u8)((op7 >> 8U) & 15U);
        cv1k_u8 mov_src = (cv1k_u8)((op7 >> 4U) & 15U);
        cv1k_u8 add_n = (cv1k_u8)((op8 >> 8U) & 15U);
        cv1k_u8 add_m = (cv1k_u8)((op8 >> 4U) & 15U);
        cv1k_u8 inner_r = (cv1k_u8)((op9 >> 8U) & 15U);
        cv1k_u8 ext_n = (cv1k_u8)((op11 >> 8U) & 15U);
        cv1k_u8 ext_m = (cv1k_u8)((op11 >> 4U) & 15U);
        if (mov_data == tmp_r && add_dst == dst_r && mov_src == bit_r &&
            add_n == work_r && add_m == work_r && ext_n == bit_r && ext_m == work_r) {
            hc->kind = IR_HOT_BITBYTE_COPY;
            hc->r0 = bit_r; hc->r1 = src_r; hc->r2 = tmp_r; hc->r3 = dst_r;
            hc->r4 = ctr_r; hc->r5 = work_r; hc->r6 = inner_r;
            hc->inc0 = (cv1k_s8)(int8_t)(op4 & 0xffU);
            return;
        }
    }

    if ((op0 & 0xf00fU) == 0x2002U && (op1 & 0xf000U) == 0x7000U && (op2 & 0xf00fU) == 0x6002U &&
        ((op3 & 0xf0ffU) == 0x4009U || (op3 & 0xf0ffU) == 0x4019U || (op3 & 0xf0ffU) == 0x4029U) &&
        (op4 & 0xf00fU) == 0x3002U && (op5 & 0xff00U) == 0x8f00U && (op6 & 0xf000U) == 0x7000U &&
        pc + 14U + (cv1k_u32)((cv1k_s32)(int8_t)(op5 & 0xffU) * 2) == pc) {
        cv1k_u8 limit_r = (cv1k_u8)((op2 >> 8U) & 15U);
        cv1k_u8 cmp_n = (cv1k_u8)((op4 >> 8U) & 15U);
        cv1k_u8 cmp_m = (cv1k_u8)((op4 >> 4U) & 15U);
        if (((op3 >> 8U) & 15U) == limit_r && cmp_m == limit_r && ((op1 >> 8U) & 15U) == cmp_n) {
            hc->kind = IR_HOT_STORE_FILL;
            hc->r0 = (cv1k_u8)((op0 >> 8U) & 15U);      /* store pointer */
            hc->r1 = (cv1k_u8)((op0 >> 4U) & 15U);      /* store value */
            hc->r2 = cmp_n;                              /* counter */
            hc->r3 = (cv1k_u8)((op2 >> 4U) & 15U);      /* limit address */
            hc->r4 = limit_r;
            hc->r5 = (cv1k_u8)((op3 & 0xffU) == 0x09U ? 2U : (op3 & 0xffU) == 0x19U ? 8U : 16U);
            hc->r6 = (cv1k_u8)((op6 >> 8U) & 15U);      /* delay register */
            hc->inc0 = (cv1k_s8)(int8_t)(op1 & 0xffU);
            hc->inc1 = (cv1k_s8)(int8_t)(op6 & 0xffU);
            return;
        }
    }
}

static int ir_try_dt_bfs_nop_delay_slot(struct sh7709s_cpu *cpu, struct cv1k_bus *bus)
{
    if (cpu == NULL || bus == NULL || cpu->sleep_mode != 0U || cpu->halted != 0U || cpu->m_delay == 0U) return 0;
    cv1k_u32 pc = cpu->pc & 0xfffffffeU;
    if (cv1k_bus_fetch16(bus, pc) != 0x0009U) return 0;
    if (cv1k_bus_fetch16(bus, pc - 2U) != 0x8ffcU) return 0;
    if ((cv1k_bus_fetch16(bus, pc - 4U) & 0xf0ffU) != 0x4010U) return 0;
    if (cv1k_bus_fetch16(bus, pc - 6U) != 0x0009U) return 0;
    if (cpu->m_delay != pc - 6U) return 0;
    cpu->ppc = pc;
    cpu->pc = cpu->m_delay;
    cpu->m_delay = 0U;
    cpu->cycles += 1U;
    return 1;
}

static int ir_try_hot_mdelay_step(struct sh7709s_cpu *cpu, struct cv1k_bus *bus)
{
    if (cpu == NULL || bus == NULL || cpu->sleep_mode != 0U || cpu->halted != 0U || cpu->m_delay == 0U) return 0;
    cv1k_u32 pc = cpu->pc & 0xfffffffeU;
    cv1k_u16 op = cv1k_bus_fetch16(bus, pc);
    unsigned n = (unsigned)((op >> 8U) & 15U);
    unsigned m = (unsigned)((op >> 4U) & 15U);
#define IR_MDELAY_ENTER() do { cpu->ppc = pc; cpu->pc = cpu->m_delay; cpu->m_delay = 0U; } while (0)
#define IR_MDELAY_DONE() do { cpu->cycles += 1U; return 1; } while (0)
#define IR_MDELAY_SET_T(v) do { cpu->sr = (cpu->sr & ~1U) | ((v) ? 1U : 0U); } while (0)
    if (op == 0x0009U) { IR_MDELAY_ENTER(); IR_MDELAY_DONE(); }
    if ((op & 0xf000U) == 0x7000U) { IR_MDELAY_ENTER(); cpu->r[n] += (cv1k_u32)(int8_t)(op & 0xffU); IR_MDELAY_DONE(); }
    if ((op & 0xf000U) == 0xe000U) { IR_MDELAY_ENTER(); cpu->r[n] = (cv1k_u32)(int8_t)(op & 0xffU); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0x8800U) { IR_MDELAY_ENTER(); IR_MDELAY_SET_T(cpu->r[0] == (cv1k_u32)(int8_t)(op & 0xffU)); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xc800U) { IR_MDELAY_ENTER(); IR_MDELAY_SET_T((cpu->r[0] & (cv1k_u32)(op & 0xffU)) == 0U); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xc900U) { IR_MDELAY_ENTER(); cpu->r[0] &= (cv1k_u32)(op & 0xffU); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xca00U) { IR_MDELAY_ENTER(); cpu->r[0] ^= (cv1k_u32)(op & 0xffU); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xcb00U) { IR_MDELAY_ENTER(); cpu->r[0] |= (cv1k_u32)(op & 0xffU); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x300cU) { IR_MDELAY_ENTER(); cpu->r[n] += cpu->r[m]; IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x3008U) { IR_MDELAY_ENTER(); cpu->r[n] -= cpu->r[m]; IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x2008U) { IR_MDELAY_ENTER(); IR_MDELAY_SET_T((cpu->r[n] & cpu->r[m]) == 0U); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x3000U) { IR_MDELAY_ENTER(); IR_MDELAY_SET_T(cpu->r[n] == cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x3002U) { IR_MDELAY_ENTER(); IR_MDELAY_SET_T(cpu->r[n] >= cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x3003U) { IR_MDELAY_ENTER(); IR_MDELAY_SET_T((cv1k_s32)cpu->r[n] >= (cv1k_s32)cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x3006U) { IR_MDELAY_ENTER(); IR_MDELAY_SET_T(cpu->r[n] > cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x3007U) { IR_MDELAY_ENTER(); IR_MDELAY_SET_T((cv1k_s32)cpu->r[n] > (cv1k_s32)cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x2009U) { IR_MDELAY_ENTER(); cpu->r[n] &= cpu->r[m]; IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x200aU) { IR_MDELAY_ENTER(); cpu->r[n] ^= cpu->r[m]; IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x200bU) { IR_MDELAY_ENTER(); cpu->r[n] |= cpu->r[m]; IR_MDELAY_DONE(); }
    if ((op & 0xf0ffU) == 0x4010U) { IR_MDELAY_ENTER(); cpu->r[n]--; IR_MDELAY_SET_T(cpu->r[n] == 0U); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x6003U) { IR_MDELAY_ENTER(); cpu->r[n] = cpu->r[m]; IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x600cU) { IR_MDELAY_ENTER(); cpu->r[n] = cpu->r[m] & 0xffU; IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x600dU) { IR_MDELAY_ENTER(); cpu->r[n] = cpu->r[m] & 0xffffU; IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x600eU) { IR_MDELAY_ENTER(); cpu->r[n] = (cv1k_u32)(int8_t)(cpu->r[m] & 0xffU); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x600fU) { IR_MDELAY_ENTER(); cpu->r[n] = (cv1k_u32)(int16_t)(cpu->r[m] & 0xffffU); IR_MDELAY_DONE(); }
    switch (op & 0xf0ffU) {
    case 0x4008U: IR_MDELAY_ENTER(); cpu->r[n] <<= 2;  IR_MDELAY_DONE();
    case 0x4018U: IR_MDELAY_ENTER(); cpu->r[n] <<= 8;  IR_MDELAY_DONE();
    case 0x4028U: IR_MDELAY_ENTER(); cpu->r[n] <<= 16; IR_MDELAY_DONE();
    case 0x4009U: IR_MDELAY_ENTER(); cpu->r[n] >>= 2;  IR_MDELAY_DONE();
    case 0x4019U: IR_MDELAY_ENTER(); cpu->r[n] >>= 8;  IR_MDELAY_DONE();
    case 0x4029U: IR_MDELAY_ENTER(); cpu->r[n] >>= 16; IR_MDELAY_DONE();
    case 0x4000U: case 0x4020U: IR_MDELAY_ENTER(); IR_MDELAY_SET_T((cpu->r[n] >> 31) & 1U); cpu->r[n] <<= 1; IR_MDELAY_DONE();
    case 0x4001U: IR_MDELAY_ENTER(); IR_MDELAY_SET_T(cpu->r[n] & 1U); cpu->r[n] >>= 1; IR_MDELAY_DONE();
    case 0x4021U: IR_MDELAY_ENTER(); IR_MDELAY_SET_T(cpu->r[n] & 1U); cpu->r[n] = (cv1k_u32)((cv1k_s32)cpu->r[n] >> 1); IR_MDELAY_DONE();
    case 0x4011U: IR_MDELAY_ENTER(); IR_MDELAY_SET_T((cv1k_s32)cpu->r[n] >= 0); IR_MDELAY_DONE();
    case 0x4015U: IR_MDELAY_ENTER(); IR_MDELAY_SET_T((cv1k_s32)cpu->r[n] > 0); IR_MDELAY_DONE();
    default: break;
    }
    if ((op & 0xf00fU) == 0x2000U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write8(bus, cpu->r[n], cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x2001U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write16(bus, cpu->r[n], cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x2002U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write32(bus, cpu->r[n], cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x6000U) { IR_MDELAY_ENTER(); cpu->r[n] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x6001U) { IR_MDELAY_ENTER(); cpu->r[n] = (cv1k_u32)(int16_t)cv1k_sh3_jit_read16(bus, cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x6002U) { IR_MDELAY_ENTER(); cpu->r[n] = cv1k_sh3_jit_read32(bus, cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf000U) == 0x1000U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write32(bus, cpu->r[n] + ((cv1k_u32)(op & 0x0fU) * 4U), cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf000U) == 0x5000U) { IR_MDELAY_ENTER(); cpu->r[n] = cv1k_sh3_jit_read32(bus, cpu->r[m] + ((cv1k_u32)(op & 0x0fU) * 4U)); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x0004U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write8(bus, cpu->r[n] + cpu->r[0], cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x0005U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write16(bus, cpu->r[n] + cpu->r[0], cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x0006U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write32(bus, cpu->r[n] + cpu->r[0], cpu->r[m]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x000cU) { IR_MDELAY_ENTER(); cpu->r[n] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->r[m] + cpu->r[0]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x000dU) { IR_MDELAY_ENTER(); cpu->r[n] = (cv1k_u32)(int16_t)cv1k_sh3_jit_read16(bus, cpu->r[m] + cpu->r[0]); IR_MDELAY_DONE(); }
    if ((op & 0xf00fU) == 0x000eU) { IR_MDELAY_ENTER(); cpu->r[n] = cv1k_sh3_jit_read32(bus, cpu->r[m] + cpu->r[0]); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xc000U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write8(bus, cpu->gbr + (cv1k_u32)(op & 0xffU), cpu->r[0]); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xc100U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write16(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 2U), cpu->r[0]); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xc200U) { IR_MDELAY_ENTER(); cv1k_sh3_jit_write32(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 4U), cpu->r[0]); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xc400U) { IR_MDELAY_ENTER(); cpu->r[0] = (cv1k_u32)(int8_t)cv1k_sh3_jit_read8(bus, cpu->gbr + (cv1k_u32)(op & 0xffU)); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xc500U) { IR_MDELAY_ENTER(); cpu->r[0] = (cv1k_u32)(int16_t)cv1k_sh3_jit_read16(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 2U)); IR_MDELAY_DONE(); }
    if ((op & 0xff00U) == 0xc600U) { IR_MDELAY_ENTER(); cpu->r[0] = cv1k_sh3_jit_read32(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 4U)); IR_MDELAY_DONE(); }
#undef IR_MDELAY_SET_T
#undef IR_MDELAY_DONE
#undef IR_MDELAY_ENTER
    return 0;
}

static int ir_try_hot_fallback_step(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u16 op)
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

cv1k_u32 cv1k_irjit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                              cv1k_u32 cycle_budget, cv1k_u32 tmu_interval)
{
    if (tmu_interval == 0U) tmu_interval = 2048U;
    ir_fb_maybe_init(); ir_slice_maybe_init();
    cv1k_u32 start = cpu->cycles, frame_end = start + cycle_budget;
    cv1k_u32 next_tmu = cpu->cycles + tmu_interval;
    while ((cv1k_s32)(cpu->cycles - frame_end) < 0) {
        if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        int irq_ready = 0;
        if (CV1K_UNLIKELY(cpu->pend_mask != 0U)) {
            irq_ready = irc_irq_can_accept(cpu);
            if (irq_ready) { sh7709s_accept_pending_irq(cpu, bus); irq_ready = irc_irq_can_accept(cpu); }
        }
        int used = 0;
        if (cpu->sleep_mode == 0U && cpu->m_delay == 0U && cpu->halted == 0U) {
            const cv1k_u32 to_tmu = next_tmu - cpu->cycles;
            if (!irq_ready && to_tmu <= 3U) {
                cv1k_u16 hop = cv1k_bus_fetch16(bus, cpu->pc);
                if ((hop & 0xff00U) == 0x8900U || (hop & 0xff00U) == 0x8b00U ||
                    (hop & 0xff00U) == 0x8d00U || (hop & 0xff00U) == 0x8f00U ||
                    hop == 0x000bU || (hop & 0xf000U) == 0xa000U || (hop & 0xf000U) == 0xb000U ||
                    (hop & 0xf0ffU) == 0x402bU || (hop & 0xf0ffU) == 0x400bU ||
                    (hop & 0xf0ffU) == 0x0023U || (hop & 0xf0ffU) == 0x0003U) {
                    if (ir_try_hot_fallback_step(cpu, bus, hop)) { used = 1; IR_STAT_INC(g_ir_hits); }
                }
            }
            struct ir_cached *b = used ? NULL : irc_get(bus, cpu->pc);
            if (!used && !irq_ready && b != NULL) {
                switch (b->hot.kind) {
                case IR_HOT_BYTE_COPY: used = ir_run_hot_byte_copy_cached(cpu, bus, next_tmu, frame_end, &b->hot); break;
                case IR_HOT_STORE_FILL: used = ir_run_hot_store_fill_cached(cpu, bus, next_tmu, frame_end, &b->hot); break;
                case IR_HOT_TCOND_NOP: used = ir_run_hot_tcond_cached(cpu, bus, next_tmu, frame_end, &b->hot); break;
                case IR_HOT_DTNOP: used = ir_run_hot_dtnop_cached(cpu, bus, next_tmu, frame_end, &b->hot); break;
                case IR_HOT_BITBYTE_COPY: used = ir_run_hot_bitbyte_copy_cached(cpu, bus, next_tmu, frame_end, &b->hot); break;
                case IR_HOT_WORD_MERGE: used = ir_run_hot_word_merge_cached(cpu, bus, next_tmu, frame_end, &b->hot); break;
                default: break;
                }
                if (used) IR_STAT_INC(g_ir_hits);
            }
            if (!used && b != NULL && b->fn != NULL && b->cycles != 0U && b->cycles <= to_tmu) {
                cv1k_u32 to_frame2 = frame_end - cpu->cycles;
                g_ir_chain_limit = cpu->cycles + (((cv1k_s32)(to_tmu - to_frame2) < 0) ? to_tmu : to_frame2);
                b->fn(cpu, bus); used = 1; IR_STAT_INC(g_ir_hits);
                if (cpu->pend_mask != 0U && irc_irq_can_accept(cpu)) sh7709s_accept_pending_irq(cpu, bus);
            } else if (!used && g_ir_slice_on && b != NULL && b->fn != NULL && b->cycles != 0U && to_tmu > 0U) {
                cv1k_u32 to_frame2 = frame_end - cpu->cycles;
                cv1k_u32 lim = ((cv1k_s32)(to_tmu - to_frame2) < 0) ? to_tmu : to_frame2;
                struct ir_slice_cached *sl = ir_slice_get(bus, cpu->pc, lim);
                if (sl != NULL && sl->fn != NULL && sl->cycles != 0U && sl->cycles <= lim) {
                    sl->fn(cpu, bus); used = 1; IR_STAT_INC(g_ir_hits); IR_STAT_INC(g_ir_slice_runs);
                    if (cpu->pend_mask != 0U && irc_irq_can_accept(cpu)) sh7709s_accept_pending_irq(cpu, bus);
                }
            }
        }
        if (!used && ir_try_dt_bfs_nop_delay_slot(cpu, bus)) {
            used = 1; IR_STAT_INC(g_ir_hits);
        }
        if (!used && ir_try_hot_mdelay_step(cpu, bus)) {
            used = 1; IR_STAT_INC(g_ir_hits);
        }
        if (!used) {
            cv1k_u16 fop = cv1k_bus_fetch16(bus, cpu->pc);
            if (g_ir_fb) g_ir_fb[fop]++;
            IR_STAT_INC(g_ir_falls); if (cpu->m_delay) IR_STAT_INC(g_ir_falls_md);
            if (!ir_try_hot_fallback_step(cpu, bus, fop)) sh7709s_step(cpu, bus);
        }  /* branches / unsupported */
        if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        if (cpu->pc == cpu->idle_pc0 || cpu->pc == cpu->idle_pc1) {
            cv1k_u32 jump = ((cv1k_s32)(next_tmu - frame_end) < 0) ? next_tmu : frame_end;
            if ((cv1k_s32)(jump - cpu->cycles) > 0) cpu->cycles = jump;
            if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        }
    }
    return cpu->cycles - start;
}
