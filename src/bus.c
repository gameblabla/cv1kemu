/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * CV1000 bus and SH7709S peripheral glue for this ANSI C sandbox.
 *
 * Portions are adapted from MAME's Cave CV1000 driver and SH-3 device code,
 * including CV1000 memory/port routing, PORT_J FPGA access, SH7709S internal
 * register behavior, TMU/INTC details, and bounded DMAC behavior.
 *
 * MAME sources include:
 *   mame-master/src/mame/cave/cv1k.cpp
 *   mame-master/src/devices/cpu/sh/sh3comn.*
 *   mame-master/src/devices/cpu/sh/sh4*.*
 *
 * MAME license: BSD-3-Clause.  See NOTICE and docs/MAME_DERIVED.md for
 * source-specific copyright attribution.
 */
#include "bus.h"
#include "emu.h"
#include "platform.h"
#include "sh3_jit/cv1k_sh3_c23_jit.h"
#include "sh3_jit/cv1k_ir.h"
#include <stddef.h>
#include <string.h>

static void shio_write_be32(struct cv1k_machine *m, cv1k_u32 off, cv1k_u32 value);

void cv1k_bus_bind(struct cv1k_bus *bus, struct cv1k_machine *machine)
{
    bus->machine = machine;
}

void cv1k_bus_irq_ack(struct cv1k_bus *bus, cv1k_u32 level, cv1k_u32 event)
{
    struct cv1k_machine *m;
    cv1k_u32 intevt;
    m = bus ? bus->machine : NULL;
    if (m == NULL) return;
    m->irq_last_level = level;
    m->irq_last_event = event;
    /* MAME's SH3 exception path maintains both INTEVT and INTEVT2.
     * INTEVT2 is the line/source event code from the SH7709S INTC map
     * (for example IRQ2 => 0x640, TUNI0 => 0x400), exposed by
     * intc_7709_map at physical 0x04000000.  For SH3 interrupts MAME also
     * updates CCN INTEVT at 0xffffffd8; line-style IRQ events >= 0x600 are
     * converted to the priority-coded 0x3e0 - priority * 0x20 value, while
     * lower internal-event codes are mirrored directly.  Earlier sandbox
     * builds only wrote INTEVT2, so code that read the CCN interrupt-cause
     * register saw stale zero.
     */
    shio_write_be32(m, 0x0000UL, event);
    if (event >= 0x600UL) intevt = 0x3e0UL - ((level & 0x0fUL) * 0x20UL);
    else intevt = event;
    shio_write_be32(m, 0xffd8UL, intevt);
}

