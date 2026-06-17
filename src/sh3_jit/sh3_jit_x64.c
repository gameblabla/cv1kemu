/* C23 x64 backend for the SH-3 JIT.  This is a deliberately small byte
 * emitter with no C++ runtime dependency. */

#include "sh3_jit/sh3_jit_backend.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#define SH_T   0x00000001U
#define OFF_R(n) (offsetof(struct sh7709s_cpu, r) + (size_t)(n) * sizeof(cv1k_u32))
#define OFF_PC offsetof(struct sh7709s_cpu, pc)
#define OFF_PR offsetof(struct sh7709s_cpu, pr)
#define OFF_SR offsetof(struct sh7709s_cpu, sr)
#define OFF_GBR offsetof(struct sh7709s_cpu, gbr)
#define OFF_VBR offsetof(struct sh7709s_cpu, vbr)
#define OFF_MACH offsetof(struct sh7709s_cpu, mach)
#define OFF_MACL offsetof(struct sh7709s_cpu, macl)
#define OFF_EA offsetof(struct sh7709s_cpu, ea)
#define OFF_PPC offsetof(struct sh7709s_cpu, ppc)
#define OFF_M_DELAY offsetof(struct sh7709s_cpu, m_delay)
#define OFF_CYCLES offsetof(struct sh7709s_cpu, cycles)

static cv1k_s32 sext8(cv1k_u32 v) { return (cv1k_s32)(int8_t)(v & 0xffU); }
static cv1k_s32 sext12(cv1k_u32 v) { v &= 0xfffU; return (cv1k_s32)((v ^ 0x800U) - 0x800U); }

enum xr { RAX=0, RCX=1, RDX=2, RBX=3, RSP=4, RBP=5, RSI=6, RDI=7, R8=8, R9=9, R10=10, R11=11, R12=12, R13=13 };

struct x64e {
    uint8_t *p;
    size_t size;
    size_t cap;
    int fail;
};

static void *jit_alloc(size_t cap)
{
#if (defined(__x86_64__) || defined(_M_X64)) && defined(_WIN32)
    return VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#elif (defined(__x86_64__) || defined(_M_X64)) && (defined(__unix__) || defined(__APPLE__))
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    cap = (cap + pagesz - 1U) & ~(pagesz - 1U);
    void *p = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#else
    (void)cap;
    return NULL;
#endif
}

static void jit_free(void *p, size_t cap)
{
#if (defined(__x86_64__) || defined(_M_X64)) && defined(_WIN32)
    (void)cap;
    if (p != NULL) VirtualFree(p, 0, MEM_RELEASE);
#elif (defined(__x86_64__) || defined(_M_X64)) && (defined(__unix__) || defined(__APPLE__))
    if (p != NULL && cap != 0U) {
        size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
        cap = (cap + pagesz - 1U) & ~(pagesz - 1U);
        munmap(p, cap);
    }
#else
    (void)p; (void)cap;
#endif
}

static void flush_icache(void *p, size_t n)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)p, (char *)p + n);
#else
    (void)p; (void)n;
#endif
}

static void e8(struct x64e *e, uint8_t v) { if (e->size < e->cap) e->p[e->size++] = v; else e->fail = 1; }
static void e32(struct x64e *e, uint32_t v) { for (unsigned i = 0; i < 4; i++) e8(e, (uint8_t)(v >> (i * 8))); }
static void e64(struct x64e *e, uint64_t v) { for (unsigned i = 0; i < 8; i++) e8(e, (uint8_t)(v >> (i * 8))); }
static void rex(struct x64e *e, int w, unsigned r, unsigned x, unsigned b)
{
    uint8_t p = (uint8_t)(0x40U | (w ? 8U : 0U) | ((r >> 3U) ? 4U : 0U) | ((x >> 3U) ? 2U : 0U) | ((b >> 3U) ? 1U : 0U));
    if (p != 0x40U) e8(e, p);
}
static void modrm(struct x64e *e, unsigned mod, unsigned reg, unsigned rm) { e8(e, (uint8_t)((mod << 6) | ((reg & 7U) << 3) | (rm & 7U))); }
static void sib(struct x64e *e, unsigned scale, unsigned index, unsigned base) { e8(e, (uint8_t)((scale << 6) | ((index & 7U) << 3) | (base & 7U))); }

static void mem_r12(struct x64e *e, unsigned reg, size_t off)
{
    modrm(e, 2, reg, 4); sib(e, 0, 4, 4); e32(e, (uint32_t)off);
}

