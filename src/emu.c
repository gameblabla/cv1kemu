/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * CV1000 machine scaffold for this ANSI C sandbox.
 *
 * MAME-derived behavior in this file is limited to board/reset/scheduler
 * facts and isolated SH7709S/CV1000 constants from the MAME reference tree.
 * The DDPSDOJ copied-code accelerators and bounded auto-blit handoff are
 * local sandbox glue around missing SH7709S cache/MMU/DMAC/IRQ fidelity; they
 * are not copied MAME framework code.
 *
 * See NOTICE and docs/MAME_DERIVED.md for attribution details.
 */
#include "emu.h"
#include "platform.h"
#include "mame_cv1k_derived.h"
#include <string.h>
#include <stdio.h>

#define CV1K_SH_SR_T 0x00000001UL

int cv1k_machine_init(struct cv1k_machine *m, int model)
{
    cv1k_u32 ram_size;
    memset(m, 0, sizeof(*m));
    m->model = model;
    ram_size = (model == CV1K_MODEL_D) ? CV1K_MAIN_RAM_D_SIZE : CV1K_MAIN_RAM_B_SIZE;
    m->boot_rom = (cv1k_u8 *)cv1k_xmalloc(CV1K_BOOT_ROM_MAX);
    m->main_ram = (cv1k_u8 *)cv1k_xmalloc(ram_size);
    m->cache_ram = (cv1k_u8 *)cv1k_xmalloc(CV1K_CACHE_RAM_SIZE);
    m->sound_rom = (cv1k_u8 *)cv1k_xmalloc(CV1K_SOUND_ROM_MAX);
    if (m->boot_rom == NULL || m->main_ram == NULL || m->cache_ram == NULL || m->sound_rom == NULL) return 0;
    m->main_ram_size = ram_size;
    m->cache_ram_size = CV1K_CACHE_RAM_SIZE;
    if (!cv1k_nand_init(&m->nand, CV1K_NAND_DEFAULT_SIZE)) return 0;
    if (!cv1k_video_init(&m->video)) return 0;
    cv1k_input_default(&m->input);
    cv1k_bus_bind(&m->bus, m);
    cv1k_machine_reset(m);
    return 1;
}

void cv1k_machine_shutdown(struct cv1k_machine *m)
{
    cv1k_video_shutdown(&m->video);
    cv1k_nand_shutdown(&m->nand);
    cv1k_free(m->boot_rom);
    cv1k_free(m->main_ram);
    cv1k_free(m->cache_ram);
    cv1k_free(m->sound_rom);
    memset(m, 0, sizeof(*m));
}


static void cv1k_shio_reset_write8(struct cv1k_machine *m, cv1k_u32 off, cv1k_u8 value)
{
    m->sh_io[off & (CV1K_REGION_SH_IO_SIZE - 1UL)] = value;
}

static void cv1k_shio_reset_write16(struct cv1k_machine *m, cv1k_u32 off, cv1k_u16 value)
{
    cv1k_shio_reset_write8(m, off, (cv1k_u8)((value >> 8) & 0xffU));
    cv1k_shio_reset_write8(m, off + 1UL, (cv1k_u8)(value & 0xffU));
}

static void cv1k_shio_reset_write32(struct cv1k_machine *m, cv1k_u32 off, cv1k_u32 value)
{
    cv1k_shio_reset_write8(m, off, (cv1k_u8)((value >> 24) & 0xffUL));
    cv1k_shio_reset_write8(m, off + 1UL, (cv1k_u8)((value >> 16) & 0xffUL));
    cv1k_shio_reset_write8(m, off + 2UL, (cv1k_u8)((value >> 8) & 0xffUL));
    cv1k_shio_reset_write8(m, off + 3UL, (cv1k_u8)(value & 0xffUL));
}

static void cv1k_machine_apply_mame_sh7709s_reset_io(struct cv1k_machine *m)
{
    /* v40: apply the concrete SH7709S internal-register reset/start values
     * that MAME initializes in sh34_base_device::device_reset(),
     * sh3_base_device::device_reset() and sh3_base_device::device_start().
     * Previous sandbox builds zeroed the permissive SH I/O shadow, so code
     * reading untouched internal registers saw values unlike MAME/hardware
     * (notably TMU TCOR/TCNT=0xffffffff, CMCOR=0xffff, BSC wait registers,
     * SCI status bytes and 7709 port control defaults).
     */

    /* CPG / BSC defaults. */
    cv1k_shio_reset_write16(m, 0xff80UL, 0x0102U); /* FRQCR */
    cv1k_shio_reset_write16(m, 0xff62UL, 0x3ff0U); /* BCR2 */
    cv1k_shio_reset_write16(m, 0xff64UL, 0x3ff3U); /* WCR1 */
    cv1k_shio_reset_write16(m, 0xff66UL, 0xffffU); /* WCR2 */

    /* TMU: MAME starts stopped with compare/count registers preloaded. */
    cv1k_shio_reset_write32(m, 0xfe94UL, 0xffffffffUL); /* TCOR0 */
    cv1k_shio_reset_write32(m, 0xfe98UL, 0xffffffffUL); /* TCNT0 */
    cv1k_shio_reset_write32(m, 0xfea0UL, 0xffffffffUL); /* TCOR1 */
    cv1k_shio_reset_write32(m, 0xfea4UL, 0xffffffffUL); /* TCNT1 */
    cv1k_shio_reset_write32(m, 0xfeacUL, 0xffffffffUL); /* TCOR2 */
    cv1k_shio_reset_write32(m, 0xfeb0UL, 0xffffffffUL); /* TCNT2 */

    /* SCI / CMT / ADC / DAC reset values from MAME's SH7709 device. */
    cv1k_shio_reset_write8(m, 0xfe82UL, 0xffU);  /* SCBRR */
    cv1k_shio_reset_write8(m, 0xfe86UL, 0xffU);  /* SCTDR */
    cv1k_shio_reset_write8(m, 0xfe88UL, 0x84U);  /* SCSSR */
    cv1k_shio_reset_write16(m, 0x0076UL, 0xffffU); /* CMCOR */
    cv1k_shio_reset_write8(m, 0x0092UL, 0x07U);  /* ADCR */
    cv1k_shio_reset_write8(m, 0x00a4UL, 0x1fU);  /* DADCR */

    /* SH7709 port control defaults. */
    cv1k_shio_reset_write16(m, 0x0104UL, 0xaaaaU); /* PCCR */
    cv1k_shio_reset_write16(m, 0x0106UL, 0xaa8aU); /* PDCR */
    cv1k_shio_reset_write16(m, 0x0108UL, 0xaaaaU); /* PECR */
    cv1k_shio_reset_write16(m, 0x010aUL, 0xaaaaU); /* PFCR */
    cv1k_shio_reset_write16(m, 0x010cUL, 0xaaaaU); /* PGCR */
    cv1k_shio_reset_write16(m, 0x010eUL, 0xaaaaU); /* PHCR */
    cv1k_shio_reset_write16(m, 0x0116UL, 0xa888U); /* SCPCR */

    /* IRDA / SCIF / UDI reset values. */
    cv1k_shio_reset_write8(m, 0x0142UL, 0xffU);  /* SCBRR1 */
    cv1k_shio_reset_write16(m, 0x0148UL, 0x0060U); /* SCSSR1 */
    cv1k_shio_reset_write8(m, 0x0152UL, 0xffU);  /* SCBRR2 */
    cv1k_shio_reset_write16(m, 0x0158UL, 0x0060U); /* SCSSR2 */
    cv1k_shio_reset_write16(m, 0x0200UL, 0xffffU); /* SDIR */
}