void cv1k_bus_exception_ack(struct cv1k_bus *bus, cv1k_u32 event, cv1k_u32 tra)
{
    struct cv1k_machine *m;
    cv1k_u32 idx;
    m = bus ? bus->machine : NULL;
    if (m == NULL) return;
    m->exception_events++;
    m->exception_last_event = event;
    m->exception_last_tra = tra;
    /* SH-3/SH-4 diagnostic convention: EXPEVT and TRA live in the high
     * internal register window.  This sandbox stores them in the same
     * permissive P4 shadow used for INTEVT2 so boot code and traces can see
     * the last exception cause deterministically.
     */
    idx = 0xffd4UL & (CV1K_REGION_SH_IO_SIZE - 1UL);
    m->sh_io[idx] = (cv1k_u8)((event >> 24) & 0xffUL);
    m->sh_io[(idx + 1UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)((event >> 16) & 0xffUL);
    m->sh_io[(idx + 2UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)((event >> 8) & 0xffUL);
    m->sh_io[(idx + 3UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)(event & 0xffUL);
    idx = 0xffd0UL & (CV1K_REGION_SH_IO_SIZE - 1UL);
    m->sh_io[idx] = (cv1k_u8)((tra >> 24) & 0xffUL);
    m->sh_io[(idx + 1UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)((tra >> 16) & 0xffUL);
    m->sh_io[(idx + 2UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)((tra >> 8) & 0xffUL);
    m->sh_io[(idx + 3UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)(tra & 0xffUL);
}

int cv1k_bus_mame_trapa_enabled(struct cv1k_bus *bus)
{
    struct cv1k_machine *m;
    m = bus ? bus->machine : NULL;
    if (m == NULL) return 0;
    return m->mame_trapa ? 1 : 0;
}

static CV1K_ALWAYS_INLINE int in_range(cv1k_u32 addr, cv1k_u32 base, cv1k_u32 size)
{
    return addr >= base && addr < (base + size);
}

static CV1K_ALWAYS_INLINE int sh_addr_is_p2_uncached(cv1k_u32 addr)
{
    return addr >= 0xa0000000UL && addr <= 0xbfffffffUL;
}

static int cv1k_is_physical_device_window(cv1k_u32 addr)
{
    if (in_range(addr, CV1K_ADDR_SH_IO, CV1K_REGION_SH_IO_SIZE)) return 1;
    if (in_range(addr, CV1K_ADDR_NAND_IO, CV1K_REGION_IO_SIZE)) return 1;
    if (in_range(addr, CV1K_ADDR_YMZ770, CV1K_REGION_IO_SIZE)) return 1;
    if (in_range(addr, CV1K_ADDR_RTC_EE, CV1K_REGION_IO_SIZE)) return 1;
    if (in_range(addr, CV1K_ADDR_BLITTER, CV1K_REGION_BLITTER_SIZE)) return 1;
    if (in_range(addr, CV1K_ADDR_CACHE, CV1K_CACHE_RAM_SIZE)) return 1;
    return 0;
}

static CV1K_HOT cv1k_u32 cpu_addr_translate(struct cv1k_machine *m, cv1k_u32 addr);
static void mame_cache_meta_access(struct cv1k_machine *m, cv1k_u32 vaddr, cv1k_u32 phys, int write, int fetch);
static void icache_invalidate_line(struct cv1k_machine *m, cv1k_u32 phys);
static void dcache_invalidate_line_public(struct cv1k_machine *m, cv1k_u32 phys);
static void dcache_dma_write8(struct cv1k_machine *m, cv1k_u32 phys, cv1k_u8 data);
static cv1k_u8 cv1k_bus_dma_read8(struct cv1k_machine *m, cv1k_u32 addr);
static void cv1k_bus_dma_write8(struct cv1k_machine *m, cv1k_u32 addr, cv1k_u8 data);
static void cv1k_bus_invalidate_dcache_all(struct cv1k_bus *bus);
static cv1k_u32 tlb_page_size_from_ptel(cv1k_u32 ptel);

static cv1k_u32 shio_read_be32(struct cv1k_machine *m, cv1k_u32 off)
{
    cv1k_u32 idx;
    idx = off & (CV1K_REGION_SH_IO_SIZE - 1UL);
    return ((cv1k_u32)m->sh_io[idx] << 24) | ((cv1k_u32)m->sh_io[(idx + 1UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] << 16) | ((cv1k_u32)m->sh_io[(idx + 2UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] << 8) | (cv1k_u32)m->sh_io[(idx + 3UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)];
}

static void shio_write_be32(struct cv1k_machine *m, cv1k_u32 off, cv1k_u32 value)
{
    cv1k_u32 idx;
    idx = off & (CV1K_REGION_SH_IO_SIZE - 1UL);
    m->sh_io[idx] = (cv1k_u8)((value >> 24) & 0xffUL);
    m->sh_io[(idx + 1UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)((value >> 16) & 0xffUL);
    m->sh_io[(idx + 2UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)((value >> 8) & 0xffUL);
    m->sh_io[(idx + 3UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)(value & 0xffUL);
}


static cv1k_u16 shio_read_be16(struct cv1k_machine *m, cv1k_u32 off)
{
    cv1k_u32 idx;
    idx = off & (CV1K_REGION_SH_IO_SIZE - 1UL);
    return (cv1k_u16)(((cv1k_u16)m->sh_io[idx] << 8) | (cv1k_u16)m->sh_io[(idx + 1UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)]);
}

static void shio_write_be16(struct cv1k_machine *m, cv1k_u32 off, cv1k_u16 value)
{
    cv1k_u32 idx;
    idx = off & (CV1K_REGION_SH_IO_SIZE - 1UL);
    m->sh_io[idx] = (cv1k_u8)((value >> 8) & 0xffU);
    m->sh_io[(idx + 1UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)] = (cv1k_u8)(value & 0xffU);
}

static cv1k_u32 tmu_tcor_off(int ch)
{
    if (ch == 0) return 0xfe94UL;
    if (ch == 1) return 0xfea0UL;
    return 0xfeacUL;
}

static cv1k_u32 tmu_tcnt_off(int ch)
{
    if (ch == 0) return 0xfe98UL;
    if (ch == 1) return 0xfea4UL;
    return 0xfeb0UL;
}

static cv1k_u32 tmu_tcr_off(int ch)
{
    if (ch == 0) return 0xfe9cUL;
    if (ch == 1) return 0xfea8UL;
    return 0xfeb4UL;
}

static int tmu_channel_from_off(cv1k_u32 off)
{
    off &= 0xffffUL;
    if (off >= 0xfe94UL && off <= 0xfe9dUL) return 0;
    if (off >= 0xfea0UL && off <= 0xfea9UL) return 1;
    if (off >= 0xfeacUL && off <= 0xfebbUL) return 2;
    return -1;
}

static cv1k_u32 tmu_event_for_channel(int ch)
{
    if (ch == 0) return 0x400UL;
    if (ch == 1) return 0x420UL;
    return 0x440UL;
}

static cv1k_u32 tmu_priority_for_channel(struct cv1k_machine *m, int ch)
{
    cv1k_u16 ipra;
    ipra = shio_read_be16(m, 0xfee2UL);
    if (ch == 0) return (cv1k_u32)((ipra >> 12) & 0x0fU);
    if (ch == 1) return (cv1k_u32)((ipra >> 8) & 0x0fU);
    return (cv1k_u32)((ipra >> 4) & 0x0fU);
}

static void dmac_complete_timers(struct cv1k_machine *m);
static void dmac_schedule_completion(struct cv1k_machine *m, cv1k_u32 base, cv1k_u32 chcr, cv1k_u32 transfers);

static CV1K_HOT void tmu_update_channel(struct cv1k_machine *m, int ch)
{
    static const cv1k_u32 divs[8] = { 4UL, 16UL, 64UL, 256UL, 1024UL, 1UL, 1UL, 1UL };
    cv1k_u8 tstr;
    cv1k_u32 now;
    cv1k_u32 last;
    cv1k_u32 delta;
    cv1k_u32 ticks;
    cv1k_u32 div;
    cv1k_u32 tcnt;
    cv1k_u32 tcor;
    cv1k_u32 underflows;
    cv1k_u16 tcr;
    cv1k_u32 event;
    cv1k_u32 pri;
    if (m == NULL || ch < 0 || ch > 2) return;
    tstr = m->sh_io[0xfe92UL & (CV1K_REGION_SH_IO_SIZE - 1UL)];
    now = m->cpu.cycles;
    last = m->tmu_last_cycles[ch];
    if ((tstr & (1U << ch)) == 0U) { m->tmu_last_cycles[ch] = now; return; }
    if (last == 0UL) { m->tmu_last_cycles[ch] = now; return; }
    delta = now - last;
    tcr = shio_read_be16(m, tmu_tcr_off(ch));
    div = divs[tcr & 7U] * CV1K_TMU_PCLK_DIV;
    ticks = delta / div;
    if (ticks == 0UL) return;
    m->tmu_last_cycles[ch] = last + ticks * div;
    tcnt = shio_read_be32(m, tmu_tcnt_off(ch));
    tcor = shio_read_be32(m, tmu_tcor_off(ch));

    underflows = 0UL;
    if (tcnt >= ticks) {
        tcnt -= ticks;
    } else {
        ticks -= (tcnt + 1UL);
        underflows = 1UL;
        if (tcor == 0xffffffffUL) {
            tcnt = 0xffffffffUL - ticks;
        } else {
            cv1k_u32 period;
            period = tcor + 1UL;
            underflows += ticks / period;
            ticks %= period;
            tcnt = tcor - ticks;
        }
        tcr = (cv1k_u16)(tcr | 0x0100U);
        shio_write_be16(m, tmu_tcr_off(ch), tcr);
        m->tmu_underflows[ch] += underflows;
        event = tmu_event_for_channel(ch);
        pri = tmu_priority_for_channel(m, ch);
        m->tmu_last_event = event;
        m->tmu_last_priority = pri;
        /* The SH7709S underflow bit is level/sticky until software clears TCR.
         * Collapsing batched underflows to one pending event is equivalent at
         * the interrupt controller and avoids reposting the same event thousands
         * of times while the CPU is catching up. */
        if (m->mame_tmu_irq && (tcr & 0x0020U) != 0U && pri != 0UL) sh7709s_request_irq_event(&m->cpu, event, (int)pri);
    }
    shio_write_be32(m, tmu_tcnt_off(ch), tcnt);
}

CV1K_HOT void cv1k_bus_tmu_tick(struct cv1k_bus *bus)
{
    struct cv1k_machine *m;
    m = bus ? bus->machine : NULL;
    if (m == NULL) return;
    dmac_complete_timers(m);
    tmu_update_channel(m, 0);
    tmu_update_channel(m, 1);
    tmu_update_channel(m, 2);
}

CV1K_HOT cv1k_u32 cv1k_bus_cycles_until_event(struct cv1k_bus *bus)
{
    struct cv1k_machine *m;
    cv1k_u32 best = 0xffffffffUL;
    cv1k_u32 now;
    cv1k_u32 mask;
    int ch;
    static const cv1k_u32 divs[8] = { 4UL, 16UL, 64UL, 256UL, 1024UL, 1UL, 1UL, 1UL };

    m = bus ? bus->machine : NULL;
    if (m == NULL) return best;
    now = m->cpu.cycles;

    mask = m->dma_timer_mask;
    while (mask != 0UL) {
        cv1k_u32 bit = mask & (0UL - mask);
        cv1k_u32 dch = (cv1k_u32)__builtin_ctz(mask);
        cv1k_u32 due = m->dma_timer_due[dch];
        cv1k_u32 delta = ((cv1k_s32)(due - now) <= 0) ? 0UL : (due - now);
        if (delta < best) best = delta;
        mask ^= bit;
    }

    for (ch = 0; ch < 3; ch++) {
        cv1k_u8 tstr = m->sh_io[0xfe92UL & (CV1K_REGION_SH_IO_SIZE - 1UL)];
        cv1k_u16 tcr;
        cv1k_u32 div;
        cv1k_u32 tcnt;
        cv1k_u32 delta;
        if ((tstr & (1U << ch)) == 0U) continue;
        tcr = shio_read_be16(m, tmu_tcr_off(ch));
        div = divs[tcr & 7U] * CV1K_TMU_PCLK_DIV;
        tcnt = shio_read_be32(m, tmu_tcnt_off(ch));
        delta = (tcnt == 0xffffffffUL) ? 0xffffffffUL : ((tcnt + 1UL) * div);
        if (delta < best) best = delta;
    }
    return best;
}

static cv1k_u32 dmac_transfer_size_from_chcr(cv1k_u32 chcr)
{
    /* MAME sh4dmac.cpp uses the SH-3 transfer-size table for SH7709S:
     *   sh3_dmasize[(CHCR >> 3) & 3] = { 1, 2, 4, 16 } bytes.
     * Earlier sandbox builds always transferred single bytes.  That happened
     * to match the observed DDPSDOJ NAND CHCR 00004421 path, but it was not
     * a faithful SH7709S DMAC model for later RAM/RAM and device transfers.
     */
    static const cv1k_u32 sh3_dmasize[4] = { 1UL, 2UL, 4UL, 16UL };
    return sh3_dmasize[(chcr >> 3) & 3UL];
}

static void dmac_pre_adjust(cv1k_u32 *addr, cv1k_u32 mode, cv1k_u32 unit)
{
    /* MAME pre-decrements for decrementing source/destination modes, then
     * performs the access, then post-increments for incrementing modes.
     * Mode 3 is invalid in MAME; leave it fixed in this standalone scaffold.
     */
    if (mode == 2UL) *addr -= unit;
}

static void dmac_post_adjust(cv1k_u32 *addr, cv1k_u32 mode, cv1k_u32 unit)
{
    if (mode == 1UL) *addr += unit;
}

static cv1k_u32 dmac_physical_am(cv1k_u32 addr)
{
    /* SH34_AM in MAME is the 29-bit physical-address mask used by SH-3/SH-4
     * DMA transfers after P1/P2 alias stripping.
     */
    return addr & 0x1fffffffUL;
}

static cv1k_u32 dmac_align_for_unit(cv1k_u32 addr, cv1k_u32 unit)
{
    /* MAME sh4dmac.cpp aligns SH-3 DMA source and destination addresses
     * before 16-bit, 32-bit and 16-byte transfers.  Earlier sandbox builds
     * only masked through SH34_AM and then copied byte-by-byte, which made
     * misaligned word/long/line DMAs behave differently from MAME.
     */
    if (unit == 2UL) return addr & ~1UL;
    if (unit == 4UL) return addr & ~3UL;
    if (unit == 16UL) return addr & ~15UL;
    return addr;
}

static void dmac_complete_timers(struct cv1k_machine *m)
{
    cv1k_u32 mask;
    cv1k_u32 now;
    if (m == NULL) return;
    mask = m->dma_timer_mask;
    if (CV1K_LIKELY(mask == 0UL)) return;
    now = m->cpu.cycles;
    while (mask != 0UL) {
        cv1k_u32 bit = mask & (0UL - mask);
        cv1k_u32 ch = (cv1k_u32)__builtin_ctz(mask);
        if ((cv1k_s32)(now - m->dma_timer_due[ch]) >= 0) {
            cv1k_u32 base = m->dma_timer_base[ch];
            cv1k_u32 chcr = m->dma_timer_chcr[ch];
            shio_write_be32(m, base + 0x08UL, 0UL);
            shio_write_be32(m, base + 0x0cUL, (chcr & ~1UL) | 2UL);
            m->dma_timer_active[ch] = 0UL;
            m->dma_timer_mask &= ~bit;
        }
        mask ^= bit;
    }
}

static void dmac_schedule_completion(struct cv1k_machine *m, cv1k_u32 base, cv1k_u32 chcr, cv1k_u32 transfers)
{
    cv1k_u32 ch;
    cv1k_u32 delay;
    if (m == NULL) return;
    ch = (base - 0x20UL) >> 4;
    if (ch >= 4UL) return;
    /* MAME sh4_dma_transfer(channel, timermode=1) performs the memory
     * transfer immediately but does not set CHCR.TE/clear DMATCR until the
     * DMA timer callback at 2 * count + 1 cycles.  Cap only enormous waits
     * so the standalone harness remains usable.
     */
    delay = (transfers > 0x7fffffUL) ? 0x00ffffffUL : (transfers * 2UL + 1UL);
    if (delay < 2UL) delay = 2UL;
    m->dma_timer_active[ch] = 1UL;
    m->dma_timer_mask |= (1UL << ch);
    m->dma_timer_due[ch] = m->cpu.cycles + delay;
    m->dma_timer_chcr[ch] = chcr;
    m->dma_timer_base[ch] = base;
}

static int dmac_try_fast_nand_data_to_ram(struct cv1k_machine *m, cv1k_u32 *sar, cv1k_u32 *dar, cv1k_u32 tcr, cv1k_u32 src_mode, cv1k_u32 dst_mode, cv1k_u32 unit)
{
    cv1k_u32 src_phys;
    cv1k_u32 dst_phys;
    cv1k_u32 i;
    cv1k_u32 max_ram;
    cv1k_u8 data;

    if (m == NULL || sar == NULL || dar == NULL) return 0;
    if (unit != 1UL || src_mode != 0UL || dst_mode != 1UL) return 0;

    src_phys = dmac_physical_am(*sar);
    dst_phys = dmac_physical_am(*dar);
    if (src_phys != CV1K_ADDR_NAND_IO) return 0;

    max_ram = 0UL;
    if (in_range(dst_phys, CV1K_ADDR_WORK_RAM, m->main_ram_size)) {
        max_ram = (CV1K_ADDR_WORK_RAM + m->main_ram_size) - dst_phys;
        if (max_ram > tcr) max_ram = tcr;
    }

    /* v38: --mame-full-dmatcr uses MAME's 0x1000000 zero-DMATCR count.
     * This fast path keeps that diagnostic tractable for CV1000's common
     * fixed NAND data-port -> incrementing RAM DMA shape while avoiding
     * per-byte address translation.  Reads still go through the MAME-derived
     * NAND state machine, including its non-sequential-row behavior.  Writes
     * beyond mapped work RAM are ignored, matching the effective behavior of
     * MAME's address map.
     */
    for (i = 0UL; i < tcr; i++) {
        data = cv1k_nand_data_r(&m->nand);
        if (i < max_ram) {
            cv1k_u32 phys;
            phys = dst_phys + i;
            if (m->dcache_enabled) dcache_dma_write8(m, phys, data);
            else m->main_ram[phys - CV1K_ADDR_WORK_RAM] = data;
            if (m->dma_cache_sync) {
                if ((phys & (CV1K_DCACHE_LINE_SIZE - 1UL)) == 0UL) {
                    dcache_invalidate_line_public(m, phys);
                    m->dma_cache_invalidations++;
                }
                if ((phys & 0x1ffUL) == 0UL) {
                    icache_invalidate_line(m, phys);
                    m->dma_cache_invalidations++;
                }
            }
        }
    }

    *dar += tcr;
    return 1;
}

static CV1K_HOT void maybe_sh_dma(struct cv1k_machine *m, cv1k_u32 off)
{
    cv1k_u32 base;
    cv1k_u32 sar;
    cv1k_u32 dar;
    cv1k_u32 start_sar;
    cv1k_u32 start_dar;
    cv1k_u32 tcr;
    cv1k_u32 chcr;
    cv1k_u32 status;
    cv1k_u32 i;
    cv1k_u32 src_mode;
    cv1k_u32 dmaor;
    cv1k_u32 rs;
    cv1k_u32 dst_mode;
    cv1k_u32 unit;
    cv1k_u8 data;
    cv1k_u32 nand_page0;
    cv1k_u32 nand_page1;
    cv1k_u32 nand_block0;
    cv1k_u32 nand_block1;
    cv1k_u32 nand_col0;
    cv1k_u32 nand_col1;
    dmac_complete_timers(m);
    if (off == 0x61UL) {
        /* MAME calls sh4_dmac_check for all channels when DMAOR is written. */
        maybe_sh_dma(m, 0x2fUL);
        maybe_sh_dma(m, 0x3fUL);
        maybe_sh_dma(m, 0x4fUL);
        maybe_sh_dma(m, 0x5fUL);
        return;
    }
    if ((off & 0x0fUL) != 0x0fUL) return;
    base = off & ~0x0fUL;
    if (base != 0x20UL && base != 0x30UL && base != 0x40UL && base != 0x50UL) return;
    chcr = shio_read_be32(m, base + 0x0cUL);
    dmaor = ((cv1k_u32)m->sh_io[0x60] << 8) | (cv1k_u32)m->sh_io[0x61];
    if ((chcr & dmaor & 1UL) == 0UL) return;
    rs = (chcr >> 8) & 0x0fUL;
    if (rs < 2UL || rs > 6UL) return;
    if ((chcr & 2UL) != 0UL) return;
    if ((dmaor & 6UL) != 0UL) return;
    sar = shio_read_be32(m, base + 0x00UL);
    dar = shio_read_be32(m, base + 0x04UL);
    tcr = shio_read_be32(m, base + 0x08UL);
    src_mode = (chcr >> 12) & 3UL;
    dst_mode = (chcr >> 14) & 3UL;
    unit = 1UL;
    unit = dmac_transfer_size_from_chcr(chcr);
    if (tcr == 0UL) {
        /* MAME treats zero DMATCR as 0x1000000 transfers.  The conservative
         * default keeps v37's finite harness bound because the standalone
         * core still lacks MAME's asynchronous DMA timer/scheduler;
         * --mame-full-dmatcr enables the exact count with the fast NAND path.
         */
        tcr = m->mame_full_dmatcr ? 0x1000000UL : 0x40000UL;
    }
    if (!m->mame_full_dmatcr && tcr > 0x40000UL) tcr = 0x40000UL;

    start_sar = sar;
    start_dar = dar;
    nand_page0 = cv1k_nand_current_page(&m->nand);
    nand_block0 = cv1k_nand_current_block(&m->nand);
    nand_col0 = m->nand.column;
    sar = (sar & ~0x1fffffffUL) | dmac_align_for_unit(dmac_physical_am(sar), unit);
    dar = (dar & ~0x1fffffffUL) | dmac_align_for_unit(dmac_physical_am(dar), unit);
    if (!dmac_try_fast_nand_data_to_ram(m, &sar, &dar, tcr, src_mode, dst_mode, unit)) {
        for (i = 0UL; i < tcr; i++) {
            cv1k_u32 j;
            cv1k_u32 src_access;
            cv1k_u32 dst_access;
            dmac_pre_adjust(&sar, src_mode, unit);
            dmac_pre_adjust(&dar, dst_mode, unit);
            src_access = sar & 0x1fffffffUL;
            dst_access = dar & 0x1fffffffUL;
            for (j = 0UL; j < unit; j++) {
                data = cv1k_bus_dma_read8(m, src_access + j);
                cv1k_bus_dma_write8(m, dst_access + j, data);
            }
            dmac_post_adjust(&sar, src_mode, unit);
            dmac_post_adjust(&dar, dst_mode, unit);
        }
    }
    nand_page1 = cv1k_nand_current_page(&m->nand);
    nand_block1 = cv1k_nand_current_block(&m->nand);
    nand_col1 = m->nand.column;
    m->dma_transfers++;
    m->dma_bytes += tcr * unit;
    m->last_dma_sar = start_sar;
    m->last_dma_dar = start_dar;
    m->last_dma_tcr = tcr;
    m->last_dma_chcr = chcr;
    m->last_dma_src_mode = src_mode;
    m->last_dma_dst_mode = dst_mode;
    status = (chcr & ~1UL) | 2UL;
    m->last_dma_status = status;
    m->last_dma_nand_page0 = nand_page0;
    m->last_dma_nand_page1 = nand_page1;
    m->last_dma_nand_block0 = nand_block0;
    m->last_dma_nand_block1 = nand_block1;
    m->last_dma_nand_col0 = nand_col0;
    m->last_dma_nand_col1 = nand_col1;
    /* MAME's sh4_dmac_check passes SAR/DAR/DMATCR by local copy for
     * memory-to-memory RS modes and only the completion callback clears
     * DMATCR / sets CHCR.TE.  Do not write back SAR/DAR for RS > 3, and do
     * not expose TE immediately; schedule it through the small cycle timer.
     */
    if (rs <= 3UL) {
        shio_write_be32(m, base + 0x00UL, sar);
        shio_write_be32(m, base + 0x04UL, dar);
        shio_write_be32(m, base + 0x08UL, 0UL);
        shio_write_be32(m, base + 0x0cUL, status);
    } else {
        dmac_schedule_completion(m, base, chcr, tcr);
    }
}

static CV1K_HOT cv1k_u8 sh_io_port_r(struct cv1k_machine *m, cv1k_u32 off)
{
    int tmu_ch;
    dmac_complete_timers(m);
    tmu_ch = tmu_channel_from_off(off);
    if (tmu_ch >= 0) tmu_update_channel(m, tmu_ch);
    /* MAME's sh3_internal_map exposes the actual SH7709 port data
     * registers in the physical internal register window.  The callback
     * numbers below (SH3_PORT_C = 0x12*8, etc.) are only the AS_IO routing
     * IDs used by sh3comn.cpp once PCDR/PDDR/PEDR/PFDR/PJDR/PLDR are read.
     */
    switch (off) {
    case 0x124UL: return cv1k_bus_port_read(&m->bus, 'C'); /* PCDR */
    case 0x126UL: return cv1k_bus_port_read(&m->bus, 'D'); /* PDDR */
    case 0x128UL: return cv1k_bus_port_read(&m->bus, 'E'); /* PEDR */
    case 0x12aUL: return cv1k_bus_port_read(&m->bus, 'F'); /* PFDR */
    case 0x130UL: return cv1k_video_fpga_read(&m->video);  /* PJDR */
    case 0x134UL: return cv1k_bus_port_read(&m->bus, 'L'); /* PLDR */
    default: break;
    }
    /* MAME's sh3comn.h exposes the SH-3 port callbacks at arbitrary AS_IO
     * offsets: C=0x90, D=0x98, E=0xa0, F=0xa8, J=0xc0, L=0xd0.
     * CV1000 maps PORT_J to the FPGA serial-loading port.  Earlier sandbox
     * builds accidentally routed PORT_J through the blitter register reader,
     * while also keeping a private 0x128/0x130 FPGA alias.  Match MAME's
     * port_map here and keep the old alias below only as a compatibility
     * fallback for traces produced by earlier sandbox assumptions.
     */
    switch (off & ~7UL) {
    case 0x90UL: return cv1k_bus_port_read(&m->bus, 'C');
    case 0x98UL: return cv1k_bus_port_read(&m->bus, 'D');
    case 0xa0UL: return cv1k_bus_port_read(&m->bus, 'E');
    case 0xa8UL: return cv1k_bus_port_read(&m->bus, 'F');
    case 0xc0UL: return cv1k_video_fpga_read(&m->video);
    case 0xd0UL: return cv1k_bus_port_read(&m->bus, 'L');
    default: break;
    }
    /* DMAOR is mapped at 0x04000060-0x04000061 in MAME's SH7709S map.
     * Earlier sandbox builds treated these bytes as a generic ready poll and
     * forced zero, which hid the DMA master-enable bit from code that reads
     * the controller back.
     */
    return m->sh_io[off & (CV1K_REGION_SH_IO_SIZE - 1UL)];
}

static CV1K_HOT void sh_io_port_w(struct cv1k_machine *m, cv1k_u32 off, cv1k_u8 data)
{
    int tmu_ch;
    dmac_complete_timers(m);
    if (off == 0xfe92UL) {
        tmu_update_channel(m, 0);
        tmu_update_channel(m, 1);
        tmu_update_channel(m, 2);
    } else {
        tmu_ch = tmu_channel_from_off(off);
        if (tmu_ch >= 0) tmu_update_channel(m, tmu_ch);
    }
    switch (off & ~7UL) {
    case 0xc0UL:
        /* MAME cv1k_state::port_map maps SH3_PORT_J directly to
         * cv1k_blitter_device::fpga_w.  Program-space blitter registers are
         * separately installed at 0x18000000-0x18000057.
         */
        cv1k_video_fpga_write(&m->video, data);
        m->sh_io[off & (CV1K_REGION_SH_IO_SIZE - 1UL)] = data;
        return;
    default:
        break;
    }
    if (off == 0x130UL) {
        cv1k_video_fpga_write(&m->video, data);
        m->sh_io[off & (CV1K_REGION_SH_IO_SIZE - 1UL)] = data;
        return;
    }
    m->sh_io[off & (CV1K_REGION_SH_IO_SIZE - 1UL)] = data;
    if (off == 0xfe92UL) {
        cv1k_u32 now;
        now = m->cpu.cycles;
        if ((data & 1U) != 0U) m->tmu_last_cycles[0] = now;
        if ((data & 2U) != 0U) m->tmu_last_cycles[1] = now;
        if ((data & 4U) != 0U) m->tmu_last_cycles[2] = now;
    } else {
        tmu_ch = tmu_channel_from_off(off);
        if (tmu_ch >= 0) {
            m->tmu_last_cycles[tmu_ch] = m->cpu.cycles;
            if ((off & ~1UL) == tmu_tcr_off(tmu_ch)) {
                cv1k_u16 tcr_now;
                cv1k_u32 event;
                /* MAME sh4tmu.cpp unrequests a TUNI interrupt when TCR.TIE
                 * or TCR.UNF becomes clear after a TCR write.  v39-v43 set
                 * the underflow flag and could request the optional TMU IRQ,
                 * but never modeled the corresponding acknowledge/cancel path.
                 */
                tcr_now = shio_read_be16(m, tmu_tcr_off(tmu_ch));
                event = tmu_event_for_channel(tmu_ch);
                if ((tcr_now & 0x0020U) == 0U || (tcr_now & 0x0100U) == 0U) sh7709s_clear_irq_event(&m->cpu, event);
            }
        }
    }
    if (off >= 0xffe0UL && off <= 0xffffUL) {
        /* v43: match MAME's SH3 CCN/MMU register map.  Earlier sandbox
         * revisions accidentally used 0xffff_ff60/64/70, which are BSC
         * wait-state registers (BCR2/WCR1/RTCOR), as PTEH/PTEL/MMUCR.
         * MAME sh3_base_device::ccn_map uses:
         *   MMUCR 0xffff_ffe0, CCR 0xffff_ffec,
         *   PTEH  0xffff_fff0, PTEL 0xffff_fff4,
         *   TTB   0xffff_fff8, TEA  0xffff_fffc.
         * Honor MMUCR.TI at the correct offset.
         */
        if ((off & ~3UL) == 0xffe0UL && (shio_read_be32(m, 0xffe0UL) & 0x00000004UL) != 0UL) {
            memset(m->tlb_valid, 0, sizeof(m->tlb_valid));
        }
    }
    if (m->strict_cache_ops && ((off & 0xfff0UL) == 0xffe0UL || (off & 0xfff0UL) == 0xff60UL || off == 0xffecUL)) {
        cv1k_bus_invalidate_icache_all(&m->bus);
        cv1k_bus_invalidate_dcache_all(&m->bus);
    }
    maybe_sh_dma(m, off);
}

void cv1k_bus_ldtlb(struct cv1k_bus *bus)
{
    struct cv1k_machine *m;
    cv1k_u32 pteh;
    cv1k_u32 ptel;
    cv1k_u32 size;
    cv1k_u32 mask;
    cv1k_u32 slot;
    m = bus->machine;
    if (m == NULL) return;
    /* MAME's SH3 ccn_map places PTEH/PTEL at 0xfffffff0/0xfffffff4.
     * v42 read the BSC window at 0xffffff60/64 by mistake, which confused
     * bus-timing register state with TLB state.
     */
    pteh = shio_read_be32(m, 0xfff0UL);
    ptel = shio_read_be32(m, 0xfff4UL);
    size = tlb_page_size_from_ptel(ptel);
    mask = ~(size - 1UL);
    slot = (m->tlb_loads & 15UL);
    m->tlb_vpn[slot] = pteh & mask;
    m->tlb_ppn[slot] = (ptel & 0x1ffffc00UL) & mask;
    m->tlb_mask[slot] = mask;
    m->tlb_valid[slot] = ((ptel & 0x00000100UL) != 0UL) ? 1U : 0U;
    m->tlb_loads++;
}

static cv1k_u32 tlb_page_size_from_ptel(cv1k_u32 ptel)
{
    cv1k_u32 sz;
    sz = ((ptel >> 6) & 2UL) | ((ptel >> 4) & 1UL);
    switch (sz) {
    case 0UL: return 0x00000400UL;
    case 1UL: return 0x00001000UL;
    case 2UL: return 0x00010000UL;
    default: return 0x00100000UL;
    }
}

static CV1K_HOT cv1k_u32 cpu_addr_translate(struct cv1k_machine *m, cv1k_u32 addr)
{
    cv1k_u32 i;
    /* SH-3 area aliases used by the CV1000 boot ROM.
     * P1 0x80000000-0x9fffffff and P2 0xa0000000-0xbfffffff alias the
     * first 512 MiB of the physical address map and bypass the TLB.
     */
    if (addr >= 0x80000000UL && addr <= 0xbfffffffUL) { if (m != NULL) m->alias_p1p2++; return addr & 0x1fffffffUL; }
    /* SH-3 P4 internal register space.  MAME's SH core owns this space;
     * the sandbox redirects the high register window into the permissive
     * SH I/O shadow so cache/MMU/INTC probes are readable/writable instead
     * of becoming open-bus failures.
     */
    if (addr >= 0xffff0000UL) { if (m != NULL) m->alias_p4++; return CV1K_ADDR_SH_IO + (addr & 0xffffUL); }
    if (m != NULL) {
        for (i = 0UL; i < 16UL; i++) {
            if (m->tlb_valid[i] && ((addr ^ m->tlb_vpn[i]) & m->tlb_mask[i]) == 0UL) {
                cv1k_u32 phys;
                phys = m->tlb_ppn[i] | (addr & ~m->tlb_mask[i]);
                m->tlb_hits++;
                m->tlb_last_virt = addr;
                m->tlb_last_phys = phys;
                return phys;
            }
        }
        if (addr < 0x80000000UL || (addr >= 0xc0000000UL && addr < 0xe0000000UL)) m->tlb_misses++;
    }
    /* Coarse SH-3 MMU shortcuts used by the DDPSDOJ boot path after it
     * enables virtual addressing.  The real SH7709S uses programmable TLB
     * entries; this sandbox records only the high-value aliases seen in the
     * boot trace.  Preserve 0x00000000-0x003fffff as boot ROM, then map the
     * remaining low 16 MiB P0 virtual window onto CV1000-D work RAM.  This
     * covers allocator/table pointers such as 0x00516eba without turning
     * early reset-vector reads into RAM reads.
     */
    if (addr >= 0x40000000UL && addr < 0x50000000UL) {
        if (m != NULL) m->alias_400++;
        if (m != NULL && m->compact_400_alias) return CV1K_ADDR_WORK_RAM + (addr & 0x001fffffUL);
        return CV1K_ADDR_WORK_RAM + (addr & 0x00ffffffUL);
    }
    if (m != NULL && m->wide_p0_alias && addr >= CV1K_REGION_BOOT_ROM_SIZE && addr < 0x80000000UL) {
        if (cv1k_is_physical_device_window(addr)) return addr;
        if (m != NULL) m->alias_p0_wide++;
        return CV1K_ADDR_WORK_RAM + (addr & 0x00ffffffUL);
    }
    if (addr >= CV1K_REGION_BOOT_ROM_SIZE && addr < 0x01000000UL) { if (m != NULL) m->alias_p0_work++; return CV1K_ADDR_WORK_RAM + (addr & 0x00ffffffUL); }
    if (addr >= 0xe0000000UL && addr < 0xe1000000UL) { if (m != NULL) m->alias_e0++; return CV1K_ADDR_WORK_RAM + (addr & 0x00ffffffUL); }
    return addr;
}



static int mame_cache_meta_is_cacheable(cv1k_u32 addr)
{
    cv1k_u32 region;
    region = addr >> 29;
    return region != 5UL && region != 7UL;
}

#define SH7709S_AREA_MASK 0x1fffffffUL
#define SH7709S_CACHE_LINE_BURST_READ_PENALTY 6UL
#define SH7709S_CACHE_LINE_BURST_WRITE_PENALTY 4UL
#define SH7709S_CACHE_MISS_STALL_PENALTY 4UL
#define SH7709S_BUS_ACCESS_PENALTY 2UL

static cv1k_u32 sh7709s_cache_get_area(cv1k_u32 address)
{
    return (address & SH7709S_AREA_MASK) >> 26;
}

static int sh7709s_cache_is_sdram_region(cv1k_u32 address)
{
    cv1k_u32 area;
    area = sh7709s_cache_get_area(address);
    return area == 2UL || area == 3UL;
}

static int sh7709s_cache_can_burst(cv1k_u32 address)
{
    return sh7709s_cache_is_sdram_region(address);
}

static cv1k_u32 sh7709s_cache_wcr1_timing(cv1k_u32 address, cv1k_u16 wcr1)
{
    cv1k_u32 area;
    cv1k_u32 area_shift;
    cv1k_u32 area_val;
    area = sh7709s_cache_get_area(address);
    if (area > 6UL || area == 1UL) return 0UL;
    area_shift = 12UL - (area * 2UL);
    area_val = ((cv1k_u32)wcr1 >> area_shift) & 0x3UL;
    if (area_val == 0UL) return 1UL;
    return area_val;
}

static cv1k_u32 sh7709s_cache_wcr2_timing(cv1k_u32 address, cv1k_u16 wcr2)
{
    cv1k_u32 area;
    cv1k_u32 area_val;
    int burst_capable;
    area = sh7709s_cache_get_area(address);
    burst_capable = sh7709s_cache_can_burst(address);
    if (area > 6UL || area == 1UL) return 0UL;
    area_val = 0UL;
    if (area == 0UL) {
        area_val = (cv1k_u32)wcr2 & 0x7UL;
    } else if (area == 2UL || area == 3UL) {
        wcr2 >>= 3;
        if (area == 3UL) wcr2 >>= 2;
        area_val = (cv1k_u32)wcr2 & 0x3UL;
        if (area_val == 0UL) return 1UL;
        return area_val;
    } else {
        wcr2 >>= (cv1k_u16)(7U + ((area - 4UL) * 3UL));
        area_val = (cv1k_u32)wcr2 & 0x7UL;
    }
    if (burst_capable) {
        switch (area_val) {
        case 0UL: return 2UL;
        case 1UL: return 2UL;
        case 2UL: return 3UL;
        case 3UL: return 4UL;
        case 4UL: return 4UL;
        case 5UL: return 6UL;
        case 6UL: return 8UL;
        default: return 10UL;
        }
    }
    switch (area_val) {
    case 0UL: return 0UL;
    case 1UL: return 1UL;
    case 2UL: return 2UL;
    case 3UL: return 3UL;
    case 4UL: return 4UL;
    case 5UL: return 6UL;
    case 6UL: return 8UL;
    default: return 10UL;
    }
}

static cv1k_u32 sh7709s_cache_mcr_tpc(cv1k_u16 mcr)
{
    switch (((cv1k_u32)mcr >> 14) & 0x3UL) {
    case 0UL: return 2UL;
    case 1UL: return 5UL;
    case 2UL: return 8UL;
    default: return 11UL;
    }
}

static cv1k_u32 sh7709s_cache_mcr_rcd(cv1k_u16 mcr)
{
    return (((cv1k_u32)mcr >> 12) & 0x3UL) + 1UL;
}

static cv1k_u32 sh7709s_cache_mcr_trwl(cv1k_u16 mcr)
{
    return (((cv1k_u32)mcr >> 10) & 0x3UL) + 1UL;
}

static cv1k_u32 sh7709s_cache_mcr_tras(cv1k_u16 mcr)
{
    return (((cv1k_u32)mcr >> 8) & 0x3UL) + 2UL;
}

static cv1k_u32 sh7709s_cache_external_penalty(struct cv1k_machine *m, cv1k_u32 address, int writeback)
{
    cv1k_u32 penalty;
    cv1k_u32 area;
    cv1k_u32 rcd;
    cv1k_u16 wcr1;
    cv1k_u16 wcr2;
    cv1k_u16 mcr;
    int burst_capable;

    if (m == NULL) return 0UL;
    penalty = 0UL;
    area = sh7709s_cache_get_area(address);
    burst_capable = sh7709s_cache_can_burst(address);
    wcr1 = shio_read_be16(m, 0xff64UL);
    wcr2 = shio_read_be16(m, 0xff66UL);
    mcr = shio_read_be16(m, 0xff68UL);

    if (area != (cv1k_u32)m->sh7709s_cache_last_area ||
        ((cv1k_u32)m->sh7709s_cache_last_was_write != (cv1k_u32)(writeback ? 1 : 0))) {
        penalty += sh7709s_cache_wcr1_timing(address, wcr1);
    }
    m->sh7709s_cache_last_area = (cv1k_u8)area;
    m->sh7709s_cache_last_was_write = (cv1k_u8)(writeback ? 1U : 0U);

    if (burst_capable && !writeback) penalty += SH7709S_CACHE_LINE_BURST_READ_PENALTY;
    if (burst_capable && writeback) penalty += SH7709S_CACHE_LINE_BURST_WRITE_PENALTY - 1UL;

    if (!burst_capable) {
        penalty += (SH7709S_BUS_ACCESS_PENALTY + sh7709s_cache_wcr2_timing(address, wcr2)) * 4UL;
    } else if (!writeback) {
        penalty += SH7709S_CACHE_MISS_STALL_PENALTY + sh7709s_cache_wcr2_timing(address, wcr2);
    }

    if (sh7709s_cache_is_sdram_region(address)) {
        rcd = sh7709s_cache_mcr_rcd(mcr);
        penalty += SH7709S_BUS_ACCESS_PENALTY;
        penalty += 1UL; /* ACTV command issue */
        penalty += rcd;
        penalty += 1UL; /* column command issue */
        penalty += sh7709s_cache_mcr_tpc(mcr);
        penalty += sh7709s_cache_mcr_tras(mcr);
        if (rcd >= 2UL) penalty += rcd - 1UL;
    }
    return penalty;
}

static void sh7709s_cache_apply_penalty(struct cv1k_machine *m, cv1k_u32 penalty)
{
    if (m == NULL || penalty == 0UL) return;
    m->cpu.cycles += penalty;
    m->sh7709s_cache_penalty_cycles += penalty;
    m->sh7709s_cache_penalty_events++;
}

static void mame_cache_meta_access(struct cv1k_machine *m, cv1k_u32 vaddr, cv1k_u32 phys, int write, int fetch)
{
    cv1k_u32 cache_address;
    cv1k_u32 cache_block;
    cv1k_u32 i;
    cv1k_u32 j;
    cv1k_u32 victim;
    cv1k_u8 old_lru;
    cv1k_u8 lru;
    int hit;
    int dirty_evict;
    cv1k_u32 evict_address;
    cv1k_u32 penalty;

    if (m == NULL) return;
    if (!m->mame_cache_meta && !m->sh7709s_cache_timing) return;
    if (m->sh7709s_cache_timing_suppress != 0) return;
    if (!mame_cache_meta_is_cacheable(vaddr)) return;
    if (cv1k_is_physical_device_window(phys)) return;

    if (m->mame_cache_meta) {
        if (fetch) m->mame_cache_fetches++;
        else if (write) m->mame_cache_writes++;
        else m->mame_cache_reads++;
    }

    cache_address = phys / CV1K_SH7709S_CACHE_LINE_SIZE;
    cache_block = cache_address % CV1K_SH7709S_CACHE_BLOCKS;
    hit = 0;
    dirty_evict = 0;
    evict_address = 0UL;

    for (i = 0UL; i < CV1K_SH7709S_CACHE_ASSOCIATIVITY; i++) {
        if (m->mame_cache_tag[cache_block][i] == cache_address) {
            if (write) m->mame_cache_dirty[cache_block][i] = 1U;
            old_lru = m->mame_cache_lru[cache_block][i];
            if (old_lru != (cv1k_u8)(CV1K_SH7709S_CACHE_ASSOCIATIVITY - 1UL)) {
                for (j = 0UL; j < CV1K_SH7709S_CACHE_ASSOCIATIVITY; j++) {
                    if (m->mame_cache_lru[cache_block][j] > old_lru) m->mame_cache_lru[cache_block][j]--;
                }
                m->mame_cache_lru[cache_block][i] = (cv1k_u8)(CV1K_SH7709S_CACHE_ASSOCIATIVITY - 1UL);
            }
            hit = 1;
            if (m->mame_cache_meta) m->mame_cache_hits++;
            break;
        }
    }

    if (!hit) {
        if (m->mame_cache_meta) m->mame_cache_misses++;
        victim = 0UL;
        for (i = 0UL; i < CV1K_SH7709S_CACHE_ASSOCIATIVITY; i++) {
            if (m->mame_cache_lru[cache_block][i] == 0U) {
                victim = i;
                break;
            }
        }
        if (m->mame_cache_dirty[cache_block][victim]) {
            dirty_evict = 1;
            evict_address = m->mame_cache_tag[cache_block][victim] * CV1K_SH7709S_CACHE_LINE_SIZE;
            if (m->mame_cache_meta) m->mame_cache_dirty_evicts++;
        }

        lru = m->mame_cache_lru[cache_block][victim];
        m->mame_cache_tag[cache_block][victim] = cache_address;
        m->mame_cache_lru[cache_block][victim] = (cv1k_u8)(CV1K_SH7709S_CACHE_ASSOCIATIVITY - 1UL);
        m->mame_cache_dirty[cache_block][victim] = (cv1k_u8)(write ? 1U : 0U);
        for (j = 0UL; j < CV1K_SH7709S_CACHE_ASSOCIATIVITY; j++) {
            if (j != victim && m->mame_cache_lru[cache_block][j] > lru) m->mame_cache_lru[cache_block][j]--;
        }

        if (m->sh7709s_cache_timing) {
            penalty = 0UL;
            if (dirty_evict) {
                m->sh7709s_cache_wb_address = evict_address;
                penalty += 1UL; /* move evicted line to writeback buffer */
            }
            penalty += sh7709s_cache_external_penalty(m, phys, 0);
            sh7709s_cache_apply_penalty(m, penalty);
        }
    }

    if (m->sh7709s_cache_timing && m->sh7709s_cache_wb_address != 0UL) {
        cv1k_u32 wb;
        wb = m->sh7709s_cache_wb_address;
        m->sh7709s_cache_wb_address = 0UL;
        penalty = sh7709s_cache_mcr_trwl(shio_read_be16(m, 0xff68UL));
        penalty += sh7709s_cache_external_penalty(m, wb, 1);
        sh7709s_cache_apply_penalty(m, penalty);
    }
}

void cv1k_bus_cache_access(struct cv1k_bus *bus, cv1k_u32 addr, int write, int fetch)
{
    struct cv1k_machine *m;
    cv1k_u32 phys;
    m = bus ? bus->machine : NULL;
    if (m == NULL) return;
    if (!m->mame_cache_meta && !m->sh7709s_cache_timing) return;
    phys = cpu_addr_translate(m, addr);
    mame_cache_meta_access(m, addr, phys, write, fetch);
}

static cv1k_u32 dcache_line_base(cv1k_u32 phys)
{
    return phys & ~(CV1K_DCACHE_LINE_SIZE - 1UL);
}

static cv1k_u32 dcache_index(cv1k_u32 phys)
{
    return (phys / CV1K_DCACHE_LINE_SIZE) & (CV1K_DCACHE_LINES - 1UL);
}

static void dcache_fill_line(struct cv1k_machine *m, cv1k_u32 phys)
{
    cv1k_u32 base;
    cv1k_u32 idx;
    cv1k_u32 off;
    cv1k_u32 i;
    base = dcache_line_base(phys);
    idx = dcache_index(phys);
    m->dcache_tag[idx] = base;
    m->dcache_valid[idx] = 1U;
    for (i = 0UL; i < CV1K_DCACHE_LINE_SIZE; i++) {
        off = base + i - CV1K_ADDR_WORK_RAM;
        if (off < m->main_ram_size) m->dcache_data[idx][i] = m->main_ram[off];
        else m->dcache_data[idx][i] = 0xffU;
    }
}

static cv1k_u8 dcache_read8(struct cv1k_machine *m, cv1k_u32 phys)
{
    cv1k_u32 idx;
    cv1k_u32 base;
    cv1k_u32 off;
    idx = dcache_index(phys);
    base = dcache_line_base(phys);
    if (m->dcache_valid[idx] && m->dcache_tag[idx] == base) {
        m->dcache_hits++;
    } else {
        dcache_fill_line(m, phys);
        m->dcache_misses++;
    }
    off = phys - base;
    return m->dcache_data[idx][off];
}

static void dcache_cpu_write8(struct cv1k_machine *m, cv1k_u32 phys, cv1k_u8 data)
{
    cv1k_u32 idx;
    cv1k_u32 base;
    cv1k_u32 off;
    m->main_ram[phys - CV1K_ADDR_WORK_RAM] = data;
    idx = dcache_index(phys);
    base = dcache_line_base(phys);
    if (!(m->dcache_valid[idx] && m->dcache_tag[idx] == base)) {
        /* The SH7709S cache is write-allocate.  DDPSDOJ relies on cleared
         * RAM lines staying visible through cache after later NAND DMAs
         * replace the backing RAM underneath copied code/data overlays.
         */
        dcache_fill_line(m, phys);
        m->dcache_misses++;
    }
    off = phys - base;
    m->dcache_data[idx][off] = data;
}

static void dcache_dma_write8(struct cv1k_machine *m, cv1k_u32 phys, cv1k_u8 data)
{
    cv1k_u32 idx;
    cv1k_u32 base;
    m->main_ram[phys - CV1K_ADDR_WORK_RAM] = data;
    idx = dcache_index(phys);
    base = dcache_line_base(phys);
    if (m->dcache_valid[idx] && m->dcache_tag[idx] == base) m->dcache_dma_stale++;
}

static cv1k_u8 cv1k_bus_dma_read8(struct cv1k_machine *m, cv1k_u32 addr)
{
    cv1k_u32 off;
    cv1k_u32 phys;
    if (m == NULL) return 0xffU;
    phys = dmac_physical_am(addr);

    if (in_range(phys, CV1K_ADDR_SH_IO, CV1K_REGION_SH_IO_SIZE)) {
        return sh_io_port_r(m, phys - CV1K_ADDR_SH_IO);
    }
    if (in_range(phys, CV1K_ADDR_BOOT_ROM, CV1K_REGION_BOOT_ROM_SIZE)) {
        off = phys - CV1K_ADDR_BOOT_ROM;
        if (off < m->boot_rom_size) return m->boot_rom[off];
        return 0xffU;
    }
    if (in_range(phys, CV1K_ADDR_WORK_RAM, m->main_ram_size)) {
        return m->main_ram[phys - CV1K_ADDR_WORK_RAM];
    }
    if (in_range(phys, CV1K_ADDR_NAND_IO, CV1K_REGION_IO_SIZE)) {
        off = phys - CV1K_ADDR_NAND_IO;
        if (off == 0UL) return cv1k_nand_data_r(&m->nand);
        return 0xffU;
    }
    if (in_range(phys, CV1K_ADDR_RTC_EE, CV1K_REGION_IO_SIZE)) {
        off = phys - CV1K_ADDR_RTC_EE;
        if (off == 1UL) return (cv1k_u8)(0xfeU | cv1k_rtc9701_read_bit(&m->rtc));
        return 0x00U;
    }
    if (in_range(phys, CV1K_ADDR_BLITTER, CV1K_REGION_BLITTER_SIZE)) {
        return cv1k_video_read8(&m->video, phys - CV1K_ADDR_BLITTER);
    }
    if (in_range(phys, CV1K_ADDR_CACHE, m->cache_ram_size)) {
        return m->cache_ram[phys - CV1K_ADDR_CACHE];
    }

    m->unmapped_reads++;
    m->last_unmapped_read = phys;
    return 0xffU;
}

static void cv1k_bus_dma_write8(struct cv1k_machine *m, cv1k_u32 addr, cv1k_u8 data)
{
    cv1k_u32 off;
    cv1k_u32 phys;
    if (m == NULL) return;
    /* MAME sh4dmac.cpp masks DMA SAR/DAR with SH34_AM and accesses the
     * program address space directly.  Do not run these addresses through
     * the CPU P0/TLB compatibility aliases; doing that turns unmapped low
     * physical destinations into CV1000 work-RAM writes and corrupts the
     * DDPSDOJ boot stack.
     */
    phys = dmac_physical_am(addr);
    if (in_range(phys, CV1K_ADDR_WORK_RAM, m->main_ram_size)) {
        if (m->dcache_enabled) dcache_dma_write8(m, phys, data);
        else m->main_ram[phys - CV1K_ADDR_WORK_RAM] = data;
        if (m->dma_cache_sync) {
            if ((phys & (CV1K_DCACHE_LINE_SIZE - 1UL)) == 0UL) {
                dcache_invalidate_line_public(m, phys);
                m->dma_cache_invalidations++;
            }
            if ((phys & 0x1ffUL) == 0UL) {
                icache_invalidate_line(m, phys);
                m->dma_cache_invalidations++;
            }
        }
        return;
    }
    if (in_range(phys, CV1K_ADDR_SH_IO, CV1K_REGION_SH_IO_SIZE)) {
        sh_io_port_w(m, phys - CV1K_ADDR_SH_IO, data);
        return;
    }
    if (in_range(phys, CV1K_ADDR_BOOT_ROM, CV1K_REGION_BOOT_ROM_SIZE)) {
        return;
    }
    if (in_range(phys, CV1K_ADDR_NAND_IO, CV1K_REGION_IO_SIZE)) {
        off = phys - CV1K_ADDR_NAND_IO;
        if (off == 0UL) cv1k_nand_data_w(&m->nand, data);
        else if (off == 1UL) cv1k_nand_command_w(&m->nand, data);
        else if (off == 2UL) cv1k_nand_address_w(&m->nand, data);
        return;
    }
    if (in_range(phys, CV1K_ADDR_YMZ770, CV1K_REGION_IO_SIZE)) {
        cv1k_ymz770_write(&m->ymz, phys - CV1K_ADDR_YMZ770, data);
        return;
    }
    if (in_range(phys, CV1K_ADDR_RTC_EE, CV1K_REGION_IO_SIZE)) {
        off = phys - CV1K_ADDR_RTC_EE;
        if (off == 1UL) cv1k_rtc9701_write_lines(&m->rtc, data, cv1k_now_unix());
        else if (off == 3UL) cv1k_nand_set_ce(&m->nand, (data & 0x01U) == 0U);
        return;
    }
    if (in_range(phys, CV1K_ADDR_BLITTER, CV1K_REGION_BLITTER_SIZE)) {
        cv1k_video_write8(&m->video, phys - CV1K_ADDR_BLITTER, data, m->main_ram, m->main_ram_size);
        return;
    }
    if (in_range(phys, CV1K_ADDR_CACHE, m->cache_ram_size)) {
        m->cache_ram[phys - CV1K_ADDR_CACHE] = data;
        return;
    }

    m->unmapped_writes++;
    m->last_unmapped_write = phys;
    m->last_unmapped_write_data = (cv1k_u32)data;
}


static cv1k_u32 cv1k_bus_blitter_read32(struct cv1k_machine *m, cv1k_u32 phys)
{
    cv1k_u32 off;
    if (m == NULL) return 0xffffffffUL;
    off = phys - CV1K_ADDR_BLITTER;
    if ((off & ~3UL) == 0x10UL) {
        cv1k_machine_sync_video_busy(m);
        if (m->mame_speedup && (m->video.busy || m->video.busy_cycles_left != 0UL)) {
            cv1k_u32 pc;
            pc = m->cpu.ppc;
            if (pc == m->last_blitter_status_pc) m->blitter_status_spin_reads++;
            else { m->last_blitter_status_pc = pc; m->blitter_status_spin_reads = 0UL; }
            if (m->blitter_status_spin_reads != 0UL) cv1k_machine_fast_forward_blitter_busy(m);
        } else {
            m->last_blitter_status_pc = 0UL;
            m->blitter_status_spin_reads = 0UL;
        }
    }
    return cv1k_video_read32(&m->video, off);
}

static cv1k_u8 cv1k_bus_blitter_read8(struct cv1k_machine *m, cv1k_u32 phys)
{
    cv1k_u32 v;
    cv1k_u32 off;
    cv1k_u32 shift;
    if (m == NULL) return 0xffU;
    off = phys - CV1K_ADDR_BLITTER;
    if ((off & ~3UL) == 0x10UL) {
        v = cv1k_bus_blitter_read32(m, phys & ~3UL);
        shift = (3UL - (off & 3UL)) * 8UL;
        return (cv1k_u8)((v >> shift) & 0xffUL);
    }
    return cv1k_video_read8(&m->video, off);
}

CV1K_HOT cv1k_u8 cv1k_bus_read8(struct cv1k_bus *bus, cv1k_u32 addr)
{
    struct cv1k_machine *m;
    cv1k_u32 off;
    cv1k_u32 original_addr;
    m = bus->machine;
    if (m == NULL) return 0xffU;
    original_addr = addr;
    addr = cpu_addr_translate(m, addr);
    if (CV1K_UNLIKELY(m->mame_cache_meta)) mame_cache_meta_access(m, original_addr, addr, 0, 0);

    if (in_range(addr, CV1K_ADDR_SH_IO, CV1K_REGION_SH_IO_SIZE)) {
        return sh_io_port_r(m, addr - CV1K_ADDR_SH_IO);
    }

    if (in_range(addr, CV1K_ADDR_BOOT_ROM, CV1K_REGION_BOOT_ROM_SIZE)) {
        off = addr - CV1K_ADDR_BOOT_ROM;
        if (off < m->boot_rom_size) return m->boot_rom[off];
        return 0xffU;
    }

    if (in_range(addr, CV1K_ADDR_WORK_RAM, m->main_ram_size)) {
        if (m->dcache_enabled && !sh_addr_is_p2_uncached(original_addr)) return dcache_read8(m, addr);
        return m->main_ram[addr - CV1K_ADDR_WORK_RAM];
    }

    if (in_range(addr, CV1K_ADDR_NAND_IO, CV1K_REGION_IO_SIZE)) {
        off = addr - CV1K_ADDR_NAND_IO;
        if (off == 0UL) return cv1k_nand_data_r(&m->nand);
        return 0xffU;
    }

    if (in_range(addr, CV1K_ADDR_RTC_EE, CV1K_REGION_IO_SIZE)) {
        off = addr - CV1K_ADDR_RTC_EE;
        if (off == 1UL) return (cv1k_u8)(0xfeU | cv1k_rtc9701_read_bit(&m->rtc));
        return 0x00U;
    }

    if (in_range(addr, CV1K_ADDR_BLITTER, CV1K_REGION_BLITTER_SIZE)) {
        return cv1k_bus_blitter_read8(m, addr);
    }

    if (in_range(addr, CV1K_ADDR_CACHE, m->cache_ram_size)) {
        return m->cache_ram[addr - CV1K_ADDR_CACHE];
    }

    m->unmapped_reads++;
    m->last_unmapped_read = addr;
    return 0xffU;
}

CV1K_HOT cv1k_u16 cv1k_bus_read16(struct cv1k_bus *bus, cv1k_u32 addr)
{
    cv1k_u16 a;
    cv1k_u16 b;
    a = (cv1k_u16)cv1k_bus_read8(bus, addr);
    b = (cv1k_u16)cv1k_bus_read8(bus, addr + 1UL);
    return (cv1k_u16)((a << 8) | b);
}


static void icache_store16(struct cv1k_machine *m, cv1k_u32 phys, cv1k_u16 op)
{
    cv1k_u32 idx;
    phys &= 0xfffffffeUL;
    idx = (phys >> 1) & (CV1K_ICACHE_LINES - 1UL);
    m->icache_valid[idx] = 1U;
    m->icache_tag[idx] = phys;
    m->icache_op[idx] = op;
}

static int icache_lookup16(struct cv1k_machine *m, cv1k_u32 phys, cv1k_u16 *op)
{
    cv1k_u32 idx;
    phys &= 0xfffffffeUL;
    idx = (phys >> 1) & (CV1K_ICACHE_LINES - 1UL);
    if (m->icache_valid[idx] && m->icache_tag[idx] == phys) {
        *op = m->icache_op[idx];
        return 1;
    }
    return 0;
}

static void icache_prefill_block(struct cv1k_machine *m, cv1k_u32 phys)
{
    cv1k_u32 base;
    cv1k_u32 p;
    cv1k_u32 off;
    cv1k_u16 op;
    base = phys & ~0x1ffUL;
    for (p = base; p < base + 0x200UL; p += 2UL) {
        if (!in_range(p, CV1K_ADDR_WORK_RAM, m->main_ram_size)) continue;
        off = p - CV1K_ADDR_WORK_RAM;
        op = (cv1k_u16)(((cv1k_u16)m->main_ram[off] << 8) | (cv1k_u16)m->main_ram[off + 1UL]);
        icache_store16(m, p, op);
    }
}


static void icache_invalidate_line(struct cv1k_machine *m, cv1k_u32 phys)
{
    if (sh7709s_c23jit_enabled()) sh7709s_c23jit_reset();
    if (cv1k_ir_enabled()) cv1k_ir_reset();
    cv1k_u32 base;
    cv1k_u32 p;
    cv1k_u32 idx;
    base = phys & ~0x1ffUL;
    for (p = base; p < base + 0x200UL; p += 2UL) {
        idx = (p >> 1) & (CV1K_ICACHE_LINES - 1UL);
        if (m->icache_valid[idx] && m->icache_tag[idx] == p) m->icache_valid[idx] = 0U;
    }
}

static void dcache_invalidate_line_public(struct cv1k_machine *m, cv1k_u32 phys)
{
    cv1k_u32 idx;
    cv1k_u32 base;
    idx = dcache_index(phys);
    base = dcache_line_base(phys);
    if (m->dcache_valid[idx] && m->dcache_tag[idx] == base) m->dcache_valid[idx] = 0U;
}

void cv1k_bus_invalidate_icache_all(struct cv1k_bus *bus)
{
    struct cv1k_machine *m;
    m = bus->machine;
    if (m == NULL) return;
    memset(m->icache_valid, 0, sizeof(m->icache_valid));
    if (sh7709s_c23jit_enabled()) sh7709s_c23jit_reset();
    if (cv1k_ir_enabled()) cv1k_ir_reset();
}

static void cv1k_bus_invalidate_dcache_all(struct cv1k_bus *bus)
{
    struct cv1k_machine *m;
    m = bus->machine;
    if (m == NULL) return;
    memset(m->dcache_valid, 0, sizeof(m->dcache_valid));
}

void cv1k_bus_cache_op(struct cv1k_bus *bus, cv1k_u32 addr, int op)
{
    struct cv1k_machine *m;
    cv1k_u32 phys;
    m = bus->machine;
    if (m == NULL) return;
    if (!m->strict_cache_ops) return;
    phys = cpu_addr_translate(m, addr);
    if (op == CV1K_CACHEOP_PREF) {
        if (in_range(phys, CV1K_ADDR_WORK_RAM, m->main_ram_size)) icache_prefill_block(m, phys);
        return;
    }
    if (in_range(phys, CV1K_ADDR_WORK_RAM, m->main_ram_size)) {
        dcache_invalidate_line_public(m, phys);
        icache_invalidate_line(m, phys);
    }
}

cv1k_u16 cv1k_bus_fetch16(struct cv1k_bus *bus, cv1k_u32 addr)
{
    struct cv1k_machine *m;
    cv1k_u32 phys;
    cv1k_u16 op;
    m = bus->machine;
    if (m == NULL) return 0xffffU;
    phys = cpu_addr_translate(m, addr) & 0xfffffffeUL;
    if (CV1K_UNLIKELY(m->mame_cache_meta)) mame_cache_meta_access(m, addr, phys, 0, 1);
    if (in_range(phys, CV1K_ADDR_WORK_RAM, m->main_ram_size)) {
        if (icache_lookup16(m, phys, &op)) {
            m->icache_hits++;
            return op;
        }
        icache_prefill_block(m, phys);
        if (icache_lookup16(m, phys, &op)) {
            m->icache_misses++;
            return op;
        }
    }
    return cv1k_bus_read16(bus, addr);
}

CV1K_HOT cv1k_u32 cv1k_bus_read32(struct cv1k_bus *bus, cv1k_u32 addr)
{
    struct cv1k_machine *m;
    cv1k_u32 phys;
    cv1k_u32 original_addr;
    cv1k_u32 a;
    cv1k_u32 b;
    m = bus ? bus->machine : NULL;
    if (m == NULL) return 0xffffffffUL;
    original_addr = addr;
    phys = cpu_addr_translate(m, addr);
    if (in_range(phys, CV1K_ADDR_BLITTER, CV1K_REGION_BLITTER_SIZE) && ((phys & 3UL) == 0UL)) {
        if (CV1K_UNLIKELY(m->mame_cache_meta)) mame_cache_meta_access(m, original_addr, phys, 0, 0);
        return cv1k_bus_blitter_read32(m, phys);
    }
    a = (cv1k_u32)cv1k_bus_read16(bus, addr);
    b = (cv1k_u32)cv1k_bus_read16(bus, addr + 2UL);
    return (a << 16) | b;
}

CV1K_HOT void cv1k_bus_write8(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u8 data)
{
    struct cv1k_machine *m;
    cv1k_u32 off;
    cv1k_u32 original_addr;
    m = bus->machine;
    if (m == NULL) return;
    original_addr = addr;
    addr = cpu_addr_translate(m, addr);
    if (CV1K_UNLIKELY(m->mame_cache_meta)) mame_cache_meta_access(m, original_addr, addr, 1, 0);

    if (in_range(addr, CV1K_ADDR_SH_IO, CV1K_REGION_SH_IO_SIZE)) {
        sh_io_port_w(m, addr - CV1K_ADDR_SH_IO, data);
        return;
    }

    if (in_range(addr, CV1K_ADDR_BOOT_ROM, CV1K_REGION_BOOT_ROM_SIZE)) {
        return;
    }

    if (in_range(addr, CV1K_ADDR_WORK_RAM, m->main_ram_size)) {
        if (m->dcache_enabled && !sh_addr_is_p2_uncached(original_addr)) dcache_cpu_write8(m, addr, data);
        else m->main_ram[addr - CV1K_ADDR_WORK_RAM] = data;
        return;
    }

    if (in_range(addr, CV1K_ADDR_NAND_IO, CV1K_REGION_IO_SIZE)) {
        off = addr - CV1K_ADDR_NAND_IO;
        if (off == 0UL) cv1k_nand_data_w(&m->nand, data);
        else if (off == 1UL) cv1k_nand_command_w(&m->nand, data);
        else if (off == 2UL) cv1k_nand_address_w(&m->nand, data);
        return;
    }

    if (in_range(addr, CV1K_ADDR_YMZ770, CV1K_REGION_IO_SIZE)) {
        cv1k_ymz770_write(&m->ymz, addr - CV1K_ADDR_YMZ770, data);
        return;
    }

    if (in_range(addr, CV1K_ADDR_RTC_EE, CV1K_REGION_IO_SIZE)) {
        off = addr - CV1K_ADDR_RTC_EE;
        if (off == 1UL) cv1k_rtc9701_write_lines(&m->rtc, data, cv1k_now_unix());
        else if (off == 3UL) cv1k_nand_set_ce(&m->nand, (data & 0x01U) == 0U);
        return;
    }

    if (in_range(addr, CV1K_ADDR_BLITTER, CV1K_REGION_BLITTER_SIZE)) {
        cv1k_video_write8(&m->video, addr - CV1K_ADDR_BLITTER, data, m->main_ram, m->main_ram_size);
        return;
    }

    if (in_range(addr, CV1K_ADDR_CACHE, m->cache_ram_size)) {
        m->cache_ram[addr - CV1K_ADDR_CACHE] = data;
        return;
    }

    m->unmapped_writes++;
    m->last_unmapped_write = addr;
    m->last_unmapped_write_data = (cv1k_u32)data;
}

CV1K_HOT void cv1k_bus_write16(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u16 data)
{
    cv1k_bus_write8(bus, addr, (cv1k_u8)((data >> 8) & 0xffU));
    cv1k_bus_write8(bus, addr + 1UL, (cv1k_u8)(data & 0xffU));
}

CV1K_HOT void cv1k_bus_write32(struct cv1k_bus *bus, cv1k_u32 addr, cv1k_u32 data)
{
    struct cv1k_machine *m;
    cv1k_u32 phys;
    cv1k_u32 original_addr;
    m = bus ? bus->machine : NULL;
    if (m == NULL) return;
    original_addr = addr;
    phys = cpu_addr_translate(m, addr);

    if (CV1K_LIKELY((phys & 3UL) == 0UL)) {
        if (in_range(phys, CV1K_ADDR_BLITTER, CV1K_REGION_BLITTER_SIZE)) {
            if (CV1K_UNLIKELY(m->mame_cache_meta)) mame_cache_meta_access(m, original_addr, phys, 1, 0);
            cv1k_video_write32(&m->video, phys - CV1K_ADDR_BLITTER, data, m->main_ram, m->main_ram_size);
            return;
        }
        if (in_range(phys, CV1K_ADDR_WORK_RAM, m->main_ram_size) && !m->dcache_enabled) {
            cv1k_u32 off = phys - CV1K_ADDR_WORK_RAM;
            if (CV1K_UNLIKELY(m->mame_cache_meta)) mame_cache_meta_access(m, original_addr, phys, 1, 0);
            if (off + 3UL < m->main_ram_size) {
                m->main_ram[off] = (cv1k_u8)((data >> 24) & 0xffU);
                m->main_ram[off + 1UL] = (cv1k_u8)((data >> 16) & 0xffU);
                m->main_ram[off + 2UL] = (cv1k_u8)((data >> 8) & 0xffU);
                m->main_ram[off + 3UL] = (cv1k_u8)(data & 0xffU);
                return;
            }
        }
    }

    cv1k_bus_write16(bus, addr, (cv1k_u16)((data >> 16) & 0xffffUL));
    cv1k_bus_write16(bus, addr + 2UL, (cv1k_u16)(data & 0xffffUL));
}

cv1k_u8 cv1k_bus_port_read(struct cv1k_bus *bus, int port_id)
{
    struct cv1k_machine *m;
    cv1k_u8 value;
    m = bus->machine;
    if (m == NULL) return 0xffU;
    value = 0xffU;
    switch (port_id) {
    case 'C':
        value = cv1k_input_port_c(&m->input);
        m->port_reads_c++;
        m->port_last_c = value;
        m->port_pc_c = m->cpu.pc;
        break;
    case 'D':
        value = cv1k_input_port_d(&m->input);
        m->port_reads_d++;
        m->port_last_d = value;
        m->port_pc_d = m->cpu.pc;
        break;
    case 'E':
        value = (cv1k_u8)((cv1k_nand_is_busy(&m->nand) ? 0x00U : 0x20U) | 0xdfU);
        m->port_reads_e++;
        m->port_last_e = value;
        m->port_pc_e = m->cpu.pc;
        break;
    case 'F':
        value = cv1k_input_port_f(&m->input);
        m->port_reads_f++;
        m->port_last_f = value;
        m->port_pc_f = m->cpu.pc;
        break;
    case 'L':
        value = cv1k_input_port_l(&m->input);
        m->port_reads_l++;
        m->port_last_l = value;
        m->port_pc_l = m->cpu.pc;
        break;
    default:
        break;
    }
    return value;
}
