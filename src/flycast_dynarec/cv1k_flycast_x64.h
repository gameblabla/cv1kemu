#ifndef CV1K_FLYCAST_X64_H
#define CV1K_FLYCAST_X64_H
/* Compatibility shim: these API names are retained for older CLI/scripts.
 * The implementation is now the self-contained C23 SH-3 JIT in src/sh3_jit/. */

#include "cv1k_types.h"

struct sh7709s_cpu;
struct cv1k_bus;

#ifdef __cplusplus
extern "C" {
#endif

int sh7709s_flycast_x64_available(void);
void sh7709s_flycast_x64_enable(int enabled);
int sh7709s_flycast_x64_enabled(void);
void sh7709s_flycast_x64_reset(void);
cv1k_u32 sh7709s_flycast_x64_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                       cv1k_u32 cycle_budget, cv1k_u32 tmu_interval);
void sh7709s_flycast_x64_stats(cv1k_u32 *blocks, cv1k_u32 *hits, cv1k_u32 *fallbacks, cv1k_u32 *invalidations);

#ifdef __cplusplus
}
#endif

#endif
