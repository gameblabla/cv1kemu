#ifndef CPU_SH7709S_H
#define CPU_SH7709S_H

#include "cv1k_types.h"

struct cv1k_bus;

struct sh7709s_cpu {
    cv1k_u32 r[16];
    cv1k_u32 rb[8];
    cv1k_u32 pc;
    cv1k_u32 pr;
    cv1k_u32 gbr;
    cv1k_u32 vbr;
    cv1k_u32 mach;
    cv1k_u32 macl;
    cv1k_u32 sr;
    cv1k_u32 ssr;
    cv1k_u32 spc;
    cv1k_u32 sgr;
    cv1k_u32 tra;
    cv1k_u32 cycles;
    cv1k_u32 illegal_count;
    cv1k_u32 halted;
    cv1k_u32 irq_pending;
    cv1k_u32 irq_line;
    cv1k_u32 irq_level;
    cv1k_u32 irq_event;
    cv1k_u32 irq_ack_count;
    /* Multi-source interrupt controller: each asserted source keeps its INTEVT2
     * event code and priority until accepted (external lines) or explicitly
     * cleared (internal peripherals such as TMU), so vblank IRQ2 and the sound
     * TMU timer no longer overwrite each other. */
    cv1k_u32 pend_event[8];
    cv1k_u32 pend_pri[8];
    cv1k_u32 pend_mask;       /* bit i set when pend_event[i] is occupied */
    cv1k_u32 exception_count;
    cv1k_u32 last_illegal_pc;
    cv1k_u32 last_illegal_op;
    cv1k_u32 irq_delay_slot_guard;
    /* MAME SH-3 interpreter additions */
    cv1k_u32 ea;          /* effective-address scratch */
    cv1k_u32 m_delay;     /* pending delay-slot branch target (0 = none) */
    cv1k_u32 sleep_mode;  /* 0 normal, 1 sleeping, 2 woke from exception */
    cv1k_u32 ppc;         /* previous PC (debug) */
    cv1k_s32 icount;      /* per-step cycle accumulator (negative = consumed) */
    /* Per-game vblank-wait idle loop PCs (MAME cv1k install_speedups idlepc and
     * idlepc+2).  When the main thread parks here the rest of the frame is pure
     * spin, so the frame runners fast-forward to the next timer event instead of
     * interpreting ~1.7M spin instructions.  Defaults to the ddpdfk set; the
     * romset loader overrides them for the detected game. */
    cv1k_u32 idle_pc0;
    cv1k_u32 idle_pc1;
};

void sh7709s_reset(struct sh7709s_cpu *cpu);
void sh7709s_request_irq(struct sh7709s_cpu *cpu, int level);
void sh7709s_request_irq_line(struct sh7709s_cpu *cpu, int line, int priority);
void sh7709s_request_irq_event(struct sh7709s_cpu *cpu, cv1k_u32 event, int priority);
void sh7709s_clear_irq_event(struct sh7709s_cpu *cpu, cv1k_u32 event);
int sh7709s_accept_pending_irq(struct sh7709s_cpu *cpu, struct cv1k_bus *bus);
int sh7709s_step(struct sh7709s_cpu *cpu, struct cv1k_bus *bus);
/* Execute a predecoded, straight-line opcode span.  Used by the wasm block-JIT
 * backend so browser builds can cache guest fetch/decode while preserving the
 * exact MAME-derived per-op semantics.  The span must not contain delay-slot or
 * PC-changing control-flow ops.  The runner still tests IRQ acceptance after
 * each opcode and stops early if an interrupt vectors. */
cv1k_u32 sh7709s_run_linear_ops(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                const cv1k_u16 *ops, cv1k_u32 op_count);
void sh7709s_run(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 instructions);
cv1k_u32 sh7709s_run_until_idle(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                                cv1k_u32 cycle_budget, cv1k_u32 idle_pc0, cv1k_u32 idle_pc1);
cv1k_u32 sh7709s_run_frame_interpreter(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                           cv1k_u32 cycle_budget, cv1k_u32 tmu_interval);
cv1k_u32 sh7709s_run_frame(struct sh7709s_cpu *cpu, struct cv1k_bus *bus,
                           cv1k_u32 cycle_budget, cv1k_u32 tmu_interval);

#endif
