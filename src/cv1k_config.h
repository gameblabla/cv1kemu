#ifndef CV1K_CONFIG_H
#define CV1K_CONFIG_H

/* SH7709S cache-access timing model.
 *
 * MAME modelled the SH-3 cache cycle-accurately starting with commit
 * b7c814a (in 0.288); that adds sh7709s_device::cache_access/access_penalty/
 * drc_update_icache to the hot path and costs ~25-30% on CV1000 titles (it is
 * why a 0.288 build is much slower than the pre-cache 0.282 binary).  Our
 * equivalent models (--mame-cache-meta / --sh7709s-cache-timing) drive
 * cv1k_bus_cache_access() from every SH-3 memory access.
 *
 * Default (CV1K_CACHE_ACCURATE == 0): the per-access cache hooks are compiled
 * out entirely, so the fast build pays nothing for them (matches 0.282-era
 * speed).  Build with `make ... CACHE=accurate` (-DCV1K_CACHE_ACCURATE=1) to
 * compile the hooks back in and default the accurate cache timing on, matching
 * MAME 0.288's behaviour. */
#ifndef CV1K_CACHE_ACCURATE
#define CV1K_CACHE_ACCURATE 0
#endif

#define CV1K_SCREEN_W 320U
#define CV1K_SCREEN_H 240U
#define CV1K_FRAMEBUFFER_W 512U
#define CV1K_FRAMEBUFFER_H 512U
#define CV1K_VRAM_W 0x2000UL
#define CV1K_VRAM_H 0x1000UL
#define CV1K_VRAM_PIXELS (CV1K_VRAM_W * CV1K_VRAM_H)
#define CV1K_VRAM_TILE_W 256UL
#define CV1K_VRAM_TILE_H 256UL
#define CV1K_VRAM_TILES_X (CV1K_VRAM_W / CV1K_VRAM_TILE_W)
#define CV1K_VRAM_TILES_Y (CV1K_VRAM_H / CV1K_VRAM_TILE_H)
#define CV1K_VRAM_TILE_COUNT (CV1K_VRAM_TILES_X * CV1K_VRAM_TILES_Y)
#define CV1K_CLIP_MARGIN 32UL

#define CV1K_BOOT_ROM_MAX (4UL * 1024UL * 1024UL)
#define CV1K_MAIN_RAM_B_SIZE (8UL * 1024UL * 1024UL)
#define CV1K_MAIN_RAM_D_SIZE (16UL * 1024UL * 1024UL)
#define CV1K_CACHE_RAM_SIZE (16UL * 1024UL * 1024UL)
#define CV1K_NAND_DEFAULT_SIZE (0x08400000UL) /* 128MiB page data plus 4MiB OOB/spare, matching MAME ROM_REGION. */
#define CV1K_SOUND_ROM_MAX (8UL * 1024UL * 1024UL)

#define CV1K_CPU_CLOCK_HZ 102400000UL
#define CV1K_YMZ770_CLOCK_HZ 16384000UL
#define CV1K_RTC_CLOCK_HZ 32768UL
#define CV1K_REFRESH_MILLIHZ 60024UL
#define CV1K_CYCLES_PER_VBLANK 1705984UL /* 102.4MHz / 60.024Hz, matching MAME CV1000 board clocks. */

/* The SH7709S on-chip TMU is fed by the peripheral module clock (Pphi), not the
 * CPU clock (Iphi).  On the CV1000 the CPG runs Pphi at Iphi/4, exactly like
 * MAME's sh34_base_device::sh4_parse_configuration (m_pm_clock = m_clock / 4),
 * whose TMU schedules underflows from from_hz(m_pm_clock) * prescaler.  Our TMU
 * counts CV1K_CPU_CLOCK_HZ cycles, so the on-chip prescaler must be scaled by
 * this factor or the timers (and any game logic they drive, e.g. the YMZ770
 * music sequencer re-trigger) run 4x too fast. */
#define CV1K_TMU_PCLK_DIV 4UL

#define CV1K_ADDR_BOOT_ROM 0x00000000UL
#define CV1K_ADDR_WORK_RAM 0x0c000000UL
#define CV1K_ADDR_NAND_IO  0x10000000UL
#define CV1K_ADDR_YMZ770   0x10400000UL
#define CV1K_ADDR_RTC_EE   0x10c00000UL
#define CV1K_ADDR_BLITTER  0x18000000UL
#define CV1K_ADDR_CACHE    0xf0000000UL

#define CV1K_REGION_BOOT_ROM_SIZE 0x00400000UL
#define CV1K_REGION_IO_SIZE 8UL
#define CV1K_REGION_BLITTER_SIZE 0x58UL
#define CV1K_ADDR_SH_IO    0x04000000UL
#define CV1K_REGION_SH_IO_SIZE 0x00010000UL
#define CV1K_ICACHE_LINES 65536UL
#define CV1K_DCACHE_LINES 4096UL
#define CV1K_DCACHE_LINE_SIZE 32UL

#define CV1K_SH7709S_CACHE_SIZE (16UL * 1024UL)
#define CV1K_SH7709S_CACHE_LINE_SIZE 16UL
#define CV1K_SH7709S_CACHE_ASSOCIATIVITY 4UL
#define CV1K_SH7709S_CACHE_ENTRY_COUNT (CV1K_SH7709S_CACHE_SIZE / CV1K_SH7709S_CACHE_LINE_SIZE)
#define CV1K_SH7709S_CACHE_BLOCKS (CV1K_SH7709S_CACHE_ENTRY_COUNT / CV1K_SH7709S_CACHE_ASSOCIATIVITY)

#define CV1K_MODEL_B 0
#define CV1K_MODEL_D 1

#define CV1K_NAND_PAGE_SIZE 2048UL
#define CV1K_NAND_OOB_SIZE 64UL
#define CV1K_NAND_PAGE_TOTAL (CV1K_NAND_PAGE_SIZE + CV1K_NAND_OOB_SIZE)
#define CV1K_NAND_PAGES_PER_BLOCK 64UL
#define CV1K_NAND_BLOCK_TOTAL (CV1K_NAND_PAGE_TOTAL * CV1K_NAND_PAGES_PER_BLOCK)

#endif