static void mov_r64_r64(struct x64e *e, unsigned dst, unsigned src)
{ rex(e, 1, src, 0, dst); e8(e, 0x89); modrm(e, 3, src, dst); }
static void mov_r64_imm64(struct x64e *e, unsigned dst, uint64_t imm)
{ rex(e, 1, 0, 0, dst); e8(e, (uint8_t)(0xb8U + (dst & 7U))); e64(e, imm); }
static void mov_r32_r32(struct x64e *e, unsigned dst, unsigned src)
{ rex(e, 0, src, 0, dst); e8(e, 0x89); modrm(e, 3, src, dst); }
static void mov_r32_imm32(struct x64e *e, unsigned dst, uint32_t imm)
{ rex(e, 0, 0, 0, dst); e8(e, (uint8_t)(0xb8U + (dst & 7U))); e32(e, imm); }
static void mov_r32_m32(struct x64e *e, unsigned dst, size_t off)
{ rex(e, 0, dst, 0, R12); e8(e, 0x8b); mem_r12(e, dst, off); }
static void mov_m32_r32(struct x64e *e, size_t off, unsigned src)
{ rex(e, 0, src, 0, R12); e8(e, 0x89); mem_r12(e, src, off); }
static void mov_m32_imm32(struct x64e *e, size_t off, uint32_t imm)
{ rex(e, 0, 0, 0, R12); e8(e, 0xc7); mem_r12(e, 0, off); e32(e, imm); }
static void bin_m32_imm32(struct x64e *e, unsigned ext, size_t off, uint32_t imm)
{ rex(e, 0, ext, 0, R12); e8(e, 0x81); mem_r12(e, ext, off); e32(e, imm); }
static void add_m32_imm32(struct x64e *e, size_t off, uint32_t imm) { bin_m32_imm32(e, 0, off, imm); }
static void or_m32_imm32(struct x64e *e, size_t off, uint32_t imm) { bin_m32_imm32(e, 1, off, imm); }
static void and_m32_imm32(struct x64e *e, size_t off, uint32_t imm) { bin_m32_imm32(e, 4, off, imm); }
static void sub_m32_imm32(struct x64e *e, size_t off, uint32_t imm) { bin_m32_imm32(e, 5, off, imm); }
static void xor_m32_imm32(struct x64e *e, size_t off, uint32_t imm) { bin_m32_imm32(e, 6, off, imm); }
static void cmp_m32_imm32(struct x64e *e, size_t off, uint32_t imm) { bin_m32_imm32(e, 7, off, imm); }
static void bin_m32_r32(struct x64e *e, uint8_t opc, size_t off, unsigned src)
{ rex(e, 0, src, 0, R12); e8(e, opc); mem_r12(e, src, off); }
static void add_m32_r32(struct x64e *e, size_t off, unsigned src) { bin_m32_r32(e, 0x01, off, src); }
static void or_m32_r32(struct x64e *e, size_t off, unsigned src) { bin_m32_r32(e, 0x09, off, src); }
static void and_m32_r32(struct x64e *e, size_t off, unsigned src) { bin_m32_r32(e, 0x21, off, src); }
static void sub_m32_r32(struct x64e *e, size_t off, unsigned src) { bin_m32_r32(e, 0x29, off, src); }
static void xor_m32_r32(struct x64e *e, size_t off, unsigned src) { bin_m32_r32(e, 0x31, off, src); }
static void movsx_r32_m8(struct x64e *e, unsigned dst, size_t off) { rex(e,0,dst,0,R12); e8(e,0x0f); e8(e,0xbe); mem_r12(e,dst,off); }
static void movsx_r32_m16(struct x64e *e, unsigned dst, size_t off) { rex(e,0,dst,0,R12); e8(e,0x0f); e8(e,0xbf); mem_r12(e,dst,off); }
static void movzx_r32_m8(struct x64e *e, unsigned dst, size_t off) { rex(e,0,dst,0,R12); e8(e,0x0f); e8(e,0xb6); mem_r12(e,dst,off); }
static void movzx_r32_m16(struct x64e *e, unsigned dst, size_t off) { rex(e,0,dst,0,R12); e8(e,0x0f); e8(e,0xb7); mem_r12(e,dst,off); }
static void movsx_eax_al(struct x64e *e) { e8(e,0x0f); e8(e,0xbe); e8(e,0xc0); }
static void movsx_eax_ax(struct x64e *e) { e8(e,0x0f); e8(e,0xbf); e8(e,0xc0); }
static void movzx_eax_al(struct x64e *e) { e8(e,0x0f); e8(e,0xb6); e8(e,0xc0); }
static void cmp_r32_m32(struct x64e *e, unsigned r, size_t off) { rex(e,0,r,0,R12); e8(e,0x3b); mem_r12(e,r,off); }
static void test_r32_r32(struct x64e *e, unsigned a, unsigned b) { rex(e,0,b,0,a); e8(e,0x85); modrm(e,3,b,a); }
static void test_eax_imm32(struct x64e *e, uint32_t imm) { e8(e,0xa9); e32(e,imm); }
static void setcc_al(struct x64e *e, uint8_t cc) { e8(e,0x0f); e8(e,cc); e8(e,0xc0); }
static void sh_m32_imm8(struct x64e *e, unsigned ext, size_t off, uint8_t imm) { rex(e,0,ext,0,R12); e8(e,0xc1); mem_r12(e,ext,off); e8(e,imm); }
static void sh_r32_cl(struct x64e *e, unsigned ext, unsigned reg) { rex(e,0,ext,0,reg); e8(e,0xd3); modrm(e,3,ext,reg); }
static void and_r32_imm32(struct x64e *e, unsigned r, uint32_t imm) { rex(e,0,4,0,r); e8(e,0x81); modrm(e,3,4,r); e32(e,imm); }
static void add_r32_imm32(struct x64e *e, unsigned r, uint32_t imm) { rex(e,0,0,0,r); e8(e,0x81); modrm(e,3,0,r); e32(e,imm); }
static void sub_r32_r32(struct x64e *e, unsigned dst, unsigned src) { rex(e,0,src,0,dst); e8(e,0x29); modrm(e,3,src,dst); }
static void or_r32_r32(struct x64e *e, unsigned dst, unsigned src) { rex(e,0,src,0,dst); e8(e,0x09); modrm(e,3,src,dst); }
static void add_r32_m32(struct x64e *e, unsigned dst, size_t off) { rex(e,0,dst,0,R12); e8(e,0x03); mem_r12(e,dst,off); }
static void not_r32(struct x64e *e, unsigned r) { rex(e,0,2,0,r); e8(e,0xf7); modrm(e,3,2,r); }
static void neg_r32(struct x64e *e, unsigned r) { rex(e,0,3,0,r); e8(e,0xf7); modrm(e,3,3,r); }
static void sar_r32_imm8(struct x64e *e, unsigned r, uint8_t imm) { rex(e,0,7,0,r); e8(e,0xc1); modrm(e,3,7,r); e8(e,imm); }

static size_t jcc32(struct x64e *e, uint8_t cc) { e8(e,0x0f); e8(e,(uint8_t)(0x80U | cc)); size_t p=e->size; e32(e,0); return p; }
static size_t jmp32(struct x64e *e) { e8(e,0xe9); size_t p=e->size; e32(e,0); return p; }
static void patch32(struct x64e *e, size_t at, size_t target)
{
    int32_t rel = (int32_t)((intptr_t)target - (intptr_t)(at + 4U));
    memcpy(e->p + at, &rel, 4);
}
static void call_abs(struct x64e *e, const void *fn) { mov_r64_imm64(e, RAX, (uintptr_t)fn); e8(e,0xff); e8(e,0xd0); }

static void prologue(struct x64e *e)
{
    e8(e,0x41); e8(e,0x54); /* push r12 */
    e8(e,0x41); e8(e,0x55); /* push r13 */
    mov_r64_r64(e, R12, RDI);
    mov_r64_r64(e, R13, RSI);
    e8(e,0x48); e8(e,0x83); e8(e,0xec); e8(e,0x08);
}

static void return_common(struct x64e *e)
{
    e8(e,0x48); e8(e,0x83); e8(e,0xc4); e8(e,0x08);
    e8(e,0x41); e8(e,0x5d);
    e8(e,0x41); e8(e,0x5c);
    e8(e,0xc3);
}

static void epilogue(struct x64e *e, cv1k_u32 next_pc, cv1k_u32 cycles)
{
    mov_m32_imm32(e, OFF_PC, next_pc);
    add_m32_imm32(e, OFF_CYCLES, cycles);
    mov_r32_imm32(e, RAX, cycles);
    return_common(e);
}

static void store_t_from_al(struct x64e *e)
{
    movzx_eax_al(e);
    mov_r32_m32(e, RCX, OFF_SR);
    and_r32_imm32(e, RCX, ~SH_T);
    or_r32_r32(e, RCX, RAX);
    mov_m32_r32(e, OFF_SR, RCX);
}

