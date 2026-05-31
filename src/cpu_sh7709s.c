#include "cpu_sh7709s.h"
#include "bus.h"
#include <string.h>

#define SR_T 0x00000001UL
#define SR_Q 0x00000100UL
#define SR_M 0x00000200UL
#define SR_S 0x00000002UL
#define SR_BL 0x10000000UL
#define SR_RB 0x20000000UL
#define SR_MD 0x40000000UL

static cv1k_s32 sx(cv1k_u32 value, int bits)
{
    cv1k_u32 mask;
    cv1k_u32 top;
    mask = (bits >= 32) ? 0xffffffffUL : ((1UL << bits) - 1UL);
    top = 1UL << (bits - 1);
    value &= mask;
    if ((value & top) != 0UL) return (cv1k_s32)(value | (~mask));
    return (cv1k_s32)value;
}

static void set_t(struct sh7709s_cpu *cpu, int v)
{
    if (v) cpu->sr |= SR_T;
    else cpu->sr &= ~SR_T;
}


static void swap_banked_r0_r7(struct sh7709s_cpu *cpu)
{
    cv1k_u32 i;
    cv1k_u32 t;
    for (i = 0UL; i < 8UL; i++) {
        t = cpu->r[i];
        cpu->r[i] = cpu->rb[i];
        cpu->rb[i] = t;
    }
}

static void set_sr_full(struct sh7709s_cpu *cpu, cv1k_u32 v)
{
    if (((cpu->sr ^ v) & SR_RB) != 0UL) swap_banked_r0_r7(cpu);
    cpu->sr = v;
}

static int get_t(const struct sh7709s_cpu *cpu)
{
    return (cpu->sr & SR_T) ? 1 : 0;
}

static void set_q(struct sh7709s_cpu *cpu, int v)
{
    if (v) cpu->sr |= SR_Q;
    else cpu->sr &= ~SR_Q;
}

static void set_m(struct sh7709s_cpu *cpu, int v)
{
    if (v) cpu->sr |= SR_M;
    else cpu->sr &= ~SR_M;
}

static int get_q(const struct sh7709s_cpu *cpu)
{
    return (cpu->sr & SR_Q) ? 1 : 0;
}

static int get_m(const struct sh7709s_cpu *cpu)
{
    return (cpu->sr & SR_M) ? 1 : 0;
}

static cv1k_u32 read_pc_rel32(struct cv1k_bus *bus, cv1k_u32 pc, cv1k_u32 disp)
{
    cv1k_u32 ea;
    cv1k_u32 hi;
    cv1k_u32 lo;
    ea = ((pc + 4UL) & 0xfffffffcUL) + disp * 4UL;
    /* Literal pools sit beside copied RAM code on CV1000.  Use the same
     * fetch-side cache model as instruction words so a DMA overlay does not
     * immediately destroy literals already resident in the SH cache. */
    hi = (cv1k_u32)cv1k_bus_fetch16(bus, ea);
    lo = (cv1k_u32)cv1k_bus_fetch16(bus, ea + 2UL);
    return (hi << 16) | lo;
}

static cv1k_u16 read_pc_rel16(struct cv1k_bus *bus, cv1k_u32 pc, cv1k_u32 disp)
{
    cv1k_u32 base;
    base = pc + 4UL;
    return cv1k_bus_fetch16(bus, base + disp * 2UL);
}

static void delay_slot(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 slot_pc, cv1k_u32 target)
{
    cpu->pc = slot_pc & 0xfffffffeUL;
    sh7709s_step(cpu, bus);
    cpu->pc = target & 0xfffffffeUL;
}

static void rte_delay_slot_then_restore(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 slot_pc)
{
    /* MAME's SH3/SH7709S RTE path is a delayed branch: execute the delay
     * slot while still in the current exception context, then RTE copies
     * SSR->SR and SPC->PC.  The previous sandbox restored SR before running
     * the slot, which made optional IRQ/vblank experiments leave supervisor
     * state at the wrong time.  Read SPC/SSR after the slot, matching MAME's
     * generated path where generate_delay_slot() precedes the RTE helper.
     */
    cpu->pc = slot_pc & 0xfffffffeUL;
    sh7709s_step(cpu, bus);
    set_sr_full(cpu, cpu->ssr);
    cpu->pc = cpu->spc & 0xfffffffeUL;
}


static void mul_u32_to_u64(cv1k_u32 a, cv1k_u32 b, cv1k_u32 *hi, cv1k_u32 *lo)
{
    cv1k_u32 aa[4];
    cv1k_u32 bb[4];
    cv1k_u32 out[8];
    int i;
    int j;
    aa[0] = a & 0xffUL; aa[1] = (a >> 8) & 0xffUL; aa[2] = (a >> 16) & 0xffUL; aa[3] = (a >> 24) & 0xffUL;
    bb[0] = b & 0xffUL; bb[1] = (b >> 8) & 0xffUL; bb[2] = (b >> 16) & 0xffUL; bb[3] = (b >> 24) & 0xffUL;
    for (i = 0; i < 8; i++) out[i] = 0UL;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) out[i + j] += aa[i] * bb[j];
    for (i = 0; i < 7; i++) { out[i + 1] += out[i] >> 8; out[i] &= 0xffUL; }
    out[7] &= 0xffUL;
    *lo = out[0] | (out[1] << 8) | (out[2] << 16) | (out[3] << 24);
    *hi = out[4] | (out[5] << 8) | (out[6] << 16) | (out[7] << 24);
}