void cv1k_machine_reset(struct cv1k_machine *m)
{
    if (m->main_ram != NULL) memset(m->main_ram, 0, (size_t)m->main_ram_size);
    if (m->cache_ram != NULL) memset(m->cache_ram, 0, (size_t)m->cache_ram_size);
    memset(m->sh_io, 0, sizeof(m->sh_io));
    cv1k_machine_apply_mame_sh7709s_reset_io(m);
    memset(m->tlb_vpn, 0, sizeof(m->tlb_vpn));
    memset(m->tlb_ppn, 0, sizeof(m->tlb_ppn));
    memset(m->tlb_mask, 0, sizeof(m->tlb_mask));
    memset(m->tlb_valid, 0, sizeof(m->tlb_valid));
    m->tlb_loads = 0UL;
    m->tlb_hits = 0UL;
    m->tlb_misses = 0UL;
    m->tlb_last_virt = 0UL;
    m->tlb_last_phys = 0UL;
    memset(m->icache_tag, 0, sizeof(m->icache_tag));
    memset(m->icache_op, 0, sizeof(m->icache_op));
    memset(m->icache_valid, 0, sizeof(m->icache_valid));
    memset(m->dcache_tag, 0, sizeof(m->dcache_tag));
    memset(m->dcache_data, 0, sizeof(m->dcache_data));
    memset(m->dcache_valid, 0, sizeof(m->dcache_valid));
    m->icache_hits = 0UL;
    m->icache_misses = 0UL;
    m->dcache_hits = 0UL;
    m->dcache_misses = 0UL;
    m->dcache_dma_stale = 0UL;
    memset(m->mame_cache_tag, 0, sizeof(m->mame_cache_tag));
    memset(m->mame_cache_lru, 0, sizeof(m->mame_cache_lru));
    memset(m->mame_cache_dirty, 0, sizeof(m->mame_cache_dirty));
    m->mame_cache_hits = 0UL;
    m->mame_cache_misses = 0UL;
    m->mame_cache_dirty_evicts = 0UL;
    m->mame_cache_fetches = 0UL;
    m->mame_cache_reads = 0UL;
    m->mame_cache_writes = 0UL;
    m->dma_cache_invalidations = 0UL;
    m->alias_p1p2 = 0UL;
    m->alias_p4 = 0UL;
    m->alias_p0_wide = 0UL;
    m->alias_p0_work = 0UL;
    m->alias_400 = 0UL;
    m->alias_e0 = 0UL;
    sh7709s_reset(&m->cpu);
    cv1k_nand_reset(&m->nand);
    cv1k_rtc9701_reset(&m->rtc);
    cv1k_ymz770_reset(&m->ymz);
    cv1k_video_reset(&m->video);
    m->frames = 0UL;
    m->dma_transfers = 0UL;
    m->dma_bytes = 0UL;
    m->last_dma_sar = 0UL;
    m->last_dma_dar = 0UL;
    m->last_dma_tcr = 0UL;
    m->last_dma_chcr = 0UL;
    m->last_dma_src_mode = 0UL;
    m->last_dma_dst_mode = 0UL;
    m->last_dma_status = 0UL;
    memset(m->dma_timer_active, 0, sizeof(m->dma_timer_active));
    memset(m->dma_timer_due, 0, sizeof(m->dma_timer_due));
    memset(m->dma_timer_chcr, 0, sizeof(m->dma_timer_chcr));
    memset(m->dma_timer_base, 0, sizeof(m->dma_timer_base));
    m->last_dma_nand_page0 = 0UL;
    m->last_dma_nand_page1 = 0UL;
    m->last_dma_nand_block0 = 0UL;
    m->last_dma_nand_block1 = 0UL;
    m->last_dma_nand_col0 = 0UL;
    m->last_dma_nand_col1 = 0UL;
    m->unmapped_reads = 0UL;
    m->unmapped_writes = 0UL;
    m->last_unmapped_read = 0UL;
    m->last_unmapped_write = 0UL;
    m->last_unmapped_write_data = 0UL;
    m->boot_assists = 0UL;
    m->irq_requests = 0UL;
    m->irq_last_event = 0UL;
    m->irq_last_level = 0UL;
    m->exception_events = 0UL;
    m->exception_last_event = 0UL;
    m->exception_last_tra = 0UL;
    m->mame_speedup_spins = 0UL;
    memset(m->tmu_underflows, 0, sizeof(m->tmu_underflows));
    memset(m->tmu_last_cycles, 0, sizeof(m->tmu_last_cycles));
    m->tmu_last_event = 0UL;
    m->tmu_last_priority = 0UL;
    m->port_reads_c = 0UL;
    m->port_reads_d = 0UL;
    m->port_reads_e = 0UL;
    m->port_reads_f = 0UL;
    m->port_reads_l = 0UL;
    m->port_pc_c = 0UL;
    m->port_pc_d = 0UL;
    m->port_pc_e = 0UL;
    m->port_pc_f = 0UL;
    m->port_pc_l = 0UL;
    m->port_last_c = 0xffU;
    m->port_last_d = 0xffU;
    m->port_last_e = 0xffU;
    m->port_last_f = 0xffU;
    m->port_last_l = 0xffU;
    m->last_active_pc = 0UL;
    m->auto_blits = 0UL;
    m->auto_blit_last_base = 0UL;
    m->auto_blit_last_end = 0UL;
    m->auto_blit_last_sig = 0UL;
    m->auto_blit_skips = 0UL;
}


int cv1k_machine_load_boot(struct cv1k_machine *m, const char *path)
{
    cv1k_u32 got;
    got = 0UL;
    if (!cv1k_read_file(path, m->boot_rom, CV1K_BOOT_ROM_MAX, &got)) return 0;
    m->boot_rom_size = got;
    /* MAME reset fetches the initial stack pointer with read_long(4) after
     * placing PC at 0xa0000000.  This standalone reset runs before the ROM is
     * loaded, so mirror that visible post-ROM reset state here once U4 is
     * available.  The CV1000 boot code soon initializes its own RAM stack,
     * but matching the reset vector removes one more framework mismatch.
     */
    if (got >= 8UL) {
        m->cpu.pc = 0xa0000000UL;
        m->cpu.r[15] = ((cv1k_u32)m->boot_rom[4] << 24) |
                      ((cv1k_u32)m->boot_rom[5] << 16) |
                      ((cv1k_u32)m->boot_rom[6] << 8) |
                       (cv1k_u32)m->boot_rom[7];
    }
    return 1;
}

int cv1k_machine_load_nand(struct cv1k_machine *m, const char *path)
{
    return cv1k_nand_load(&m->nand, path);
}

int cv1k_machine_load_sound(struct cv1k_machine *m, const char *path)
{
    cv1k_u32 got;
    got = 0UL;
    if (!cv1k_read_file(path, m->sound_rom, CV1K_SOUND_ROM_MAX, &got)) return 0;
    m->sound_rom_size = got;
    return 1;
}

int cv1k_machine_load_ram(struct cv1k_machine *m, const char *path)
{
    cv1k_u32 got;
    got = 0UL;
    if (m->main_ram == NULL || m->main_ram_size == 0UL) return 0;
    memset(m->main_ram, 0, (size_t)m->main_ram_size);
    if (!cv1k_read_file(path, m->main_ram, m->main_ram_size, &got)) return 0;
    CV1K_UNUSED(got);
    return 1;
}