static void set_args_bus_addr_reg(struct x64e *e, unsigned addr_reg)
{
    mov_r64_r64(e, RDI, R13);
    mov_r32_r32(e, RSI, addr_reg);
}

static void set_args_bus_addr_imm(struct x64e *e, uint32_t addr)
{
    mov_r64_r64(e, RDI, R13);
    mov_r32_imm32(e, RSI, addr);
}

static void read_mem_to_reg(struct x64e *e, unsigned size, unsigned addr_reg, unsigned dst, int sign_extend)
{
    set_args_bus_addr_reg(e, addr_reg);
    if (size == 1) call_abs(e, (const void *)cv1k_sh3_jit_read8);
    else if (size == 2) call_abs(e, (const void *)cv1k_sh3_jit_read16);
    else call_abs(e, (const void *)cv1k_sh3_jit_read32);
    if (size == 1 && sign_extend) movsx_eax_al(e);
    else if (size == 2 && sign_extend) movsx_eax_ax(e);
    mov_m32_r32(e, OFF_R(dst), RAX);
}

static void write_mem_from_reg(struct x64e *e, unsigned size, unsigned addr_reg, unsigned src)
{
    mov_r32_r32(e, R8, addr_reg);
    mov_r32_m32(e, RDX, OFF_R(src));
    mov_r64_r64(e, RDI, R13);
    mov_r32_r32(e, RSI, R8);
    if (size == 1) call_abs(e, (const void *)cv1k_sh3_jit_write8);
    else if (size == 2) call_abs(e, (const void *)cv1k_sh3_jit_write16);
    else call_abs(e, (const void *)cv1k_sh3_jit_write32);
}

static void load_addr_reg_disp(struct x64e *e, unsigned base_reg, cv1k_u32 disp, cv1k_u32 scale, unsigned out)
{
    mov_r32_m32(e, out, OFF_R(base_reg));
    if (disp != 0U) add_r32_imm32(e, out, disp * scale);
    mov_m32_r32(e, OFF_EA, out);
}

