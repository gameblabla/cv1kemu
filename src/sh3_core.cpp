/*
 * MAME-derived SH7709S (Hitachi SH-3) interpreter core for the CV1000 sandbox.
 *
 * The instruction semantics in src/sh_common_ops.inc are extracted verbatim
 * from MAME's src/devices/cpu/sh/sh.cpp (sh_common_execution interpreter, the
 * shared SH-2/3/4 opcode handlers and dispatch).  The SH-3 specific opcodes,
 * exception/interrupt acceptance and step loop below are ported from MAME's
 * sh4.cpp / sh4comn.cpp (sh34_base_device / sh3_base_device), with the DRC,
 * MMU and device framework removed.  The CV1000 SH-3 runs with the MMU
 * disabled, so memory accesses pass full virtual addresses straight to the
 * existing cv1k bus, which performs region translation as before.
 *
 * MAME: license:BSD-3-Clause  copyright-holders:David Haywood, Jesus Ramos, R. Belmont
 */

extern "C" {
#include "cv1k_types.h"
#include "cv1k_config.h"
#include "cpu_sh7709s.h"
#include "bus.h"
}

#include <stdint.h>
#include <string.h>

/* SR / FPSCR bit definitions (MAME names, from sh.h / sh4comn.h) */
#define SH_T   0x00000001
#define SH_S   0x00000002
#define SH_I   0x000000f0
#define SH_Q   0x00000100
#define SH_M   0x00000200
#define FD     0x00008000
#define BL     0x10000000
#define sRB    0x20000000
#define MD     0x40000000
#define SH34_AM    0x1fffffff
#define SH34_FLAGS (MD | sRB | BL | FD | SH_M | SH_Q | SH_I | SH_S | SH_T)

#define BUSY_LOOP_HACKS 0

#define REG_N  ((opcode >> 8) & 15)
#define REG_M  ((opcode >> 4) & 15)

#ifndef BIT
#define BIT(x,n) (((uint32_t)(x) >> (n)) & 1U)
#endif

/* MAME util::sext(value, bits) -> sign-extend low <bits> of value */
static inline uint32_t sh_sext(uint32_t value, unsigned bits)
{
    if (bits == 0 || bits >= 32) return value;
    uint32_t m = (uint32_t)1 << (bits - 1);
    value &= ((uint32_t)1 << bits) - 1;
    return (value ^ m) - m;
}

/* MAME bit rotate helpers (osd/util) */
static inline uint32_t rotl_32(uint32_t v, int n)
{
    n &= 31;
    return n ? ((v << n) | (v >> (32 - n))) : v;
}
static inline uint32_t rotr_32(uint32_t v, int n)
{
    n &= 31;
    return n ? ((v >> n) | (v << (32 - n))) : v;
}

class sh34_cpu {
public:
    struct sh7709s_cpu *st;
    struct cv1k_bus *bus;
    uint16_t cur_opcode;

    /* ---- memory accessors: full virtual address, bus does translation ---- */
    uint8_t  read_byte(uint32_t a)            { return cv1k_bus_read8(bus, a); }
    uint16_t read_word(uint32_t a)            { return cv1k_bus_read16(bus, a); }
    uint32_t read_long(uint32_t a)            { return cv1k_bus_read32(bus, a); }
    void     write_byte(uint32_t a, uint8_t d){ cv1k_bus_write8(bus, a, d); }
    void     write_word(uint32_t a, uint16_t d){ cv1k_bus_write16(bus, a, d); }
    void     write_long(uint32_t a, uint32_t d){ cv1k_bus_write32(bus, a, d); }

    /* ---- shared SH-2/3/4 interpreter (verbatim from MAME sh.cpp) ---- */
#define m_sh2_state st
#include "sh_common_ops.inc"
#undef m_sh2_state

    /* ---- SH-3 register bank handling (single non-active shadow in rb[]) ---- */
    void swap_bank()
    {
        for (int i = 0; i < 8; i++) {
            uint32_t t = st->r[i];
            st->r[i] = st->rb[i];
            st->rb[i] = t;
        }
    }

    /* ---- SH-3/4 specific opcodes (ported from MAME sh4.cpp) ---- */
    void ILLEGAL()
    {
        /* sh34 treats undefined opcodes as NOP; record for diagnostics */
        st->last_illegal_pc = st->ppc;
        st->last_illegal_op = cur_opcode;
        st->illegal_count++;
    }

    void TODO(const uint16_t) {}