void cv1k_machine_blit(struct cv1k_machine *m, cv1k_u32 addr)
{
    cv1k_video_execute_list(&m->video, addr, m->main_ram, m->main_ram_size, 4096UL);
}



static cv1k_u16 cv1k_shio_read_be16_direct(const struct cv1k_machine *m, cv1k_u32 off)
{
    cv1k_u32 idx;
    if (m == NULL) return 0U;
    idx = off & (CV1K_REGION_SH_IO_SIZE - 1UL);
    return (cv1k_u16)(((cv1k_u16)m->sh_io[idx] << 8) | (cv1k_u16)m->sh_io[(idx + 1UL) & (CV1K_REGION_SH_IO_SIZE - 1UL)]);
}

static int cv1k_irq2_priority_from_iprc(const struct cv1k_machine *m)
{
    cv1k_u16 iprc;
    /* MAME sh3_base_device::iprc_w assigns IRL2 priority from bits 8..11
     * of IPRC at SH7709S internal offset 0x04000016.  The vblank source is
     * wired to IRQ2, so use that priority for the line while preserving the
     * IRQ2 event code.
     */
    iprc = cv1k_shio_read_be16_direct(m, 0x16UL);
    return (int)((iprc >> 8) & 0x0fU);
}








static int addr_is_work_alias(cv1k_u32 a)
{
    if (a >= CV1K_ADDR_WORK_RAM && a < CV1K_ADDR_WORK_RAM + CV1K_MAIN_RAM_D_SIZE) return 1;
    if (a >= 0x8c000000UL && a < 0x8d000000UL) return 1;
    if (a >= 0xac000000UL && a < 0xad000000UL) return 1;
    return 0;
}

static cv1k_u32 swapw32(cv1k_u32 v)
{
    return (v << 16) | ((v >> 16) & 0xffffUL);
}

static int cv1k_ddpsdoj_accel_text_list(struct cv1k_machine *m)
{
    cv1k_u32 src;
    cv1k_u32 dst;
    cv1k_u32 cursor;
    cv1k_u32 step;
    cv1k_u32 base;
    cv1k_u32 head;
    cv1k_u32 dim;
    cv1k_u32 tint;
    cv1k_u32 count;
    cv1k_u8 b;
    cv1k_u32 post;
    cv1k_u32 r1;
    cv1k_u32 val;
    if (m->cpu.pc != 0x0c1dd7b8UL) return 0;
    if (cv1k_bus_fetch16(&m->bus, 0x0c1dd7b8UL) != 0x6044U ||
        cv1k_bus_fetch16(&m->bus, 0x0c1dd7bcUL) != 0x8d36U ||
        cv1k_bus_fetch16(&m->bus, 0x0c1dd7c0UL) != 0x70e0U ||
        cv1k_bus_fetch16(&m->bus, 0x0c1dd7f6UL) != 0x6153U ||
        cv1k_bus_fetch16(&m->bus, 0x0c1dd802UL) != 0xafd9U) return 0;
    src = m->cpu.r[4];
    dst = m->cpu.r[7];
    if (!addr_is_work_alias(src) || !addr_is_work_alias(dst)) return 0;
    cursor = m->cpu.r[5];
    step = m->cpu.r[10];
    base = m->cpu.r[2];
    head = m->cpu.r[6];
    dim = m->cpu.r[8];
    tint = m->cpu.r[9];
    for (count = 0UL; count < 4096UL; count++) {
        b = cv1k_bus_read8(&m->bus, src);
        src++;
        if (b == 0U) {
            m->cpu.r[0] = 0UL;
            m->cpu.r[4] = src;
            m->cpu.r[5] = cursor;
            m->cpu.r[7] = dst;
            m->cpu.sr |= CV1K_SH_SR_T;
            m->cpu.pc = 0x0c1dd82cUL;
            m->cpu.cycles += (count + 1UL) * 20UL;
            m->boot_assists++;
            return 1;
        }
        post = (cv1k_u32)((cv1k_s32)(cv1k_s8)b - 32L);
        if (post != 0UL) {
            r1 = post;
            r1 >>= 2;
            r1 >>= 2;
            r1 >>= 1;
            r1 <<= 2;
            r1 <<= 1;
            val = (((post & 0x1fUL) << 3) + base) | swapw32(0x000003f0UL - r1);
            cv1k_bus_write32(&m->bus, dst, head);
            cv1k_bus_write32(&m->bus, dst + 4UL, val);
            cv1k_bus_write32(&m->bus, dst + 8UL, cursor);
            cv1k_bus_write32(&m->bus, dst + 12UL, dim);
            cv1k_bus_write32(&m->bus, dst + 16UL, tint);
            dst += 20UL;
        }
        cursor = (cursor & 0xffff0000UL) | ((cursor + step) & 0x0000ffffUL);
    }
    return 0;
}

static int cv1k_ddpsdoj_run_lz_tokens(struct cv1k_machine *m, cv1k_u32 tokens)
{
    cv1k_u32 dst;
    cv1k_u32 bit_count;
    cv1k_u32 src;
    cv1k_u32 bits;
    cv1k_u32 bit_src;
    cv1k_u32 remain;
    cv1k_u32 i;
    cv1k_u32 j;
    cv1k_u32 word;
    cv1k_u32 offset;
    cv1k_u32 len;
    cv1k_u32 copy_src;
    cv1k_u8 b;
    if (tokens < 16UL || tokens > 0x10000UL) return 0;
    dst = cv1k_bus_read32(&m->bus, 0x0c65e94cUL);
    bit_count = cv1k_bus_read32(&m->bus, 0x0c65e950UL);
    src = cv1k_bus_read32(&m->bus, 0x0c65e948UL);
    bits = cv1k_bus_read32(&m->bus, 0x0c65e954UL);
    bit_src = cv1k_bus_read32(&m->bus, 0x0c65e944UL);
    remain = cv1k_bus_read32(&m->bus, 0x0c65e940UL);
    if (remain <= tokens || remain == 0UL) return 0;
    if (!addr_is_work_alias(dst) || !addr_is_work_alias(src) || !addr_is_work_alias(bit_src)) return 0;
    for (i = 0UL; i < tokens; i++) {
        if (bit_count == 0UL) {
            bits = ((cv1k_u32)cv1k_bus_read8(&m->bus, bit_src) << 24) |
                   ((cv1k_u32)cv1k_bus_read8(&m->bus, bit_src + 1UL) << 16) |
                   ((cv1k_u32)cv1k_bus_read8(&m->bus, bit_src + 2UL) << 8) |
                   (cv1k_u32)cv1k_bus_read8(&m->bus, bit_src + 3UL);
            bit_src += 4UL;
            bit_count = 32UL;
        }
        if ((bits & 0x80000000UL) == 0UL) {
            b = cv1k_bus_read8(&m->bus, src);
            src++;
            cv1k_bus_write8(&m->bus, dst, b);
            dst++;
        } else {
            word = ((cv1k_u32)cv1k_bus_read8(&m->bus, src) << 8) | (cv1k_u32)cv1k_bus_read8(&m->bus, src + 1UL);
            src += 2UL;
            offset = word >> 5;
            len = (word & 0x1fUL) + 3UL;
            if (offset == 0UL || offset > 0x100000UL) return 0;
            copy_src = dst - offset;
            for (j = 0UL; j < len; j++) {
                b = cv1k_bus_read8(&m->bus, copy_src);
                copy_src++;
                cv1k_bus_write8(&m->bus, dst, b);
                dst++;
            }
        }
        bit_count--;
        bits <<= 1;
        remain--;
    }
    cv1k_bus_write32(&m->bus, 0x0c65e94cUL, dst);
    cv1k_bus_write32(&m->bus, 0x0c65e948UL, src);
    cv1k_bus_write32(&m->bus, 0x0c65e950UL, bit_count);
    cv1k_bus_write32(&m->bus, 0x0c65e954UL, bits);
    cv1k_bus_write32(&m->bus, 0x0c65e944UL, bit_src);
    cv1k_bus_write32(&m->bus, 0x0c65e940UL, remain);
    return 1;
}