static void emit_one(struct x64e *e, cv1k_u16 op, cv1k_u32 pc)
{
    unsigned n = (op >> 8) & 15U;
    unsigned m = (op >> 4) & 15U;
    if (op == 0x0009U) return; /* NOP */
    /* Focused C-helper calls (exact interpreter semantics) for ops we do not
     * emit inline; keeps blocks containing them fully compiled. */
    if ((op & 0xf0ffU) == 0x4024U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, n); call_abs(e, (const void *)cv1k_sh3_jit_rotcl); return; } /* ROTCL Rn */
    if ((op & 0xf0ffU) == 0x4025U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, n); call_abs(e, (const void *)cv1k_sh3_jit_rotcr); return; } /* ROTCR Rn */
    if (op == 0x0019U)             { mov_r64_r64(e, RDI, R12); call_abs(e, (const void *)cv1k_sh3_jit_div0u); return; } /* DIV0U */
    if ((op & 0xf00fU) == 0x2007U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, m); mov_r32_imm32(e, RDX, n); call_abs(e, (const void *)cv1k_sh3_jit_div0s); return; } /* DIV0S Rm,Rn */
    if ((op & 0xf00fU) == 0x3004U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, m); mov_r32_imm32(e, RDX, n); call_abs(e, (const void *)cv1k_sh3_jit_div1); return; } /* DIV1 Rm,Rn */
    if ((op & 0xf00fU) == 0x0007U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, m); mov_r32_imm32(e, RDX, n); call_abs(e, (const void *)cv1k_sh3_jit_mull);  return; } /* MUL.L Rm,Rn */
    if ((op & 0xf00fU) == 0x6008U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, m); mov_r32_imm32(e, RDX, n); call_abs(e, (const void *)cv1k_sh3_jit_swapb); return; } /* SWAP.B Rm,Rn */
    if ((op & 0xf00fU) == 0x6009U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, m); mov_r32_imm32(e, RDX, n); call_abs(e, (const void *)cv1k_sh3_jit_swapw); return; } /* SWAP.W Rm,Rn */
    if ((op & 0xf00fU) == 0x600aU) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, m); mov_r32_imm32(e, RDX, n); call_abs(e, (const void *)cv1k_sh3_jit_negc);  return; } /* NEGC Rm,Rn */
    if ((op & 0xf0ffU) == 0x4004U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, n); call_abs(e, (const void *)cv1k_sh3_jit_rotl);  return; } /* ROTL Rn */
    if ((op & 0xf0ffU) == 0x4005U) { mov_r64_r64(e, RDI, R12); mov_r32_imm32(e, RSI, n); call_abs(e, (const void *)cv1k_sh3_jit_rotr);  return; } /* ROTR Rn */
    if ((op & 0xf000U) == 0xe000U) { mov_m32_imm32(e, OFF_R(n), (uint32_t)sext8(op)); return; }
    if ((op & 0xf000U) == 0x7000U) { add_m32_imm32(e, OFF_R(n), (uint32_t)sext8(op)); return; }
    if ((op & 0xf00fU) == 0x6003U) { mov_r32_m32(e, RAX, OFF_R(m)); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x300cU) { mov_r32_m32(e, RAX, OFF_R(m)); add_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x3008U) { mov_r32_m32(e, RAX, OFF_R(m)); sub_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x2008U) { mov_r32_m32(e, RAX, OFF_R(n)); mov_r32_m32(e, RCX, OFF_R(m)); test_r32_r32(e, RAX, RCX); setcc_al(e, 0x94); store_t_from_al(e); return; }
    if ((op & 0xf00fU) == 0x2009U) { mov_r32_m32(e, RAX, OFF_R(m)); and_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x200bU) { mov_r32_m32(e, RAX, OFF_R(m)); or_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x200aU) { mov_r32_m32(e, RAX, OFF_R(m)); xor_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x6007U) { mov_r32_m32(e, RAX, OFF_R(m)); not_r32(e, RAX); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x600bU) { mov_r32_m32(e, RAX, OFF_R(m)); neg_r32(e, RAX); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x600cU) { movzx_r32_m8(e, RAX, OFF_R(m)); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x600dU) { movzx_r32_m16(e, RAX, OFF_R(m)); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x600eU) { movsx_r32_m8(e, RAX, OFF_R(m)); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x600fU) { movsx_r32_m16(e, RAX, OFF_R(m)); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf00fU) == 0x3000U) { mov_r32_m32(e, RAX, OFF_R(n)); cmp_r32_m32(e, RAX, OFF_R(m)); setcc_al(e, 0x94); store_t_from_al(e); return; }
    if ((op & 0xf00fU) == 0x3002U) { mov_r32_m32(e, RAX, OFF_R(n)); cmp_r32_m32(e, RAX, OFF_R(m)); setcc_al(e, 0x93); store_t_from_al(e); return; }
    if ((op & 0xf00fU) == 0x3003U) { mov_r32_m32(e, RAX, OFF_R(n)); cmp_r32_m32(e, RAX, OFF_R(m)); setcc_al(e, 0x9d); store_t_from_al(e); return; }
    if ((op & 0xf00fU) == 0x3006U) { mov_r32_m32(e, RAX, OFF_R(n)); cmp_r32_m32(e, RAX, OFF_R(m)); setcc_al(e, 0x97); store_t_from_al(e); return; }
    if ((op & 0xf00fU) == 0x3007U) { mov_r32_m32(e, RAX, OFF_R(n)); cmp_r32_m32(e, RAX, OFF_R(m)); setcc_al(e, 0x9f); store_t_from_al(e); return; }
    if ((op & 0xff00U) == 0x8800U) { cmp_m32_imm32(e, OFF_R(0), (uint32_t)sext8(op)); setcc_al(e, 0x94); store_t_from_al(e); return; }
    if ((op & 0xff00U) == 0xc900U) { and_m32_imm32(e, OFF_R(0), (uint32_t)(op & 0xffU)); return; }
    if ((op & 0xff00U) == 0xcb00U) { or_m32_imm32(e, OFF_R(0), (uint32_t)(op & 0xffU)); return; }
    if ((op & 0xff00U) == 0xca00U) { xor_m32_imm32(e, OFF_R(0), (uint32_t)(op & 0xffU)); return; }
    if ((op & 0xff00U) == 0xc800U) { mov_r32_m32(e, RAX, OFF_R(0)); test_eax_imm32(e, (uint32_t)(op & 0xffU)); setcc_al(e, 0x94); store_t_from_al(e); return; }
    if ((op & 0xff00U) == 0xc000U) { mov_r32_m32(e, RCX, OFF_GBR); add_r32_imm32(e, RCX, op & 0xffU); mov_m32_r32(e, OFF_EA, RCX); write_mem_from_reg(e, 1, RCX, 0); return; }
    if ((op & 0xff00U) == 0xc100U) { mov_r32_m32(e, RCX, OFF_GBR); add_r32_imm32(e, RCX, (op & 0xffU) * 2U); mov_m32_r32(e, OFF_EA, RCX); write_mem_from_reg(e, 2, RCX, 0); return; }
    if ((op & 0xff00U) == 0xc200U) { mov_r32_m32(e, RCX, OFF_GBR); add_r32_imm32(e, RCX, (op & 0xffU) * 4U); mov_m32_r32(e, OFF_EA, RCX); write_mem_from_reg(e, 4, RCX, 0); return; }
    if ((op & 0xff00U) == 0xc400U) { mov_r32_m32(e, RCX, OFF_GBR); add_r32_imm32(e, RCX, op & 0xffU); mov_m32_r32(e, OFF_EA, RCX); read_mem_to_reg(e, 1, RCX, 0, 1); return; }
    if ((op & 0xff00U) == 0xc500U) { mov_r32_m32(e, RCX, OFF_GBR); add_r32_imm32(e, RCX, (op & 0xffU) * 2U); mov_m32_r32(e, OFF_EA, RCX); read_mem_to_reg(e, 2, RCX, 0, 1); return; }
    if ((op & 0xff00U) == 0xc600U) { mov_r32_m32(e, RCX, OFF_GBR); add_r32_imm32(e, RCX, (op & 0xffU) * 4U); mov_m32_r32(e, OFF_EA, RCX); read_mem_to_reg(e, 4, RCX, 0, 0); return; }
    if ((op & 0xf0ffU) == 0x0029U) { mov_r32_m32(e, RAX, OFF_SR); and_r32_imm32(e, RAX, SH_T); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if (op == 0x0008U) { and_m32_imm32(e, OFF_SR, ~SH_T); return; }
    if (op == 0x0018U) { or_m32_imm32(e, OFF_SR, SH_T); return; }
    if ((op & 0xf0ffU) == 0x4010U) { sub_m32_imm32(e, OFF_R(n), 1); setcc_al(e, 0x94); store_t_from_al(e); return; }
    if ((op & 0xf0ffU) == 0x4000U || (op & 0xf0ffU) == 0x4020U) { mov_r32_m32(e, RAX, OFF_R(n)); e8(e,0xc1); e8(e,0xe8); e8(e,31); sh_m32_imm8(e, 4, OFF_R(n), 1); mov_r32_m32(e, RCX, OFF_SR); and_r32_imm32(e, RCX, ~SH_T); or_r32_r32(e, RCX, RAX); mov_m32_r32(e, OFF_SR, RCX); return; }
    if ((op & 0xf0ffU) == 0x4001U) { mov_r32_m32(e, RAX, OFF_R(n)); and_r32_imm32(e, RAX, 1); sh_m32_imm8(e, 5, OFF_R(n), 1); mov_r32_m32(e, RCX, OFF_SR); and_r32_imm32(e, RCX, ~SH_T); or_r32_r32(e, RCX, RAX); mov_m32_r32(e, OFF_SR, RCX); return; }
    if ((op & 0xf0ffU) == 0x4021U) { mov_r32_m32(e, RAX, OFF_R(n)); and_r32_imm32(e, RAX, 1); sh_m32_imm8(e, 7, OFF_R(n), 1); mov_r32_m32(e, RCX, OFF_SR); and_r32_imm32(e, RCX, ~SH_T); or_r32_r32(e, RCX, RAX); mov_m32_r32(e, OFF_SR, RCX); return; }
    if ((op & 0xf0ffU) == 0x4008U) { sh_m32_imm8(e, 4, OFF_R(n), 2); return; }
    if ((op & 0xf0ffU) == 0x4009U) { sh_m32_imm8(e, 5, OFF_R(n), 2); return; }
    if ((op & 0xf0ffU) == 0x4018U) { sh_m32_imm8(e, 4, OFF_R(n), 8); return; }
    if ((op & 0xf0ffU) == 0x4019U) { sh_m32_imm8(e, 5, OFF_R(n), 8); return; }
    if ((op & 0xf0ffU) == 0x4028U) { sh_m32_imm8(e, 4, OFF_R(n), 16); return; }
    if ((op & 0xf0ffU) == 0x4029U) { sh_m32_imm8(e, 5, OFF_R(n), 16); return; }
    if ((op & 0xf0ffU) == 0x4011U) { cmp_m32_imm32(e, OFF_R(n), 0); setcc_al(e, 0x9d); store_t_from_al(e); return; }
    if ((op & 0xf0ffU) == 0x4015U) { cmp_m32_imm32(e, OFF_R(n), 0); setcc_al(e, 0x9f); store_t_from_al(e); return; }
    if ((op & 0xf00fU) == 0x400cU) { /* SHAD Rm,Rn */
        mov_r32_m32(e, RCX, OFF_R(m)); mov_r32_m32(e, RAX, OFF_R(n)); test_r32_r32(e, RCX, RCX);
        size_t j_neg = jcc32(e, 0x88); /* js */
        and_r32_imm32(e, RCX, 31); sh_r32_cl(e, 4, RAX); mov_m32_r32(e, OFF_R(n), RAX); size_t j_done = jmp32(e);
        size_t lab_neg = e->size; and_r32_imm32(e, RCX, 31); size_t j_zero = jcc32(e, 0x84); /* jz */
        mov_r32_imm32(e, RDX, 32); sub_r32_r32(e, RDX, RCX); mov_r32_r32(e, RCX, RDX); sh_r32_cl(e, 7, RAX); mov_m32_r32(e, OFF_R(n), RAX); size_t j_done2 = jmp32(e);
        size_t lab_zero = e->size; sar_r32_imm8(e, RAX, 31); mov_m32_r32(e, OFF_R(n), RAX);
        size_t lab_done = e->size; patch32(e, j_neg, lab_neg); patch32(e, j_zero, lab_zero); patch32(e, j_done, lab_done); patch32(e, j_done2, lab_done); return;
    }
    if ((op & 0xf00fU) == 0x400dU) { /* SHLD Rm,Rn */
        mov_r32_m32(e, RCX, OFF_R(m)); mov_r32_m32(e, RAX, OFF_R(n)); test_r32_r32(e, RCX, RCX);
        size_t j_neg = jcc32(e, 0x88);
        and_r32_imm32(e, RCX, 31); sh_r32_cl(e, 4, RAX); mov_m32_r32(e, OFF_R(n), RAX); size_t j_done = jmp32(e);
        size_t lab_neg = e->size; and_r32_imm32(e, RCX, 31); size_t j_zero = jcc32(e, 0x84);
        mov_r32_imm32(e, RDX, 32); sub_r32_r32(e, RDX, RCX); mov_r32_r32(e, RCX, RDX); sh_r32_cl(e, 5, RAX); mov_m32_r32(e, OFF_R(n), RAX); size_t j_done2 = jmp32(e);
        size_t lab_zero = e->size; mov_m32_imm32(e, OFF_R(n), 0);
        size_t lab_done = e->size; patch32(e, j_neg, lab_neg); patch32(e, j_zero, lab_zero); patch32(e, j_done, lab_done); patch32(e, j_done2, lab_done); return;
    }
    if ((op & 0xf0ffU) == 0x0012U) { mov_r32_m32(e, RAX, OFF_GBR); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf0ffU) == 0x0022U) { mov_r32_m32(e, RAX, OFF_VBR); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf0ffU) == 0x002aU) { mov_r32_m32(e, RAX, OFF_PR); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf0ffU) == 0x4002U) { sub_m32_imm32(e, OFF_R(n), 4); mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); mov_r32_r32(e, R8, RCX); mov_r32_m32(e, RDX, OFF_MACH); mov_r64_r64(e, RDI, R13); mov_r32_r32(e, RSI, R8); call_abs(e, (const void *)cv1k_sh3_jit_write32); return; }
    if ((op & 0xf0ffU) == 0x4012U) { sub_m32_imm32(e, OFF_R(n), 4); mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); mov_r32_r32(e, R8, RCX); mov_r32_m32(e, RDX, OFF_MACL); mov_r64_r64(e, RDI, R13); mov_r32_r32(e, RSI, R8); call_abs(e, (const void *)cv1k_sh3_jit_write32); return; }
    if ((op & 0xf0ffU) == 0x4022U) { sub_m32_imm32(e, OFF_R(n), 4); mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); mov_r32_r32(e, R8, RCX); mov_r32_m32(e, RDX, OFF_PR); mov_r64_r64(e, RDI, R13); mov_r32_r32(e, RSI, R8); call_abs(e, (const void *)cv1k_sh3_jit_write32); return; }
    if ((op & 0xf0ffU) == 0x4013U) { sub_m32_imm32(e, OFF_R(n), 4); mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); mov_r32_r32(e, R8, RCX); mov_r32_m32(e, RDX, OFF_GBR); mov_r64_r64(e, RDI, R13); mov_r32_r32(e, RSI, R8); call_abs(e, (const void *)cv1k_sh3_jit_write32); return; }
    if ((op & 0xf0ffU) == 0x4023U) { sub_m32_imm32(e, OFF_R(n), 4); mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); mov_r32_r32(e, R8, RCX); mov_r32_m32(e, RDX, OFF_VBR); mov_r64_r64(e, RDI, R13); mov_r32_r32(e, RSI, R8); call_abs(e, (const void *)cv1k_sh3_jit_write32); return; }
    if ((op & 0xf0ffU) == 0x4006U) { mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); set_args_bus_addr_reg(e, RCX); call_abs(e, (const void *)cv1k_sh3_jit_read32); mov_m32_r32(e, OFF_MACH, RAX); add_m32_imm32(e, OFF_R(n), 4); return; }
    if ((op & 0xf0ffU) == 0x4016U) { mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); set_args_bus_addr_reg(e, RCX); call_abs(e, (const void *)cv1k_sh3_jit_read32); mov_m32_r32(e, OFF_MACL, RAX); add_m32_imm32(e, OFF_R(n), 4); return; }
    if ((op & 0xf0ffU) == 0x4026U) { mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); set_args_bus_addr_reg(e, RCX); call_abs(e, (const void *)cv1k_sh3_jit_read32); mov_m32_r32(e, OFF_PR, RAX); add_m32_imm32(e, OFF_R(n), 4); return; }
    if ((op & 0xf0ffU) == 0x4017U) { mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); set_args_bus_addr_reg(e, RCX); call_abs(e, (const void *)cv1k_sh3_jit_read32); mov_m32_r32(e, OFF_GBR, RAX); add_m32_imm32(e, OFF_R(n), 4); return; }
    if ((op & 0xf0ffU) == 0x4027U) { mov_r32_m32(e, RCX, OFF_R(n)); mov_m32_r32(e, OFF_EA, RCX); set_args_bus_addr_reg(e, RCX); call_abs(e, (const void *)cv1k_sh3_jit_read32); mov_m32_r32(e, OFF_VBR, RAX); add_m32_imm32(e, OFF_R(n), 4); return; }
    if ((op & 0xf0ffU) == 0x000aU) { mov_r32_m32(e, RAX, OFF_MACH); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf0ffU) == 0x001aU) { mov_r32_m32(e, RAX, OFF_MACL); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf0ffU) == 0x400aU) { mov_r32_m32(e, RAX, OFF_R(n)); mov_m32_r32(e, OFF_MACH, RAX); return; }
    if ((op & 0xf0ffU) == 0x401aU) { mov_r32_m32(e, RAX, OFF_R(n)); mov_m32_r32(e, OFF_MACL, RAX); return; }
    if ((op & 0xf0ffU) == 0x401eU) { mov_r32_m32(e, RAX, OFF_R(n)); mov_m32_r32(e, OFF_GBR, RAX); return; }
    if ((op & 0xf0ffU) == 0x402eU) { mov_r32_m32(e, RAX, OFF_R(n)); mov_m32_r32(e, OFF_VBR, RAX); return; }
    if ((op & 0xf000U) == 0x9000U) { cv1k_u32 ea = pc + 4U + ((cv1k_u32)(op & 0xffU) * 2U); mov_m32_imm32(e, OFF_EA, ea); set_args_bus_addr_imm(e, ea); call_abs(e, (const void *)cv1k_sh3_jit_read16); movsx_eax_ax(e); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xf000U) == 0xd000U) { cv1k_u32 ea = ((pc + 4U) & ~3U) + ((cv1k_u32)(op & 0xffU) * 4U); mov_m32_imm32(e, OFF_EA, ea); set_args_bus_addr_imm(e, ea); call_abs(e, (const void *)cv1k_sh3_jit_read32); mov_m32_r32(e, OFF_R(n), RAX); return; }
    if ((op & 0xff00U) == 0xc700U) { cv1k_u32 ea = ((pc + 4U) & ~3U) + ((cv1k_u32)(op & 0xffU) * 4U); mov_m32_imm32(e, OFF_EA, ea); mov_m32_imm32(e, OFF_R(0), ea); return; }
    if ((op & 0xff00U) == 0x8000U) { load_addr_reg_disp(e, m, op & 0x0fU, 1U, RCX); write_mem_from_reg(e, 1, RCX, 0); return; }
    if ((op & 0xff00U) == 0x8100U) { load_addr_reg_disp(e, m, op & 0x0fU, 2U, RCX); write_mem_from_reg(e, 2, RCX, 0); return; }
    if ((op & 0xff00U) == 0x8400U) { load_addr_reg_disp(e, m, op & 0x0fU, 1U, RCX); read_mem_to_reg(e, 1, RCX, 0, 1); return; }
    if ((op & 0xff00U) == 0x8500U) { load_addr_reg_disp(e, m, op & 0x0fU, 2U, RCX); read_mem_to_reg(e, 2, RCX, 0, 1); return; }
    if ((op & 0xf000U) == 0x5000U) { load_addr_reg_disp(e, m, op & 0x0fU, 4U, RCX); read_mem_to_reg(e, 4, RCX, n, 0); return; }
    if ((op & 0xf000U) == 0x1000U) { load_addr_reg_disp(e, n, op & 0x0fU, 4U, RCX); write_mem_from_reg(e, 4, RCX, m); return; }
    if ((op & 0xf00fU) == 0x6000U) { load_addr_reg_disp(e, m, 0, 1, RCX); read_mem_to_reg(e, 1, RCX, n, 1); return; }
    if ((op & 0xf00fU) == 0x6001U) { load_addr_reg_disp(e, m, 0, 1, RCX); read_mem_to_reg(e, 2, RCX, n, 1); return; }
    if ((op & 0xf00fU) == 0x6002U) { load_addr_reg_disp(e, m, 0, 1, RCX); read_mem_to_reg(e, 4, RCX, n, 0); return; }
    if ((op & 0xf00fU) == 0x2000U) { load_addr_reg_disp(e, n, 0, 1, RCX); write_mem_from_reg(e, 1, RCX, m); return; }
    if ((op & 0xf00fU) == 0x2001U) { load_addr_reg_disp(e, n, 0, 1, RCX); write_mem_from_reg(e, 2, RCX, m); return; }
    if ((op & 0xf00fU) == 0x2002U) { load_addr_reg_disp(e, n, 0, 1, RCX); write_mem_from_reg(e, 4, RCX, m); return; }
    if ((op & 0xf00fU) == 0x6004U) { load_addr_reg_disp(e, m, 0, 1, RCX); read_mem_to_reg(e, 1, RCX, n, 1); if (n != m) add_m32_imm32(e, OFF_R(m), 1); return; }
    if ((op & 0xf00fU) == 0x6005U) { load_addr_reg_disp(e, m, 0, 1, RCX); read_mem_to_reg(e, 2, RCX, n, 1); if (n != m) add_m32_imm32(e, OFF_R(m), 2); return; }
    if ((op & 0xf00fU) == 0x6006U) { load_addr_reg_disp(e, m, 0, 1, RCX); read_mem_to_reg(e, 4, RCX, n, 0); if (n != m) add_m32_imm32(e, OFF_R(m), 4); return; }
    if ((op & 0xf00fU) == 0x2004U) { sub_m32_imm32(e, OFF_R(n), 1); load_addr_reg_disp(e, n, 0, 1, RCX); write_mem_from_reg(e, 1, RCX, m); return; }
    if ((op & 0xf00fU) == 0x2005U) { sub_m32_imm32(e, OFF_R(n), 2); load_addr_reg_disp(e, n, 0, 1, RCX); write_mem_from_reg(e, 2, RCX, m); return; }
    if ((op & 0xf00fU) == 0x2006U) { sub_m32_imm32(e, OFF_R(n), 4); load_addr_reg_disp(e, n, 0, 1, RCX); write_mem_from_reg(e, 4, RCX, m); return; }
    if ((op & 0xf00fU) == 0x000cU) { mov_r32_m32(e, RCX, OFF_R(m)); add_r32_m32(e, RCX, OFF_R(0)); mov_m32_r32(e, OFF_EA, RCX); read_mem_to_reg(e, 1, RCX, n, 1); return; }
    if ((op & 0xf00fU) == 0x000dU) { mov_r32_m32(e, RCX, OFF_R(m)); add_r32_m32(e, RCX, OFF_R(0)); mov_m32_r32(e, OFF_EA, RCX); read_mem_to_reg(e, 2, RCX, n, 1); return; }
    if ((op & 0xf00fU) == 0x000eU) { mov_r32_m32(e, RCX, OFF_R(m)); add_r32_m32(e, RCX, OFF_R(0)); mov_m32_r32(e, OFF_EA, RCX); read_mem_to_reg(e, 4, RCX, n, 0); return; }
    if ((op & 0xf00fU) == 0x0004U) { mov_r32_m32(e, RCX, OFF_R(n)); add_r32_m32(e, RCX, OFF_R(0)); mov_m32_r32(e, OFF_EA, RCX); write_mem_from_reg(e, 1, RCX, m); return; }
    if ((op & 0xf00fU) == 0x0005U) { mov_r32_m32(e, RCX, OFF_R(n)); add_r32_m32(e, RCX, OFF_R(0)); mov_m32_r32(e, OFF_EA, RCX); write_mem_from_reg(e, 2, RCX, m); return; }
    if ((op & 0xf00fU) == 0x0006U) { mov_r32_m32(e, RCX, OFF_R(n)); add_r32_m32(e, RCX, OFF_R(0)); mov_m32_r32(e, OFF_EA, RCX); write_mem_from_reg(e, 4, RCX, m); return; }
}

