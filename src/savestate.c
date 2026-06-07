#include "savestate.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>

#define SS_MAGIC "CV1KSS50"

static int write_u32(FILE *f, cv1k_u32 v)
{
    cv1k_u8 b[4];
    cv1k_put_be32(b, v);
    return fwrite(b, 1U, 4U, f) == 4U;
}

static int read_u32(FILE *f, cv1k_u32 *v)
{
    cv1k_u8 b[4];
    if (fread(b, 1U, 4U, f) != 4U) return 0;
    *v = cv1k_be32(b);
    return 1;
}

static int write_block(FILE *f, const void *p, cv1k_u32 n)
{
    return fwrite(p, 1U, (size_t)n, f) == (size_t)n;
}

static int read_block(FILE *f, void *p, cv1k_u32 n)
{
    return fread(p, 1U, (size_t)n, f) == (size_t)n;
}

static int save_cpu(FILE *f, struct sh7709s_cpu *cpu)
{
    cv1k_u32 i;
    for (i = 0UL; i < 16UL; i++) if (!write_u32(f, cpu->r[i])) return 0;
    for (i = 0UL; i < 8UL; i++) if (!write_u32(f, cpu->rb[i])) return 0;
    if (!write_u32(f, cpu->pc) || !write_u32(f, cpu->pr) || !write_u32(f, cpu->gbr) || !write_u32(f, cpu->vbr)) return 0;
    if (!write_u32(f, cpu->mach) || !write_u32(f, cpu->macl) || !write_u32(f, cpu->sr) || !write_u32(f, cpu->cycles)) return 0;
    if (!write_u32(f, cpu->ssr) || !write_u32(f, cpu->spc) || !write_u32(f, cpu->sgr) || !write_u32(f, cpu->tra)) return 0;
    if (!write_u32(f, cpu->illegal_count) || !write_u32(f, cpu->halted) || !write_u32(f, cpu->irq_pending)) return 0;
    if (!write_u32(f, cpu->irq_line) || !write_u32(f, cpu->irq_level) || !write_u32(f, cpu->irq_event) || !write_u32(f, cpu->irq_ack_count) || !write_u32(f, cpu->exception_count)) return 0;
    if (!write_u32(f, cpu->last_illegal_pc) || !write_u32(f, cpu->last_illegal_op)) return 0;
    return 1;
}

static int load_cpu(FILE *f, struct sh7709s_cpu *cpu)
{
    cv1k_u32 i;
    for (i = 0UL; i < 16UL; i++) if (!read_u32(f, &cpu->r[i])) return 0;
    for (i = 0UL; i < 8UL; i++) if (!read_u32(f, &cpu->rb[i])) return 0;
    if (!read_u32(f, &cpu->pc) || !read_u32(f, &cpu->pr) || !read_u32(f, &cpu->gbr) || !read_u32(f, &cpu->vbr)) return 0;
    if (!read_u32(f, &cpu->mach) || !read_u32(f, &cpu->macl) || !read_u32(f, &cpu->sr) || !read_u32(f, &cpu->cycles)) return 0;
    if (!read_u32(f, &cpu->ssr) || !read_u32(f, &cpu->spc) || !read_u32(f, &cpu->sgr) || !read_u32(f, &cpu->tra)) return 0;
    if (!read_u32(f, &cpu->illegal_count) || !read_u32(f, &cpu->halted) || !read_u32(f, &cpu->irq_pending)) return 0;
    if (!read_u32(f, &cpu->irq_line) || !read_u32(f, &cpu->irq_level) || !read_u32(f, &cpu->irq_event) || !read_u32(f, &cpu->irq_ack_count) || !read_u32(f, &cpu->exception_count)) return 0;
    if (!read_u32(f, &cpu->last_illegal_pc) || !read_u32(f, &cpu->last_illegal_op)) return 0;
    return 1;
}