static int cv1k_ddpsdoj_accel_lz_entry(struct cv1k_machine *m)
{
    if (m->cpu.pc != 0x0c1fc250UL) return 0;
    if (cv1k_bus_fetch16(&m->bus, 0x0c1fc250UL) != 0x2f86U ||
        cv1k_bus_fetch16(&m->bus, 0x0c1fc256UL) != 0x6a43U ||
        cv1k_bus_fetch16(&m->bus, 0x0c1fc274UL) != 0x2448U ||
        cv1k_bus_fetch16(&m->bus, 0x0c1fc300UL) != 0x6783U ||
        cv1k_bus_fetch16(&m->bus, 0x0c1fc2b2UL) != 0xd626U) return 0;
    if (!cv1k_ddpsdoj_run_lz_tokens(m, m->cpu.r[4])) return 0;
    m->cpu.r[0] = 1UL;
    m->cpu.pc = m->cpu.pr;
    m->cpu.cycles += m->cpu.r[4] * 24UL;
    m->boot_assists++;
    return 1;
}


static void cv1k_boot_assist_pre_step(struct cv1k_machine *m)
{
    cv1k_u32 a;
    cv1k_u32 v;
    cv1k_u32 block;
    cv1k_u32 idx;
    if (m == NULL) return;
    if (cv1k_ddpsdoj_accel_text_list(m)) return;
    if (cv1k_ddpsdoj_accel_lz_entry(m)) return;

    /* v17 aggressive invalid-target guard.  Some experimental runs still jump
     * through payload-derived high addresses after a NAND/DMAC/cache-coherency
     * divergence.  Real SH7709S hardware would vector an address/TLB exception;
     * the scaffold exception path is incomplete, so in aggressive mode return
     * to PR only when the target fetch is blank/open-bus and PR is sane RAM.
     */
    if (m->aggressive_boot_assists &&
        !((m->cpu.pc < CV1K_BOOT_ROM_MAX) || (m->cpu.pc >= 0x0c000000UL && m->cpu.pc < 0x0d000000UL))) {
        cv1k_u16 opbad;
        opbad = cv1k_bus_fetch16(&m->bus, m->cpu.pc);
        if ((opbad == 0x0000U || opbad == 0xffffU || opbad == 0xf359U) && m->cpu.pr >= 0x0c000000UL && m->cpu.pr < 0x0d000000UL) {
            m->cpu.pc = m->cpu.pr;
            m->boot_assists++;
            return;
        }
    }

    /* v18 diagnostic exception bridge.  Long experimental DDPSDOJ runs can
     * fetch bytes from overlaid NAND payload as SH opcodes after the current
     * cache/DMAC model loses coherency.  A real SH7709S would take an illegal
     * instruction/address exception through VBR; the scaffold exception path
     * is not yet complete.  In aggressive mode, recognize only the observed
     * payload-derived opcodes at copied-RAM/P1 code addresses and return to a
     * sane PR so the next hardware-sensitive stage can be exposed. */
    if (m->aggressive_boot_assists && m->cpu.pr >= 0x0c000000UL && m->cpu.pr < 0x0d000000UL) {
        cv1k_u16 opx;
        opx = cv1k_bus_fetch16(&m->bus, m->cpu.pc);
        if ((m->cpu.pc == 0x0c1fba66UL && opx == 0xff03U) ||
            (m->cpu.pc == 0x8c1d9978UL && opx == 0xf359U) ||
            (m->cpu.pc >= 0xdf000000UL && opx == 0xffffU)) {
            m->cpu.pc = m->cpu.pr;
            m->boot_assists++;
            return;
        }
    }

    /* v10 DDPSDOJ guard: after the experimental cache/literal-prefetch path,
     * an incomplete MMU/NAND state can indirect into the SH high virtual alias
     * e00xxxxx where the mapped work-RAM payload is blank.  Real hardware would
     * take a TLB/address exception through the game vector table; the sandbox
     * exception model is still skeletal, so return to PR for this precise
     * high-alias blank-code case to keep the boot probe moving and count it.
     */
    if (m->cpu.pc >= 0xe0000000UL && m->cpu.pc < 0xe1000000UL) {
        cv1k_u16 opx;
        opx = cv1k_bus_fetch16(&m->bus, m->cpu.pc);
        if ((opx == 0x0000U || opx == 0xffffU) && m->cpu.pr >= 0x0c000000UL && m->cpu.pr < 0x0d000000UL) {
            m->cpu.pc = m->cpu.pr;
            m->boot_assists++;
            return;
        }
    }

    /* DDPSDOJ boot assist: the current partial SH/MMU/cache model builds a
     * late allocator/free-list record containing P0 virtual work-RAM pointers
     * where the real boot code later expects compact table indices.  This is
     * not a correct hardware emulation; it is an explicit compatibility shim
     * to get beyond the known v5 blocker while the exact SH7709S cache/TLB and
     * peripheral side effects are still missing. */
    if (m->cpu.pc == 0x0c30cba0UL) {
        a = m->cpu.r[4];
        v = cv1k_bus_read32(&m->bus, a);
        if (v >= 0x40000000UL && v < 0x41000000UL) {
            block = cv1k_bus_read32(&m->bus, 0x0c7f83c8UL);
            if (block == 0UL || block > 0x10000UL) block = 0x840UL;
            idx = (v - 0x40000000UL) / block;
            if (idx > 0UL) idx--;
            if (idx < 0x400UL) {
                cv1k_bus_write32(&m->bus, a, idx);
                m->boot_assists++;
            }
        }
        return;
    }

    /* v7 DDPSDOJ allocator-scan assist.  When the incomplete P0/TLB model
     * leaves a 0x40000000-based virtual work-RAM pointer in the temporary
     * allocator record, the boot loop at 0c30b6ae walks it down by 0x840-byte
     * blocks.  Normalize only that P0 work-RAM form to the intended bounded
     * offset and jump to the post-loop path. */
    if (m->cpu.pc == 0x0c30b6aeUL) {
        a = m->cpu.r[14];
        if (a >= 0x0c000000UL && a < 0x0d000000UL) {
            v = cv1k_bus_read32(&m->bus, a + 24UL);
            block = cv1k_bus_read32(&m->bus, 0x0c7f83c8UL);
            if (block == 0x840UL && v >= 0x40000000UL && v < 0x41000000UL) {
                cv1k_u32 off;
                cv1k_u32 loops;
                cv1k_u32 rem;
                cv1k_u32 final_ptr;
                cv1k_u32 count;
                off = v - 0x40000000UL;
                loops = off / block;
                rem = off - (loops * block);
                if (loops > 0UL || rem != 0UL) {
                    /* Collapse the observed DDPSDOJ allocator/free-list scan.
                     * The real SH7709S MMU/TLB maps this 0x40000000 P0 window
                     * onto work RAM.  The current sandbox only has a coarse
                     * alias, so this loop can burn millions of interpreter
                     * steps walking a pointer by 0x840 bytes.  Execute the
                     * loop's net register/RAM effect in one step for this exact
                     * frame shape; it remains a documented compatibility assist.
                     */
                    final_ptr = rem;
                    count = cv1k_bus_read32(&m->bus, a + 16UL);
                    cv1k_bus_write32(&m->bus, a + 16UL, count + loops);
                    cv1k_bus_write32(&m->bus, a + 20UL, final_ptr);
                    cv1k_bus_write32(&m->bus, a + 24UL, final_ptr);
                    m->cpu.r[1] = final_ptr;
                    m->cpu.r[2] = final_ptr;
                    m->cpu.pc = 0x0c30b6d6UL;
                    m->boot_assists++;
                }
            }
        }
    }


    /* v11 DDPSDOJ high-virtual status-loop assist.  The v10 boot probe reaches
     * copied RAM code at 0c1fb3dc that reads a small status value through a
     * 0x4bxxxxxx virtual pointer.  With the current coarse SH7709S TLB model
     * that pointer aliases into payload bytes and the loop never falls
     * through.  Normalize only this observed status field to zero so the
     * boot path can continue to the next hardware-sensitive stage. */
    if (m->aggressive_boot_assists && m->cpu.pc == 0x0c1fb3dcUL && m->cpu.r[10] >= 0x40000000UL && m->cpu.r[10] < 0x50000000UL) {
        a = m->cpu.r[10] + 8UL;
        v = cv1k_bus_read32(&m->bus, a);
        if (v > 5UL) {
            cv1k_bus_write32(&m->bus, a, 0UL);
            m->boot_assists++;
        }
    }


    /* v16 DDPSDOJ NAND copy status assist.  With --dcache + --wide-p0-alias
     * the boot path can reach a helper at 0c30b716 after copying about
     * 41 MiB from U2.  The helper tests R0 and otherwise falls into an
     * intentional BRA self-loop.  On hardware this value is produced by the
     * previous SH7709S/DMAC/NAND/cache-coherency sequence.  The sandbox still
     * lacks that exact coherency model, so --aggressive-assists may mark the
     * helper as successful and continue to expose the next blocker. */
    if (m->cpu.r[0] == 0UL && m->cpu.r[6] == 0xa4000020UL &&
        (m->cpu.r[2] > 0UL && m->cpu.r[2] < 0x10000UL)) {
        /* v50: the same helper can also be reached after the cache-enabled
         * NAND copy path.  It tests a status/result value generated by the
         * preceding SH7709S/DMAC/NAND/cache-coherency sequence.  The standalone
         * scaffold still lacks MAME's complete coherency model, but the stuck
         * state is extremely specific: R6 points at the SH7709S DMA channel 0
         * register window, R2 contains the allocator block size/count/result range,
         * and the code is either at the TST or the deliberate BRA self-loop.
         * Mark only that completed-helper state successful so execution can
         * continue to the next real hardware mismatch.
         */
        if (m->cpu.pc == 0x0c30b716UL &&
            cv1k_bus_fetch16(&m->bus, 0x0c30b716UL) == 0x2008U &&
            cv1k_bus_fetch16(&m->bus, 0x0c30b718UL) == 0x8b02U) {
            m->cpu.r[0] = 1UL;
            m->boot_assists++;
        } else if (m->cpu.pc == 0x0c30b71aUL && m->cpu.pr == 0x0c30b716UL &&
                   cv1k_bus_fetch16(&m->bus, 0x0c30b71aUL) == 0xaffeU) {
            m->cpu.r[0] = 1UL;
            m->cpu.pc = 0x0c30b720UL;
            m->boot_assists++;
        }
    }

    /* v51 DDPSDOJ copied-overlay RAM-flag wait.  After the second visible
     * NAND payload phase the cached code at 0c1e3cfc waits for a word flag
     * whose backing RAM has already been replaced by a newer NAND overlay in
     * the standalone cache/DMAC model.  MAME's SH7709S/cache path reaches this
     * wait with the flag cleared.  Recognize only the exact cached instruction
     * and literal shape and clear the two observed overlay-local wait words.
     */
    if ((m->cpu.pc == 0x0c1e3cfeUL || m->cpu.pc == 0x0c1e3d00UL || m->cpu.pc == 0x0c1e3d02UL ||
         m->cpu.pc == 0x0c1e3d1aUL || m->cpu.pc == 0x0c1e3d1cUL || m->cpu.pc == 0x0c1e3d1eUL) &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e3cfcUL) == 0xd004U &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e3cfeUL) == 0x6102U &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e3d00UL) == 0x2118U &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e3d02UL) == 0x8ffcU &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e3d10UL) == 0x0c1eU &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e3d12UL) == 0x33a0U &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e3d14UL) == 0x0c1eU &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e3d16UL) == 0x33b4U) {
        if ((m->cpu.r[0] == 0x0c1e33a0UL && cv1k_bus_read32(&m->bus, 0x0c1e33a0UL) != 0UL) ||
            (m->cpu.r[0] == 0x0c1e33b4UL && cv1k_bus_read32(&m->bus, 0x0c1e33b4UL) != 0UL)) {
            cv1k_bus_write32(&m->bus, m->cpu.r[0], 0UL);
            m->cpu.r[1] = 0UL;
            m->cpu.sr |= 1UL;
            m->boot_assists++;
        }
    }

    /* v52 DDPSDOJ copied-overlay blitter/check worker wait.  The ROM/RAM
     * check frontend at 0c1e41f8 posts a worker flag at 0c3178e4, calls the
     * MAME idle wait helper, then spins until that flag clears.  In MAME this
     * is synchronized by the scheduler/work-queue path; the standalone core
     * reaches the same wait with the flag left nonzero and keeps rechecking
     * NAND pages without ever polling inputs.  Match only the exact wait-loop
     * instruction/literal shape and clear the posted flag so the copied overlay
     * can continue.
     */
    if ((m->cpu.pc == 0x0c1e41f8UL || m->cpu.pc == 0x0c1e41fcUL || m->cpu.pc == 0x0c1e41feUL ||
         m->cpu.pc == 0x0c1e4200UL) &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e41f8UL) == 0x490bU &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e41faUL) == 0x64a2U &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e41fcUL) == 0x6482U &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e41feUL) == 0x2448U &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e4200UL) == 0x8bfaU &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e428cUL) == 0x0c31U &&
        cv1k_bus_fetch16(&m->bus, 0x0c1e428eUL) == 0x78e4U &&
        m->cpu.r[8] == 0x0c3178e4UL &&
        cv1k_bus_read32(&m->bus, 0x0c3178e4UL) != 0UL) {
        cv1k_bus_write32(&m->bus, 0x0c3178e4UL, 0UL);
        m->cpu.r[4] = 0UL;
        m->cpu.sr |= 1UL;
        m->boot_assists++;
    }

    /* v47 DDPSDOJ bounded counter-loop accelerator.  The v46 default path
     * reaches a copied-RAM helper at 0c30b6ae that repeatedly subtracts the
     * stride stored at [literal+8] (0x840 in the observed DDPSDOJ image) from
     * a stack counter at [R14+0x18], mirrors it at [R14+0x14], increments
     * [R14+0x10] only while the post-subtract value stays non-negative, then
     * exits when CMP/PZ fails.  This is not a semantic shortcut: it recognizes
     * the exact SH instruction sequence and applies the same register/RAM/SR
     * effects in one host step so the standalone interpreter does not spend
     * whole frames in a deterministic scheduler-local loop. */
    if (m->cpu.pc == 0x0c30b6aeUL &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6aeUL) == 0x51e6U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6b0UL) == 0x2118U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6b2UL) == 0x8b05U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6c0UL) == 0xd14aU &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6c2UL) == 0x52e6U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6c4UL) == 0x5112U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6c6UL) == 0x3218U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6c8UL) == 0x6123U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6caUL) == 0x1e16U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6ccUL) == 0x51e6U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6ceUL) == 0x1e15U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6d0UL) == 0x51e6U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6d2UL) == 0x4111U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6d4UL) == 0x890cU &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6f0UL) == 0x51e4U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6f2UL) == 0x7101U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6f4UL) == 0x1e14U &&
        cv1k_bus_fetch16(&m->bus, 0x0c30b6f6UL) == 0xafdaU) {
        cv1k_u32 sp;
        cv1k_u32 counter_addr;
        cv1k_u32 mirror_addr;
        cv1k_u32 index_addr;
        cv1k_u32 literal_addr;
        cv1k_u32 stride;
        cv1k_u32 counter;
        cv1k_u32 index;
        cv1k_u32 loops;
        cv1k_u32 final_counter;
        sp = m->cpu.r[14];
        counter_addr = sp + 0x18UL;
        mirror_addr = sp + 0x14UL;
        index_addr = sp + 0x10UL;
        /* The helper's MOV.L @(disp,PC),R1 fetches the literal through the
         * same cached path as copied-RAM opcodes.  The backing RAM at the
         * literal-pool address may have been overwritten by later DMA, so
         * use fetch16 here rather than a raw data read.
         */
        literal_addr = ((cv1k_u32)cv1k_bus_fetch16(&m->bus, 0x0c30b7ecUL) << 16) |
                       (cv1k_u32)cv1k_bus_fetch16(&m->bus, 0x0c30b7eeUL);
        stride = cv1k_bus_read32(&m->bus, literal_addr + 8UL);
        /* v50: make the erased-table fallback conservative instead of
         * --aggressive-only, but keep the guard narrow.  The observed failing
         * state is the copied-RAM allocator helper using a structure pointer
         * whose backing data has become zero/erased while the same block stride
         * is still present in the active stack frame.  On the full MAME path
         * the SH7709S/cache/NAND/DMAC sequence has a coherent allocator record
         * by the time this helper executes.  This bridge only substitutes the
         * stack-held stride when the table value is erased and the fallback
         * looks like a sane CV1000 block size.
         */
        if (stride == 0UL || stride == 0xffffffffUL || stride > 0x10000UL) {
            cv1k_u32 fallback_stride;
            fallback_stride = cv1k_bus_read32(&m->bus, sp + 0x38UL);
            if (fallback_stride != 0UL && fallback_stride <= 0x10000UL &&
                (fallback_stride & 3UL) == 0UL) stride = fallback_stride;
            else stride = 0x840UL;
        }
        counter = cv1k_bus_read32(&m->bus, counter_addr);
        if (stride != 0UL && stride <= 0x10000UL && counter != 0UL &&
            counter_addr >= 0x0c000000UL && counter_addr + 4UL <= 0x0d000000UL &&
            mirror_addr >= 0x0c000000UL && mirror_addr + 4UL <= 0x0d000000UL &&
            index_addr >= 0x0c000000UL && index_addr + 4UL <= 0x0d000000UL) {
            loops = (counter / stride) + 1UL;
            final_counter = counter - (stride * loops);
            index = cv1k_bus_read32(&m->bus, index_addr);
            if (loops != 0UL) index += loops - 1UL;
            cv1k_bus_write32(&m->bus, counter_addr, final_counter);
            cv1k_bus_write32(&m->bus, mirror_addr, final_counter);
            cv1k_bus_write32(&m->bus, index_addr, index);
            m->cpu.r[1] = final_counter;
            m->cpu.r[2] = final_counter;
            m->cpu.sr &= ~1UL;
            m->cpu.pc = 0x0c30b6d6UL;
            m->cpu.cycles += loops * 18UL;
            m->boot_assists++;
        }
    }

    /* v8 DDPSDOJ byte-fill loop accelerator.  After FPGA upload the boot
     * program clears a large RAM span with:
     *   MOV.B R5,@R4 ; DT R6 ; BF/S loop ; ADD #1,R4
     * The interpreted result is deterministic and bus-local, so execute the
     * exact loop effect in one pass once the count is large enough to matter.
     */
    if (m->cpu.pc == 0x0c1d7cfcUL && m->cpu.r[6] > 0x1000UL) {
        if (cv1k_bus_read16(&m->bus, 0x0c1d7cfcUL) == 0x2450U &&
            cv1k_bus_read16(&m->bus, 0x0c1d7cfeUL) == 0x4610U &&
            cv1k_bus_read16(&m->bus, 0x0c1d7d00UL) == 0x8ffcU &&
            cv1k_bus_read16(&m->bus, 0x0c1d7d02UL) == 0x7401U) {
            cv1k_u32 count2;
            cv1k_u32 addr2;
            cv1k_u32 i2;
            cv1k_u8 byte2;
            count2 = m->cpu.r[6];
            addr2 = m->cpu.r[4];
            byte2 = (cv1k_u8)(m->cpu.r[5] & 0xffUL);
            if (addr2 >= 0x0c000000UL && addr2 + count2 >= addr2 && addr2 + count2 <= 0x0d000000UL) {
                for (i2 = 0UL; i2 < count2; i2++) cv1k_bus_write8(&m->bus, addr2 + i2, byte2);
                m->cpu.r[4] = addr2 + count2;
                m->cpu.r[6] = 0UL;
                m->cpu.sr |= 1UL;
                m->cpu.pc = 0x0c1d7d04UL;
                m->boot_assists++;
            }
        }
    }
}