static void add_cycles_eax_return(struct x64e *e)
{
    add_m32_r32(e, OFF_CYCLES, RAX);
    return_common(e);
}

static void emit_terminal_branch(struct x64e *e, const struct cv1k_sh3_jit_block *b, size_t index, cv1k_u32 pc)
{
    cv1k_u16 op = b->ops[index];
    unsigned n = (op >> 8) & 15U;
    cv1k_u32 prefix_cycles = 0U;
    for (size_t i = 0; i < index; i++) prefix_cycles += cv1k_sh3_jit_linear_cycles(b->ops[i]);
    int inline_delay = (index + 1U < b->op_count);
    cv1k_u16 delay_op = inline_delay ? b->ops[index + 1U] : 0x0009U;
    cv1k_u32 delay_cycles = inline_delay ? cv1k_sh3_jit_linear_cycles(delay_op) : 0U;

    mov_m32_imm32(e, OFF_PPC, pc);

    if ((op & 0xf0ffU) == 0x400eU) { /* LDC Rn,SR */
        mov_r64_r64(e, RDI, R12);
        mov_r32_imm32(e, RSI, n);
        call_abs(e, (const void *)cv1k_sh3_jit_ldc_sr);
        epilogue(e, pc + 2U, prefix_cycles + 1U);
        return;
    }

    if ((op & 0xff00U) == 0x8900U || (op & 0xff00U) == 0x8b00U) { /* BT/BF */
        cv1k_u32 target = pc + 4U + ((cv1k_u32)sext8(op) * 2U);
        cv1k_u32 fall = pc + 2U;
        mov_r32_m32(e, RAX, OFF_SR);
        test_eax_imm32(e, SH_T);
        size_t j_taken = jcc32(e, ((op & 0xff00U) == 0x8900U) ? 0x85 : 0x84);
        mov_m32_imm32(e, OFF_PC, fall);
        mov_r32_imm32(e, RAX, prefix_cycles + 1U);
        size_t j_done = jmp32(e);
        size_t lab_taken = e->size;
        mov_m32_imm32(e, OFF_PC, target);
        mov_m32_imm32(e, OFF_EA, target);
        mov_r32_imm32(e, RAX, prefix_cycles + 3U);
        size_t lab_done = e->size;
        patch32(e, j_taken, lab_taken); patch32(e, j_done, lab_done);
        add_cycles_eax_return(e);
        return;
    }

    if ((op & 0xff00U) == 0x8d00U || (op & 0xff00U) == 0x8f00U) { /* BT/S BF/S */
        cv1k_u32 target = pc + 4U + ((cv1k_u32)sext8(op) * 2U);
        cv1k_u32 fall = pc + 2U;
        mov_r32_m32(e, RAX, OFF_SR); test_eax_imm32(e, SH_T);
        size_t j_taken = jcc32(e, ((op & 0xff00U) == 0x8d00U) ? 0x85 : 0x84);
        mov_m32_imm32(e, OFF_PC, fall); mov_r32_imm32(e, RAX, prefix_cycles + 1U);
        size_t j_done = jmp32(e);
        size_t lab_taken = e->size;
        if (inline_delay) {
            mov_m32_imm32(e, OFF_PPC, pc + 2U); mov_m32_imm32(e, OFF_PC, target); mov_m32_imm32(e, OFF_EA, target); mov_m32_imm32(e, OFF_M_DELAY, 0U);
            emit_one(e, delay_op, target);
            mov_r32_imm32(e, RAX, prefix_cycles + 2U + delay_cycles);
        } else {
            mov_m32_imm32(e, OFF_PC, fall); mov_m32_imm32(e, OFF_M_DELAY, target); mov_m32_imm32(e, OFF_EA, target); mov_r32_imm32(e, RAX, prefix_cycles + 2U);
        }
        size_t lab_done = e->size;
        patch32(e, j_taken, lab_taken); patch32(e, j_done, lab_done);
        add_cycles_eax_return(e);
        return;
    }

    if ((op & 0xf000U) == 0xa000U || (op & 0xf000U) == 0xb000U) { /* BRA/BSR */
        cv1k_u32 target = pc + 4U + ((cv1k_u32)sext12(op) * 2U);
        if ((op & 0xf000U) == 0xb000U) mov_m32_imm32(e, OFF_PR, pc + 4U);
        if (inline_delay) {
            mov_m32_imm32(e, OFF_PPC, pc + 2U); mov_m32_imm32(e, OFF_PC, target); mov_m32_imm32(e, OFF_EA, target); mov_m32_imm32(e, OFF_M_DELAY, 0U);
            emit_one(e, delay_op, target);
            epilogue(e, target, prefix_cycles + 2U + delay_cycles);
        } else {
            mov_m32_imm32(e, OFF_PC, pc + 2U); mov_m32_imm32(e, OFF_M_DELAY, target); mov_m32_imm32(e, OFF_EA, target);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + 2U); mov_r32_imm32(e, RAX, prefix_cycles + 2U); return_common(e);
        }
        return;
    }

    if ((op & 0xf0ffU) == 0x0023U || (op & 0xf0ffU) == 0x0003U) { /* BRAF/BSRF */
        if ((op & 0xf0ffU) == 0x0003U) mov_m32_imm32(e, OFF_PR, pc + 4U);
        mov_r32_m32(e, RAX, OFF_R(n)); add_r32_imm32(e, RAX, pc + 4U);
        if (inline_delay) {
            mov_m32_imm32(e, OFF_PPC, pc + 2U); mov_m32_r32(e, OFF_PC, RAX); mov_m32_r32(e, OFF_EA, RAX); mov_m32_imm32(e, OFF_M_DELAY, 0U);
            emit_one(e, delay_op, pc + 2U);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + 2U + delay_cycles); mov_r32_imm32(e, RAX, prefix_cycles + 2U + delay_cycles); return_common(e);
        } else {
            mov_m32_imm32(e, OFF_PC, pc + 2U); mov_m32_r32(e, OFF_M_DELAY, RAX); mov_m32_r32(e, OFF_EA, RAX);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + 2U); mov_r32_imm32(e, RAX, prefix_cycles + 2U); return_common(e);
        }
        return;
    }

    if ((op & 0xf0ffU) == 0x402bU || (op & 0xf0ffU) == 0x400bU) { /* JMP/JSR */
        if ((op & 0xf0ffU) == 0x400bU) mov_m32_imm32(e, OFF_PR, pc + 4U);
        mov_r32_m32(e, RAX, OFF_R(n));
        cv1k_u32 base_cycles = ((op & 0xf0ffU) == 0x400bU) ? 2U : 1U;
        if (inline_delay) {
            mov_m32_imm32(e, OFF_PPC, pc + 2U); mov_m32_r32(e, OFF_PC, RAX); mov_m32_r32(e, OFF_EA, RAX); mov_m32_imm32(e, OFF_M_DELAY, 0U);
            emit_one(e, delay_op, pc + 2U);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + base_cycles + delay_cycles); mov_r32_imm32(e, RAX, prefix_cycles + base_cycles + delay_cycles); return_common(e);
        } else {
            mov_m32_imm32(e, OFF_PC, pc + 2U); mov_m32_r32(e, OFF_M_DELAY, RAX); mov_m32_r32(e, OFF_EA, RAX);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + base_cycles); mov_r32_imm32(e, RAX, prefix_cycles + base_cycles); return_common(e);
        }
        return;
    }

    if (op == 0x002bU) { /* RTE */
        mov_r64_r64(e, RDI, R12);
        call_abs(e, (const void *)cv1k_sh3_jit_rte);
        if (inline_delay) {
            mov_m32_imm32(e, OFF_PPC, pc + 2U);
            mov_r32_m32(e, RAX, OFF_M_DELAY);
            mov_m32_r32(e, OFF_PC, RAX);
            mov_m32_imm32(e, OFF_M_DELAY, 0U);
            emit_one(e, delay_op, pc + 2U);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + 2U + delay_cycles); mov_r32_imm32(e, RAX, prefix_cycles + 2U + delay_cycles); return_common(e);
        } else {
            mov_m32_imm32(e, OFF_PC, pc + 2U);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + 2U); mov_r32_imm32(e, RAX, prefix_cycles + 2U); return_common(e);
        }
        return;
    }

    if (op == 0x000bU) { /* RTS */
        mov_r32_m32(e, RAX, OFF_PR);
        if (inline_delay) {
            mov_m32_imm32(e, OFF_PPC, pc + 2U); mov_m32_r32(e, OFF_PC, RAX); mov_m32_r32(e, OFF_EA, RAX); mov_m32_imm32(e, OFF_M_DELAY, 0U);
            emit_one(e, delay_op, pc + 2U);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + 2U + delay_cycles); mov_r32_imm32(e, RAX, prefix_cycles + 2U + delay_cycles); return_common(e);
        } else {
            mov_m32_imm32(e, OFF_PC, pc + 2U); mov_m32_r32(e, OFF_M_DELAY, RAX); mov_m32_r32(e, OFF_EA, RAX);
            add_m32_imm32(e, OFF_CYCLES, prefix_cycles + 2U); mov_r32_imm32(e, RAX, prefix_cycles + 2U); return_common(e);
        }
        return;
    }

    epilogue(e, pc + 2U, 1U);
}