static int save_nand(FILE *f, struct cv1k_nand *n)
{
    if (!write_u32(f, n->size) || !write_block(f, n->data, n->size)) return 0;
    if (!write_block(f, n->page_reg, CV1K_NAND_PAGE_TOTAL)) return 0;
    if (!write_u32(f, n->cursor) || !write_u32(f, n->page) || !write_u32(f, n->column) || !write_u32(f, n->addr_latch)) return 0;
    if (!write_u32(f, n->program_base) || !write_u32(f, n->read_area_offset) || !write_block(f, n->address_bytes, 5UL)) return 0;
    if (!write_u32(f, (cv1k_u32)n->address_count) || !write_u32(f, (cv1k_u32)n->command) || !write_u32(f, (cv1k_u32)n->prev_command)) return 0;
    if (!write_u32(f, (cv1k_u32)n->id_index) || !write_u32(f, (cv1k_u32)n->busy) || !write_u32(f, (cv1k_u32)n->status)) return 0;
    if (!write_u32(f, (cv1k_u32)n->manufacturer) || !write_u32(f, (cv1k_u32)n->device)) return 0;
    if (!write_u32(f, (cv1k_u32)n->address_expected) || !write_u32(f, (cv1k_u32)n->random_read_pending)) return 0;
    if (!write_block(f, n->id, 5UL) || !write_u32(f, (cv1k_u32)n->id_len)) return 0;
    if (!write_u32(f, (cv1k_u32)n->mode) || !write_u32(f, (cv1k_u32)n->pointer_mode) || !write_u32(f, (cv1k_u32)n->addr_load_ptr)) return 0;
    if (!write_u32(f, (cv1k_u32)n->mode_3065) || !write_u32(f, n->page_addr) || !write_u32(f, n->byte_addr) || !write_u32(f, n->program_byte_count)) return 0;
    if (!write_u32(f, (cv1k_u32)n->accumulated_status)) return 0;
    if (!write_u32(f, n->reads) || !write_u32(f, n->writes) || !write_u32(f, n->erases)) return 0;
    if (!write_u32(f, n->random_reads) || !write_u32(f, n->spare_reads) || !write_u32(f, n->status_reads) || !write_u32(f, n->id_reads)) return 0;
    if (!write_block(f, n->command_counts, (cv1k_u32)sizeof(n->command_counts))) return 0;
    if (!write_u32(f, n->map_blocks) || !write_u32(f, n->map_empty_blocks) || !write_u32(f, n->map_oob_marked_blocks) || !write_u32(f, n->map_spare_non_ff_pages)) return 0;
    if (!write_u32(f, n->last_read_page) || !write_u32(f, n->last_read_block) || !write_u32(f, n->last_read_column)) return 0;
    if (!write_u32(f, (cv1k_u32)n->data_only_reads) || !write_u32(f, (cv1k_u32)n->ce_enabled)) return 0;
    return 1;
}

static int load_nand(FILE *f, struct cv1k_nand *n)
{
    cv1k_u32 size;
    cv1k_u32 tmp;
    if (!read_u32(f, &size) || size != n->size || !read_block(f, n->data, n->size)) return 0;
    if (!read_block(f, n->page_reg, CV1K_NAND_PAGE_TOTAL)) return 0;
    if (!read_u32(f, &n->cursor) || !read_u32(f, &n->page) || !read_u32(f, &n->column) || !read_u32(f, &n->addr_latch)) return 0;
    if (!read_u32(f, &n->program_base) || !read_u32(f, &n->read_area_offset) || !read_block(f, n->address_bytes, 5UL)) return 0;
    if (!read_u32(f, &tmp)) return 0;
    n->address_count = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->command = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->prev_command = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->id_index = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->busy = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->status = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->manufacturer = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->device = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->address_expected = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->random_read_pending = (cv1k_u8)tmp;
    if (!read_block(f, n->id, 5UL)) return 0;
    if (!read_u32(f, &tmp)) return 0;
    n->id_len = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->mode = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->pointer_mode = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->addr_load_ptr = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->mode_3065 = (cv1k_u8)tmp;
    if (!read_u32(f, &n->page_addr) || !read_u32(f, &n->byte_addr) || !read_u32(f, &n->program_byte_count)) return 0;
    if (!read_u32(f, &tmp)) return 0;
    n->accumulated_status = (cv1k_u8)tmp;
    if (!read_u32(f, &n->reads) || !read_u32(f, &n->writes) || !read_u32(f, &n->erases)) return 0;
    if (!read_u32(f, &n->random_reads) || !read_u32(f, &n->spare_reads) || !read_u32(f, &n->status_reads) || !read_u32(f, &n->id_reads)) return 0;
    if (!read_block(f, n->command_counts, (cv1k_u32)sizeof(n->command_counts))) return 0;
    if (!read_u32(f, &n->map_blocks) || !read_u32(f, &n->map_empty_blocks) || !read_u32(f, &n->map_oob_marked_blocks) || !read_u32(f, &n->map_spare_non_ff_pages)) return 0;
    if (!read_u32(f, &n->last_read_page) || !read_u32(f, &n->last_read_block) || !read_u32(f, &n->last_read_column)) return 0;
    if (!read_u32(f, &tmp)) return 0;
    n->data_only_reads = (int)tmp;
    if (!read_u32(f, &tmp)) return 0;
    n->ce_enabled = (int)tmp;
    return 1;
}

