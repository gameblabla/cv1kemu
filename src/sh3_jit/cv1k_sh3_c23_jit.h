#ifndef CV1K_SH3_C23_JIT_H
#define CV1K_SH3_C23_JIT_H

#include "cv1k_types.h"

struct sh7709s_cpu;
struct cv1k_bus;

#ifdef __cplusplus
extern "C" {
#endif

/* Self-contained C23 SH-3 JIT frontend.  The default backend is selected at
 * compile time; currently x64 is implemented, while the backend table keeps
 * ARMv7, AArch64, and i386 slots available for later emitters. */
int sh7709s_c23jit_available(void);
const char *sh7709s_c23jit_backend_name(void);
void sh7709s_c23jit_enable(int enabled);
int sh7709s_c23jit_enabled(void);
void sh7709s_c23jit_reset(void);
cv1k_u32 sh7709s_c23jit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                  cv1k_u32 cycle_budget, cv1k_u32 tmu_interval);
void sh7709s_c23jit_stats(cv1k_u32 *blocks, cv1k_u32 *hits, cv1k_u32 *fallbacks,
                          cv1k_u32 *invalidations);

/* TEST-ONLY: compile+run one fresh block at cpu->pc; returns ops covered (0 if
 * not JIT-compilable).  Used by the differential tester (tools/jit_difftest.c)
 * to assert a JIT block matches interpreting the same instructions. */
cv1k_u32 cv1k_sh3_jit_test_run_block(struct sh7709s_cpu *cpu, struct cv1k_bus *bus);

#ifdef __cplusplus
}
#endif

#endif
