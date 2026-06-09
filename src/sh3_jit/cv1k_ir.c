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
static void add_m32r12_imm32(struct e64 *e, size_t off, uint32_t imm) { rex(e, 0, 0, 0, R12); e8(e, 0x81); mem_r12(e, 0, off); e32(e, imm); }
static void and_m32r12_imm32(struct e64 *e, size_t off, uint32_t imm) { rex(e, 0, 4, 0, R12); e8(e, 0x81); mem_r12(e, 4, off); e32(e, imm); }
static void or_m32r12_r32(struct e64 *e, size_t off, unsigned s) { rex(e, 0, s, 0, R12); e8(e, 0x09); mem_r12(e, s, off); }
/* dst = dst OP src (op r/m32, r32): 01 add, 29 sub, 21 and, 09 or, 31 xor */
static void alu_rr(struct e64 *e, uint8_t opc, unsigned d, unsigned s) { rex(e, 0, s, 0, d); e8(e, opc); modrm(e, 3, s, d); }
static void adc_rr(struct e64 *e, unsigned d, unsigned s) { rex(e, 0, s, 0, d); e8(e, 0x11); modrm(e, 3, s, d); }
static void sbb_rr(struct e64 *e, unsigned d, unsigned s) { rex(e, 0, s, 0, d); e8(e, 0x19); modrm(e, 3, s, d); }
/* dst = dst OP imm32 (81 /ext): 0 add, 1 or, 4 and, 5 sub, 6 xor */
static void alu_ri(struct e64 *e, unsigned ext, unsigned d, uint32_t imm) { rex(e, 0, ext, 0, d); e8(e, 0x81); modrm(e, 3, ext, d); e32(e, imm); }
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
static void movsx_eax_al(struct e64 *e) { e8(e, 0x0f); e8(e, 0xbe); e8(e, 0xc0); }
static void movsx_eax_ax(struct e64 *e) { e8(e, 0x0f); e8(e, 0xbf); e8(e, 0xc0); }
static void sub_rsp_imm8(struct e64 *e, uint8_t n) { e8(e, 0x48); e8(e, 0x83); e8(e, 0xec); e8(e, n); }
static void add_rsp_imm8(struct e64 *e, uint8_t n) { e8(e, 0x48); e8(e, 0x83); e8(e, 0xc4); e8(e, n); }
/* control-flow + fast-RAM helpers (fast-path inlining) */
static size_t jcc32(struct e64 *e, uint8_t cc) { e8(e, 0x0f); e8(e, (uint8_t)(0x80U | cc)); size_t p = e->size; e32(e, 0); return p; }
static size_t jmp32(struct e64 *e) { e8(e, 0xe9); size_t p = e->size; e32(e, 0); return p; }
static void patch_here(struct e64 *e, size_t at) { int32_t rel = (int32_t)(e->size - (at + 4)); if (at + 4 <= e->cap) memcpy(e->p + at, &rel, 4); }
static void patch_to(struct e64 *e, size_t at, size_t target) { int32_t rel = (int32_t)(target - (at + 4)); if (at + 4 <= e->cap) memcpy(e->p + at, &rel, 4); }
static void cmp_r32_imm32(struct e64 *e, unsigned r, uint32_t imm) { rex(e, 0, 7, 0, r); e8(e, 0x81); modrm(e, 3, 7, r); e32(e, imm); }
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

static void push_reg(struct e64 *e, unsigned r) { if (r >= 8) e8(e, 0x41); e8(e, (uint8_t)(0x50 + (r & 7U))); }
static void pop_reg(struct e64 *e, unsigned r)  { if (r >= 8) e8(e, 0x41); e8(e, (uint8_t)(0x58 + (r & 7U))); }

/* Phase-2 blocks contain no calls, so stack alignment is irrelevant; we save
 * only the callee-saved registers actually used (r12=cpu plus whichever of
 * rbx/r14/r15 the allocator assigned), keeping per-block overhead minimal. */