static int save_video(FILE *f, struct cv1k_video *v)
{
    cv1k_u32 i;
    for (i = 0UL; i < (0x58UL / 4UL); i++) if (!write_u32(f, v->regs[i])) return 0;
    if (!write_u32(f, cv1k_video_vram_bytes()) || !write_block(f, v->vram1555, cv1k_video_vram_bytes())) return 0;
    if (!write_u32(f, (cv1k_u32)(CV1K_FRAMEBUFFER_W * CV1K_FRAMEBUFFER_H * sizeof(cv1k_u32))) || !write_block(f, v->screen_rgb, (cv1k_u32)(CV1K_FRAMEBUFFER_W * CV1K_FRAMEBUFFER_H * sizeof(cv1k_u32)))) return 0;
    if (!write_u32(f, v->frame_counter) || !write_u32(f, v->executed_ops) || !write_u32(f, v->upload_ops) || !write_u32(f, v->draw_ops)) return 0;
    if (!write_u32(f, v->unknown_ops) || !write_u32(f, v->clip_ops) || !write_u32(f, v->last_unknown_op) || !write_u32(f, v->busy_cycles_ns)) return 0;
    if (!write_u32(f, v->blit_idle_op_bytes) || !write_u32(f, v->blit_hline_penalty_ns) || !write_u32(f, v->blit_over_frame_count)) return 0;
    if (!write_u32(f, v->gfx_scroll_x) || !write_u32(f, v->gfx_scroll_y)) return 0;
    if (!write_u32(f, (cv1k_u32)v->clip_x) || !write_u32(f, (cv1k_u32)v->clip_y) || !write_u32(f, (cv1k_u32)v->clip_w) || !write_u32(f, (cv1k_u32)v->clip_h)) return 0;
    if (!write_u32(f, (cv1k_u32)v->busy)) return 0;
    if (!write_u32(f, v->fpga_firmware_pos) || !write_u32(f, v->fpga_firmware_checksum) || !write_u32(f, v->fpga_firmware_done)) return 0;
    if (!write_u32(f, (cv1k_u32)v->fpga_firmware_version)) return 0;
    if (!write_u32(f, (cv1k_u32)v->fpga_firmware_port) || !write_u32(f, (cv1k_u32)v->fpga_firmware_byte)) return 0;
    return 1;
}