static void mul_s32_to_u64(cv1k_u32 a, cv1k_u32 b, cv1k_u32 *hi, cv1k_u32 *lo)
{
    int neg;
    cv1k_u32 ua;
    cv1k_u32 ub;
    neg = (((a ^ b) & 0x80000000UL) != 0UL) ? 1 : 0;
    ua = ((a & 0x80000000UL) != 0UL) ? ((~a) + 1UL) : a;
    ub = ((b & 0x80000000UL) != 0UL) ? ((~b) + 1UL) : b;
    mul_u32_to_u64(ua, ub, hi, lo);
    if (neg) {
        *lo = (~(*lo)) + 1UL;
        *hi = ~(*hi);
        if (*lo == 0UL) *hi += 1UL;
    }
}

static void div1(struct sh7709s_cpu *cpu, int m, int n)
{
    cv1k_u32 old_q;
    cv1k_u32 tmp0;
    cv1k_u32 tmp1;
    int old_t;
    old_q = (cpu->sr & SR_Q) ? 1UL : 0UL;
    old_t = get_t(cpu);
    set_q(cpu, (cpu->r[n] & 0x80000000UL) != 0UL);
    cpu->r[n] = (cpu->r[n] << 1) | (cv1k_u32)old_t;
    tmp0 = cpu->r[n];
    if (old_q == 0UL) {
        if (get_m(cpu) == 0) {
            cpu->r[n] -= cpu->r[m];
            tmp1 = (cpu->r[n] > tmp0) ? 1UL : 0UL;
            set_q(cpu, get_q(cpu) ? (tmp1 == 0UL) : (tmp1 != 0UL));
        } else {
            cpu->r[n] += cpu->r[m];
            tmp1 = (cpu->r[n] < tmp0) ? 1UL : 0UL;
            set_q(cpu, get_q(cpu) ? (tmp1 != 0UL) : (tmp1 == 0UL));
        }
    } else {
        if (get_m(cpu) == 0) {
            cpu->r[n] += cpu->r[m];
            tmp1 = (cpu->r[n] < tmp0) ? 1UL : 0UL;
            set_q(cpu, get_q(cpu) ? (tmp1 == 0UL) : (tmp1 != 0UL));
        } else {
            cpu->r[n] -= cpu->r[m];
            tmp1 = (cpu->r[n] > tmp0) ? 1UL : 0UL;
            set_q(cpu, get_q(cpu) ? (tmp1 != 0UL) : (tmp1 == 0UL));
        }
    }
    set_t(cpu, get_q(cpu) == get_m(cpu));
}

void sh7709s_reset(struct sh7709s_cpu *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
    /* MAME sh34_base_device::device_reset() starts the SH-3/SH-4 core
     * in the P2 reset area at 0xa0000000, not at physical 0.  The sandbox
     * address translator maps P2 to the same boot ROM bytes, but exposing
     * the MAME-visible reset PC matters for traces and for any code that
     * observes the reset context before the first branch.
     */
    cpu->pc = 0xa0000000UL;
    cpu->sr = 0x700000f0UL;
}

void sh7709s_request_irq(struct sh7709s_cpu *cpu, int level)
{
    sh7709s_request_irq_line(cpu, level, level);
}

void sh7709s_request_irq_line(struct sh7709s_cpu *cpu, int line, int priority)
{
    cpu->irq_pending = 1UL;
    cpu->irq_line = (cv1k_u32)line;
    cpu->irq_level = (cv1k_u32)priority;
    cpu->irq_event = 0UL;
}

void sh7709s_request_irq_event(struct sh7709s_cpu *cpu, cv1k_u32 event, int priority)
{
    cpu->irq_pending = 1UL;
    cpu->irq_line = 0UL;
    cpu->irq_level = (cv1k_u32)priority;
    cpu->irq_event = event;
}

void sh7709s_clear_irq_event(struct sh7709s_cpu *cpu, cv1k_u32 event)
{
    if (cpu == NULL) return;
    /* MAME's TMU TCR writes call sh4_exception_unrequest(TUNI*) when
     * either TIE or UNF is clear.  This helper gives the standalone SH7709S
     * scaffold the same pending-event cancellation semantics for internal
     * event-code interrupts without changing external IRQ-line handling.
     */
    if (cpu->irq_pending != 0UL && cpu->irq_event == event) {
        cpu->irq_pending = 0UL;
        cpu->irq_event = 0UL;
        cpu->irq_line = 0UL;
        cpu->irq_level = 0UL;
    }
}

static void take_exception(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 event, cv1k_u32 tra, cv1k_u32 target)
{
    cv1k_bus_exception_ack(bus, event, tra);
    cpu->ssr = cpu->sr;
    cpu->spc = cpu->pc;
    cpu->sgr = cpu->r[15];
    cpu->tra = tra;
    set_sr_full(cpu, cpu->sr | (SR_MD | SR_RB | SR_BL));
    cpu->pc = target & 0xfffffffeUL;
    cpu->exception_count++;
    cpu->halted = 0UL;
}