    void LDTLB(const uint16_t)
    {
        cv1k_bus_ldtlb(bus); /* MMU disabled on CV1000; keep bus TLB hook */
    }

    void MOVCAL(const uint16_t opcode)
    {
        st->ea = st->r[REG_N];
        write_long(st->ea, st->r[0]);
    }

    void CLRS(const uint16_t) { st->sr &= ~SH_S; }
    void SETS(const uint16_t) { st->sr |=  SH_S; }

    void PREFM(const uint16_t)
    {
        /* CV1000 does not use the SH-4 store queues; prefetch is a NOP. */
    }

    /*  LDC Rm,SR */
    void LDCSR(const uint16_t opcode)
    {
        uint32_t reg = st->r[REG_N];
        if ((reg & sRB) != (st->sr & sRB))
            swap_bank();
        st->sr = reg & SH34_FLAGS;
    }

    /*  LDC.L @Rm+,SR */
    void LDCMSR(const uint16_t opcode)
    {
        uint32_t old = st->sr;
        st->ea = st->r[REG_N];
        st->sr = read_long(st->ea) & SH34_FLAGS;
        if ((old & sRB) != (st->sr & sRB))
            swap_bank();
        st->r[REG_N] += 4;
        st->icount -= 2;
    }

    /*  RTE */
    void RTE()
    {
        st->m_delay = st->ea = st->spc;
        if ((st->ssr & sRB) != (st->sr & sRB))
            swap_bank();
        st->sr = st->ssr;
        st->icount--;
    }

    /*  TRAPA #imm */
    void TRAPA(uint32_t i)
    {
        uint32_t imm = i & 0xff;
        st->tra = imm << 2;
        cv1k_bus_exception_ack(bus, 0x00000160UL, st->tra);
        st->ssr = st->sr;
        st->spc = st->pc;
        st->sgr = st->r[15];
        st->sr |= MD;
        if (!(st->sr & sRB)) swap_bank();
        st->sr |= sRB;
        st->sr |= BL;
        st->pc = st->vbr + 0x00000100UL;
        st->icount -= 7;
        st->exception_count++;
    }

    /*  STC Rm_BANK,Rn  / @-Rn */
    void STCRBANK(const uint16_t opcode)
    {
        st->r[REG_N] = st->rb[REG_M & 7];
    }
    void STCMRBANK(const uint16_t opcode)
    {
        uint32_t n = REG_N;
        st->r[n] -= 4;
        st->ea = st->r[n];
        write_long(st->ea, st->rb[REG_M & 7]);
        st->icount--;
    }
    void LDCRBANK(const uint16_t opcode)
    {
        st->rb[REG_M & 7] = st->r[REG_N];
    }
    void LDCMRBANK(const uint16_t opcode)
    {
        uint32_t n = REG_N;
        st->ea = st->r[n];
        st->rb[REG_M & 7] = read_long(st->ea);
        st->r[n] += 4;
    }

    /*  SGR / SSR / SPC moves */
    void STCSGR(const uint16_t opcode) { st->r[REG_N] = st->sgr; }
    void STCSSR(const uint16_t opcode) { st->r[REG_N] = st->ssr; }
    void STCSPC(const uint16_t opcode) { st->r[REG_N] = st->spc; }
    void LDCSSR(const uint16_t opcode) { st->ssr = st->r[REG_N]; }
    void LDCSPC(const uint16_t opcode) { st->spc = st->r[REG_N]; }

    void STCMSGR(const uint16_t opcode)
    { uint32_t n = REG_N; st->r[n] -= 4; st->ea = st->r[n]; write_long(st->ea, st->sgr); }
    void STCMSSR(const uint16_t opcode)
    { uint32_t n = REG_N; st->r[n] -= 4; st->ea = st->r[n]; write_long(st->ea, st->ssr); }
    void STCMSPC(const uint16_t opcode)
    { uint32_t n = REG_N; st->r[n] -= 4; st->ea = st->r[n]; write_long(st->ea, st->spc); }
    void LDCMSSR(const uint16_t opcode)
    { st->ea = st->r[REG_N]; st->ssr = read_long(st->ea); st->r[REG_N] += 4; }
    void LDCMSPC(const uint16_t opcode)
    { st->ea = st->r[REG_N]; st->spc = read_long(st->ea); st->r[REG_N] += 4; }