static int load_video(FILE *f, struct cv1k_video *v)
{
    cv1k_u32 i;
    cv1k_u32 size;
    cv1k_u32 tmp;
    for (i = 0UL; i < (0x58UL / 4UL); i++) if (!read_u32(f, &v->regs[i])) return 0;
    if (!read_u32(f, &size) || size != cv1k_video_vram_bytes() || !read_block(f, v->vram1555, size)) return 0;
    cv1k_video_mark_vram_dirty_all(v);
    tmp = (cv1k_u32)(CV1K_FRAMEBUFFER_W * CV1K_FRAMEBUFFER_H * sizeof(cv1k_u32));
    if (!read_u32(f, &size) || size != tmp || !read_block(f, v->screen_rgb, size)) return 0;
    if (!read_u32(f, &v->frame_counter) || !read_u32(f, &v->executed_ops) || !read_u32(f, &v->upload_ops) || !read_u32(f, &v->draw_ops)) return 0;
    if (!read_u32(f, &v->unknown_ops) || !read_u32(f, &v->clip_ops) || !read_u32(f, &v->last_unknown_op) || !read_u32(f, &v->busy_cycles_ns)) return 0;
    if (!read_u32(f, &v->blit_idle_op_bytes) || !read_u32(f, &v->blit_hline_penalty_ns) || !read_u32(f, &v->blit_over_frame_count)) return 0;
    if (!read_u32(f, &v->gfx_scroll_x) || !read_u32(f, &v->gfx_scroll_y)) return 0;
    if (!read_u32(f, &tmp)) return 0;
    v->clip_x = (cv1k_s32)tmp;
    if (!read_u32(f, &tmp)) return 0;
    v->clip_y = (cv1k_s32)tmp;
    if (!read_u32(f, &tmp)) return 0;
    v->clip_w = (cv1k_s32)tmp;
    if (!read_u32(f, &tmp)) return 0;
    v->clip_h = (cv1k_s32)tmp;
    if (!read_u32(f, &tmp)) return 0;
    v->busy = (cv1k_u8)tmp;
    if (!read_u32(f, &v->fpga_firmware_pos) || !read_u32(f, &v->fpga_firmware_checksum) || !read_u32(f, &v->fpga_firmware_done)) return 0;
    if (!read_u32(f, &tmp)) return 0;
    v->fpga_firmware_version = (cv1k_s32)tmp;
    if (!read_u32(f, &tmp)) return 0;
    v->fpga_firmware_port = (cv1k_u8)tmp;
    if (!read_u32(f, &tmp)) return 0;
    v->fpga_firmware_byte = (cv1k_u8)tmp;
    /* Older save states do not carry the v55 MMIO/visible-frame diagnostic
     * fields.  Treat any state with prior blitter execution as already past
     * the bootstrap fallback phase so loading a title-screen state cannot
     * immediately replay a guessed DDPSDOJ RAM list over it.
     */
    if (v->executed_ops != 0UL) {
        v->last_frame_nonzero = 1UL;
        v->mmio_execs = 1UL;
        v->last_mmio_list_addr = v->regs[0x08UL >> 2] & 0x1fffffffUL;
    }
    return 1;
}