void cv1k_machine_step(struct cv1k_machine *m)
{
    if (m == NULL) return;
    /* The MAME-derived SH-3 core runs the real boot/decompression/blitter
     * routines accurately, so the old PC-keyed boot assists are no longer
     * invoked.  cv1k_boot_assist_pre_step() remains available behind the
     * aggressive-boot-assists flag for diagnostics only. */
    if (m->aggressive_boot_assists) cv1k_boot_assist_pre_step(m);
    sh7709s_step(&m->cpu, &m->bus);
}

void cv1k_machine_frame(struct cv1k_machine *m)
{
    cv1k_u32 cycles_before;
    cv1k_u32 guard;

    if (m == NULL) return;
    cycles_before = m->cpu.cycles;

    /* Run one real CV1000 video frame worth of SH-3 cycles (102.4 MHz /
     * 60.024 Hz).  With the MAME-derived core this is the natural timeslice:
     * the game runs its per-frame logic and then spins on the vblank tick
     * word that its own IRQ2 handler maintains.  A cycle guard bounds the
     * loop in case a stall keeps the cycle counter from advancing. */
    guard = 0UL;
    while ((cv1k_u32)(m->cpu.cycles - cycles_before) < CV1K_CYCLES_PER_VBLANK) {
        m->last_active_pc = m->cpu.pc;
        cv1k_machine_step(m);
        /* MAME cv1k installs spin_until_interrupt at idle PC 0x0c1d1346: once
         * the per-frame game logic has parked in this vblank wait loop, the
         * rest of the frame is pure spin, so jump straight to the vsync IRQ. */
        if (m->cpu.pc == 0x0c1d1346UL || m->cpu.pc == 0x0c1d1348UL) {
            m->mame_speedup_spins++;
            break;
        }
        if (++guard > (CV1K_CYCLES_PER_VBLANK * 4UL)) break;
    }

    cv1k_video_tick_cycles(&m->video, m->cpu.cycles - cycles_before);
    cv1k_bus_tmu_tick(&m->bus);

    /* Assert IRQ2 at the vsync pulse (MAME cv1k irq2_line_hold).  The handler
     * is serviced at the start of the next frame, matching hardware. */
    if (m->irq2_enabled) {
        int irq_pri = cv1k_irq2_priority_from_iprc(m);
        sh7709s_request_irq_line(&m->cpu, 2, irq_pri);
        m->irq_requests++;
        m->irq_last_level = (cv1k_u32)irq_pri;
    }

    cv1k_video_frame(&m->video, m->main_ram, m->main_ram_size);
    m->frames++;
}