static void prologue(struct e64 *e, const unsigned *save, int nsave)
{
    for (int i = 0; i < nsave; i++) push_reg(e, save[i]);
    mov_r64_r64(e, R12, RDI);          /* r12 = cpu */
}
static void cmovcc_r32(struct e64 *e, uint8_t cc, unsigned d, unsigned s) { rex(e, 0, d, 0, s); e8(e, 0x0f); e8(e, cc); modrm(e, 3, d, s); }

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
        v0 = ir_newv(b); ir_push(b, IR_MOVI, v0, 0, 0, 0, addr);
        ir_push(b, IR_LOAD, v0, v0, 0, CV1K_IR_FLAG_SEXT, 0); b->in[b->n - 1].aux = 2U;
        ir_push(b, IR_GSTORE, 0, v0, 0, 0, n); return 1;
    }
    if ((op & 0xf000U) == 0xd000U) { /* MOV.L @(disp,PC),Rn (PC-relative constant) */
        cv1k_u32 addr = (pc & ~3U) + 4U + (cv1k_u32)(op & 0xffU) * 4U;
        v0 = ir_newv(b); ir_push(b, IR_MOVI, v0, 0, 0, 0, addr);
        ir_push(b, IR_LOAD, v0, v0, 0, 0, 0); b->in[b->n - 1].aux = 4U;
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
 * memory) — used by the microbench to measure the register-allocation win. */
static int g_ir_cache_on = 1;
void cv1k_ir_set_cache(int on) { g_ir_cache_on = on ? 1 : 0; }
/* Accurate builds need the bus/cache hook on every RAM access for visual parity;
 * keep helper memory accesses as the production default.  The direct path remains
 * available for isolated microbenching via cv1k_ir_set_fastram(1). */
static int g_ir_fastram_on = 0;
void cv1k_ir_set_fastram(int on) { g_ir_fastram_on = on ? 1 : 0; }
/* Internal-loop (back-branch) chaining: ON for the isolated bench/test, but OFF
 * in production (a loop block has no mid-loop IRQ/cycle checks, so an IRQ-waiting
 * spin loop would hang and a long loop would overrun the cycle budget). */
static int g_ir_internal_loops = 0;
void cv1k_ir_set_internal_loops(int on) { g_ir_internal_loops = on ? 1 : 0; }
static cv1k_u32 g_ir_last_compile_cycles = 0;

#define X_JA 0x07  /* jcc: unsigned above (>) */

static int call_clobbers_reg(unsigned r)
{
    return r == RAX || r == RCX || r == RDX || r == RSI || r == RDI ||
           r == R8  || r == R9  || r == R10 || r == R11;
}

static void emit_cache_note(struct e64 *e, unsigned areg, int write)
{
    mov_r64_r64(e, RDI, R13);
    if (areg != RSI) mov_r32_r32(e, RSI, areg);
    mov_r32_imm32(e, RDX, (cv1k_u32)(write ? 1U : 0U));
    mov_r32_imm32(e, RCX, 0U);
    call_rax(e, (const void *)cv1k_bus_cache_access);
}

/* Inline fast-RAM load: direct host access for P0 work-RAM, else the bus helper.
 * Uses R8/R9/R10 (free in memory blocks); preserves areg for the slow path. */