    /*  SHAD / SHLD (SH-3/4 dynamic shifts) */
    void SHAD(const uint16_t opcode)
    {
        uint32_t m = REG_M, n = REG_N;
        if ((st->r[m] & 0x80000000) == 0)
            st->r[n] = st->r[n] << (st->r[m] & 0x1F);
        else if ((st->r[m] & 0x1F) == 0)
            st->r[n] = (st->r[n] & 0x80000000) ? 0xFFFFFFFF : 0;
        else
            st->r[n] = (uint32_t)((int32_t)st->r[n] >> ((~st->r[m] & 0x1F) + 1));
    }
    void SHLD(const uint16_t opcode)
    {
        uint32_t m = REG_M, n = REG_N;
        if ((st->r[m] & 0x80000000) == 0)
            st->r[n] = st->r[n] << (st->r[m] & 0x1F);
        else if ((st->r[m] & 0x1F) == 0)
            st->r[n] = 0;
        else
            st->r[n] = st->r[n] >> ((~st->r[m] & 0x1F) + 1);
    }

    /* ---- group dispatch overrides (MAME sh34_base_device) ---- */
    void execute_one_f000(const uint16_t)
    {
        ILLEGAL(); /* SH7709S has no FPU */
    }

    void execute_one_0000(const uint16_t opcode)
    {
        switch (opcode & 0xff) {
        default: execute_one_0000_common(opcode); break;

        case 0x82: case 0x92: case 0xa2: case 0xb2:
        case 0xc2: case 0xd2: case 0xe2: case 0xf2: STCRBANK(opcode); break;

        case 0x32: STCSSR(opcode); break;
        case 0x42: STCSPC(opcode); break;
        case 0x83: PREFM(opcode);  break;
        case 0xc3: MOVCAL(opcode); break;

        case 0x38: case 0xb8: LDTLB(opcode); break;
        case 0x48: case 0xc8: CLRS(opcode);  break;
        case 0x58: case 0xd8: SETS(opcode);  break;
        case 0x3a: case 0xba: STCSGR(opcode); break;

        case 0x93: case 0xa3: case 0xb3: TODO(opcode); break;

        /* SH-4 only FPU/DBR -> illegal on SH-3 */
        case 0x5a: case 0xda: case 0x6a: case 0xea: case 0x7a: case 0xfa:
        case 0x52: case 0x62: case 0x43: case 0x63: case 0xe3:
        case 0x68: case 0xe8: case 0x4a: case 0xca:
            ILLEGAL(); break;
        }
    }

    void execute_one_4000(const uint16_t opcode)
    {
        switch (opcode & 0xff) {
        default: execute_one_4000_common(opcode); break;

        case 0x0c: case 0x1c: case 0x2c: case 0x3c: case 0x4c: case 0x5c:
        case 0x6c: case 0x7c: case 0x8c: case 0x9c: case 0xac: case 0xbc:
        case 0xcc: case 0xdc: case 0xec: case 0xfc: SHAD(opcode); break;

        case 0x0d: case 0x1d: case 0x2d: case 0x3d: case 0x4d: case 0x5d:
        case 0x6d: case 0x7d: case 0x8d: case 0x9d: case 0xad: case 0xbd:
        case 0xcd: case 0xdd: case 0xed: case 0xfd: SHLD(opcode); break;

        case 0x8e: case 0x9e: case 0xae: case 0xbe:
        case 0xce: case 0xde: case 0xee: case 0xfe: LDCRBANK(opcode); break;

        case 0x83: case 0x93: case 0xa3: case 0xb3:
        case 0xc3: case 0xd3: case 0xe3: case 0xf3: STCMRBANK(opcode); break;

        case 0x87: case 0x97: case 0xa7: case 0xb7:
        case 0xc7: case 0xd7: case 0xe7: case 0xf7: LDCMRBANK(opcode); break;

        /* SH-3 system register moves */
        case 0x32: STCMSGR(opcode); break;
        case 0x33: STCMSSR(opcode); break;
        case 0x37: LDCMSSR(opcode); break;
        case 0x3e: LDCSSR(opcode);  break;
        case 0x43: STCMSPC(opcode); break;
        case 0x47: LDCMSPC(opcode); break;
        case 0x4e: LDCSPC(opcode);  break;

        /* SH-4 only FPU/DBR -> illegal on SH-3 */
        case 0x52: case 0x56: case 0x5a:
        case 0x62: case 0x66: case 0x6a:
        case 0xf2: case 0xf6: case 0xfa:
        case 0x42: case 0x46: case 0x4a: case 0x53: case 0x57: case 0x5e:
        case 0x63: case 0x67: case 0x6e: case 0x82: case 0x86: case 0x8a:
        case 0x92: case 0x96: case 0x9a: case 0xa2: case 0xa6: case 0xaa:
        case 0xc2: case 0xc6: case 0xca: case 0xd2: case 0xd6: case 0xda:
        case 0xe2: case 0xe6: case 0xea:
            ILLEGAL(); break;
        }
    }