void cv1k_machine_status(const struct cv1k_machine *m, char *out, cv1k_u32 out_size)
{
    char *p;
    p = out;
    p += sprintf(p, "model=CV1000-%c pc=%08lx sr=%08lx frames=%lu cycles=%lu illegal=%lu ",
        (m->model == CV1K_MODEL_D) ? 'D' : 'B',
        (unsigned long)m->cpu.pc,
        (unsigned long)m->cpu.sr,
        (unsigned long)m->frames,
        (unsigned long)m->cpu.cycles,
        (unsigned long)m->cpu.illegal_count);
    p += sprintf(p, "last_illegal=%08lx:%04lx vbr=%08lx spc=%08lx irq_ack=%lu nand=%luB nand_r=%lu nand_w=%lu ",
        (unsigned long)m->cpu.last_illegal_pc,
        (unsigned long)m->cpu.last_illegal_op,
        (unsigned long)m->cpu.vbr,
        (unsigned long)m->cpu.spc,
        (unsigned long)m->cpu.irq_ack_count,
        (unsigned long)m->nand.size,
        (unsigned long)m->nand.reads,
        (unsigned long)m->nand.writes);
    p += sprintf(p, "blit_ops=%lu up=%lu draw=%lu clip=%lu unk=%lu/%04lx bns=%lu hpen=%lu of=%lu ymz_writes=%lu ymz_reg=%lu ymz_key=%lu/%lu dma=%lu dma_bytes=%lu dmat=%lu%lu%lu%lu ",
        (unsigned long)m->video.executed_ops,
        (unsigned long)m->video.upload_ops,
        (unsigned long)m->video.draw_ops,
        (unsigned long)m->video.clip_ops,
        (unsigned long)m->video.unknown_ops,
        (unsigned long)m->video.last_unknown_op,
        (unsigned long)m->video.busy_cycles_ns,
        (unsigned long)m->video.blit_hline_penalty_ns,
        (unsigned long)m->video.blit_over_frame_count,
        (unsigned long)m->ymz.writes,
        (unsigned long)m->ymz.reg_writes,
        (unsigned long)m->ymz.keyons,
        (unsigned long)m->ymz.keyoffs,
        (unsigned long)m->dma_transfers,
        (unsigned long)m->dma_bytes,
        (unsigned long)m->dma_timer_active[0],
        (unsigned long)m->dma_timer_active[1],
        (unsigned long)m->dma_timer_active[2],
        (unsigned long)m->dma_timer_active[3]);
    p += sprintf(p, "scroll=%lu/%lu clip=%ld,%ld,%ld,%ld list=%06lx lup=%06lx:%lu,%lu,%lu,%lu/%lu/%lu ldr=%06lx:%04lx/%04lx:%lu,%lu>%ld,%ld:%lu,%lu/%lu/%lu/%lu fnz=%lu ",
        (unsigned long)m->video.gfx_scroll_x,
        (unsigned long)m->video.gfx_scroll_y,
        (long)m->video.clip_x,
        (long)m->video.clip_y,
        (long)m->video.clip_w,
        (long)m->video.clip_h,
        (unsigned long)m->video.last_list_addr,
        (unsigned long)m->video.last_upload_addr,
        (unsigned long)m->video.last_upload_x,
        (unsigned long)m->video.last_upload_y,
        (unsigned long)m->video.last_upload_w,
        (unsigned long)m->video.last_upload_h,
        (unsigned long)m->video.last_upload_nonzero,
        (unsigned long)m->video.upload_nonzero_total,
        (unsigned long)m->video.last_draw_addr,
        (unsigned long)m->video.last_draw_flags,
        (unsigned long)m->video.last_draw_alpha,
        (unsigned long)m->video.last_draw_src_x,
        (unsigned long)m->video.last_draw_src_y,
        (long)m->video.last_draw_dst_x,
        (long)m->video.last_draw_dst_y,
        (unsigned long)m->video.last_draw_w,
        (unsigned long)m->video.last_draw_h,
        (unsigned long)m->video.last_draw_src_nonzero,
        (unsigned long)m->video.last_draw_written,
        (unsigned long)m->video.last_draw_written_nonzero,
        (unsigned long)m->video.last_frame_nonzero);
    p += sprintf(p, "last_dma=%08lx>%08lx/%lu/%08lx/m%lu%lu/%08lx np=%lu-%lu nb=%lu-%lu nc=%lu-%lu irq_req=%lu/%lu/%08lx irqcpu=%lu/%lu/%04x ex=%lu/%08lx/%08lx ",
        (unsigned long)m->last_dma_sar,
        (unsigned long)m->last_dma_dar,
        (unsigned long)m->last_dma_tcr,
        (unsigned long)m->last_dma_chcr,
        (unsigned long)m->last_dma_src_mode,
        (unsigned long)m->last_dma_dst_mode,
        (unsigned long)m->last_dma_status,
        (unsigned long)m->last_dma_nand_page0,
        (unsigned long)m->last_dma_nand_page1,
        (unsigned long)m->last_dma_nand_block0,
        (unsigned long)m->last_dma_nand_block1,
        (unsigned long)m->last_dma_nand_col0,
        (unsigned long)m->last_dma_nand_col1,
        (unsigned long)m->irq_requests,
        (unsigned long)m->irq_last_level,
        (unsigned long)m->irq_last_event,
        (unsigned long)m->cpu.irq_line,
        (unsigned long)m->cpu.irq_level,
        (unsigned)cv1k_shio_read_be16_direct(m, 0x16UL),
        (unsigned long)m->exception_events,
        (unsigned long)m->exception_last_event,
        (unsigned long)m->exception_last_tra);
    p += sprintf(p, "tmu=%lu/%lu/%lu:%08lx/%lu assists=%lu fpga_bits=%lu fpga_done=%lu fpga_sum=%02lx fpga_fw=%ld icache=%lu/%lu ",
        (unsigned long)m->tmu_underflows[0],
        (unsigned long)m->tmu_underflows[1],
        (unsigned long)m->tmu_underflows[2],
        (unsigned long)m->tmu_last_event,
        (unsigned long)m->tmu_last_priority,
        (unsigned long)m->boot_assists,
        (unsigned long)m->video.fpga_firmware_pos,
        (unsigned long)m->video.fpga_firmware_done,
        (unsigned long)m->video.fpga_firmware_checksum,
        (long)m->video.fpga_firmware_version,
        (unsigned long)m->icache_hits,
        (unsigned long)m->icache_misses);
    p += sprintf(p, "dcache=%lu/%lu stale=%lu mcache=%d/%lu/%lu/%lu/%lu/%lu/%lu cachectl=%d mtrap=%d mspeed=%d/%lu active=%08lx breg=%08lx/%08lx/%08lx/%08lx mmio=%lu/%08lx autoblit=%lu/%06lx-%06lx skip=%lu fulldma=%d mtmu=%d widep0=%d compact400=%d dmasync=%d dmainv=%lu ndata=%d ports=C%02x/%lu@%08lx D%02x/%lu@%08lx E%02x/%lu@%08lx F%02x/%lu@%08lx L%02x/%lu@%08lx nandcmd=%02lx pg=%lu col=%lu rnd=%lu spr=%lu nmap=%lu/%lu/%lu/%lu ce=%d ",
        (unsigned long)m->dcache_hits,
        (unsigned long)m->dcache_misses,
        (unsigned long)m->dcache_dma_stale,
        m->mame_cache_meta,
        (unsigned long)m->mame_cache_hits,
        (unsigned long)m->mame_cache_misses,
        (unsigned long)m->mame_cache_dirty_evicts,
        (unsigned long)m->mame_cache_fetches,
        (unsigned long)m->mame_cache_reads,
        (unsigned long)m->mame_cache_writes,
        m->strict_cache_ops,
        m->mame_trapa,
        m->mame_speedup,
        (unsigned long)m->mame_speedup_spins,
        (unsigned long)m->last_active_pc,
        (unsigned long)m->video.regs[0x04UL >> 2],
        (unsigned long)m->video.regs[0x08UL >> 2],
        (unsigned long)m->video.regs[0x14UL >> 2],
        (unsigned long)m->video.regs[0x18UL >> 2],
        (unsigned long)m->video.mmio_execs,
        (unsigned long)m->video.last_mmio_list_addr,
        (unsigned long)m->auto_blits,
        (unsigned long)m->auto_blit_last_base,
        (unsigned long)m->auto_blit_last_end,
        (unsigned long)m->auto_blit_skips,
        m->mame_full_dmatcr,
        m->mame_tmu_irq,
        m->wide_p0_alias,
        m->compact_400_alias,
        m->dma_cache_sync,
        (unsigned long)m->dma_cache_invalidations,
        m->nand.data_only_reads,
        (unsigned)m->port_last_c, (unsigned long)m->port_reads_c, (unsigned long)m->port_pc_c,
        (unsigned)m->port_last_d, (unsigned long)m->port_reads_d, (unsigned long)m->port_pc_d,
        (unsigned)m->port_last_e, (unsigned long)m->port_reads_e, (unsigned long)m->port_pc_e,
        (unsigned)m->port_last_f, (unsigned long)m->port_reads_f, (unsigned long)m->port_pc_f,
        (unsigned)m->port_last_l, (unsigned long)m->port_reads_l, (unsigned long)m->port_pc_l,
        (unsigned long)m->nand.command,
        (unsigned long)m->nand.page,
        (unsigned long)m->nand.column,
        (unsigned long)m->nand.random_reads,
        (unsigned long)m->nand.spare_reads,
        (unsigned long)m->nand.map_blocks,
        (unsigned long)m->nand.map_empty_blocks,
        (unsigned long)m->nand.map_oob_marked_blocks,
        (unsigned long)m->nand.map_spare_non_ff_pages,
        m->nand.ce_enabled);
    p += sprintf(p, "alias=%lu/%lu/%lu/%lu/%lu/%lu tlb=%lu/%lu/%lu:%08lx>%08lx ",
        (unsigned long)m->alias_p1p2,
        (unsigned long)m->alias_p4,
        (unsigned long)m->alias_p0_wide,
        (unsigned long)m->alias_p0_work,
        (unsigned long)m->alias_400,
        (unsigned long)m->alias_e0,
        (unsigned long)m->tlb_loads,
        (unsigned long)m->tlb_hits,
        (unsigned long)m->tlb_misses,
        (unsigned long)m->tlb_last_virt,
        (unsigned long)m->tlb_last_phys);
    sprintf(p, "unmapped_r=%lu last_r=%08lx unmapped_w=%lu last_w=%08lx:%02lx",
        (unsigned long)m->unmapped_reads,
        (unsigned long)m->last_unmapped_read,
        (unsigned long)m->unmapped_writes,
        (unsigned long)m->last_unmapped_write,
        (unsigned long)m->last_unmapped_write_data);
    CV1K_UNUSED(out_size);
}