static void emit_load(struct e64 *e, unsigned dreg, unsigned areg, unsigned sz, int sext,
                      uint64_t ram_base, uint32_t ram_size)
{
    size_t to_slow = 0, to_done = 0; int fast = g_ir_fastram_on && !call_clobbers_reg(areg);
    if (fast) {
        emit_cache_note(e, areg, 0);
        mov_r32_r32(e, R8, areg);                   /* r8 = addr            */
        alu_ri(e, 5, R8, CV1K_ADDR_WORK_RAM);       /* r8 -= 0x0c000000 (off) */
        cmp_r32_imm32(e, R8, ram_size - sz);
        to_slow = jcc32(e, X_JA);                    /* off > size-sz -> slow */
        mov_r64_imm64(e, R9, ram_base);
        add_r64_r64(e, R9, R8);                      /* r9 = &main_ram[off]  */
        if (sz == 4)      { mov_r32_memb(e, dreg, R9); bswap_r32(e, dreg); }
        else if (sz == 2) { movzx_r32_memb16(e, dreg, R9); ror_r16_imm8(e, dreg, 8); if (sext) movsx_r32_r16(e, dreg, dreg); }
        else              { movsx_r32_memb8(e, dreg, R9); }    /* MOV.B sign-extends */
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

/* Inline fast-RAM store (byte-swapped to big-endian), else the bus helper. */
static void emit_store(struct e64 *e, unsigned areg, unsigned datareg, unsigned sz,
                       uint64_t ram_base, uint32_t ram_size)
{
    size_t to_slow = 0, to_done = 0; int fast = g_ir_fastram_on && !call_clobbers_reg(areg) && !call_clobbers_reg(datareg);
    if (fast) {
        emit_cache_note(e, areg, 1);
        mov_r32_r32(e, R8, areg);
        alu_ri(e, 5, R8, CV1K_ADDR_WORK_RAM);
        cmp_r32_imm32(e, R8, ram_size - sz);
        to_slow = jcc32(e, X_JA);
        mov_r64_imm64(e, R9, ram_base);
        add_r64_r64(e, R9, R8);
        if (sz == 4)      { mov_r32_r32(e, R10, datareg); bswap_r32(e, R10); mov_memb_r32(e, R9, R10); }
        else if (sz == 2) { mov_r32_r32(e, R10, datareg); ror_r16_imm8(e, R10, 8); mov_memb_r16(e, R9, R10); }
        else              { mov_memb_r8(e, R9, datareg); }     /* store low byte */
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
                      uint64_t ram_base, uint32_t ram_size)
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
        case IR_TSTORE: /* SR.T = imm (CLRT/SETT) — RAX is free (not a cache reg) */
            mov_r32_m32r12(e, RAX, OFF_SR);
            alu_ri(e, 4, RAX, ~1U);
            if (in->imm & 1U) alu_ri(e, 1, RAX, 1U);
            mov_m32r12_r32(e, OFF_SR, RAX);
            break;
        case IR_DECT: { /* d -= 1; SR.T = (d == 0).  Uses two scratch temps != d. */
            if (d != (unsigned)vmap[in->a]) mov_r32_r32(e, d, (unsigned)vmap[in->a]);
            int t1 = -1, t2 = -1; const unsigned cand[3] = { RAX, RCX, RDX };
            for (int ci = 0; ci < 3; ci++) { if ((unsigned)cand[ci] == d) continue; if (t1 < 0) t1 = (int)cand[ci]; else if (t2 < 0) t2 = (int)cand[ci]; }
            alu_ri(e, 5, d, 1);                                  /* sub d, 1 (sets ZF) */
            e8(e, 0x0f); e8(e, 0x94); modrm(e, 3, 0, (unsigned)t2);   /* sete t2b */
            mov_r32_m32r12(e, (unsigned)t1, OFF_SR);
            alu_ri(e, 4, (unsigned)t1, ~1U);                     /* and t1, ~T */
            e8(e, 0x0f); e8(e, 0xb6); modrm(e, 3, (unsigned)t2, (unsigned)t2); /* movzx t2, t2b */
            alu_rr(e, 0x09, (unsigned)t1, (unsigned)t2);          /* or t1, t2 */
            mov_m32r12_r32(e, OFF_SR, (unsigned)t1);
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
            emit_load(e, d, R8, in->aux, (in->flags & CV1K_IR_FLAG_SEXT) != 0, ram_base, ram_size); break;
        case IR_STOREIDX:
            mov_r32_r32(e, R8, (unsigned)vmap[in->a]);
            alu_rr(e, 0x01, R8, (unsigned)vmap[in->b]);
            emit_store(e, R8, d, in->aux, ram_base, ram_size); break;
        case IR_LOAD:  emit_load(e, d, (unsigned)vmap[in->a], in->aux, (in->flags & CV1K_IR_FLAG_SEXT) != 0, ram_base, ram_size); break;
        case IR_STORE: emit_store(e, (unsigned)vmap[in->a], (unsigned)vmap[in->b], in->aux, ram_base, ram_size); break;
        default: return 0;
        }
    }
    return 1;
}

int cv1k_ir_phase1_supported(cv1k_u16 op)
{
    struct iropbuf b; return ir_lower(op, 0, &b);
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



/* Compile a fresh block at `pc`.  On success returns ops covered and fills
 * *out_fn / *out_mem / *out_cap (caller frees via cv1k_ir_free_block). */
cv1k_u32 cv1k_ir_compile_block(struct cv1k_bus *bus, cv1k_u32 pc,
                               cv1k_ir_block_fn *out_fn, void **out_mem, size_t *out_cap)
{
    *out_fn = NULL; *out_mem = NULL; *out_cap = 0; g_ir_last_compile_cycles = 0;
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
                struct iropbuf db;
                if (!ir_lower(ds, term_bpc + 2U, &db) || db.nv > NSCRATCH) break; /* can't lower delay slot -> branch is a fallback */
                cv1k_u32 drm = 0, dwm = 0; int dorder[16]; int dnorder = 0, dhas_mem = 0;
                ir_accum_masks(&db, &drm, &dwm, dorder, &dnorder, &dhas_mem);
                if (dhas_mem) has_mem = 1;
                term_prefix_cycles = cycles;
                term_delay_cycles = cv1k_sh3_jit_linear_cycles(ds);
                term_delay = ds; term = 2; cycles += 2U + term_delay_cycles;
            } else { term_prefix_cycles = cycles; term = 1; cycles += 3U; }
            break;
        }
        if ((op & 0xf0ffU) == 0x400eU) { /* LDC Rn,SR: helper handles RB bank swap; terminal because cached regs become stale. */
            int rn = (int)((op >> 8U) & 15U), seen = 0;
            term_bpc = pc + count * 2U; term_op = op; term_prefix_cycles = cycles; term = 6; cycles += 1U; has_mem = 1;
            readmask |= 1U << rn;
            for (int j = 0; j < norder; j++) if (order[j] == rn) { seen = 1; break; }
            if (!seen && norder < 16) order[norder++] = rn;
            break;
        }
        if (op == 0x002bU) { /* RTE: compile the common standalone RTE;NOP return block only. */
            cv1k_u16 ds = cv1k_bus_fetch16(bus, pc + count * 2U + 2U);
            if (count != 0 || ds != 0x0009U) break;
            term_bpc = pc + count * 2U; term_op = op; term_delay = ds; term = 5; cycles += 2U + cv1k_sh3_jit_linear_cycles(ds);
            break;
        }
        if ((op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U ||
            (op & 0xf0ffU) == 0x0003U || (op & 0xf0ffU) == 0x0023U ||
            (op & 0xf0ffU) == 0x400bU || (op & 0xf0ffU) == 0x402bU || op == 0x000bU) {
            cv1k_u16 ds = cv1k_bus_fetch16(bus, pc + count * 2U + 2U);
            struct iropbuf db;
            if (!ir_lower(ds, pc + count * 2U + 2U, &db) || db.nv > NSCRATCH) break;
            ir_accum_masks(&db, &readmask, &writemask, order, &norder, &has_mem);
            term_bpc = pc + count * 2U; term_op = op; term_delay = ds;
            term = ((op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U) ? 3 : 4;
            cycles += (((op & 0xf0ffU) == 0x402bU) ? 1U : 2U) + cv1k_sh3_jit_linear_cycles(ds);
            break;
        }
        struct iropbuf b;
        if (!ir_lower(op, pc + count * 2U, &b)) break;
        if (b.nv > NSCRATCH) break;
        ops[count] = op;
        cycles += cv1k_sh3_jit_linear_cycles(op);
        ir_accum_masks(&b, &readmask, &writemask, order, &norder, &has_mem);
    }
    if (count == 0 && term == 0) return 0;   /* nothing lowerable here */

    /* ---- allocate: cache guest regs in host regs (first-use order) ----
     * Memory-op blocks contain helper calls, so only callee-saved registers may
     * hold guest regs across the block; ALU-only blocks may use all of them. */
    static const int MEMPOOL[] = { RBX, R14, R15 };
    const int *pool = has_mem ? MEMPOOL : GPOOL;
    int np = has_mem ? (int)(sizeof MEMPOOL / sizeof MEMPOOL[0]) : NG;
    int hreg[16]; for (int g = 0; g < 16; g++) hreg[g] = -1;
    if (g_ir_cache_on)
        for (int i = 0; i < norder && i < np; i++) hreg[order[i]] = pool[i];

    size_t cap = 16384U;
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    cap = (cap + pagesz - 1U) & ~(pagesz - 1U);
    void *mem = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) return 0;
    struct e64 e = { .p = (uint8_t *)mem, .size = 0, .cap = cap, .fail = 0 };

    /* save list: r12 (cpu) always; r13 (bus) for memory blocks; + callee-saved
     * cache regs used.  A call needs rsp 16-aligned, so pad if nsave is even. */
    unsigned save[6]; int nsave = 0; save[nsave++] = R12;
    if (has_mem) save[nsave++] = R13;
    { int rbx = 0, r14 = 0, r15 = 0;
      for (int g = 0; g < 16; g++) { if (hreg[g] == RBX) rbx = 1; else if (hreg[g] == R14) r14 = 1; else if (hreg[g] == R15) r15 = 1; }
      if (rbx) save[nsave++] = RBX;
      if (r14) save[nsave++] = R14;
      if (r15) save[nsave++] = R15; }
    int pad = (has_mem && (nsave % 2 == 0)) ? 8 : 0;

    prologue(&e, save, nsave);
    if (has_mem) mov_r64_r64(&e, R13, RSI);  /* r13 = bus (for load/store helpers) */
    if (pad) sub_rsp_imm8(&e, (uint8_t)pad);
    /* entry: load cached guest regs that are read into their host regs */
    for (int g = 0; g < 16; g++)
        if (hreg[g] >= 0 && (readmask & (1U << g))) mov_r32_m32r12(&e, (unsigned)hreg[g], OFF_R(g));
    /* body.  label[k] = code offset of guest op k, for internal back-branches. */
    size_t label[CV1K_IR_MAX_INSTS];
    for (cv1k_u32 k = 0; k < count; k++) {
        label[k] = e.size;
        if (brtarget[k] >= 0) {
            /* internal conditional branch (loop): test SR.T, jump back to the
             * target op's code.  Registers stay resident across iterations. */
            int is_bt = (ops[k] & 0xff00U) == 0x8900U;
            mov_r32_m32r12(&e, RAX, OFF_SR);
            e8(&e, 0xa8); e8(&e, 0x01);                       /* test al, 1 (T bit) */
            e8(&e, 0x0f); e8(&e, (uint8_t)(is_bt ? 0x85 : 0x84)); /* jnz / jz rel32 */
            int32_t rel = (int32_t)((intptr_t)label[brtarget[k]] - (intptr_t)(e.size + 4));
            e32(&e, (uint32_t)rel);
            continue;
        }
        struct iropbuf b;
        ir_lower(ops[k], pc + k * 2U, &b);
        mov_m32r12_imm32(&e, OFF_PPC, pc + k * 2U);
        if (!ir_emit_op(&e, &b, hreg, ram_base, ram_size)) { e.fail = 1; break; }
    }
    if (term == 1 && !e.fail) {
        cv1k_u32 taken = term_bpc + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(term_op & 0xffU) * 2);
        cv1k_u32 fall = term_bpc + 2U;
        unsigned hib = term_op & 0xff00U;
        int is_t = (hib == 0x8900U);
        mov_r32_m32r12(&e, RAX, OFF_SR);
        e8(&e, 0xa8); e8(&e, 0x01);
        size_t j_taken = jcc32(&e, (uint8_t)(is_t ? 0x85 : 0x84));
        for (int g = 0; g < 16; g++)
            if (hreg[g] >= 0 && (writemask & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
        mov_m32r12_imm32(&e, OFF_PC, fall);
        epilogue_tail(&e, save, nsave, pad, 0, 0, term_prefix_cycles + 1U);

        size_t lab_taken = e.size;
        patch_to(&e, j_taken, lab_taken);
        for (int g = 0; g < 16; g++)
            if (hreg[g] >= 0 && (writemask & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
        mov_m32r12_imm32(&e, OFF_EA, taken);
        mov_m32r12_imm32(&e, OFF_PC, taken);
        epilogue_tail(&e, save, nsave, pad, 0, 0, term_prefix_cycles + 3U);
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
        mov_m32r12_imm32(&e, OFF_PC, fall);
        epilogue_tail(&e, save, nsave, pad, 0, 0, term_prefix_cycles + 1U);

        size_t lab_taken = e.size;
        patch_to(&e, j_taken, lab_taken);
        for (int g = 0; g < 16; g++)
            if (hreg[g] >= 0 && (writemask & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
        mov_m32r12_imm32(&e, OFF_EA, taken);
        struct iropbuf db; ir_lower(term_delay, term_bpc + 2U, &db);
        int no_hreg[16]; for (int g = 0; g < 16; g++) no_hreg[g] = -1;
        mov_m32r12_imm32(&e, OFF_PPC, term_bpc + 2U);
        if (!ir_emit_op(&e, &db, no_hreg, ram_base, ram_size)) e.fail = 1;
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
        struct iropbuf db; ir_lower(term_delay, term_bpc + 2U, &db);
        mov_m32r12_imm32(&e, OFF_PPC, term_bpc + 2U);
        if (!ir_emit_op(&e, &db, hreg, ram_base, ram_size)) e.fail = 1;
    }
    /* exit: store back cached guest regs that were written */
    for (int g = 0; g < 16; g++)
        if (hreg[g] >= 0 && (writemask & (1U << g))) mov_m32r12_r32(&e, OFF_R(g), (unsigned)hreg[g]);
    if (e.fail) { munmap(mem, cap); return 0; }
    if (term == 1) {
        /* non-delayed conditional terminator: cpu->pc = (cond) ? taken : fall. */
        cv1k_u32 taken = term_bpc + 4U + (cv1k_u32)((cv1k_s32)(int8_t)(term_op & 0xffU) * 2);
        cv1k_u32 fall = term_bpc + 2U;
        unsigned hib = term_op & 0xff00U;
        int is_t = (hib == 0x8900U);
        mov_r32_m32r12(&e, RAX, OFF_SR); alu_ri(&e, 4, RAX, 1U);   /* RAX = T (ZF=!T) */
        mov_r32_imm32(&e, RCX, fall); mov_r32_imm32(&e, RDX, taken);
        cmovcc_r32(&e, is_t ? 0x45 : 0x44, RCX, RDX);             /* cmovne/cmove */
        mov_m32r12_r32(&e, OFF_PC, RCX);
        epilogue_tail(&e, save, nsave, pad, 0, 0, cycles);
    } else if (term == 2) {
        mov_r32_m32r12(&e, RAX, OFF_EA);
        mov_m32r12_r32(&e, OFF_PC, RAX);
        epilogue_tail(&e, save, nsave, pad, 0, 0, cycles);
    } else if (term == 3) {
        cv1k_u32 target = term_bpc + 4U + ((cv1k_u32)sext12(term_op) * 2U);
        mov_m32r12_imm32(&e, OFF_PC, target);
        epilogue_tail(&e, save, nsave, pad, 0, 0, cycles);
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
        epilogue_tail(&e, save, nsave, pad, 1, pc + count * 2U, cycles);
    }
    if (e.fail) { munmap(mem, cap); return 0; }
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
#if defined(__unix__) || defined(__APPLE__)
    if (mem) munmap(mem, cap);
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
struct ir_cached { cv1k_u32 pc; cv1k_u32 cycles; cv1k_ir_block_fn fn; void *mem; size_t cap; struct ir_cached *next; };
static struct ir_cached *g_irc[IRC_BUCKETS];
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
    if (getenv("CV1K_IR_FB")) { g_ir_fb = (unsigned long long *)calloc(65536, sizeof(unsigned long long)); atexit(ir_fb_dump); }
}

static cv1k_u32 irc_hash(cv1k_u32 pc) { return (pc ^ (pc >> 4) ^ (pc >> 13)) & (IRC_BUCKETS - 1U); }

void cv1k_ir_reset(void)
{
    for (cv1k_u32 i = 0; i < IRC_BUCKETS; i++) {
        struct ir_cached *c = g_irc[i];
        while (c) { struct ir_cached *n = c->next; if (c->mem) cv1k_ir_free_block(c->mem, c->cap); free(c); c = n; }
        g_irc[i] = NULL;
    }
}

/* Look up (or compile+cache) the IR block at pc.  Returns its fn, or NULL when
 * the op at pc is not IR-lowerable (caller uses the interpreter).  Negative
 * results are cached (fn == NULL) so we don't recompile non-IR PCs each time. */
static struct ir_cached *irc_get(struct cv1k_bus *bus, cv1k_u32 pc)
{
    cv1k_u32 h = irc_hash(pc);
    for (struct ir_cached *c = g_irc[h]; c; c = c->next) if (c->pc == pc) return c;
    cv1k_ir_block_fn fn = NULL; void *mem = NULL; size_t cap = 0;
    cv1k_ir_compile_block(bus, pc, &fn, &mem, &cap);
    struct ir_cached *c = (struct ir_cached *)calloc(1, sizeof(*c));
    if (c == NULL) { if (mem) cv1k_ir_free_block(mem, cap); return NULL; }
    c->pc = pc; c->cycles = fn ? g_ir_last_compile_cycles : 0U; c->fn = fn; c->mem = mem; c->cap = cap; c->next = g_irc[h]; g_irc[h] = c;
    return c;
}

#define IR_SH_I  0x000000f0U
#define IR_SH_BL 0x10000000U
static int irc_irq_can_accept(const struct sh7709s_cpu *cpu)
{
    if (cpu->m_delay != 0U || cpu->pend_mask == 0U || (cpu->sr & IR_SH_BL) != 0U) return 0;
    cv1k_u32 mask = (cpu->sr & IR_SH_I) >> 4, pm = cpu->pend_mask;
    while (pm != 0U) { int i = __builtin_ctz(pm); if ((cpu->pend_pri[i] & 0x0fU) > mask) return 1; pm &= pm - 1U; }
    return 0;
}

cv1k_u32 cv1k_irjit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                              cv1k_u32 cycle_budget, cv1k_u32 tmu_interval)
{
    if (tmu_interval == 0U) tmu_interval = 2048U;
    ir_fb_maybe_init();
    cv1k_u32 start = cpu->cycles, frame_end = start + cycle_budget;
    cv1k_u32 next_tmu = cpu->cycles + tmu_interval;
    while ((cv1k_s32)(cpu->cycles - frame_end) < 0) {
        if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        if (irc_irq_can_accept(cpu)) sh7709s_accept_pending_irq(cpu, bus);
        int used = 0;
        if (cpu->sleep_mode == 0U && cpu->m_delay == 0U && cpu->halted == 0U) {
            struct ir_cached *b = irc_get(bus, cpu->pc);
            const cv1k_u32 to_tmu = next_tmu - cpu->cycles;
            if (b != NULL && b->fn != NULL && b->cycles != 0U && b->cycles < to_tmu) {
                b->fn(cpu, bus); used = 1; g_ir_hits++;
                if (irc_irq_can_accept(cpu)) sh7709s_accept_pending_irq(cpu, bus);
            }
        }
        if (!used) { if (g_ir_fb) g_ir_fb[cv1k_bus_fetch16(bus, cpu->pc)]++; g_ir_falls++; if (cpu->m_delay) g_ir_falls_md++; sh7709s_step(cpu, bus); }  /* branches / unsupported */
        if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        if (cpu->pc == cpu->idle_pc0 || cpu->pc == cpu->idle_pc1) {
            cv1k_u32 jump = ((cv1k_s32)(next_tmu - frame_end) < 0) ? next_tmu : frame_end;
            if ((cv1k_s32)(jump - cpu->cycles) > 0) cpu->cycles = jump;
            if ((cv1k_s32)(cpu->cycles - next_tmu) >= 0) { cv1k_bus_tmu_tick(bus); next_tmu = cpu->cycles + tmu_interval; }
        }
    }
    return cpu->cycles - start;
}