    /* ---- interrupt acceptance (preserves cv1k INTEVT2 / vbr+0x600 contract) ---- */
    bool check_pending_irq()
    {
        if (st->irq_pending == 0UL) return false;
        if ((st->sr & BL) != 0UL) return false;
        uint32_t level = st->irq_level & 0x0fUL;
        uint32_t line  = st->irq_line  & 0x0fUL;
        uint32_t mask  = (st->sr >> 4) & 0x0fUL;
        if (level == 0UL || level <= mask) return false;

        uint32_t event = st->irq_event ? st->irq_event : (0x600UL + line * 0x20UL);
        cv1k_bus_irq_ack(bus, level, event);

        st->spc = st->pc;
        st->ssr = st->sr;
        st->sgr = st->r[15];
        st->sr |= MD;
        if (!(st->sr & sRB)) swap_bank();
        st->sr |= sRB;
        st->sr |= BL;
        st->pc = st->vbr + 0x600UL;
        if (st->sleep_mode == 1UL) st->sleep_mode = 2UL;
        st->irq_pending = 0UL;
        st->irq_ack_count++;
        st->exception_count++;
        return true;
    }

    /* ---- single instruction step (MAME sh34_base_device::execute_run body) ---- */
    int step()
    {
        if (st->sleep_mode == 1UL) {
            /* power-down: only an interrupt can wake the core */
            check_pending_irq();
            st->cycles += 1UL;
            return 1;
        }

        st->ppc = st->pc;
        uint16_t opcode = cv1k_bus_fetch16(bus, st->pc);
        cur_opcode = opcode;

        if (st->m_delay) {
            st->pc = st->m_delay;
            st->m_delay = 0;
        } else {
            st->pc += 2;
        }

        st->icount = 0;
        execute_one(opcode);
        int consumed = 1 - (int)st->icount;   /* base 1 + handler decrements */
        st->cycles += (cv1k_u32)consumed;

        if (!st->m_delay)
            check_pending_irq();

        return consumed;
    }
};

/* ============================ C ABI ============================ */

extern "C" void sh7709s_reset(struct sh7709s_cpu *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->pc = 0xa0000000UL;       /* SH-3 reset vector (P2 boot area) */
    cpu->sr = 0x700000f0UL;       /* MD|RB|BL, IMASK=15 */
}

extern "C" void sh7709s_request_irq(struct sh7709s_cpu *cpu, int level)
{
    sh7709s_request_irq_line(cpu, level, level);
}

extern "C" void sh7709s_request_irq_line(struct sh7709s_cpu *cpu, int line, int priority)
{
    cpu->irq_pending = 1UL;
    cpu->irq_line = (cv1k_u32)line;
    cpu->irq_level = (cv1k_u32)priority;
    cpu->irq_event = 0UL;
}

extern "C" void sh7709s_request_irq_event(struct sh7709s_cpu *cpu, cv1k_u32 event, int priority)
{
    cpu->irq_pending = 1UL;
    cpu->irq_line = 0UL;
    cpu->irq_level = (cv1k_u32)priority;
    cpu->irq_event = event;
}

extern "C" void sh7709s_clear_irq_event(struct sh7709s_cpu *cpu, cv1k_u32 event)
{
    if (cpu == NULL) return;
    if (cpu->irq_pending != 0UL && cpu->irq_event == event) {
        cpu->irq_pending = 0UL;
        cpu->irq_event = 0UL;
        cpu->irq_line = 0UL;
        cpu->irq_level = 0UL;
    }
}

extern "C" int sh7709s_step(struct sh7709s_cpu *cpu, struct cv1k_bus *bus)
{
    sh34_cpu c;
    c.st = cpu;
    c.bus = bus;
    c.cur_opcode = 0;
    return c.step();
}

extern "C" void sh7709s_run(struct sh7709s_cpu *cpu, struct cv1k_bus *bus, cv1k_u32 instructions)
{
    sh34_cpu c;
    c.st = cpu;
    c.bus = bus;
    c.cur_opcode = 0;
    while (instructions-- > 0UL)
        c.step();
}