void cv1k_machine_render_probe(struct cv1k_machine *m, const char *line1, const char *line2, const char *line3)
{
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_u32 shade;
    char status[256];
    if (m == NULL) return;
    if (m->video.screen_rgb == NULL) return;
    for (y = 0UL; y < CV1K_SCREEN_H; y++) {
        for (x = 0UL; x < CV1K_SCREEN_W; x++) {
            shade = ((x * 3UL + y * 5UL + m->frames * 7UL) & 0x3fUL);
            m->video.screen_rgb[y * CV1K_FRAMEBUFFER_W + x] = (shade << 16) | ((shade / 2UL) << 8) | (0x40UL + shade);
        }
    }
    cv1k_video_rect_rgb(&m->video, 8UL, 8UL, 304UL, 224UL, 0x080818UL);
    cv1k_video_rect_rgb(&m->video, 12UL, 12UL, 296UL, 216UL, 0x101020UL);
    cv1k_video_text_rgb(&m->video, 36UL, 28UL, "DODONPACHI", 0xffe060UL, 3UL);
    cv1k_video_text_rgb(&m->video, 37UL, 64UL, "SAIDAIOUJOU", 0xe8e8ffUL, 2UL);
    cv1k_video_text_rgb(&m->video, 28UL, 96UL, "CV1000-D ROM PROBE", 0x80ffb0UL, 1UL);
    if (line1 != NULL) cv1k_video_text_rgb(&m->video, 28UL, 116UL, line1, 0xffffffUL, 1UL);
    if (line2 != NULL) cv1k_video_text_rgb(&m->video, 28UL, 130UL, line2, 0xffffffUL, 1UL);
    if (line3 != NULL) cv1k_video_text_rgb(&m->video, 28UL, 144UL, line3, 0xffffffUL, 1UL);
    sprintf(status, "PC %08lx ILLEGAL %lu DMA %lu", (unsigned long)m->cpu.pc, (unsigned long)m->cpu.illegal_count, (unsigned long)m->dma_transfers);
    cv1k_video_text_rgb(&m->video, 28UL, 166UL, status, 0xa0c8ffUL, 1UL);
    sprintf(status, "FPGA %lu/2323240 DONE %lu SUM %02lx", (unsigned long)m->video.fpga_firmware_pos, (unsigned long)m->video.fpga_firmware_done, (unsigned long)m->video.fpga_firmware_checksum);
    cv1k_video_text_rgb(&m->video, 28UL, 180UL, status, 0xa0ffdcUL, 1UL);
    cv1k_video_text_rgb(&m->video, 28UL, 196UL, "NOT A REAL GAME TITLESCREEN YET", 0xff8080UL, 1UL);
    cv1k_video_text_rgb(&m->video, 28UL, 210UL, "SDL3 FRONTEND DISPLAYS THIS FRAMEBUFFER", 0xc0c0c0UL, 1UL);
}
