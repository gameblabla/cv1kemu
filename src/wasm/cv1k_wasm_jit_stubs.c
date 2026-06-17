#include "sh3_jit/cv1k_sh3_c23_jit.h"
#include "sh3_jit/cv1k_ir.h"
#include "cpu_sh7709s.h"
#include "bus.h"

int sh7709s_c23jit_available(void) { return 0; }
const char *sh7709s_c23jit_backend_name(void) { return "none"; }
void sh7709s_c23jit_enable(int enabled) { (void)enabled; }
int sh7709s_c23jit_enabled(void) { return 0; }
void sh7709s_c23jit_reset(void) {}
cv1k_u32 sh7709s_c23jit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                  cv1k_u32 cycle_budget, cv1k_u32 tmu_interval)
{
    (void)bus;
    (void)cycle_budget;
    (void)tmu_interval;
    return cpu ? cpu->cycles : 0u;
}
void sh7709s_c23jit_stats(cv1k_u32 *blocks, cv1k_u32 *hits, cv1k_u32 *fallbacks,
                          cv1k_u32 *invalidations)
{
    if (blocks) *blocks = 0u;
    if (hits) *hits = 0u;
    if (fallbacks) *fallbacks = 0u;
    if (invalidations) *invalidations = 0u;
}
cv1k_u32 cv1k_sh3_jit_test_run_block(struct sh7709s_cpu *cpu, struct cv1k_bus *bus)
{
    (void)cpu;
    (void)bus;
    return 0u;
}

static const struct cv1k_ir_backend g_no_ir_backend = { "none", NULL, NULL, NULL, NULL };
const struct cv1k_ir_backend *cv1k_ir_select_backend(void) { return &g_no_ir_backend; }
int cv1k_ir_phase1_supported(cv1k_u16 op) { (void)op; return 0; }
void cv1k_ir_set_cache(int on) { (void)on; }
void cv1k_ir_set_fastram(int on) { (void)on; }
void cv1k_ir_set_internal_loops(int on) { (void)on; }
int cv1k_ir_enabled(void) { return 0; }
void cv1k_ir_enable(int on) { (void)on; }
void cv1k_ir_reset(void) {}
cv1k_u32 cv1k_irjit_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                              cv1k_u32 cycle_budget, cv1k_u32 tmu_interval)
{
    (void)bus;
    (void)cycle_budget;
    (void)tmu_interval;
    return cpu ? cpu->cycles : 0u;
}
cv1k_u32 cv1k_ir_compile_block(struct cv1k_bus *bus, cv1k_u32 pc,
                               cv1k_ir_block_fn *out_fn, void **out_mem, size_t *out_cap)
{
    (void)bus;
    (void)pc;
    if (out_fn) *out_fn = NULL;
    if (out_mem) *out_mem = NULL;
    if (out_cap) *out_cap = 0u;
    return 0u;
}
void cv1k_ir_free_block(void *mem, size_t cap) { (void)mem; (void)cap; }