int cv1k_save_state(struct cv1k_machine *m, const char *path)
{
    FILE *f;
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    if (!write_block(f, SS_MAGIC, 8UL)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->model)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->irq2_enabled)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->aggressive_boot_assists)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->dcache_enabled)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->strict_cache_ops)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->mame_cache_meta)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->wide_p0_alias)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->compact_400_alias)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->dma_cache_sync)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->vblank_irq_and_tick)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->mame_trapa)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->mame_speedup)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->mame_full_dmatcr)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)m->mame_tmu_irq)) { fclose(f); return 0; }
    if (!write_u32(f, m->mame_speedup_spins)) { fclose(f); return 0; }
    if (!write_block(f, m->tmu_underflows, (cv1k_u32)sizeof(m->tmu_underflows))) { fclose(f); return 0; }
    if (!write_u32(f, m->tmu_last_event) || !write_u32(f, m->tmu_last_priority)) { fclose(f); return 0; }
    if (!write_block(f, m->tmu_last_cycles, (cv1k_u32)sizeof(m->tmu_last_cycles))) { fclose(f); return 0; }
    if (!save_cpu(f, &m->cpu)) { fclose(f); return 0; }
    if (!write_u32(f, m->frames) || !write_u32(f, m->dma_transfers) || !write_u32(f, m->dma_bytes)) { fclose(f); return 0; }
    if (!write_u32(f, m->dma_cache_invalidations)) { fclose(f); return 0; }
    if (!write_block(f, m->dma_timer_active, (cv1k_u32)sizeof(m->dma_timer_active))) { fclose(f); return 0; }
    if (!write_block(f, m->dma_timer_due, (cv1k_u32)sizeof(m->dma_timer_due))) { fclose(f); return 0; }
    if (!write_block(f, m->dma_timer_chcr, (cv1k_u32)sizeof(m->dma_timer_chcr))) { fclose(f); return 0; }
    if (!write_block(f, m->dma_timer_base, (cv1k_u32)sizeof(m->dma_timer_base))) { fclose(f); return 0; }
    if (!write_u32(f, m->alias_p1p2) || !write_u32(f, m->alias_p4) || !write_u32(f, m->alias_p0_wide)) { fclose(f); return 0; }
    if (!write_u32(f, m->alias_p0_work) || !write_u32(f, m->alias_400) || !write_u32(f, m->alias_e0)) { fclose(f); return 0; }
    if (!write_u32(f, m->last_dma_sar) || !write_u32(f, m->last_dma_dar) || !write_u32(f, m->last_dma_tcr) || !write_u32(f, m->last_dma_chcr)) { fclose(f); return 0; }
    if (!write_u32(f, m->last_dma_src_mode) || !write_u32(f, m->last_dma_dst_mode) || !write_u32(f, m->last_dma_status)) { fclose(f); return 0; }
    if (!write_u32(f, m->last_dma_nand_page0) || !write_u32(f, m->last_dma_nand_page1)) { fclose(f); return 0; }
    if (!write_u32(f, m->last_dma_nand_block0) || !write_u32(f, m->last_dma_nand_block1)) { fclose(f); return 0; }
    if (!write_u32(f, m->last_dma_nand_col0) || !write_u32(f, m->last_dma_nand_col1)) { fclose(f); return 0; }
    if (!write_u32(f, m->irq_requests) || !write_u32(f, m->irq_last_level) || !write_u32(f, m->irq_last_event)) { fclose(f); return 0; }
    if (!write_u32(f, m->exception_events) || !write_u32(f, m->exception_last_event) || !write_u32(f, m->exception_last_tra)) { fclose(f); return 0; }
    if (!write_u32(f, m->boot_assists)) { fclose(f); return 0; }
    if (!write_u32(f, m->unmapped_reads) || !write_u32(f, m->unmapped_writes)) { fclose(f); return 0; }
    if (!write_u32(f, m->last_unmapped_read) || !write_u32(f, m->last_unmapped_write) || !write_u32(f, m->last_unmapped_write_data)) { fclose(f); return 0; }
    if (!write_u32(f, m->main_ram_size) || !write_block(f, m->main_ram, m->main_ram_size)) { fclose(f); return 0; }
    if (!write_u32(f, m->cache_ram_size) || !write_block(f, m->cache_ram, m->cache_ram_size)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)sizeof(m->sh_io)) || !write_block(f, m->sh_io, (cv1k_u32)sizeof(m->sh_io))) { fclose(f); return 0; }
    if (!write_block(f, m->tlb_vpn, (cv1k_u32)sizeof(m->tlb_vpn))) { fclose(f); return 0; }
    if (!write_block(f, m->tlb_ppn, (cv1k_u32)sizeof(m->tlb_ppn))) { fclose(f); return 0; }
    if (!write_block(f, m->tlb_mask, (cv1k_u32)sizeof(m->tlb_mask))) { fclose(f); return 0; }
    if (!write_block(f, m->tlb_valid, (cv1k_u32)sizeof(m->tlb_valid))) { fclose(f); return 0; }
    if (!write_u32(f, m->tlb_loads) || !write_u32(f, m->tlb_hits) || !write_u32(f, m->tlb_misses)) { fclose(f); return 0; }
    if (!write_u32(f, m->tlb_last_virt) || !write_u32(f, m->tlb_last_phys)) { fclose(f); return 0; }
    if (!write_u32(f, CV1K_ICACHE_LINES)) { fclose(f); return 0; }
    if (!write_block(f, m->icache_tag, (cv1k_u32)sizeof(m->icache_tag))) { fclose(f); return 0; }
    if (!write_block(f, m->icache_op, (cv1k_u32)sizeof(m->icache_op))) { fclose(f); return 0; }
    if (!write_block(f, m->icache_valid, (cv1k_u32)sizeof(m->icache_valid))) { fclose(f); return 0; }
    if (!write_u32(f, m->icache_hits) || !write_u32(f, m->icache_misses)) { fclose(f); return 0; }
    if (!write_u32(f, CV1K_DCACHE_LINES) || !write_u32(f, CV1K_DCACHE_LINE_SIZE)) { fclose(f); return 0; }
    if (!write_block(f, m->dcache_tag, (cv1k_u32)sizeof(m->dcache_tag))) { fclose(f); return 0; }
    if (!write_block(f, m->dcache_data, (cv1k_u32)sizeof(m->dcache_data))) { fclose(f); return 0; }
    if (!write_block(f, m->dcache_valid, (cv1k_u32)sizeof(m->dcache_valid))) { fclose(f); return 0; }
    if (!write_u32(f, m->dcache_hits) || !write_u32(f, m->dcache_misses) || !write_u32(f, m->dcache_dma_stale)) { fclose(f); return 0; }
    if (!write_u32(f, CV1K_SH7709S_CACHE_BLOCKS) || !write_u32(f, CV1K_SH7709S_CACHE_ASSOCIATIVITY) || !write_u32(f, CV1K_SH7709S_CACHE_LINE_SIZE)) { fclose(f); return 0; }
    if (!write_block(f, m->mame_cache_tag, (cv1k_u32)sizeof(m->mame_cache_tag))) { fclose(f); return 0; }
    if (!write_block(f, m->mame_cache_lru, (cv1k_u32)sizeof(m->mame_cache_lru))) { fclose(f); return 0; }
    if (!write_block(f, m->mame_cache_dirty, (cv1k_u32)sizeof(m->mame_cache_dirty))) { fclose(f); return 0; }
    if (!write_u32(f, m->mame_cache_hits) || !write_u32(f, m->mame_cache_misses) || !write_u32(f, m->mame_cache_dirty_evicts)) { fclose(f); return 0; }
    if (!write_u32(f, m->mame_cache_fetches) || !write_u32(f, m->mame_cache_reads) || !write_u32(f, m->mame_cache_writes)) { fclose(f); return 0; }
    if (!save_nand(f, &m->nand)) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)sizeof(m->rtc)) || !write_block(f, &m->rtc, (cv1k_u32)sizeof(m->rtc))) { fclose(f); return 0; }
    if (!write_u32(f, (cv1k_u32)sizeof(m->ymz)) || !write_block(f, &m->ymz, (cv1k_u32)sizeof(m->ymz))) { fclose(f); return 0; }
    if (!save_video(f, &m->video)) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

