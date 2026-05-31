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
    cv1k_u32 exception_count;
    cv1k_u32 last_illegal_pc;
    cv1k_u32 last_illegal_op;
};

void sh7709s_reset(struct sh7709s_cpu *cpu);
void sh7709s_request_irq(struct sh7709s_cpu *cpu, int level);
void sh7709s_request_irq_line(struct sh7709s_cpu *cpu, int line, int priority);
void sh7709s_request_irq_event(struct sh7709s_cpu *cpu, cv1k_u32 event, int priority);
void sh7709s_clear_irq_event(struct sh7709s_cpu *cpu, cv1k_u32 event);
int sh7709s_step(struct sh7709s_cpu *cpu, struct cv1k_bus *bus);
void sh7709s_run(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 instructions);

#endif