static int x64_available(void)
{
#if defined(__x86_64__) && !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
    return 1;
#else
    return 0;
#endif
}

static int x64_compile(struct cv1k_sh3_jit_block *b)
{
    if (!x64_available() || b == NULL || b->op_count == 0) return -1;
    b->code_capacity = 16384U;
    b->code_mem = jit_alloc(b->code_capacity);
    if (!b->code_mem) return -1;
    struct x64e e = { .p = (uint8_t *)b->code_mem, .size = 0U, .cap = b->code_capacity, .fail = 0 };
    prologue(&e);
    for (size_t i = 0; i < b->op_count; i++) {
        cv1k_u32 pc = b->pc + (cv1k_u32)i * 2U;
        if (cv1k_sh3_jit_is_terminal_branch(b->ops[i])) {
            emit_terminal_branch(&e, b, i, pc);
            break;
        }
        mov_m32_imm32(&e, OFF_PPC, pc);
        emit_one(&e, b->ops[i], pc);
        if (i + 1U == b->op_count) epilogue(&e, b->pc + (cv1k_u32)b->op_count * 2U, cv1k_sh3_jit_block_cycles_max(b->ops, b->op_count));
    }
    if (e.fail || e.size == 0U || e.size >= b->code_capacity) {
        jit_free(b->code_mem, b->code_capacity);
        b->code_mem = NULL;
        b->code_capacity = 0U;
        return -1;
    }
    b->code_size = e.size;
    b->fn = (cv1k_sh3_jit_fn)b->code_mem;
    flush_icache(b->code_mem, b->code_size);
    return 0;
}