int cv1k_load_state(struct cv1k_machine *m, const char *path)
{
    FILE *f;
    char magic[8];
    cv1k_u32 model;
    cv1k_u32 size;
    cv1k_u32 i;
    f = fopen(path, "rb");
    if (f == NULL) return 0;
    if (!read_block(f, magic, 8UL)) { fclose(f); return 0; }
    if (memcmp(magic, SS_MAGIC, 8U) != 0) { fclose(f); return 0; }
    if (!read_u32(f, &model)) { fclose(f); return 0; }
    if ((int)model != m->model) { fclose(f); return 0; }
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->irq2_enabled = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->aggressive_boot_assists = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->dcache_enabled = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->strict_cache_ops = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->mame_cache_meta = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->wide_p0_alias = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->compact_400_alias = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->dma_cache_sync = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->vblank_irq_and_tick = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->mame_trapa = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->mame_speedup = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->mame_full_dmatcr = (int)size;
    if (!read_u32(f, &size)) { fclose(f); return 0; }
    m->mame_tmu_irq = (int)size;
    if (!read_u32(f, &m->mame_speedup_spins)) { fclose(f); return 0; }
    if (!read_block(f, m->tmu_underflows, (cv1k_u32)sizeof(m->tmu_underflows))) { fclose(f); return 0; }
    if (!read_u32(f, &m->tmu_last_event) || !read_u32(f, &m->tmu_last_priority)) { fclose(f); return 0; }
    if (!read_block(f, m->tmu_last_cycles, (cv1k_u32)sizeof(m->tmu_last_cycles))) { fclose(f); return 0; }
    if (!load_cpu(f, &m->cpu)) { fclose(f); return 0; }
    if (!read_u32(f, &m->frames) || !read_u32(f, &m->dma_transfers) || !read_u32(f, &m->dma_bytes)) { fclose(f); return 0; }
    if (!read_u32(f, &m->dma_cache_invalidations)) { fclose(f); return 0; }
    if (!read_block(f, m->dma_timer_active, (cv1k_u32)sizeof(m->dma_timer_active))) { fclose(f); return 0; }
    m->dma_timer_mask = 0UL;
    for (i = 0; i < 4; i++) if (m->dma_timer_active[i] != 0UL) m->dma_timer_mask |= (1UL << (cv1k_u32)i);
    if (!read_block(f, m->dma_timer_due, (cv1k_u32)sizeof(m->dma_timer_due))) { fclose(f); return 0; }
    if (!read_block(f, m->dma_timer_chcr, (cv1k_u32)sizeof(m->dma_timer_chcr))) { fclose(f); return 0; }
    if (!read_block(f, m->dma_timer_base, (cv1k_u32)sizeof(m->dma_timer_base))) { fclose(f); return 0; }
    if (!read_u32(f, &m->alias_p1p2) || !read_u32(f, &m->alias_p4) || !read_u32(f, &m->alias_p0_wide)) { fclose(f); return 0; }
    if (!read_u32(f, &m->alias_p0_work) || !read_u32(f, &m->alias_400) || !read_u32(f, &m->alias_e0)) { fclose(f); return 0; }
    if (!read_u32(f, &m->last_dma_sar) || !read_u32(f, &m->last_dma_dar) || !read_u32(f, &m->last_dma_tcr) || !read_u32(f, &m->last_dma_chcr)) { fclose(f); return 0; }
    if (!read_u32(f, &m->last_dma_src_mode) || !read_u32(f, &m->last_dma_dst_mode) || !read_u32(f, &m->last_dma_status)) { fclose(f); return 0; }
    if (!read_u32(f, &m->last_dma_nand_page0) || !read_u32(f, &m->last_dma_nand_page1)) { fclose(f); return 0; }
    if (!read_u32(f, &m->last_dma_nand_block0) || !read_u32(f, &m->last_dma_nand_block1)) { fclose(f); return 0; }
    if (!read_u32(f, &m->last_dma_nand_col0) || !read_u32(f, &m->last_dma_nand_col1)) { fclose(f); return 0; }
    if (!read_u32(f, &m->irq_requests) || !read_u32(f, &m->irq_last_level) || !read_u32(f, &m->irq_last_event)) { fclose(f); return 0; }
    if (!read_u32(f, &m->exception_events) || !read_u32(f, &m->exception_last_event) || !read_u32(f, &m->exception_last_tra)) { fclose(f); return 0; }
    if (!read_u32(f, &m->boot_assists)) { fclose(f); return 0; }
    if (!read_u32(f, &m->unmapped_reads) || !read_u32(f, &m->unmapped_writes)) { fclose(f); return 0; }
    if (!read_u32(f, &m->last_unmapped_read) || !read_u32(f, &m->last_unmapped_write) || !read_u32(f, &m->last_unmapped_write_data)) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != m->main_ram_size || !read_block(f, m->main_ram, m->main_ram_size)) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != m->cache_ram_size || !read_block(f, m->cache_ram, m->cache_ram_size)) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != (cv1k_u32)sizeof(m->sh_io) || !read_block(f, m->sh_io, (cv1k_u32)sizeof(m->sh_io))) { fclose(f); return 0; }
    if (!read_block(f, m->tlb_vpn, (cv1k_u32)sizeof(m->tlb_vpn))) { fclose(f); return 0; }
    if (!read_block(f, m->tlb_ppn, (cv1k_u32)sizeof(m->tlb_ppn))) { fclose(f); return 0; }
    if (!read_block(f, m->tlb_mask, (cv1k_u32)sizeof(m->tlb_mask))) { fclose(f); return 0; }
    if (!read_block(f, m->tlb_valid, (cv1k_u32)sizeof(m->tlb_valid))) { fclose(f); return 0; }
    if (!read_u32(f, &m->tlb_loads) || !read_u32(f, &m->tlb_hits) || !read_u32(f, &m->tlb_misses)) { fclose(f); return 0; }
    if (!read_u32(f, &m->tlb_last_virt) || !read_u32(f, &m->tlb_last_phys)) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != CV1K_ICACHE_LINES) { fclose(f); return 0; }
    if (!read_block(f, m->icache_tag, (cv1k_u32)sizeof(m->icache_tag))) { fclose(f); return 0; }
    if (!read_block(f, m->icache_op, (cv1k_u32)sizeof(m->icache_op))) { fclose(f); return 0; }
    if (!read_block(f, m->icache_valid, (cv1k_u32)sizeof(m->icache_valid))) { fclose(f); return 0; }
    if (!read_u32(f, &m->icache_hits) || !read_u32(f, &m->icache_misses)) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != CV1K_DCACHE_LINES) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != CV1K_DCACHE_LINE_SIZE) { fclose(f); return 0; }
    if (!read_block(f, m->dcache_tag, (cv1k_u32)sizeof(m->dcache_tag))) { fclose(f); return 0; }
    if (!read_block(f, m->dcache_data, (cv1k_u32)sizeof(m->dcache_data))) { fclose(f); return 0; }
    if (!read_block(f, m->dcache_valid, (cv1k_u32)sizeof(m->dcache_valid))) { fclose(f); return 0; }
    if (!read_u32(f, &m->dcache_hits) || !read_u32(f, &m->dcache_misses) || !read_u32(f, &m->dcache_dma_stale)) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != CV1K_SH7709S_CACHE_BLOCKS) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != CV1K_SH7709S_CACHE_ASSOCIATIVITY) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != CV1K_SH7709S_CACHE_LINE_SIZE) { fclose(f); return 0; }
    if (!read_block(f, m->mame_cache_tag, (cv1k_u32)sizeof(m->mame_cache_tag))) { fclose(f); return 0; }
    if (!read_block(f, m->mame_cache_lru, (cv1k_u32)sizeof(m->mame_cache_lru))) { fclose(f); return 0; }
    if (!read_block(f, m->mame_cache_dirty, (cv1k_u32)sizeof(m->mame_cache_dirty))) { fclose(f); return 0; }
    if (!read_u32(f, &m->mame_cache_hits) || !read_u32(f, &m->mame_cache_misses) || !read_u32(f, &m->mame_cache_dirty_evicts)) { fclose(f); return 0; }
    if (!read_u32(f, &m->mame_cache_fetches) || !read_u32(f, &m->mame_cache_reads) || !read_u32(f, &m->mame_cache_writes)) { fclose(f); return 0; }
    if (!load_nand(f, &m->nand)) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != (cv1k_u32)sizeof(m->rtc) || !read_block(f, &m->rtc, (cv1k_u32)sizeof(m->rtc))) { fclose(f); return 0; }
    if (!read_u32(f, &size) || size != (cv1k_u32)sizeof(m->ymz) || !read_block(f, &m->ymz, (cv1k_u32)sizeof(m->ymz))) { fclose(f); return 0; }
    if (!load_video(f, &m->video)) { fclose(f); return 0; }
    fclose(f);
    return 1;
}
