#ifndef CV1K_SH3_JIT_BACKEND_H
#define CV1K_SH3_JIT_BACKEND_H

#include "cv1k_types.h"
#include "cpu_sh7709s.h"
#include "bus.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef cv1k_u32 (*cv1k_sh3_jit_fn)(struct sh7709s_cpu *, struct cv1k_bus *);

enum cv1k_sh3_jit_arch {
    CV1K_SH3_JIT_ARCH_NONE = 0,
    CV1K_SH3_JIT_ARCH_X64,
    CV1K_SH3_JIT_ARCH_X86,
    CV1K_SH3_JIT_ARCH_ARMV7,
    CV1K_SH3_JIT_ARCH_AARCH64
};

struct cv1k_sh3_jit_block {
    cv1k_u32 pc;
    cv1k_u32 cycles;
    cv1k_u8 negative;
    cv1k_u8 op_count;
    cv1k_u16 ops[48];
    void *code_mem;
    size_t code_size;
    size_t code_capacity;
    cv1k_sh3_jit_fn fn;
    struct cv1k_sh3_jit_block *next;
};

struct cv1k_sh3_jit_backend {
    const char *name;
    enum cv1k_sh3_jit_arch arch;
    int (*available)(void);
    int (*compile)(struct cv1k_sh3_jit_block *block);
    void (*free_code)(struct cv1k_sh3_jit_block *block);
};

const struct cv1k_sh3_jit_backend *cv1k_sh3_jit_select_backend(void);

/* Shared helpers called from generated code. */
cv1k_u8  cv1k_sh3_jit_read8(struct cv1k_bus *bus, cv1k_u32 addr);
cv1k_u16 cv1k_sh3_jit_read16(struct cv1k_bus *bus, cv1k_u32 addr);
cv1k_u32 cv1k_sh3_jit_read32(struct cv1k_bus *bus, cv1k_u32 addr);
void cv1k_sh3_jit_write8(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data);
void cv1k_sh3_jit_write16(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data);
void cv1k_sh3_jit_write32(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data);
void cv1k_sh3_jit_ldc_sr(struct sh7709s_cpu *cpu, cv1k_u32 n);
void cv1k_sh3_jit_rte(struct sh7709s_cpu *cpu);
/* Focused single-op helpers (exact interpreter semantics) for ops the codegen
 * does not emit inline, so a block containing them still compiles instead of
 * falling the whole block back to the interpreter. */
void cv1k_sh3_jit_rotcl(struct sh7709s_cpu *cpu, cv1k_u32 n);
void cv1k_sh3_jit_rotcr(struct sh7709s_cpu *cpu, cv1k_u32 n);
void cv1k_sh3_jit_div0s(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n);
void cv1k_sh3_jit_div0u(struct sh7709s_cpu *cpu);
void cv1k_sh3_jit_div1(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n);
void cv1k_sh3_jit_mull(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n);
void cv1k_sh3_jit_negc(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n);
void cv1k_sh3_jit_swapb(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n);
void cv1k_sh3_jit_swapw(struct sh7709s_cpu *cpu, cv1k_u32 m, cv1k_u32 n);
void cv1k_sh3_jit_rotl(struct sh7709s_cpu *cpu, cv1k_u32 n);
void cv1k_sh3_jit_rotr(struct sh7709s_cpu *cpu, cv1k_u32 n);

cv1k_u32 cv1k_sh3_jit_linear_cycles(cv1k_u16 op);
cv1k_u32 cv1k_sh3_jit_block_cycles_max(const cv1k_u16 *ops, size_t count);
int cv1k_sh3_jit_is_terminal_branch(cv1k_u16 op);
int cv1k_sh3_jit_branch_has_delay_slot(cv1k_u16 op);
int cv1k_sh3_jit_supported_linear(cv1k_u16 op);
int cv1k_sh3_jit_delay_can_inline(cv1k_u16 op);

#ifdef __cplusplus
}
#endif

#endif