static int do_irq_if_possible(struct sh7709s_cpu *cpu, struct cv1k_bus *bus)
{
    cv1k_u32 mask;
    cv1k_u32 level;
    cv1k_u32 line;
    cv1k_u32 event;
    if (cpu->irq_pending == 0UL) return 0;
    if ((cpu->sr & SR_BL) != 0UL) return 0;
    line = cpu->irq_line & 0x0fUL;
    level = cpu->irq_level & 0x0fUL;
    mask = (cpu->sr >> 4) & 0x0fUL;
    if (level == 0UL || level <= mask) return 0;
    /* MAME SH3/SH7709S separates interrupt line identity from interrupt
     * priority.  The INTEVT2 event code identifies the accepted line
     * (IRQ2 => 0x640), while the priority is taken from the INTC priority
     * register and compared against SR.IMASK.  Earlier sandbox builds used
     * one value for both, so raising priority for IRQ2 would corrupt the
     * event code and low-priority IRQ2 could never be accepted.
     */
    if (cpu->irq_event != 0UL) event = cpu->irq_event;
    else event = 0x600UL + (line * 0x20UL);
    cv1k_bus_irq_ack(bus, level, event);
    take_exception(cpu, bus, event, 0UL, cpu->vbr + 0x600UL);
    cpu->irq_ack_count++;
    cpu->irq_pending = 0UL;
    return 1;
}