static void x64_free_code(struct cv1k_sh3_jit_block *b)
{
    if (b && b->code_mem) {
        jit_free(b->code_mem, b->code_capacity);
        b->code_mem = NULL;
        b->code_capacity = b->code_size = 0U;
        b->fn = NULL;
    }
}

static int unavailable(void) { return 0; }
static int no_compile(struct cv1k_sh3_jit_block *b) { (void)b; return -1; }
static void no_free(struct cv1k_sh3_jit_block *b) { (void)b; }

static const struct cv1k_sh3_jit_backend g_backends[] = {
    { "x64-c23", CV1K_SH3_JIT_ARCH_X64, x64_available, x64_compile, x64_free_code },
    { "aarch64-c23-stub", CV1K_SH3_JIT_ARCH_AARCH64, unavailable, no_compile, no_free },
    { "armv7-c23-stub", CV1K_SH3_JIT_ARCH_ARMV7, unavailable, no_compile, no_free },
    { "x86-c23-stub", CV1K_SH3_JIT_ARCH_X86, unavailable, no_compile, no_free },
    { NULL, CV1K_SH3_JIT_ARCH_NONE, unavailable, no_compile, no_free }
};

const struct cv1k_sh3_jit_backend *cv1k_sh3_jit_select_backend(void)
{
    for (size_t i = 0; g_backends[i].name != NULL; i++) {
        if (g_backends[i].available && g_backends[i].available()) return &g_backends[i];
    }
    return NULL;
}
