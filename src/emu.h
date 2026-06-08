#ifndef CV1K_EMU_H
#define CV1K_EMU_H

#include "cv1k_types.h"
#include "cv1k_config.h"
#include "cpu_sh7709s.h"
#include "nand.h"
#include "rtc9701.h"
#include "sound_ymz770.h"
#include "video.h"
#include "input.h"
#include "bus.h"

struct cv1k_machine {
    int model;
    struct sh7709s_cpu cpu;
    struct cv1k_bus bus;
    struct cv1k_nand nand;
    struct cv1k_rtc9701 rtc;
    struct cv1k_ymz770 ymz;
    struct cv1k_video video;
    struct cv1k_input input;
    cv1k_u8 *boot_rom;
    cv1k_u32 boot_rom_size;
    cv1k_u8 *main_ram;
    cv1k_u32 main_ram_size;
    cv1k_u8 *cache_ram;
    cv1k_u32 cache_ram_size;
    cv1k_u8 *sound_rom;
    cv1k_u32 sound_rom_size;
    cv1k_u8 sh_io[CV1K_REGION_SH_IO_SIZE];
    cv1k_u32 tlb_vpn[16];
    cv1k_u32 tlb_ppn[16];
    cv1k_u32 tlb_mask[16];
    cv1k_u8 tlb_valid[16];
    cv1k_u32 tlb_loads;
    cv1k_u32 tlb_hits;
    cv1k_u32 tlb_misses;
    cv1k_u32 tlb_last_virt;
    cv1k_u32 tlb_last_phys;
    cv1k_u32 icache_tag[CV1K_ICACHE_LINES];
    cv1k_u16 icache_op[CV1K_ICACHE_LINES];
    cv1k_u8 icache_valid[CV1K_ICACHE_LINES];
    cv1k_u32 icache_hits;
    cv1k_u32 icache_misses;
    cv1k_u32 dcache_tag[CV1K_DCACHE_LINES];
    cv1k_u8 dcache_data[CV1K_DCACHE_LINES][CV1K_DCACHE_LINE_SIZE];
    cv1k_u8 dcache_valid[CV1K_DCACHE_LINES];
    cv1k_u32 dcache_hits;
    cv1k_u32 dcache_misses;
    cv1k_u32 dcache_dma_stale;
    int mame_cache_meta;
    cv1k_u32 mame_cache_tag[CV1K_SH7709S_CACHE_BLOCKS][CV1K_SH7709S_CACHE_ASSOCIATIVITY];
    cv1k_u8 mame_cache_lru[CV1K_SH7709S_CACHE_BLOCKS][CV1K_SH7709S_CACHE_ASSOCIATIVITY];
    cv1k_u8 mame_cache_dirty[CV1K_SH7709S_CACHE_BLOCKS][CV1K_SH7709S_CACHE_ASSOCIATIVITY];
    cv1k_u32 mame_cache_hits;
    cv1k_u32 mame_cache_misses;
    cv1k_u32 mame_cache_dirty_evicts;
    cv1k_u32 mame_cache_fetches;
    cv1k_u32 mame_cache_reads;
    cv1k_u32 mame_cache_writes;
    int sh7709s_cache_timing;
    int sh7709s_cache_timing_suppress;
    cv1k_u32 sh7709s_cache_wb_address;
    cv1k_u8 sh7709s_cache_last_area;
    cv1k_u8 sh7709s_cache_last_was_write;
    cv1k_u32 sh7709s_cache_penalty_cycles;
    cv1k_u32 sh7709s_cache_penalty_events;
    int irq2_enabled;
    int aggressive_boot_assists;
    int dcache_enabled;
    int strict_cache_ops;
    int wide_p0_alias;
    int compact_400_alias;
    int dma_cache_sync;
    cv1k_u32 dma_cache_invalidations;
    cv1k_u32 alias_p1p2;
    cv1k_u32 alias_p4;
    cv1k_u32 alias_p0_wide;
    cv1k_u32 alias_p0_work;
    cv1k_u32 alias_400;
    cv1k_u32 alias_e0;
    cv1k_u32 frames;
    cv1k_u32 dma_transfers;
    cv1k_u32 dma_bytes;
    cv1k_u32 last_dma_sar;
    cv1k_u32 last_dma_dar;
    cv1k_u32 last_dma_tcr;
    cv1k_u32 last_dma_chcr;
    cv1k_u32 last_dma_src_mode;
    cv1k_u32 last_dma_dst_mode;
    cv1k_u32 last_dma_status;
    cv1k_u32 dma_timer_active[4];
    cv1k_u32 dma_timer_due[4];
    cv1k_u32 dma_timer_chcr[4];
    cv1k_u32 dma_timer_base[4];
    cv1k_u32 dma_timer_mask;
    cv1k_u32 last_dma_nand_page0;
    cv1k_u32 last_dma_nand_page1;
    cv1k_u32 last_dma_nand_block0;
    cv1k_u32 last_dma_nand_block1;
    cv1k_u32 last_dma_nand_col0;
    cv1k_u32 last_dma_nand_col1;
    cv1k_u32 unmapped_reads;
    cv1k_u32 unmapped_writes;
    cv1k_u32 last_unmapped_read;
    cv1k_u32 last_unmapped_write;
    cv1k_u32 last_unmapped_write_data;
    cv1k_u32 boot_assists;
    cv1k_u32 irq_requests;
    cv1k_u32 irq_last_event;
    cv1k_u32 irq_last_level;
    cv1k_u32 exception_events;
    cv1k_u32 exception_last_event;
    cv1k_u32 exception_last_tra;
    int vblank_irq_and_tick;
    int mame_trapa;
    int mame_speedup;
    int render_screen;
    int mame_full_dmatcr;
    int mame_tmu_irq;
    int threaded_render;
    int threaded_audio;
    int display_rotation;
    int video_renderer;
    int gles2_tile_cache;
    int gles2_gpu_blitter;
    cv1k_u32 threaded_render_jobs;
    cv1k_u32 threaded_audio_jobs;
    cv1k_u32 mame_speedup_spins;
    cv1k_u32 tmu_underflows[3];
    cv1k_u32 tmu_last_event;
    cv1k_u32 tmu_last_priority;
    cv1k_u32 tmu_last_cycles[3];
    cv1k_u32 port_reads_c;
    cv1k_u32 port_reads_d;
    cv1k_u32 port_reads_e;
    cv1k_u32 port_reads_f;
    cv1k_u32 port_reads_l;
    cv1k_u32 port_pc_c;
    cv1k_u32 port_pc_d;
    cv1k_u32 port_pc_e;
    cv1k_u32 port_pc_f;
    cv1k_u32 port_pc_l;
    cv1k_u8 port_last_c;
    cv1k_u8 port_last_d;
    cv1k_u8 port_last_e;
    cv1k_u8 port_last_f;
    cv1k_u8 port_last_l;
    cv1k_u32 last_active_pc;
    cv1k_u32 auto_blits;
    cv1k_u32 auto_blit_last_base;
    cv1k_u32 auto_blit_last_end;
    cv1k_u32 auto_blit_last_sig;
    cv1k_u32 auto_blit_skips;
    cv1k_u32 video_busy_sync_cycles;
    cv1k_u32 last_blitter_status_pc;
    cv1k_u32 blitter_status_spin_reads;
};

int cv1k_machine_init(struct cv1k_machine *m, int model);
void cv1k_machine_shutdown(struct cv1k_machine *m);
void cv1k_machine_reset(struct cv1k_machine *m);
int cv1k_machine_load_boot(struct cv1k_machine *m, const char *path);
int cv1k_machine_load_nand(struct cv1k_machine *m, const char *path);
int cv1k_machine_load_sound(struct cv1k_machine *m, const char *path);
int cv1k_machine_load_ram(struct cv1k_machine *m, const char *path);
void cv1k_machine_blit(struct cv1k_machine *m, cv1k_u32 addr);
void cv1k_machine_step(struct cv1k_machine *m);
void cv1k_machine_frame(struct cv1k_machine *m);
void cv1k_machine_frame_advance(struct cv1k_machine *m, int render);
void cv1k_machine_sync_video_busy(struct cv1k_machine *m);
void cv1k_machine_fast_forward_blitter_busy(struct cv1k_machine *m);
void cv1k_machine_status(const struct cv1k_machine *m, char *out, cv1k_u32 out_size);
void cv1k_machine_render_probe(struct cv1k_machine *m, const char *line1, const char *line2, const char *line3);

#endif