int sh7709s_step(struct sh7709s_cpu *cpu, struct cv1k_bus *bus)
{
    cv1k_u16 op;
    cv1k_u32 oldpc;
    int n;
    int m;
    cv1k_s32 disp;
    cv1k_u32 tmp;
    cv1k_u32 ea;
    cv1k_u32 imm;
    cv1k_u32 target;
    cv1k_s32 ss;

    if (cpu->halted != 0UL) {
        cpu->cycles += 1UL;
        do_irq_if_possible(cpu, bus);
        return 1;
    }

    if (do_irq_if_possible(cpu, bus)) {
        cpu->cycles += 8UL;
        return 1;
    }

    cpu->pc &= 0xfffffffeUL;
    oldpc = cpu->pc;
    op = cv1k_bus_fetch16(bus, cpu->pc);
    cpu->pc += 2UL;
    cpu->cycles += 1UL;

    n = (int)((op >> 8) & 0x0fU);
    m = (int)((op >> 4) & 0x0fU);

    if (op == 0x0009U) return 1; /* NOP */
    if (op == 0x0008U) { set_t(cpu, 0); return 1; } /* CLRT */
    if (op == 0x0018U) { set_t(cpu, 1); return 1; } /* SETT */
    if (op == 0x0019U) { cpu->sr &= ~(SR_M | SR_Q | SR_T); return 1; } /* DIV0U */
    if (op == 0x001bU) { cpu->halted = 1UL; return 1; } /* SLEEP */
    if (op == 0x0028U) { cpu->mach = 0UL; cpu->macl = 0UL; return 1; } /* CLRMAC */
    if (op == 0x0048U) { cpu->sr &= ~SR_S; return 1; } /* CLRS */
    if (op == 0x0058U) { cpu->sr |= SR_S; return 1; } /* SETS */
    if (op == 0x000bU) { target = cpu->pr; delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* RTS */
    if (op == 0x002bU) { rte_delay_slot_then_restore(cpu, bus, oldpc + 2UL); return 1; } /* RTE */

    /* Immediate ALU and PC-relative loads. */
    if ((op & 0xf000U) == 0xe000U) { cpu->r[n] = (cv1k_u32)sx((cv1k_u32)(op & 0xffU), 8); return 1; } /* MOV #imm,Rn */
    if ((op & 0xf000U) == 0x7000U) { cpu->r[n] += (cv1k_u32)sx((cv1k_u32)(op & 0xffU), 8); return 1; } /* ADD #imm,Rn */
    if ((op & 0xf000U) == 0x9000U) { cpu->r[n] = (cv1k_u32)sx((cv1k_u32)read_pc_rel16(bus, oldpc, (cv1k_u32)(op & 0xffU)), 16); return 1; } /* MOV.W @(disp,PC),Rn */
    if ((op & 0xf000U) == 0xd000U) { cpu->r[n] = read_pc_rel32(bus, oldpc, (cv1k_u32)(op & 0xffU)); return 1; } /* MOV.L @(disp,PC),Rn */
    if ((op & 0xff00U) == 0xc700U) { cpu->r[0] = ((oldpc + 4UL) & 0xfffffffcUL) + ((cv1k_u32)(op & 0xffU) * 4UL); return 1; } /* MOVA */

    if ((op & 0xff00U) == 0x8800U) { set_t(cpu, cpu->r[0] == (cv1k_u32)sx((cv1k_u32)(op & 0xffU), 8)); return 1; } /* CMP/EQ #imm,R0 */
    if ((op & 0xff00U) == 0xc800U) { imm = (cv1k_u32)(op & 0xffU); set_t(cpu, (cpu->r[0] & imm) == 0UL); return 1; }
    if ((op & 0xff00U) == 0xc900U) { imm = (cv1k_u32)(op & 0xffU); cpu->r[0] &= imm; return 1; }
    if ((op & 0xff00U) == 0xca00U) { imm = (cv1k_u32)(op & 0xffU); cpu->r[0] ^= imm; return 1; }
    if ((op & 0xff00U) == 0xcb00U) { imm = (cv1k_u32)(op & 0xffU); cpu->r[0] |= imm; return 1; }
    if ((op & 0xff00U) == 0xcc00U) { imm = (cv1k_u32)(op & 0xffU); set_t(cpu, (cv1k_bus_read8(bus, cpu->gbr + cpu->r[0]) & imm) == 0UL); return 1; }
    if ((op & 0xff00U) == 0xcd00U) { imm = (cv1k_u32)(op & 0xffU); ea = cpu->gbr + cpu->r[0]; cv1k_bus_write8(bus, ea, (cv1k_u8)(cv1k_bus_read8(bus, ea) & imm)); return 1; }
    if ((op & 0xff00U) == 0xce00U) { imm = (cv1k_u32)(op & 0xffU); ea = cpu->gbr + cpu->r[0]; cv1k_bus_write8(bus, ea, (cv1k_u8)(cv1k_bus_read8(bus, ea) ^ imm)); return 1; }
    if ((op & 0xff00U) == 0xcf00U) { imm = (cv1k_u32)(op & 0xffU); ea = cpu->gbr + cpu->r[0]; cv1k_bus_write8(bus, ea, (cv1k_u8)(cv1k_bus_read8(bus, ea) | imm)); return 1; }

    /* Branches. */
    if ((op & 0xf000U) == 0xa000U) { disp = sx((cv1k_u32)(op & 0x0fffU), 12); target = oldpc + 4UL + (cv1k_u32)(disp * 2L); delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* BRA */
    if ((op & 0xf000U) == 0xb000U) { disp = sx((cv1k_u32)(op & 0x0fffU), 12); cpu->pr = oldpc + 4UL; target = oldpc + 4UL + (cv1k_u32)(disp * 2L); delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* BSR */
    if ((op & 0xff00U) == 0x8900U) { if (get_t(cpu)) cpu->pc = oldpc + 4UL + (cv1k_u32)(sx((cv1k_u32)(op & 0xffU), 8) * 2L); return 1; } /* BT */
    if ((op & 0xff00U) == 0x8b00U) { if (!get_t(cpu)) cpu->pc = oldpc + 4UL + (cv1k_u32)(sx((cv1k_u32)(op & 0xffU), 8) * 2L); return 1; } /* BF */
    if ((op & 0xff00U) == 0x8d00U) { if (get_t(cpu)) target = oldpc + 4UL + (cv1k_u32)(sx((cv1k_u32)(op & 0xffU), 8) * 2L); else target = oldpc + 4UL; delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* BT/S */
    if ((op & 0xff00U) == 0x8f00U) { if (!get_t(cpu)) target = oldpc + 4UL + (cv1k_u32)(sx((cv1k_u32)(op & 0xffU), 8) * 2L); else target = oldpc + 4UL; delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* BF/S */
    if ((op & 0xf0ffU) == 0x0003U) { cpu->pr = oldpc + 4UL; target = oldpc + 4UL + cpu->r[n]; delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* BSRF */
    if ((op & 0xf0ffU) == 0x0023U) { target = oldpc + 4UL + cpu->r[n]; delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* BRAF */
    if ((op & 0xf0ffU) == 0x400bU) { cpu->pr = oldpc + 4UL; target = cpu->r[n]; delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* JSR @Rn */
    if ((op & 0xf0ffU) == 0x402bU) { target = cpu->r[n]; delay_slot(cpu, bus, oldpc + 2UL, target); return 1; } /* JMP @Rn */

    /* Register moves and memory moves. */
    if ((op & 0xf00fU) == 0x6003U) { cpu->r[n] = cpu->r[m]; return 1; } /* MOV Rm,Rn */
    if ((op & 0xf00fU) == 0x2000U) { cv1k_bus_write8(bus, cpu->r[n], (cv1k_u8)(cpu->r[m] & 0xffUL)); return 1; }
    if ((op & 0xf00fU) == 0x2001U) { cv1k_bus_write16(bus, cpu->r[n], (cv1k_u16)(cpu->r[m] & 0xffffUL)); return 1; }
    if ((op & 0xf00fU) == 0x2002U) { cv1k_bus_write32(bus, cpu->r[n], cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x2004U) { cpu->r[n] -= 1UL; cv1k_bus_write8(bus, cpu->r[n], (cv1k_u8)(cpu->r[m] & 0xffUL)); return 1; }
    if ((op & 0xf00fU) == 0x2005U) { cpu->r[n] -= 2UL; cv1k_bus_write16(bus, cpu->r[n], (cv1k_u16)(cpu->r[m] & 0xffffUL)); return 1; }
    if ((op & 0xf00fU) == 0x2006U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x6000U) { cpu->r[n] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read8(bus, cpu->r[m]), 8); return 1; }
    if ((op & 0xf00fU) == 0x6001U) { cpu->r[n] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read16(bus, cpu->r[m]), 16); return 1; }
    if ((op & 0xf00fU) == 0x6002U) { cpu->r[n] = cv1k_bus_read32(bus, cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x6004U) { cpu->r[n] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read8(bus, cpu->r[m]), 8); if (m != n) cpu->r[m] += 1UL; return 1; }
    if ((op & 0xf00fU) == 0x6005U) { cpu->r[n] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read16(bus, cpu->r[m]), 16); if (m != n) cpu->r[m] += 2UL; return 1; }
    if ((op & 0xf00fU) == 0x6006U) { cpu->r[n] = cv1k_bus_read32(bus, cpu->r[m]); if (m != n) cpu->r[m] += 4UL; return 1; }

    if ((op & 0xf000U) == 0x1000U) { ea = cpu->r[n] + (((cv1k_u32)(op & 0x0fU)) * 4UL); cv1k_bus_write32(bus, ea, cpu->r[m]); return 1; }
    if ((op & 0xf000U) == 0x5000U) { ea = cpu->r[m] + (((cv1k_u32)(op & 0x0fU)) * 4UL); cpu->r[n] = cv1k_bus_read32(bus, ea); return 1; }
    if ((op & 0xf000U) == 0x8000U) {
        /* 8ndd group.  SH encodes the register in bits 7..4; R0 is
         * implicit as the source/destination.  Earlier scaffold revisions
         * accidentally used R0 as the base and Rm as the value, which
         * corrupted stack arguments during the CV1000 boot path.
         */
        switch ((op >> 8) & 0x0fU) {
        case 0: ea = cpu->r[m] + (cv1k_u32)(op & 0x0fU); cv1k_bus_write8(bus, ea, (cv1k_u8)(cpu->r[0] & 0xffUL)); return 1;
        case 1: ea = cpu->r[m] + ((cv1k_u32)(op & 0x0fU) * 2UL); cv1k_bus_write16(bus, ea, (cv1k_u16)(cpu->r[0] & 0xffffUL)); return 1;
        case 4: ea = cpu->r[m] + (cv1k_u32)(op & 0x0fU); cpu->r[0] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read8(bus, ea), 8); return 1;
        case 5: ea = cpu->r[m] + ((cv1k_u32)(op & 0x0fU) * 2UL); cpu->r[0] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read16(bus, ea), 16); return 1;
        default: break;
        }
    }
    if ((op & 0xf00fU) == 0x0004U) { ea = cpu->r[n] + cpu->r[0]; cv1k_bus_write8(bus, ea, (cv1k_u8)(cpu->r[m] & 0xffUL)); return 1; }
    if ((op & 0xf00fU) == 0x0005U) { ea = cpu->r[n] + cpu->r[0]; cv1k_bus_write16(bus, ea, (cv1k_u16)(cpu->r[m] & 0xffffUL)); return 1; }
    if ((op & 0xf00fU) == 0x0006U) { ea = cpu->r[n] + cpu->r[0]; cv1k_bus_write32(bus, ea, cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x000cU) { ea = cpu->r[m] + cpu->r[0]; cpu->r[n] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read8(bus, ea), 8); return 1; }
    if ((op & 0xf00fU) == 0x000dU) { ea = cpu->r[m] + cpu->r[0]; cpu->r[n] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read16(bus, ea), 16); return 1; }
    if ((op & 0xf00fU) == 0x000eU) { ea = cpu->r[m] + cpu->r[0]; cpu->r[n] = cv1k_bus_read32(bus, ea); return 1; }
    if ((op & 0xff00U) == 0xc000U) { cv1k_bus_write8(bus, cpu->gbr + (cv1k_u32)(op & 0xffU), (cv1k_u8)(cpu->r[0] & 0xffUL)); return 1; }
    if ((op & 0xff00U) == 0xc100U) { cv1k_bus_write16(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 2UL), (cv1k_u16)(cpu->r[0] & 0xffffUL)); return 1; }
    if ((op & 0xff00U) == 0xc200U) { cv1k_bus_write32(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 4UL), cpu->r[0]); return 1; }
    if ((op & 0xff00U) == 0xc400U) { cpu->r[0] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read8(bus, cpu->gbr + (cv1k_u32)(op & 0xffU)), 8); return 1; }
    if ((op & 0xff00U) == 0xc500U) { cpu->r[0] = (cv1k_u32)sx((cv1k_u32)cv1k_bus_read16(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 2UL)), 16); return 1; }
    if ((op & 0xff00U) == 0xc600U) { cpu->r[0] = cv1k_bus_read32(bus, cpu->gbr + ((cv1k_u32)(op & 0xffU) * 4UL)); return 1; }

    /* ALU. */
    if ((op & 0xf00fU) == 0x300cU) { cpu->r[n] += cpu->r[m]; return 1; }
    if ((op & 0xf00fU) == 0x3008U) { cpu->r[n] -= cpu->r[m]; return 1; }
    if ((op & 0xf00fU) == 0x300eU) { tmp = cpu->r[n]; cpu->r[n] += cpu->r[m] + (cv1k_u32)get_t(cpu); set_t(cpu, cpu->r[n] < tmp || (get_t(cpu) && cpu->r[n] == tmp)); return 1; }
    if ((op & 0xf00fU) == 0x300aU) { tmp = cpu->r[n]; cpu->r[n] -= cpu->r[m] + (cv1k_u32)get_t(cpu); set_t(cpu, tmp < cpu->r[n] || (get_t(cpu) && tmp == cpu->r[n])); return 1; }
    if ((op & 0xf00fU) == 0x300fU) { tmp = cpu->r[n]; cpu->r[n] += cpu->r[m]; set_t(cpu, ((~(cpu->r[n] ^ cpu->r[m]) & (tmp ^ cpu->r[n])) & 0x80000000UL) != 0UL); return 1; } /* ADDV */
    if ((op & 0xf00fU) == 0x3000U) { set_t(cpu, cpu->r[n] == cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x3002U) { set_t(cpu, cpu->r[n] >= cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x3003U) { set_t(cpu, (cv1k_s32)cpu->r[n] >= (cv1k_s32)cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x3006U) { set_t(cpu, cpu->r[n] > cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x3007U) { set_t(cpu, (cv1k_s32)cpu->r[n] > (cv1k_s32)cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x2008U) { set_t(cpu, (cpu->r[n] & cpu->r[m]) == 0UL); return 1; }
    if ((op & 0xf00fU) == 0x2009U) { cpu->r[n] &= cpu->r[m]; return 1; }
    if ((op & 0xf00fU) == 0x200aU) { cpu->r[n] ^= cpu->r[m]; return 1; }
    if ((op & 0xf00fU) == 0x200bU) { cpu->r[n] |= cpu->r[m]; return 1; }
    if ((op & 0xf00fU) == 0x200cU) { tmp = cpu->r[n] ^ cpu->r[m]; set_t(cpu, ((tmp & 0xff000000UL) == 0UL) || ((tmp & 0x00ff0000UL) == 0UL) || ((tmp & 0x0000ff00UL) == 0UL) || ((tmp & 0x000000ffUL) == 0UL)); return 1; } /* CMP/STR */
    if ((op & 0xf00fU) == 0x200dU) { cpu->r[n] = (cpu->r[m] << 16) | ((cpu->r[n] >> 16) & 0xffffUL); return 1; }
    if ((op & 0xf00fU) == 0x2007U) { set_m(cpu, (cpu->r[n] & 0x80000000UL) != 0UL); set_q(cpu, (cpu->r[m] & 0x80000000UL) != 0UL); set_t(cpu, get_m(cpu) != get_q(cpu)); return 1; } /* DIV0S */
    if ((op & 0xf00fU) == 0x3004U) { div1(cpu, m, n); return 1; }
    if ((op & 0xf00fU) == 0x300bU) { tmp = cpu->r[n]; cpu->r[n] -= cpu->r[m]; set_t(cpu, (((tmp ^ cpu->r[m]) & (tmp ^ cpu->r[n])) & 0x80000000UL) != 0UL); return 1; } /* SUBV */

    if ((op & 0xf00fU) == 0x6007U) { cpu->r[n] = ~cpu->r[m]; return 1; }
    if ((op & 0xf00fU) == 0x6008U) { cpu->r[n] = (cpu->r[m] & 0xffff0000UL) | ((cpu->r[m] & 0x000000ffUL) << 8) | ((cpu->r[m] & 0x0000ff00UL) >> 8); return 1; }
    if ((op & 0xf00fU) == 0x6009U) { cpu->r[n] = (cpu->r[m] << 16) | ((cpu->r[m] >> 16) & 0xffffUL); return 1; }
    if ((op & 0xf00fU) == 0x600aU) { tmp = 0UL - cpu->r[m] - (cv1k_u32)get_t(cpu); set_t(cpu, (0UL < cpu->r[m]) || (get_t(cpu) && tmp == 0xffffffffUL)); cpu->r[n] = tmp; return 1; }
    if ((op & 0xf00fU) == 0x600bU) { cpu->r[n] = 0UL - cpu->r[m]; return 1; }
    if ((op & 0xf00fU) == 0x600cU) { cpu->r[n] = cpu->r[m] & 0xffUL; return 1; }
    if ((op & 0xf00fU) == 0x600dU) { cpu->r[n] = cpu->r[m] & 0xffffUL; return 1; }
    if ((op & 0xf00fU) == 0x600eU) { cpu->r[n] = (cv1k_u32)sx(cpu->r[m], 8); return 1; }
    if ((op & 0xf00fU) == 0x600fU) { cpu->r[n] = (cv1k_u32)sx(cpu->r[m], 16); return 1; }

    /* Multiply / MAC subset. */
    if ((op & 0xf00fU) == 0x0007U) { cpu->macl = (cv1k_u32)((cv1k_s32)cpu->r[n] * (cv1k_s32)cpu->r[m]); return 1; }
    if ((op & 0xf00fU) == 0x200eU) { cpu->macl = (cv1k_u32)((cpu->r[n] & 0xffffUL) * (cpu->r[m] & 0xffffUL)); return 1; }
    if ((op & 0xf00fU) == 0x200fU) { cpu->macl = (cv1k_u32)((cv1k_s32)(cv1k_s16)(cpu->r[n] & 0xffffUL) * (cv1k_s32)(cv1k_s16)(cpu->r[m] & 0xffffUL)); return 1; }
    if ((op & 0xf00fU) == 0x300dU) { mul_s32_to_u64(cpu->r[n], cpu->r[m], &cpu->mach, &cpu->macl); return 1; } /* DMULS.L */
    if ((op & 0xf00fU) == 0x3005U) { mul_u32_to_u64(cpu->r[n], cpu->r[m], &cpu->mach, &cpu->macl); return 1; } /* DMULU.L */

    /* Unary shifts/rotates/control. Use low-byte exact patterns. */
    if ((op & 0xf0ffU) == 0x4000U) { set_t(cpu, (cpu->r[n] >> 31) & 1UL); cpu->r[n] <<= 1; return 1; }
    if ((op & 0xf0ffU) == 0x4001U) { set_t(cpu, cpu->r[n] & 1UL); cpu->r[n] >>= 1; return 1; }
    if ((op & 0xf0ffU) == 0x4004U) { set_t(cpu, (cpu->r[n] >> 31) & 1UL); cpu->r[n] = (cpu->r[n] << 1) | (cpu->r[n] >> 31); return 1; }
    if ((op & 0xf0ffU) == 0x4005U) { set_t(cpu, cpu->r[n] & 1UL); cpu->r[n] = (cpu->r[n] >> 1) | (cpu->r[n] << 31); return 1; }
    if ((op & 0xf0ffU) == 0x4008U) { set_t(cpu, (cpu->r[n] >> 30) & 1UL); cpu->r[n] <<= 2; return 1; }
    if ((op & 0xf0ffU) == 0x4009U) { set_t(cpu, (cpu->r[n] >> 1) & 1UL); cpu->r[n] >>= 2; return 1; }
    if ((op & 0xf0ffU) == 0x4010U) { cpu->r[n]--; set_t(cpu, cpu->r[n] == 0UL); return 1; }
    if ((op & 0xf0ffU) == 0x4011U) { set_t(cpu, ((cv1k_s32)cpu->r[n]) >= 0L); return 1; }
    if ((op & 0xf0ffU) == 0x4015U) { set_t(cpu, ((cv1k_s32)cpu->r[n]) > 0L); return 1; }
    if ((op & 0xf0ffU) == 0x4018U) { cpu->r[n] <<= 8; return 1; }
    if ((op & 0xf0ffU) == 0x4019U) { cpu->r[n] >>= 8; return 1; }
    if ((op & 0xf0ffU) == 0x4020U) { set_t(cpu, (cpu->r[n] >> 31) & 1UL); cpu->r[n] <<= 1; return 1; }
    if ((op & 0xf0ffU) == 0x4021U) { set_t(cpu, cpu->r[n] & 1UL); cpu->r[n] = (cv1k_u32)(((cv1k_s32)cpu->r[n]) >> 1); return 1; }
    if ((op & 0xf0ffU) == 0x4024U) { tmp = cpu->r[n] >> 31; cpu->r[n] = (cpu->r[n] << 1) | (cv1k_u32)get_t(cpu); set_t(cpu, (int)tmp); return 1; }
    if ((op & 0xf0ffU) == 0x4025U) { tmp = cpu->r[n] & 1UL; cpu->r[n] = (cpu->r[n] >> 1) | ((cv1k_u32)get_t(cpu) << 31); set_t(cpu, (int)tmp); return 1; }
    if ((op & 0xf0ffU) == 0x4028U) { cpu->r[n] <<= 16; return 1; }
    if ((op & 0xf0ffU) == 0x4029U) { cpu->r[n] >>= 16; return 1; }
    if ((op & 0xf00fU) == 0x400cU) {
        /* SHAD Rm,Rn.  Match MAME sh34_base_device::SHAD exactly for
         * negative counts: counts that are a negative multiple of 32 produce
         * all sign bits, not a no-op shift by zero.  The previous sandbox
         * used ((0-count)&31), which is correct for -1..-31 but wrong for
         * -32, -64, etc.
         */
        if ((cpu->r[m] & 0x80000000UL) == 0UL) {
            cpu->r[n] <<= (cpu->r[m] & 0x1fUL);
        } else if ((cpu->r[m] & 0x1fUL) == 0UL) {
            cpu->r[n] = (cpu->r[n] & 0x80000000UL) ? 0xffffffffUL : 0UL;
        } else {
            cpu->r[n] = (cv1k_u32)((cv1k_s32)cpu->r[n] >> (((~cpu->r[m]) & 0x1fUL) + 1UL));
        }
        return 1;
    } /* SHAD */
    if ((op & 0xf00fU) == 0x400dU) {
        /* SHLD Rm,Rn.  Match MAME sh34_base_device::SHLD for the same
         * negative-multiple-of-32 edge case: logical result is zero.
         */
        if ((cpu->r[m] & 0x80000000UL) == 0UL) {
            cpu->r[n] <<= (cpu->r[m] & 0x1fUL);
        } else if ((cpu->r[m] & 0x1fUL) == 0UL) {
            cpu->r[n] = 0UL;
        } else {
            cpu->r[n] >>= (((~cpu->r[m]) & 0x1fUL) + 1UL);
        }
        return 1;
    } /* SHLD */

    /* System register transfers. */
    if ((op & 0xf0ffU) == 0x0002U) { cpu->r[n] = cpu->sr; return 1; }
    if ((op & 0xf0ffU) == 0x0012U) { cpu->r[n] = cpu->gbr; return 1; }
    if ((op & 0xf0ffU) == 0x0022U) { cpu->r[n] = cpu->vbr; return 1; }
    if ((op & 0xf0ffU) == 0x0032U) { cpu->r[n] = cpu->ssr; return 1; }
    if ((op & 0xf0ffU) == 0x0042U) { cpu->r[n] = cpu->spc; return 1; }
    if ((op & 0xf0ffU) == 0x000aU) { cpu->r[n] = cpu->mach; return 1; }
    if ((op & 0xf0ffU) == 0x001aU) { cpu->r[n] = cpu->macl; return 1; }
    if ((op & 0xf0ffU) == 0x002aU) { cpu->r[n] = cpu->pr; return 1; }
    if ((op & 0xf0ffU) == 0x400eU) { set_sr_full(cpu, cpu->r[n]); return 1; }
    if ((op & 0xf0ffU) == 0x401eU) { cpu->gbr = cpu->r[n]; return 1; }
    if ((op & 0xf0ffU) == 0x402eU) { cpu->vbr = cpu->r[n]; return 1; }
    if ((op & 0xf0ffU) == 0x403eU) { cpu->ssr = cpu->r[n]; return 1; }
    if ((op & 0xf0ffU) == 0x404eU) { cpu->spc = cpu->r[n]; return 1; }
    if ((op & 0xf0ffU) == 0x400aU) { cpu->mach = cpu->r[n]; return 1; }
    if ((op & 0xf0ffU) == 0x401aU) { cpu->macl = cpu->r[n]; return 1; }
    if ((op & 0xf0ffU) == 0x402aU) { cpu->pr = cpu->r[n]; return 1; }
    if ((op & 0xf0ffU) == 0x4003U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->sr); return 1; }
    if ((op & 0xf0ffU) == 0x4013U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->gbr); return 1; }
    if ((op & 0xf0ffU) == 0x4023U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->vbr); return 1; }
    if ((op & 0xf0ffU) == 0x4033U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->ssr); return 1; }
    if ((op & 0xf0ffU) == 0x4043U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->spc); return 1; }
    if ((op & 0xf0ffU) == 0x4002U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->mach); return 1; }
    if ((op & 0xf0ffU) == 0x4012U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->macl); return 1; }
    if ((op & 0xf0ffU) == 0x4022U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->pr); return 1; }

    /* SH-3 banked register transfers.  The DDPSDOJ boot code restores
     * exception/supervisor contexts with LDC.L @Rn+,Rm_BANK forms after the
     * FPGA/NAND setup phase.  In this scaffold rb[] represents the alternate
     * R0-R7 bank not currently visible through cpu->r[0..7]. */
    if ((op & 0xf08fU) == 0x4087U) { tmp = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; cpu->rb[(op >> 4) & 7U] = tmp; return 1; } /* LDC.L @Rn+,Rm_BANK */
    if ((op & 0xf08fU) == 0x408eU) { cpu->rb[(op >> 4) & 7U] = cpu->r[n]; return 1; } /* LDC Rn,Rm_BANK */
    if ((op & 0xf08fU) == 0x0082U) { cpu->r[n] = cpu->rb[(op >> 4) & 7U]; return 1; } /* STC Rm_BANK,Rn */
    if ((op & 0xf08fU) == 0x4083U) { cpu->r[n] -= 4UL; cv1k_bus_write32(bus, cpu->r[n], cpu->rb[(op >> 4) & 7U]); return 1; } /* STC.L Rm_BANK,@-Rn */
    if ((op & 0xf0ffU) == 0x4007U) { tmp = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; set_sr_full(cpu, tmp); return 1; }
    if ((op & 0xf0ffU) == 0x4017U) { cpu->gbr = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; return 1; }
    if ((op & 0xf0ffU) == 0x4027U) { cpu->vbr = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; return 1; }
    if ((op & 0xf0ffU) == 0x4037U) { cpu->ssr = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; return 1; }
    if ((op & 0xf0ffU) == 0x4047U) { cpu->spc = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; return 1; }
    if ((op & 0xf0ffU) == 0x4006U) { cpu->mach = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; return 1; }
    if ((op & 0xf0ffU) == 0x4016U) { cpu->macl = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; return 1; }
    if ((op & 0xf0ffU) == 0x4026U) { cpu->pr = cv1k_bus_read32(bus, cpu->r[n]); cpu->r[n] += 4UL; return 1; }

    if ((op & 0xf0ffU) == 0x0029U) { cpu->r[n] = get_t(cpu) ? 1UL : 0UL; return 1; }
    if ((op & 0xf0ffU) == 0x0039U) { cpu->r[n] = get_t(cpu) ? 0UL : 1UL; return 1; } /* MOVRT, SH2A but harmless */
    if ((op & 0xf0ffU) == 0x401bU) {
        /* TAS.B tests the original byte, then sets bit 7 atomically. */
        tmp = cv1k_bus_read8(bus, cpu->r[n]);
        set_t(cpu, (tmp & 0xffUL) == 0UL);
        tmp |= 0x80UL;
        cv1k_bus_write8(bus, cpu->r[n], (cv1k_u8)tmp);
        return 1;
    } /* TAS.B approximation */
    if ((op & 0xff00U) == 0xc300U) {
        /* MAME's SH3/SH7709S path stores TRA=imm<<2, SSR, SPC and SGR,
         * sets MD/RB/BL, exposes EXPEVT=0x160 and vectors to VBR+0x100.
         * The old scaffold kept TRAPA non-invasive for DDPSDOJ diagnostics;
         * --mame-trapa enables the MAME-faithful vectoring path for testing.
         */
        tmp = ((cv1k_u32)(op & 0xffU)) << 2;
        if (cv1k_bus_mame_trapa_enabled(bus)) {
            take_exception(cpu, bus, 0x00000160UL, tmp, cpu->vbr + 0x00000100UL);
        } else {
            cpu->tra = tmp;
            cv1k_bus_exception_ack(bus, 0x00000160UL, cpu->tra);
            cpu->exception_count++;
        }
        return 1;
    } /* TRAPA */
    if (op == 0x001bU) { cpu->halted = 1UL; return 1; } /* SLEEP */
    if (op == 0x0038U) { cv1k_bus_ldtlb(bus); return 1; } /* LDTLB */
    if ((op & 0xf0ffU) == 0x0083U) { cv1k_bus_cache_op(bus, cpu->r[n], CV1K_CACHEOP_PREF); return 1; } /* PREF @Rn */
    if ((op & 0xf0ffU) == 0x0093U) { cv1k_bus_cache_op(bus, cpu->r[n], CV1K_CACHEOP_OCBI); return 1; } /* OCBI @Rn */
    if ((op & 0xf0ffU) == 0x00a3U) { cv1k_bus_cache_op(bus, cpu->r[n], CV1K_CACHEOP_OCBP); return 1; } /* OCBP @Rn */
    if ((op & 0xf0ffU) == 0x00b3U) { cv1k_bus_cache_op(bus, cpu->r[n], CV1K_CACHEOP_OCBWB); return 1; } /* OCBWB @Rn */

    cpu->illegal_count += 1UL;
    cpu->last_illegal_pc = oldpc;
    cpu->last_illegal_op = (cv1k_u32)op;
    return 0;
}

void sh7709s_run(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 instructions)
{
    cv1k_u32 i;
    for (i = 0UL; i < instructions; i++) {
        sh7709s_step(cpu, bus);
    }
}
