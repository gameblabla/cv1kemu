/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * CV1000 blitter/video scaffold for this ANSI C sandbox.
 *
 * Portions are adapted from MAME's Cave CV1000 video implementation:
 *   mame-master/src/mame/cave/cv1k_v.cpp
 *   mame-master/src/mame/cave/cv1k_v.h
 *   mame-master/src/mame/cave/cv1k_v_pixel.ipp
 *
 * MAME license: BSD-3-Clause.
 * MAME copyright-holders: David Haywood, Luca Elia, MetalliC.
 * The generated pixel-loop fragment additionally names David Haywood.
 *
 * See NOTICE and docs/MAME_DERIVED.md for attribution details.
 */
#include "video.h"
#include "platform.h"
#include "mame_cv1k_derived.h"
#include <stdalign.h>

/* The renderer's inner loops call the MAME 5-bit blend tables billions of
 * times over long runs.  The checked functions remain exported for other
 * users, but video.c builds the tables during cv1k_video_init(), so direct
 * table macros are safe here and avoid three function calls per RGB pixel
 * operation. */
#define CV1K_MUL5(x,y) (cv1k_mame_colrtable[(x) & 0x1fU][(y) & 0x3fU])
#define CV1K_MUL5_REV(x,y) (cv1k_mame_colrtable_rev[(x) & 0x1fU][(y) & 0x3fU])
#define CV1K_ADD5(x,y) (cv1k_mame_colrtable_add[(x) & 0x1fU][(y) & 0x1fU])
#define cv1k_mame_mul5(x,y) CV1K_MUL5((x),(y))
#define cv1k_mame_mul5_rev(x,y) CV1K_MUL5_REV((x),(y))
#define cv1k_mame_add5(x,y) CV1K_ADD5((x),(y))
#include <stdio.h>
#include <string.h>

static alignas(CV1K_CACHE_ALIGN) cv1k_u32 cv1k_rgb1555_table[65536];
static cv1k_u8 cv1k_rgb1555_table_ready;

static void cv1k_build_rgb1555_table(void)
{
    cv1k_u32 i;
    if (cv1k_rgb1555_table_ready) return;
    for (i = 0UL; i < 65536UL; i++) {
        cv1k_u32 r = (i >> 10) & 0x1fU;
        cv1k_u32 g = (i >> 5) & 0x1fU;
        cv1k_u32 b = i & 0x1fU;
        cv1k_rgb1555_table[i] = (r << 19) | (g << 11) | (b << 3);
    }
    cv1k_rgb1555_table_ready = 1U;
}

static CV1K_ALWAYS_INLINE cv1k_u32 rgb1555_to_rgb888(cv1k_u16 p)
{
    return cv1k_rgb1555_table[p];
}

static cv1k_u16 make1555(cv1k_u8 r, cv1k_u8 g, cv1k_u8 b, cv1k_u8 a)
{
    return (cv1k_u16)(((a ? 1U : 0U) << 15) | ((cv1k_u16)(r & 0x1fU) << 10) | ((cv1k_u16)(g & 0x1fU) << 5) | (cv1k_u16)(b & 0x1fU));
}

static cv1k_u16 apply_tint(cv1k_u16 src, cv1k_u8 mul_r, cv1k_u8 mul_g, cv1k_u8 mul_b)
{
    cv1k_u8 r;
    cv1k_u8 g;
    cv1k_u8 b;
    cv1k_u8 a;
    a = (cv1k_u8)((src >> 15) & 1U);
    r = (cv1k_u8)((src >> 10) & 0x1fU);
    g = (cv1k_u8)((src >> 5) & 0x1fU);
    b = (cv1k_u8)(src & 0x1fU);
    /* MAME tint_to_clr converts 8-bit bias bytes with >> 2; 0x80 is
     * the normal 0x20 multiplier.  Do not special-case zero: in MAME it is
     * a real black/zero tint value, not a request for the normal multiplier.
     */
    r = cv1k_mame_mul5(r, (cv1k_u8)(mul_r >> 2));
    g = cv1k_mame_mul5(g, (cv1k_u8)(mul_g >> 2));
    b = cv1k_mame_mul5(b, (cv1k_u8)(mul_b >> 2));
    return make1555(r, g, b, a);
}

struct cv1k_clr5 {
    cv1k_u8 r;
    cv1k_u8 g;
    cv1k_u8 b;
};

static struct cv1k_clr5 clr5_make(cv1k_u8 r, cv1k_u8 g, cv1k_u8 b)
{
    struct cv1k_clr5 c;
    c.r = (cv1k_u8)(r & 0x1fU);
    c.g = (cv1k_u8)(g & 0x1fU);
    c.b = (cv1k_u8)(b & 0x1fU);
    return c;
}

static struct cv1k_clr5 clr5_from1555(cv1k_u16 p)
{
    return clr5_make((cv1k_u8)((p >> 10) & 0x1fU), (cv1k_u8)((p >> 5) & 0x1fU), (cv1k_u8)(p & 0x1fU));
}

static struct cv1k_clr5 clr5_add(struct cv1k_clr5 a, struct cv1k_clr5 b)
{
    return clr5_make(cv1k_mame_add5(a.r, b.r), cv1k_mame_add5(a.g, b.g), cv1k_mame_add5(a.b, b.b));
}

static struct cv1k_clr5 clr5_mul_fixed(cv1k_u8 v, struct cv1k_clr5 c)
{
    return clr5_make(cv1k_mame_mul5(v, c.r), cv1k_mame_mul5(v, c.g), cv1k_mame_mul5(v, c.b));
}

static struct cv1k_clr5 clr5_mul_fixed_rev(cv1k_u8 v, struct cv1k_clr5 c)
{
    return clr5_make(cv1k_mame_mul5_rev(v, c.r), cv1k_mame_mul5_rev(v, c.g), cv1k_mame_mul5_rev(v, c.b));
}

static struct cv1k_clr5 clr5_square(struct cv1k_clr5 c)
{
    return clr5_make(cv1k_mame_mul5(c.r, c.r), cv1k_mame_mul5(c.g, c.g), cv1k_mame_mul5(c.b, c.b));
}

static struct cv1k_clr5 clr5_mul_3param(struct cv1k_clr5 a, struct cv1k_clr5 b)
{
    /* MAME clr_t::mul_3param(clr1, clr2): table[clr2][clr1]. */
    return clr5_make(cv1k_mame_mul5(b.r, a.r), cv1k_mame_mul5(b.g, a.g), cv1k_mame_mul5(b.b, a.b));
}

static struct cv1k_clr5 clr5_mul_rev_square(struct cv1k_clr5 c)
{
    return clr5_make(cv1k_mame_mul5_rev(c.r, c.r), cv1k_mame_mul5_rev(c.g, c.g), cv1k_mame_mul5_rev(c.b, c.b));
}

static struct cv1k_clr5 clr5_mul_rev_3param(struct cv1k_clr5 a, struct cv1k_clr5 b)
{
    /* MAME clr_t::mul_rev_3param(clr1, clr2): rev_table[clr2][clr1]. */
    return clr5_make(cv1k_mame_mul5_rev(b.r, a.r), cv1k_mame_mul5_rev(b.g, a.g), cv1k_mame_mul5_rev(b.b, a.b));
}

static struct cv1k_clr5 clr5_add_dst_mode(struct cv1k_clr5 left, struct cv1k_clr5 src, struct cv1k_clr5 dst, cv1k_u8 dst_alpha, int dst_mode)
{
    struct cv1k_clr5 right;
    switch (dst_mode & 7) {
    case 0:
        /* Generic MAME add_with_clr_mul_fixed() uses table[dst][alpha],
         * unlike clr_t::mul_fixed() which uses table[alpha][src].
         */
        right = clr5_make(cv1k_mame_mul5(dst.r, dst_alpha), cv1k_mame_mul5(dst.g, dst_alpha), cv1k_mame_mul5(dst.b, dst_alpha));
        return clr5_add(left, right);
    case 1:
        right = clr5_mul_3param(dst, src);
        return clr5_add(left, right);
    case 2:
        /* Preserve MAME's exact add_with_clr_square behaviour.  The MAME
         * helper uses clr0.r as the first addend for all three channels.
         */
        return clr5_make(
            cv1k_mame_add5(left.r, cv1k_mame_mul5(dst.r, dst.r)),
            cv1k_mame_add5(left.r, cv1k_mame_mul5(dst.g, dst.g)),
            cv1k_mame_add5(left.r, cv1k_mame_mul5(dst.b, dst.b)));
    case 3:
        return clr5_add(left, dst);
    case 4:
        right = clr5_mul_fixed_rev(dst_alpha, dst);
        return clr5_add(left, right);
    case 5:
        right = clr5_mul_rev_3param(dst, src);
        return clr5_add(left, right);
    case 6:
        right = clr5_mul_rev_square(dst);
        return clr5_add(left, right);
    default:
        return clr5_add(left, dst);
    }
}

static struct cv1k_clr5 clr5_blend_smode0(struct cv1k_clr5 s, struct cv1k_clr5 d, cv1k_u8 src_alpha, cv1k_u8 dst_alpha, int dst_mode)
{
    struct cv1k_clr5 left;
    switch (dst_mode & 7) {
    case 0:
        return clr5_make(
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.r), cv1k_mame_mul5(dst_alpha, d.r)),
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.g), cv1k_mame_mul5(dst_alpha, d.g)),
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.b), cv1k_mame_mul5(dst_alpha, d.b)));
    case 1:
        return clr5_make(
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.r), cv1k_mame_mul5(s.r, d.r)),
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.g), cv1k_mame_mul5(s.g, d.g)),
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.b), cv1k_mame_mul5(s.b, d.b)));
    case 2:
    case 3:
    case 4:
    case 6:
    case 7:
        left = clr5_mul_fixed(src_alpha, s);
        return clr5_add_dst_mode(left, s, d, dst_alpha, dst_mode);
    case 5:
        return clr5_make(
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.r), cv1k_mame_mul5_rev(s.r, d.r)),
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.g), cv1k_mame_mul5_rev(s.g, d.g)),
            cv1k_mame_add5(cv1k_mame_mul5(src_alpha, s.b), cv1k_mame_mul5_rev(s.b, d.b)));
    default:
        left = clr5_mul_fixed(src_alpha, s);
        return clr5_add(left, d);
    }
}

static struct cv1k_clr5 clr5_blend_smode2(struct cv1k_clr5 s, struct cv1k_clr5 d, cv1k_u8 dst_alpha, int dst_mode)
{
    struct cv1k_clr5 left;
    switch (dst_mode & 7) {
    case 0:
        return clr5_make(
            cv1k_mame_add5(cv1k_mame_mul5(d.r, s.r), cv1k_mame_mul5(dst_alpha, d.r)),
            cv1k_mame_add5(cv1k_mame_mul5(d.g, s.g), cv1k_mame_mul5(dst_alpha, d.g)),
            cv1k_mame_add5(cv1k_mame_mul5(d.b, s.b), cv1k_mame_mul5(dst_alpha, d.b)));
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
        left = clr5_mul_3param(s, d);
        return clr5_add_dst_mode(left, s, d, dst_alpha, dst_mode);
    default:
        left = clr5_mul_3param(s, d);
        return clr5_add(left, d);
    }
}

static CV1K_HOT cv1k_u16 blend_pixel(cv1k_u16 src, cv1k_u16 dst, cv1k_u8 src_alpha, cv1k_u8 dst_alpha, int src_mode, int dst_mode)
{
    struct cv1k_clr5 s;
    struct cv1k_clr5 d;
    struct cv1k_clr5 left;
    struct cv1k_clr5 out;
    s = clr5_from1555(src);
    d = clr5_from1555(dst);

    /* ANSI C translation of MAME cv1k_v_pixel.ipp.  Source modes 0 and 2
     * have hand-specialized destination terms in MAME; handling them through
     * the generic left+destination helper produces wrong colors for several
     * title/menu fade and highlight effects.
     */
    switch (src_mode & 7) {
    case 0:
        out = clr5_blend_smode0(s, d, src_alpha, dst_alpha, dst_mode);
        break;
    case 1:
        left = clr5_square(s);
        out = clr5_add_dst_mode(left, s, d, dst_alpha, dst_mode);
        break;
    case 2:
        out = clr5_blend_smode2(s, d, dst_alpha, dst_mode);
        break;
    case 3:
        left = s;
        out = clr5_add_dst_mode(left, s, d, dst_alpha, dst_mode);
        break;
    case 4:
        left = clr5_mul_fixed_rev(src_alpha, s);
        out = clr5_add_dst_mode(left, s, d, dst_alpha, dst_mode);
        break;
    case 5:
        left = clr5_mul_rev_square(s);
        out = clr5_add_dst_mode(left, s, d, dst_alpha, dst_mode);
        break;
    case 6:
        left = clr5_mul_rev_3param(s, d);
        out = clr5_add_dst_mode(left, s, d, dst_alpha, dst_mode);
        break;
    default:
        left = s;
        out = clr5_add_dst_mode(left, s, d, dst_alpha, dst_mode);
        break;
    }
    return (cv1k_u16)((src & 0x8000U) | (cv1k_u16)((cv1k_u16)out.r << 10) | (cv1k_u16)((cv1k_u16)out.g << 5) | (cv1k_u16)out.b);
}

static cv1k_u16 read_ram16(const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 addr)
{
    if (ram == NULL || ram_size < 2UL) return 0xffffU;
    addr %= ram_size;
    if (addr + 1UL < ram_size) return cv1k_be16(&ram[addr]);
    return (cv1k_u16)(((cv1k_u16)ram[addr] << 8) | (cv1k_u16)ram[0]);
}

static cv1k_s32 sign16(cv1k_u16 v)
{
    /* MAME cv1k_blitter_device::gfx_draw sign-extends the destination
     * coordinates as full 16-bit values with util::sext<int>(..., 16).
     * Earlier sandbox builds used a 15-bit sign helper based on the format
     * comment, which misplaces any command using bit 15 for negative X/Y.
     */
    cv1k_u32 x;
    x = (cv1k_u32)v;
    if ((x & 0x8000UL) != 0UL) x |= 0xffff0000UL;
    return (cv1k_s32)x;
}

static cv1k_u32 vram_index(cv1k_u32 x, cv1k_u32 y)
{
    return (y & (CV1K_VRAM_H - 1UL)) * CV1K_VRAM_W + (x & (CV1K_VRAM_W - 1UL));
}

cv1k_u32 cv1k_video_vram_bytes(void)
{
    return (cv1k_u32)(CV1K_VRAM_PIXELS * sizeof(cv1k_u16));
}

void cv1k_video_set_gpu_accel(struct cv1k_video *video, const struct cv1k_video_gpu_ops *ops, void *userdata)
{
    if (video == NULL) return;
    video->gpu_ops = ops;
    video->gpu_userdata = userdata;
    video->gpu_attempted_ops = 0UL;
    video->gpu_executed_ops = 0UL;
    video->gpu_fallback_ops = 0UL;
    if (ops != NULL && ops->invalidate_all != NULL) ops->invalidate_all(userdata);
}

static CV1K_ALWAYS_INLINE cv1k_u32 vram_tile_index(cv1k_u32 tx, cv1k_u32 ty)
{
    return (ty * CV1K_VRAM_TILES_X) + tx;
}

void cv1k_video_mark_vram_dirty_all(struct cv1k_video *video)
{
    cv1k_u32 i;
    cv1k_u32 gen;
    if (video == NULL) return;
    gen = video->vram_dirty_generation + 1UL;
    if (gen == 0UL) {
        gen = 1UL;
        memset(video->vram_tile_generation, 0, sizeof(video->vram_tile_generation));
    }
    video->vram_dirty_generation = gen;
    for (i = 0UL; i < CV1K_VRAM_TILE_COUNT; i++) video->vram_tile_generation[i] = gen;
    if (video->gpu_ops != NULL && video->gpu_ops->invalidate_all != NULL) video->gpu_ops->invalidate_all(video->gpu_userdata);
}

void cv1k_video_mark_vram_dirty_rect(struct cv1k_video *video, cv1k_u32 x, cv1k_u32 y, cv1k_u32 w, cv1k_u32 h)
{
    cv1k_u32 tx0;
    cv1k_u32 ty0;
    cv1k_u32 tx1;
    cv1k_u32 ty1;
    cv1k_u32 tx;
    cv1k_u32 ty;
    cv1k_u32 gen;
    if (video == NULL || w == 0UL || h == 0UL || x >= CV1K_VRAM_W || y >= CV1K_VRAM_H) return;
    if (w > CV1K_VRAM_W - x) w = CV1K_VRAM_W - x;
    if (h > CV1K_VRAM_H - y) h = CV1K_VRAM_H - y;
    gen = video->vram_dirty_generation + 1UL;
    if (gen == 0UL) {
        gen = 1UL;
        memset(video->vram_tile_generation, 0, sizeof(video->vram_tile_generation));
    }
    video->vram_dirty_generation = gen;
    tx0 = x / CV1K_VRAM_TILE_W;
    ty0 = y / CV1K_VRAM_TILE_H;
    tx1 = (x + w - 1UL) / CV1K_VRAM_TILE_W;
    ty1 = (y + h - 1UL) / CV1K_VRAM_TILE_H;
    if (tx1 >= CV1K_VRAM_TILES_X) tx1 = CV1K_VRAM_TILES_X - 1UL;
    if (ty1 >= CV1K_VRAM_TILES_Y) ty1 = CV1K_VRAM_TILES_Y - 1UL;
    for (ty = ty0; ty <= ty1; ty++) {
        for (tx = tx0; tx <= tx1; tx++) video->vram_tile_generation[vram_tile_index(tx, ty)] = gen;
    }
}

cv1k_u32 cv1k_video_vram_tile_generation(const struct cv1k_video *video, cv1k_u32 tx, cv1k_u32 ty)
{
    if (video == NULL || tx >= CV1K_VRAM_TILES_X || ty >= CV1K_VRAM_TILES_Y) return 0UL;
    return video->vram_tile_generation[vram_tile_index(tx, ty)];
}

int cv1k_video_init(struct cv1k_video *video)
{
    memset(video, 0, sizeof(*video));
    cv1k_mame_build_color_tables();
    cv1k_build_rgb1555_table();
    video->vram1555 = (cv1k_u16 *)cv1k_xmalloc(cv1k_video_vram_bytes());
    video->screen_rgb = (cv1k_u32 *)cv1k_xmalloc((cv1k_u32)(CV1K_FRAMEBUFFER_W * CV1K_FRAMEBUFFER_H * sizeof(cv1k_u32)));
    if (video->vram1555 == NULL || video->screen_rgb == NULL) return 0;
    cv1k_video_reset(video);
    return 1;
}

void cv1k_video_shutdown(struct cv1k_video *video)
{
    cv1k_free(video->vram1555);
    cv1k_free(video->screen_rgb);
    memset(video, 0, sizeof(*video));
}

void cv1k_video_reset(struct cv1k_video *video)
{
    if (video->vram1555 != NULL) memset(video->vram1555, 0, (size_t)cv1k_video_vram_bytes());
    if (video->screen_rgb != NULL) memset(video->screen_rgb, 0, (size_t)(CV1K_FRAMEBUFFER_W * CV1K_FRAMEBUFFER_H * sizeof(cv1k_u32)));
    memset(video->regs, 0, sizeof(video->regs));
    video->frame_counter = 0UL;
    video->executed_ops = 0UL;
    video->upload_ops = 0UL;
    video->draw_ops = 0UL;
    video->unknown_ops = 0UL;
    video->clip_ops = 0UL;
    video->last_unknown_op = 0UL;
    video->busy_cycles_ns = 0UL;
    video->busy_cycles_left = 0UL;
    video->blit_idle_op_bytes = 0UL;
    video->blit_hline_penalty_ns = 0UL;
    video->blit_over_frame_count = 0UL;
    video->gfx_scroll_x = 0UL;
    video->gfx_scroll_y = 0UL;
    video->clip_x = 0UL;
    video->clip_y = 0UL;
    video->clip_w = CV1K_SCREEN_W;
    video->clip_h = CV1K_SCREEN_H;
    video->last_list_addr = 0UL;
    video->mmio_execs = 0UL;
    video->last_mmio_list_addr = 0UL;
    video->last_upload_addr = 0UL;
    video->last_upload_x = 0UL;
    video->last_upload_y = 0UL;
    video->last_upload_w = 0UL;
    video->last_upload_h = 0UL;
    video->last_upload_pixels = 0UL;
    video->last_upload_nonzero = 0UL;
    video->upload_nonzero_total = 0UL;
    video->last_draw_addr = 0UL;
    video->last_draw_flags = 0UL;
    video->last_draw_alpha = 0UL;
    video->last_draw_src_x = 0UL;
    video->last_draw_src_y = 0UL;
    video->last_draw_dst_x = 0;
    video->last_draw_dst_y = 0;
    video->last_draw_w = 0UL;
    video->last_draw_h = 0UL;
    video->last_draw_src_nonzero = 0UL;
    video->last_draw_written = 0UL;
    video->last_draw_written_nonzero = 0UL;
    video->draw_src_nonzero_total = 0UL;
    video->draw_written_total = 0UL;
    video->draw_written_nonzero_total = 0UL;
    video->last_frame_nonzero = 0UL;
    video->busy = 0U;
    video->busy_cycles_left = 0UL;
    video->fpga_firmware_pos = 0UL;
    video->fpga_firmware_checksum = 0UL;
    video->fpga_firmware_done = 0UL;
    video->fpga_firmware_version = -1L;
    video->fpga_firmware_port = 0U;
    video->fpga_firmware_byte = 0U;
    video->vram_dirty_generation = 0UL;
    memset(video->vram_tile_generation, 0, sizeof(video->vram_tile_generation));
    cv1k_video_mark_vram_dirty_all(video);
}

cv1k_u8 cv1k_video_fpga_read(struct cv1k_video *video)
{
    CV1K_UNUSED(video);
    return 0xffU;
}

void cv1k_video_fpga_write(struct cv1k_video *video, cv1k_u8 data)
{
    int bit;
    if (video == NULL) return;
    if ((data & 0x08U) != 0U && (video->fpga_firmware_port & 0x10U) == 0U && (data & 0x10U) != 0U) {
        if (video->fpga_firmware_pos < 2323240UL) {
            bit = ((data & 0x20U) != 0U) ? 1 : 0;
            if (bit) video->fpga_firmware_byte = (cv1k_u8)(video->fpga_firmware_byte | (cv1k_u8)(1U << (video->fpga_firmware_pos & 7UL)));
            video->fpga_firmware_pos++;
            if ((video->fpga_firmware_pos & 7UL) == 0UL) {
                video->fpga_firmware_checksum = (video->fpga_firmware_checksum + (cv1k_u32)video->fpga_firmware_byte) & 0xffUL;
                video->fpga_firmware_byte = 0U;
            }
            if (video->fpga_firmware_pos == 2323240UL) {
                video->fpga_firmware_done = 1UL;
                switch (video->fpga_firmware_checksum & 0xffUL) {
                case 0x03UL: video->fpga_firmware_version = 0L; break;
                case 0x3eUL: video->fpga_firmware_version = 1L; break;
                case 0xf9UL: video->fpga_firmware_version = 2L; break;
                case 0xe1UL: video->fpga_firmware_version = 3L; break;
                default: video->fpga_firmware_version = -1L; break;
                }
            }
        }
    }
    video->fpga_firmware_port = data;
}

cv1k_u32 cv1k_video_read32(struct cv1k_video *video, cv1k_u32 regoff)
{
    /* ANSI C adaptation of MAME cv1k_blitter_device::blitter_r.
     * The MAME device only returns defined values for ready/status, two
     * handshake registers, and the DSW port.  DSW defaults to low-nibble
     * switches off and high bits active/unknown (0xfffffff0).  Other offsets
     * fall through to zero after logging; returning shadow registers here was
     * a v27 scaffold convenience, but it is not MAME-compatible.
     */
    switch (regoff & ~3UL) {
    case 0x10UL: return (video->busy || video->busy_cycles_left != 0UL) ? 0x00000000UL : 0x00000010UL;
    case 0x24UL: return 0xffffffffUL;
    case 0x28UL: return 0xffffffffUL;
    case 0x50UL: return 0xfffffff0UL;
    default: break;
    }
    CV1K_UNUSED(video);
    return 0x00000000UL;
}

cv1k_u8 cv1k_video_read8(struct cv1k_video *video, cv1k_u32 offset)
{
    cv1k_u32 v;
    cv1k_u32 shift;
    if (offset >= CV1K_REGION_BLITTER_SIZE) return 0xffU;
    v = cv1k_video_read32(video, offset);
    shift = (3UL - (offset & 3UL)) * 8UL;
    return (cv1k_u8)((v >> shift) & 0xffUL);
}



static void set_exec_clip_from_regs(struct cv1k_video *video)
{
    /* MAME stores the draw clip as a signed rectangle: origin - 32 through
     * origin + visible_size - 1 + 32.  Do not clamp the left/top edge here;
     * off-VRAM writes are discarded later by the renderer, but a negative clip
     * origin changes which source pixels survive at the visible edge.
     */
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_s32 min_x;
    cv1k_s32 min_y;
    cv1k_s32 max_x;
    cv1k_s32 max_y;
    x = video->regs[0x40UL >> 2] & 0x1fffUL;
    y = video->regs[0x44UL >> 2] & 0x0fffUL;
    min_x = (cv1k_s32)x - (cv1k_s32)CV1K_CLIP_MARGIN;
    min_y = (cv1k_s32)y - (cv1k_s32)CV1K_CLIP_MARGIN;
    max_x = (cv1k_s32)x + (cv1k_s32)CV1K_SCREEN_W - 1 + (cv1k_s32)CV1K_CLIP_MARGIN;
    max_y = (cv1k_s32)y + (cv1k_s32)CV1K_SCREEN_H - 1 + (cv1k_s32)CV1K_CLIP_MARGIN;
    video->clip_x = min_x;
    video->clip_y = min_y;
    video->clip_w = max_x - min_x + 1;
    video->clip_h = max_y - min_y + 1;
}

static void idle_blitter_mame(struct cv1k_video *video, cv1k_u32 operation_size_bytes);
static cv1k_u32 calculate_vram_accesses_mame(cv1k_u32 start_x, cv1k_u32 start_y, cv1k_u32 dimx, cv1k_u32 dimy);
static void finish_blit_delay_mame(struct cv1k_video *video);

static void apply_clip_command(struct cv1k_video *video, cv1k_u16 cliptype)
{
    /* MAME blit-list opcode 0xc000 consumes one parameter word.
     * Nonzero cliptype selects the configured 320x240 screen clip expanded by
     * CV1K_CLIP_MARGIN; zero opens the full 0x2000x0x1000 VRAM coordinate
     * space.  v27 treated 0xc000 as an unknown opcode and advanced only two
     * bytes, which desynchronized any list containing clip commands.
     */
    if (cliptype != 0U) {
        set_exec_clip_from_regs(video);
    } else {
        video->clip_x = 0;
        video->clip_y = 0;
        video->clip_w = (cv1k_s32)CV1K_VRAM_W;
        video->clip_h = (cv1k_s32)CV1K_VRAM_H;
    }
    video->clip_ops++;
    idle_blitter_mame(video, CV1K_CLIP_OPERATION_SIZE_BYTES);
}


static void idle_blitter_mame(struct cv1k_video *video, cv1k_u32 operation_size_bytes)
{
    /* ANSI C adaptation of MAME cv1k_blitter_device::idle_blitter().
     * The FPGA fetches blitter operations from main RAM in 64-byte chunks;
     * long runs of non-drawing operations therefore still keep the blitter
     * busy.  MAME charges 700 ns each time the idle-operation byte counter
     * crosses a 64-byte chunk boundary.
     */
    if (video == NULL) return;
    video->blit_idle_op_bytes += operation_size_bytes;
    while (video->blit_idle_op_bytes >= CV1K_OPERATION_CHUNK_SIZE_BYTES) {
        video->blit_idle_op_bytes -= CV1K_OPERATION_CHUNK_SIZE_BYTES;
        video->busy_cycles_ns += CV1K_OPERATION_READ_CHUNK_INTERVAL_NS;
    }
}

static cv1k_u32 calculate_vram_axis_rows_mame(cv1k_u32 start, cv1k_u32 dim)
{
    cv1k_u32 full;
    cv1k_u32 rem;
    cv1k_u32 rows;
    cv1k_u32 off;
    if (dim == 0UL) return 0UL;
    off = start & 31UL;
    full = dim >> 5;
    rem = dim & 31UL;
    rows = full + ((rem != 0UL) ? 1UL : 0UL);
    if (off != 0UL) {
        rows += full;
        if (rem != 0UL && off + rem > 32UL) rows++;
    }
    return rows;
}

static cv1k_u32 calculate_vram_accesses_mame(cv1k_u32 start_x, cv1k_u32 start_y, cv1k_u32 dimx, cv1k_u32 dimy)
{
    return calculate_vram_axis_rows_mame(start_x, dimx) * calculate_vram_axis_rows_mame(start_y, dimy);
}

static void finish_blit_delay_mame(struct cv1k_video *video)
{
    cv1k_u32 extra;
    if (video == NULL) return;
    extra = (video->busy_cycles_ns / CV1K_VRAM_H_LINE_PERIOD_NANOSEC) * CV1K_VRAM_H_LINE_DURATION_NANOSEC;
    video->busy_cycles_ns += extra;
    video->blit_hline_penalty_ns += extra;
    if (video->busy_cycles_ns > CV1K_FRAME_DURATION_NANOSEC) video->blit_over_frame_count++;
}

static CV1K_HOT cv1k_u32 execute_upload(struct cv1k_video *video, cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    cv1k_u32 dst_x;
    cv1k_u32 dst_y;
    cv1k_u32 w;
    cv1k_u32 h;
    cv1k_u32 px;
    cv1k_u32 py;
    cv1k_u32 pos;
    cv1k_u16 pen;
    cv1k_u32 nonzero;
    dst_x = (cv1k_u32)(read_ram16(ram, ram_size, addr + 8UL) & 0x1fffU);
    dst_y = (cv1k_u32)(read_ram16(ram, ram_size, addr + 10UL) & 0x0fffU);
    w = (cv1k_u32)(read_ram16(ram, ram_size, addr + 12UL) & 0x1fffU) + 1UL;
    h = (cv1k_u32)(read_ram16(ram, ram_size, addr + 14UL) & 0x0fffU) + 1UL;
    pos = addr + CV1K_UPLOAD_HEADER_SIZE_BYTES;
    nonzero = 0UL;
    video->last_upload_addr = addr;
    video->last_upload_x = dst_x;
    video->last_upload_y = dst_y;
    video->last_upload_w = w;
    video->last_upload_h = h;
    video->last_upload_pixels = w * h;
    for (py = 0UL; py < h; py++) {
        for (px = 0UL; px < w; px++) {
            pen = read_ram16(ram, ram_size, pos);
            pos += 2UL;
            if ((pen & 0x7fffU) != 0U) nonzero++;
            /* MAME's gfx_upload() masks the starting co-ordinates but then
             * writes through a linear bitmap row pointer.  It does not
             * wrap each uploaded pixel through the source/draw VRAM indexer.
             * The sandbox's earlier per-pixel vram_index() wrap could smear
             * over-edge upload data into the top-left atlas/title pages and
             * amplify stale list replays into visible corruption.
             */
            if (video->vram1555 != NULL && dst_y + py < CV1K_VRAM_H) {
                cv1k_u32 dst_index;
                dst_index = (dst_y + py) * CV1K_VRAM_W + dst_x + px;
                if (dst_index < CV1K_VRAM_W * CV1K_VRAM_H) video->vram1555[dst_index] = pen;
            }
        }
    }
    if (w != 0UL && h != 0UL) cv1k_video_mark_vram_dirty_rect(video, dst_x, dst_y, w, h);
    if (video->gpu_ops != NULL && video->gpu_ops->upload != NULL && w != 0UL && h != 0UL) {
        struct cv1k_video_gpu_upload_cmd gpu_cmd;
        int gpu_ok;
        gpu_cmd.addr = addr;
        gpu_cmd.dst_x = dst_x;
        gpu_cmd.dst_y = dst_y;
        gpu_cmd.w = w;
        gpu_cmd.h = h;
        video->gpu_attempted_ops++;
        gpu_ok = video->gpu_ops->upload(video->gpu_userdata, video, &gpu_cmd, ram, ram_size);
        if (gpu_ok) video->gpu_executed_ops++;
        else video->gpu_fallback_ops++;
    }
    video->last_upload_nonzero = nonzero;
    video->upload_nonzero_total += nonzero;
    video->upload_ops++;
    video->busy_cycles_ns += (((CV1K_UPLOAD_HEADER_SIZE_BYTES + w * h * 2UL) / 4UL) * CV1K_SRAM_CLK_NANOSEC);
    video->blit_idle_op_bytes = 0UL;
    return pos;
}

static CV1K_HOT cv1k_u32 execute_draw(struct cv1k_video *video, cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    cv1k_u16 flags;
    cv1k_u16 alphaw;
    cv1k_u32 src_x;
    cv1k_u32 src_y;
    cv1k_s32 dst_x;
    cv1k_s32 dst_y;
    cv1k_u32 w;
    cv1k_u32 h;
    cv1k_u8 src_alpha;
    cv1k_u8 dst_alpha;
    cv1k_u8 src_mode;
    cv1k_u8 dst_mode;
    int blend_enabled;
    int tint_enabled;
    cv1k_u8 mul_r;
    cv1k_u8 mul_g;
    cv1k_u8 mul_b;
    cv1k_u32 px;
    cv1k_u32 py;
    cv1k_u32 sx;
    cv1k_u32 clipped_w;
    cv1k_u32 clipped_h;
    cv1k_u32 src_rows;
    cv1k_u32 dst_rows;
    cv1k_u32 dst_aligned_w;
    cv1k_u32 vram_clk;
    cv1k_u32 sy;
    cv1k_s32 dx;
    cv1k_s32 dy;
    cv1k_u16 src;
    cv1k_u16 dst;
    cv1k_u32 src_nonzero;
    cv1k_u32 written;
    cv1k_u32 written_nonzero;
    cv1k_s32 clip_l;
    cv1k_s32 clip_t;
    cv1k_s32 clip_r;
    cv1k_s32 clip_b;
    cv1k_s32 vis_l;
    cv1k_s32 vis_t;
    cv1k_s32 vis_r;
    cv1k_s32 vis_b;
    cv1k_u32 px0;
    cv1k_u32 px1;
    cv1k_u32 py0;
    cv1k_u32 py1;
    flags = read_ram16(ram, ram_size, addr + 0UL);
    alphaw = read_ram16(ram, ram_size, addr + 2UL);
    src_x = (cv1k_u32)(read_ram16(ram, ram_size, addr + 4UL) & 0x1fffU);
    src_y = (cv1k_u32)(read_ram16(ram, ram_size, addr + 6UL) & 0x0fffU);
    dst_x = sign16(read_ram16(ram, ram_size, addr + 8UL));
    dst_y = sign16(read_ram16(ram, ram_size, addr + 10UL));
    w = (cv1k_u32)(read_ram16(ram, ram_size, addr + 12UL) & 0x1fffU) + 1UL;
    h = (cv1k_u32)(read_ram16(ram, ram_size, addr + 14UL) & 0x0fffU) + 1UL;
    mul_r = (cv1k_u8)(read_ram16(ram, ram_size, addr + 16UL) & 0xffU);
    mul_g = (cv1k_u8)((read_ram16(ram, ram_size, addr + 18UL) >> 8) & 0xffU);
    mul_b = (cv1k_u8)(read_ram16(ram, ram_size, addr + 18UL) & 0xffU);
    src_alpha = (cv1k_u8)(((alphaw >> 8) & 0xffU) >> 3);
    dst_alpha = (cv1k_u8)((alphaw & 0xffU) >> 3);
    src_mode = (cv1k_u8)((flags >> 4) & 7U);
    dst_mode = (cv1k_u8)(flags & 7U);
    blend_enabled = ((flags & 0x0200U) != 0U) ? 1 : 0;
    if (src_mode == 0U && src_alpha == 0x1fU && dst_mode == 4U && dst_alpha == 0x1fU) blend_enabled = 0;
    tint_enabled = (((mul_r >> 2) != 0x20U) || ((mul_g >> 2) != 0x20U) || ((mul_b >> 2) != 0x20U)) ? 1 : 0;
    if (w > CV1K_VRAM_W) w = CV1K_VRAM_W;
    if (h > CV1K_VRAM_H) h = CV1K_VRAM_H;
    if (w != 0UL) {
        if ((flags & 0x0800U) != 0U) {
            if ((src_x & 0x1fffUL) < ((src_x - (w - 1UL)) & 0x1fffUL)) {
                idle_blitter_mame(video, CV1K_DRAW_OPERATION_SIZE_BYTES);
                video->draw_ops++;
                return addr + CV1K_DRAW_OPERATION_SIZE_BYTES;
            }
        } else {
            if ((src_x & 0x1fffUL) > ((src_x + (w - 1UL)) & 0x1fffUL)) {
                idle_blitter_mame(video, CV1K_DRAW_OPERATION_SIZE_BYTES);
                video->draw_ops++;
                return addr + CV1K_DRAW_OPERATION_SIZE_BYTES;
            }
        }
    }
    video->last_draw_addr = addr;
    video->last_draw_flags = (cv1k_u32)flags;
    video->last_draw_alpha = (cv1k_u32)alphaw;
    video->last_draw_src_x = src_x;
    video->last_draw_src_y = src_y;
    video->last_draw_dst_x = dst_x;
    video->last_draw_dst_y = dst_y;
    video->last_draw_w = w;
    video->last_draw_h = h;
    src_nonzero = 0UL;
    written = 0UL;
    written_nonzero = 0UL;

    clip_l = video->clip_x;
    clip_t = video->clip_y;
    clip_r = video->clip_x + video->clip_w;
    clip_b = video->clip_y + video->clip_h;
    if (clip_l < 0) clip_l = 0;
    if (clip_t < 0) clip_t = 0;
    if (clip_r > (cv1k_s32)CV1K_VRAM_W) clip_r = (cv1k_s32)CV1K_VRAM_W;
    if (clip_b > (cv1k_s32)CV1K_VRAM_H) clip_b = (cv1k_s32)CV1K_VRAM_H;
    vis_l = dst_x > clip_l ? dst_x : clip_l;
    vis_t = dst_y > clip_t ? dst_y : clip_t;
    vis_r = (dst_x + (cv1k_s32)w) < clip_r ? (dst_x + (cv1k_s32)w) : clip_r;
    vis_b = (dst_y + (cv1k_s32)h) < clip_b ? (dst_y + (cv1k_s32)h) : clip_b;

    if (vis_l < vis_r && vis_t < vis_b && video->vram1555 != NULL) {
        px0 = (cv1k_u32)(vis_l - dst_x);
        px1 = (cv1k_u32)(vis_r - dst_x);
        py0 = (cv1k_u32)(vis_t - dst_y);
        py1 = (cv1k_u32)(vis_b - dst_y);

        /* The overwhelmingly common CV1000 path during gameplay/UI upload is
         * an untinted, unblended, alpha-tested atlas copy.  Pre-clip it once
         * and walk linear rows; this preserves the old forward-copy semantics
         * while removing four bounds/clip branches and two vram_index() calls
         * from every visible pixel. */
        if (CV1K_LIKELY(!blend_enabled && !tint_enabled && (flags & 0x0c00U) == 0U &&
            src_x + px1 <= CV1K_VRAM_W && src_y + py1 <= CV1K_VRAM_H)) {
            cv1k_u32 count = px1 - px0;
            int alpha_test = ((flags & 0x0100U) != 0U);
            for (py = py0; py < py1; py++) {
                const cv1k_u16 *sp = video->vram1555 + (src_y + py) * CV1K_VRAM_W + src_x + px0;
                cv1k_u16 *dp = video->vram1555 + ((cv1k_u32)(dst_y + (cv1k_s32)py)) * CV1K_VRAM_W + (cv1k_u32)(dst_x + (cv1k_s32)px0);
                for (px = 0UL; px < count; px++) {
                    src = sp[px];
                    if ((src & 0x7fffU) != 0U) src_nonzero++;
                    if (alpha_test && (src & 0x8000U) == 0U) continue;
                    dp[px] = src;
                    written++;
                    if ((src & 0x7fffU) != 0U) written_nonzero++;
                }
            }
        } else {
            for (py = py0; py < py1; py++) {
                sy = ((flags & 0x0400U) != 0U) ? (src_y + (h - 1UL - py)) : (src_y + py);
                dy = dst_y + (cv1k_s32)py;
                for (px = px0; px < px1; px++) {
                    sx = ((flags & 0x0800U) != 0U) ? (src_x + (w - 1UL - px)) : (src_x + px);
                    dx = dst_x + (cv1k_s32)px;
                    src = video->vram1555[vram_index(sx, sy)];
                    if ((src & 0x7fffU) != 0U) src_nonzero++;
                    if ((flags & 0x0100U) != 0U && (src & 0x8000U) == 0U) continue;
                    if (tint_enabled) src = apply_tint(src, mul_r, mul_g, mul_b);
                    if (blend_enabled) {
                        cv1k_u32 di = (cv1k_u32)dy * CV1K_VRAM_W + (cv1k_u32)dx;
                        dst = video->vram1555[di];
                        src = blend_pixel(src, dst, src_alpha, dst_alpha, (int)src_mode, (int)dst_mode);
                        video->vram1555[di] = src;
                    } else {
                        video->vram1555[(cv1k_u32)dy * CV1K_VRAM_W + (cv1k_u32)dx] = src;
                    }
                    written++;
                    if ((src & 0x7fffU) != 0U) written_nonzero++;
                }
            }
        }
    }
    if (written != 0UL) {
        cv1k_video_mark_vram_dirty_rect(video, (cv1k_u32)vis_l, (cv1k_u32)vis_t, (cv1k_u32)(vis_r - vis_l), (cv1k_u32)(vis_b - vis_t));
    }
    if (video->gpu_ops != NULL && video->gpu_ops->draw != NULL && written != 0UL) {
        struct cv1k_video_gpu_draw_cmd gpu_cmd;
        int gpu_ok;
        gpu_cmd.flags = flags;
        gpu_cmd.alphaw = alphaw;
        gpu_cmd.src_x = src_x;
        gpu_cmd.src_y = src_y;
        gpu_cmd.dst_x = dst_x;
        gpu_cmd.dst_y = dst_y;
        gpu_cmd.w = w;
        gpu_cmd.h = h;
        gpu_cmd.mul_r = mul_r;
        gpu_cmd.mul_g = mul_g;
        gpu_cmd.mul_b = mul_b;
        gpu_cmd.clip_x = video->clip_x;
        gpu_cmd.clip_y = video->clip_y;
        gpu_cmd.clip_w = video->clip_w;
        gpu_cmd.clip_h = video->clip_h;
        gpu_cmd.vis_l = vis_l;
        gpu_cmd.vis_t = vis_t;
        gpu_cmd.vis_r = vis_r;
        gpu_cmd.vis_b = vis_b;
        gpu_cmd.written = written;
        video->gpu_attempted_ops++;
        gpu_ok = video->gpu_ops->draw(video->gpu_userdata, video, &gpu_cmd);
        if (gpu_ok) video->gpu_executed_ops++;
        else video->gpu_fallback_ops++;
    }
    video->last_draw_src_nonzero = src_nonzero;
    video->last_draw_written = written;
    video->last_draw_written_nonzero = written_nonzero;
    video->draw_src_nonzero_total += src_nonzero;
    video->draw_written_total += written;
    video->draw_written_nonzero_total += written_nonzero;
    /* MAME estimates draw time from clipped source/destination VRAM row
     * accesses and 4-pixel DDR transfers.  The pixel loop above is still the
     * sandbox renderer, but the timing/accounting path now follows
     * cv1k_blitter_device::gfx_draw_shadow_copy() more closely.
     */
    clipped_w = w;
    clipped_h = h;
    if (dst_x < video->clip_x) {
        cv1k_s32 cut;
        cut = video->clip_x - dst_x;
        clipped_w = ((cv1k_u32)cut >= clipped_w) ? 0UL : (clipped_w - (cv1k_u32)cut);
    }
    if (dst_y < video->clip_y) {
        cv1k_s32 cuty;
        cuty = video->clip_y - dst_y;
        clipped_h = ((cv1k_u32)cuty >= clipped_h) ? 0UL : (clipped_h - (cv1k_u32)cuty);
    }
    if ((cv1k_s32)(dst_x + (cv1k_s32)clipped_w) > video->clip_x + video->clip_w) {
        cv1k_s32 right;
        right = video->clip_x + video->clip_w;
        clipped_w = (dst_x >= right) ? 0UL : (cv1k_u32)(right - dst_x);
    }
    if ((cv1k_s32)(dst_y + (cv1k_s32)clipped_h) > video->clip_y + video->clip_h) {
        cv1k_s32 bottom;
        bottom = video->clip_y + video->clip_h;
        clipped_h = (dst_y >= bottom) ? 0UL : (cv1k_u32)(bottom - dst_y);
    }
    if (clipped_w == 0UL || clipped_h == 0UL) {
        idle_blitter_mame(video, CV1K_DRAW_OPERATION_SIZE_BYTES);
    } else {
        src_rows = calculate_vram_accesses_mame(src_x, src_y, clipped_w, clipped_h);
        dst_rows = calculate_vram_accesses_mame((dst_x < 0) ? 0UL : (cv1k_u32)dst_x, (dst_y < 0) ? 0UL : (cv1k_u32)dst_y, clipped_w, clipped_h);
        dst_aligned_w = clipped_w + (((dst_x < 0) ? 0UL : (cv1k_u32)dst_x) & 3UL);
        dst_aligned_w += (4UL - (dst_aligned_w & 3UL)) & 3UL;
        vram_clk = clipped_w * clipped_h / 4UL + dst_aligned_w * clipped_h / 2UL + src_rows * 6UL + dst_rows * (20UL + 11UL) + 12UL;
        video->busy_cycles_ns += vram_clk * CV1K_VRAM_CLK_NANOSEC;
        video->blit_idle_op_bytes = 0UL;
    }
    video->draw_ops++;
    return addr + CV1K_DRAW_OPERATION_SIZE_BYTES;
}

static int bounded_command_fits(cv1k_u32 addr, cv1k_u32 end_addr, cv1k_u32 need)
{
    return end_addr > addr && need <= end_addr - addr;
}

static int bounded_upload_fits(cv1k_u32 addr, cv1k_u32 end_addr, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    cv1k_u32 w;
    cv1k_u32 h;
    cv1k_u32 pixels;
    cv1k_u32 need;
    if (!bounded_command_fits(addr, end_addr, CV1K_UPLOAD_HEADER_SIZE_BYTES)) return 0;
    w = (cv1k_u32)(read_ram16(ram, ram_size, addr + 12UL) & 0x1fffU) + 1UL;
    h = (cv1k_u32)(read_ram16(ram, ram_size, addr + 14UL) & 0x0fffU) + 1UL;
    if (h != 0UL && w > 0xffffffffUL / h) return 0;
    pixels = w * h;
    if (pixels > (0xffffffffUL - CV1K_UPLOAD_HEADER_SIZE_BYTES) / 2UL) return 0;
    need = CV1K_UPLOAD_HEADER_SIZE_BYTES + pixels * 2UL;
    return bounded_command_fits(addr, end_addr, need);
}

static CV1K_HOT void cv1k_video_execute_list_from(struct cv1k_video *video, cv1k_u32 addr, cv1k_u32 end_addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops)
{
    cv1k_u32 i;
    cv1k_u16 op;
    int bounded;
    if (ram == NULL || ram_size < 2UL) return;
    bounded = (end_addr > addr && end_addr <= ram_size) ? 1 : 0;
    video->busy = 1U;
    video->busy_cycles_ns = 0UL;
    video->blit_idle_op_bytes = 0UL;
    set_exec_clip_from_regs(video);
    for (i = 0UL; i < max_ops; i++) {
        if (bounded && !bounded_command_fits(addr, end_addr, 2UL)) break;
        op = read_ram16(ram, ram_size, addr);
        if (op == 0x0000U || op == 0xffffU) break;
        video->executed_ops++;
        switch (op & CV1K_BLIT_OP_MASK) {
        case CV1K_BLIT_OP_UPLOAD:
            if (bounded && !bounded_upload_fits(addr, end_addr, ram, ram_size)) {
                finish_blit_delay_mame(video);
                video->busy = 0U;
                return;
            }
            addr = execute_upload(video, addr, ram, ram_size);
            break;
        case CV1K_BLIT_OP_DRAW:
            if (bounded && !bounded_command_fits(addr, end_addr, CV1K_DRAW_OPERATION_SIZE_BYTES)) {
                finish_blit_delay_mame(video);
                video->busy = 0U;
                return;
            }
            addr = execute_draw(video, addr, ram, ram_size);
            break;
        case CV1K_BLIT_OP_CLIP:
            if (bounded && !bounded_command_fits(addr, end_addr, 4UL)) {
                finish_blit_delay_mame(video);
                video->busy = 0U;
                return;
            }
            apply_clip_command(video, read_ram16(ram, ram_size, addr + 2UL));
            /* MAME reads the opcode word and then the clip parameter word.
             * CV1K_CLIP_OPERATION_SIZE_BYTES is only the idle/timing byte
             * count for the parameter operation; the command stream advances
             * by four bytes.
             */
            addr += 4UL;
            break;
        default:
            video->unknown_ops++;
            video->last_unknown_op = (cv1k_u32)op;
            finish_blit_delay_mame(video);
            video->busy = 0U;
            return;
        }
        addr %= ram_size;
    }
    finish_blit_delay_mame(video);
    if (video->busy_cycles_ns != 0UL) {
        double cyc_d;
        cyc_d = ((double)video->busy_cycles_ns * (double)CV1K_CPU_CLOCK_HZ) / 1000000000.0;
        if (cyc_d < 1.0) cyc_d = 1.0;
        if (cyc_d > 4294967295.0) cyc_d = 4294967295.0;
        video->busy_cycles_left = (cv1k_u32)cyc_d;
        video->busy = 1U;
    } else {
        video->busy_cycles_left = 0UL;
        video->busy = 0U;
    }
}

void cv1k_video_tick_cycles(struct cv1k_video *video, cv1k_u32 cycles)
{
    if (video == NULL) return;
    if (video->busy_cycles_left == 0UL) {
        video->busy = 0U;
        return;
    }
    if (cycles >= video->busy_cycles_left) {
        video->busy_cycles_left = 0UL;
        video->busy = 0U;
    } else {
        video->busy_cycles_left -= cycles;
        video->busy = 1U;
    }
}

static cv1k_u32 cv1k_video_measure_list_end(cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops)
{
    cv1k_u32 i;
    cv1k_u32 pos;
    if (ram == NULL || ram_size < 2UL) return 0UL;
    pos = addr % ram_size;
    for (i = 0UL; i < max_ops; i++) {
        cv1k_u16 op;
        if (pos + 2UL > ram_size) return 0UL;
        op = read_ram16(ram, ram_size, pos);
        if (op == 0x0000U || op == 0xffffU) return pos + 2UL;
        switch (op & CV1K_BLIT_OP_MASK) {
        case CV1K_BLIT_OP_UPLOAD:
            {
                cv1k_u32 w;
                cv1k_u32 h;
                cv1k_u32 pixels;
                cv1k_u32 need;
                if (pos + CV1K_UPLOAD_HEADER_SIZE_BYTES > ram_size) return 0UL;
                w = (cv1k_u32)(read_ram16(ram, ram_size, pos + 12UL) & 0x1fffU) + 1UL;
                h = (cv1k_u32)(read_ram16(ram, ram_size, pos + 14UL) & 0x0fffU) + 1UL;
                if (h != 0UL && w > 0xffffffffUL / h) return 0UL;
                pixels = w * h;
                if (pixels > (0xffffffffUL - CV1K_UPLOAD_HEADER_SIZE_BYTES) / 2UL) return 0UL;
                need = CV1K_UPLOAD_HEADER_SIZE_BYTES + pixels * 2UL;
                if (pos + need < pos || pos + need > ram_size) return 0UL;
                pos += need;
            }
            break;
        case CV1K_BLIT_OP_DRAW:
            if (pos + CV1K_DRAW_OPERATION_SIZE_BYTES < pos || pos + CV1K_DRAW_OPERATION_SIZE_BYTES > ram_size) return 0UL;
            pos += CV1K_DRAW_OPERATION_SIZE_BYTES;
            break;
        case CV1K_BLIT_OP_CLIP:
            if (pos + 4UL < pos || pos + 4UL > ram_size) return 0UL;
            pos += 4UL;
            break;
        default:
            return pos + 2UL;
        }
    }
    return pos;
}

static void cv1k_video_execute_list_common(struct cv1k_video *video, cv1k_u32 addr, cv1k_u32 end_addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops)
{
    cv1k_u8 *shadow;
    cv1k_u32 local_addr;
    cv1k_u32 shadow_size;
    /* MAME snapshots the blit command stream before queuing the worker.
     * Earlier sandbox builds copied the full 8/16 MiB work RAM for every
     * execute pulse.  DDPSDOJ command/upload lists are linear, so first scan
     * the list bounds and snapshot only the prefix containing the stream; this
     * preserves self-modifying-list isolation without gigabytes of avoidable
     * memcpy traffic during boot and attract-mode blits. */
    if (ram == NULL || ram_size < 2UL) return;
    video->last_list_addr = addr;
    local_addr = addr % ram_size;
    shadow_size = 0UL;
    if (end_addr > local_addr && end_addr <= ram_size) shadow_size = end_addr;
    else shadow_size = cv1k_video_measure_list_end(local_addr, ram, ram_size, max_ops);
    if (shadow_size <= local_addr || shadow_size > ram_size) shadow_size = ram_size;

    shadow = (cv1k_u8 *)cv1k_xmalloc(shadow_size);
    if (shadow != NULL) {
        memcpy(shadow, ram, (size_t)shadow_size);
        cv1k_video_execute_list_from(video, local_addr, shadow_size, shadow, shadow_size, max_ops);
        cv1k_free(shadow);
    } else {
        cv1k_video_execute_list_from(video, addr, end_addr, ram, ram_size, max_ops);
    }
}

void cv1k_video_execute_list(struct cv1k_video *video, cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops)
{
    cv1k_video_execute_list_common(video, addr, 0UL, ram, ram_size, max_ops);
}

void cv1k_video_execute_list_bounded(struct cv1k_video *video, cv1k_u32 addr, cv1k_u32 end_addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops)
{
    cv1k_video_execute_list_common(video, addr, end_addr, ram, ram_size, max_ops);
}

CV1K_HOT void cv1k_video_write8(struct cv1k_video *video, cv1k_u32 offset, cv1k_u8 data, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    cv1k_u32 index;
    cv1k_u32 shift;
    cv1k_u32 mask;
    cv1k_u32 regbase;
    if (offset >= CV1K_REGION_BLITTER_SIZE) return;
    index = offset >> 2;
    shift = (3UL - (offset & 3UL)) * 8UL;
    mask = 0xffUL << shift;
    video->regs[index] = (video->regs[index] & ~mask) | (((cv1k_u32)data) << shift);
    regbase = offset & ~3UL;

    /* MAME's cv1k_blitter_device::blitter_w map:
     *   0x04 low byte bit 0: execute current command list
     *   0x08: command-list address
     *   0x14/0x18: screen scroll
     *   0x40/0x44: clip origin
     * The previous sandbox used an early guessed map and ran blits from
     * register 0x00.  Keep byte-granular writes, but decode side effects
     * from the MAME offsets.
     */
    if (regbase == 0x04UL && (offset & 3UL) == 3UL && (data & 0x01U) != 0U) {
        cv1k_u32 addr;
        /* MAME stores the full address in m_gfx_addr and masks it with
         * 0x1fffffff at blit time.  The RAM reader then applies the main-RAM
         * mask.  Preserve the full P1/P2/physical form here instead of
         * forcing a local 24-bit offset; this keeps diagnostics and repeated
         * launches aligned with cv1k_blitter_device::gfx_exec_w().
         */
        addr = video->regs[0x08UL >> 2] & 0x1fffffffUL;
        video->mmio_execs++;
        video->last_mmio_list_addr = addr;
        cv1k_video_execute_list(video, addr, ram, ram_size, 4096UL);
    } else if (regbase == 0x14UL || regbase == 0x18UL) {
        video->gfx_scroll_x = video->regs[0x14UL >> 2] & 0x1fffUL;
        video->gfx_scroll_y = video->regs[0x18UL >> 2] & 0x0fffUL;
    } else if (regbase == 0x40UL || regbase == 0x44UL) {
        set_exec_clip_from_regs(video);
    }
}

CV1K_HOT void cv1k_video_write32(struct cv1k_video *video, cv1k_u32 offset, cv1k_u32 data, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    cv1k_u32 regbase;
    if (video == NULL || offset >= CV1K_REGION_BLITTER_SIZE) return;
    if ((offset & 3UL) != 0UL) {
        cv1k_video_write8(video, offset, (cv1k_u8)((data >> 24) & 0xffU), ram, ram_size);
        cv1k_video_write8(video, offset + 1UL, (cv1k_u8)((data >> 16) & 0xffU), ram, ram_size);
        cv1k_video_write8(video, offset + 2UL, (cv1k_u8)((data >> 8) & 0xffU), ram, ram_size);
        cv1k_video_write8(video, offset + 3UL, (cv1k_u8)(data & 0xffU), ram, ram_size);
        return;
    }
    video->regs[offset >> 2] = data;
    regbase = offset;
    if (regbase == 0x04UL && (data & 0x01U) != 0U) {
        cv1k_u32 addr;
        addr = video->regs[0x08UL >> 2] & 0x1fffffffUL;
        video->mmio_execs++;
        video->last_mmio_list_addr = addr;
        cv1k_video_execute_list(video, addr, ram, ram_size, 4096UL);
    } else if (regbase == 0x14UL || regbase == 0x18UL) {
        video->gfx_scroll_x = video->regs[0x14UL >> 2] & 0x1fffUL;
        video->gfx_scroll_y = video->regs[0x18UL >> 2] & 0x0fffUL;
    } else if (regbase == 0x40UL || regbase == 0x44UL) {
        set_exec_clip_from_regs(video);
    }
}

CV1K_HOT void cv1k_video_frame(struct cv1k_video *video, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_u32 sx;
    cv1k_u32 sy;
    cv1k_u16 pix;
    cv1k_u32 frame_nonzero;
    CV1K_UNUSED(ram);
    CV1K_UNUSED(ram_size);
    video->frame_counter++;
    if (video->vram1555 == NULL || video->screen_rgb == NULL) return;
    if (video->executed_ops == 0UL) {
        cv1k_u32 c;
        cv1k_u32 sample;
        sample = 0UL;
        if (ram != NULL && ram_size != 0UL) sample = ram[(video->frame_counter * 97UL) % ram_size];
        for (y = 0UL; y < CV1K_SCREEN_H; y++) {
            for (x = 0UL; x < CV1K_SCREEN_W; x++) {
                c = ((x + video->frame_counter) ^ (y * 3UL) ^ sample) & 0xffUL;
                video->screen_rgb[y * CV1K_FRAMEBUFFER_W + x] = (c << 16) | ((c ^ 0x55UL) << 8) | (c ^ 0xaaUL);
            }
        }
        video->last_frame_nonzero = CV1K_SCREEN_W * CV1K_SCREEN_H;
        return;
    }

    frame_nonzero = 0UL;
    for (y = 0UL; y < CV1K_SCREEN_H; y++) {
        cv1k_u32 *dstrow;
        const cv1k_u16 *srcrow;
        /* MAME cv1k screen_update: copyscrollbitmap(dst, src, scroll=-m_gfx_scroll)
         * => dst(x,y) = src(x + gfx_scroll_x, y + gfx_scroll_y) with wrap. */
        sy = (y + (video->gfx_scroll_y & (CV1K_VRAM_H - 1UL))) & (CV1K_VRAM_H - 1UL);
        sx = video->gfx_scroll_x & (CV1K_VRAM_W - 1UL);
        dstrow = video->screen_rgb + y * CV1K_FRAMEBUFFER_W;
        if (CV1K_LIKELY(sx + CV1K_SCREEN_W <= CV1K_VRAM_W)) {
            srcrow = video->vram1555 + sy * CV1K_VRAM_W + sx;
            for (x = 0UL; x < CV1K_SCREEN_W; x++) {
                pix = srcrow[x];
                frame_nonzero += ((pix & 0x7fffU) != 0U);
                dstrow[x] = rgb1555_to_rgb888(pix);
            }
        } else {
            for (x = 0UL; x < CV1K_SCREEN_W; x++) {
                cv1k_u32 sxw = (sx + x) & (CV1K_VRAM_W - 1UL);
                pix = video->vram1555[sy * CV1K_VRAM_W + sxw];
                frame_nonzero += ((pix & 0x7fffU) != 0U);
                dstrow[x] = rgb1555_to_rgb888(pix);
            }
        }
    }
    video->last_frame_nonzero = frame_nonzero;
}


static int cv1k_str_contains_fold(const char *s, const char *needle)
{
    cv1k_u32 i;
    cv1k_u32 j;
    if (s == NULL || needle == NULL || needle[0] == '\0') return 0;
    for (i = 0U; s[i] != '\0'; i++) {
        for (j = 0U; needle[j] != '\0'; j++) {
            char a = s[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b || a == '\0') break;
        }
        if (needle[j] == '\0') return 1;
    }
    return 0;
}

int cv1k_video_display_rotation_parse(const char *name, int *out_rotation)
{
    int r;
    if (name == NULL || out_rotation == NULL) return 0;
    if (strcmp(name, "auto") == 0) r = CV1K_DISPLAY_ROT_AUTO;
    else if (strcmp(name, "0") == 0 || strcmp(name, "none") == 0 || strcmp(name, "normal") == 0 || strcmp(name, "landscape") == 0) r = CV1K_DISPLAY_ROT_0;
    else if (strcmp(name, "cw") == 0 || strcmp(name, "90") == 0 || strcmp(name, "90cw") == 0 || strcmp(name, "right") == 0) r = CV1K_DISPLAY_ROT_CW;
    else if (strcmp(name, "180") == 0 || strcmp(name, "flip") == 0) r = CV1K_DISPLAY_ROT_180;
    else if (strcmp(name, "ccw") == 0 || strcmp(name, "270") == 0 || strcmp(name, "90ccw") == 0 || strcmp(name, "left") == 0 || strcmp(name, "tate") == 0) r = CV1K_DISPLAY_ROT_CCW;
    else return 0;
    *out_rotation = r;
    return 1;
}

const char *cv1k_video_display_rotation_name(int rotation)
{
    switch (rotation) {
    case CV1K_DISPLAY_ROT_0: return "none";
    case CV1K_DISPLAY_ROT_CW: return "cw";
    case CV1K_DISPLAY_ROT_180: return "180";
    case CV1K_DISPLAY_ROT_CCW: return "ccw";
    default: return "auto";
    }
}

int cv1k_video_display_rotation_auto_for_path(const char *path)
{
    /* Akai Katana's visible orientation differs from DDPSDOJ in this sandbox.
     * Keep DDPSDOJ's historical ROT270/CCW frontend default, but use a 90-degree
     * clockwise frontend transform for akatana.zip / extracted akatana trees.
     */
    if (cv1k_str_contains_fold(path, "akatana")) return CV1K_DISPLAY_ROT_CW;
    return CV1K_DISPLAY_ROT_CCW;
}

void cv1k_video_display_dimensions(int rotation, cv1k_u32 *out_w, cv1k_u32 *out_h)
{
    if (rotation == CV1K_DISPLAY_ROT_AUTO) rotation = CV1K_DISPLAY_ROT_CCW;
    if (rotation == CV1K_DISPLAY_ROT_CW || rotation == CV1K_DISPLAY_ROT_CCW) {
        if (out_w != NULL) *out_w = CV1K_SCREEN_H;
        if (out_h != NULL) *out_h = CV1K_SCREEN_W;
    } else {
        if (out_w != NULL) *out_w = CV1K_SCREEN_W;
        if (out_h != NULL) *out_h = CV1K_SCREEN_H;
    }
}

CV1K_HOT cv1k_u32 cv1k_video_display_pixel(const struct cv1k_video *video, int rotation, cv1k_u32 x, cv1k_u32 y)
{
    cv1k_u32 sx;
    cv1k_u32 sy;
    if (video == NULL || video->screen_rgb == NULL) return 0U;
    if (rotation == CV1K_DISPLAY_ROT_AUTO) rotation = CV1K_DISPLAY_ROT_CCW;
    switch (rotation) {
    case CV1K_DISPLAY_ROT_0:
        sx = x;
        sy = y;
        break;
    case CV1K_DISPLAY_ROT_CW:
        sx = y;
        sy = (CV1K_SCREEN_H - 1U) - x;
        break;
    case CV1K_DISPLAY_ROT_180:
        sx = (CV1K_SCREEN_W - 1U) - x;
        sy = (CV1K_SCREEN_H - 1U) - y;
        break;
    case CV1K_DISPLAY_ROT_CCW:
    default:
        sx = (CV1K_SCREEN_W - 1U) - y;
        sy = x;
        break;
    }
    return video->screen_rgb[sy * CV1K_FRAMEBUFFER_W + sx];
}

CV1K_HOT void cv1k_video_make_display_xrgb8888(const struct cv1k_video *video, int rotation, cv1k_u32 *dst, cv1k_u32 dst_pitch)
{
    cv1k_u32 w;
    cv1k_u32 h;
    cv1k_u32 x;
    cv1k_u32 y;
    if (video == NULL || video->screen_rgb == NULL || dst == NULL) return;
    cv1k_video_display_dimensions(rotation, &w, &h);
    if (dst_pitch < w) return;
    if (rotation == CV1K_DISPLAY_ROT_AUTO) rotation = CV1K_DISPLAY_ROT_CCW;
    switch (rotation) {
    case CV1K_DISPLAY_ROT_0:
        for (y = 0U; y < CV1K_SCREEN_H; y++) {
            memcpy(dst + y * dst_pitch, video->screen_rgb + y * CV1K_FRAMEBUFFER_W, (size_t)CV1K_SCREEN_W * sizeof(cv1k_u32));
        }
        break;
    case CV1K_DISPLAY_ROT_CW:
        for (y = 0U; y < CV1K_SCREEN_W; y++) {
            cv1k_u32 *d = dst + y * dst_pitch;
            for (x = 0U; x < CV1K_SCREEN_H; x++) d[x] = video->screen_rgb[((CV1K_SCREEN_H - 1U) - x) * CV1K_FRAMEBUFFER_W + y];
        }
        break;
    case CV1K_DISPLAY_ROT_180:
        for (y = 0U; y < CV1K_SCREEN_H; y++) {
            cv1k_u32 *d = dst + y * dst_pitch;
            const cv1k_u32 *s = video->screen_rgb + ((CV1K_SCREEN_H - 1U) - y) * CV1K_FRAMEBUFFER_W;
            for (x = 0U; x < CV1K_SCREEN_W; x++) d[x] = s[(CV1K_SCREEN_W - 1U) - x];
        }
        break;
    case CV1K_DISPLAY_ROT_CCW:
    default:
        for (y = 0U; y < CV1K_SCREEN_W; y++) {
            cv1k_u32 *d = dst + y * dst_pitch;
            cv1k_u32 sx = (CV1K_SCREEN_W - 1U) - y;
            for (x = 0U; x < CV1K_SCREEN_H; x++) d[x] = video->screen_rgb[x * CV1K_FRAMEBUFFER_W + sx];
        }
        break;
    }
}

int cv1k_video_write_display_ppm(const struct cv1k_video *video, int rotation, const char *path)
{
    FILE *f;
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_u32 w;
    cv1k_u32 h;
    if (video == NULL || video->screen_rgb == NULL || path == NULL) return 0;
    cv1k_video_display_dimensions(rotation, &w, &h);
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    for (y = 0U; y < h; y++) {
        for (x = 0U; x < w; x++) {
            cv1k_u32 p = cv1k_video_display_pixel(video, rotation, x, y);
            fputc((int)((p >> 16) & 0xffU), f);
            fputc((int)((p >> 8) & 0xffU), f);
            fputc((int)(p & 0xffU), f);
        }
    }
    fclose(f);
    return 1;
}

CV1K_HOT cv1k_u32 cv1k_video_present_checksum(const struct cv1k_video *video)
{
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_u32 h = 2166136261UL;
    if (video == NULL || video->screen_rgb == NULL) return 0UL;
    for (y = 0UL; y < CV1K_SCREEN_H; y++) {
        const cv1k_u32 *row = video->screen_rgb + y * CV1K_FRAMEBUFFER_W;
        for (x = 0UL; x < CV1K_SCREEN_W; x++) {
            h ^= row[x];
            h *= 16777619UL;
        }
    }
    return h;
}


int cv1k_video_write_ppm(const struct cv1k_video *video, const char *path)
{
    FILE *f;
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_u32 p;
    if (video->screen_rgb == NULL) return 0;
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)CV1K_SCREEN_W, (unsigned)CV1K_SCREEN_H);
    for (y = 0UL; y < CV1K_SCREEN_H; y++) {
        for (x = 0UL; x < CV1K_SCREEN_W; x++) {
            p = video->screen_rgb[y * CV1K_FRAMEBUFFER_W + x];
            fputc((int)((p >> 16) & 0xffUL), f);
            fputc((int)((p >> 8) & 0xffUL), f);
            fputc((int)(p & 0xffUL), f);
        }
    }
    fclose(f);
    return 1;
}

static const unsigned char font5x7_unknown[7] = { 0x0eU, 0x11U, 0x01U, 0x02U, 0x04U, 0x00U, 0x04U };

static const unsigned char *font5x7_get(int ch)
{
    static const unsigned char sp[7] = {0,0,0,0,0,0,0};
    static const unsigned char colon[7] = {0,4,4,0,4,4,0};
    static const unsigned char dash[7] = {0,0,0,31,0,0,0};
    static const unsigned char dot[7] = {0,0,0,0,0,6,6};
    static const unsigned char slash[7] = {1,2,2,4,8,8,16};
    static const unsigned char zero[7] = {14,17,19,21,25,17,14};
    static const unsigned char one[7] = {4,12,4,4,4,4,14};
    static const unsigned char two[7] = {14,17,1,2,4,8,31};
    static const unsigned char three[7] = {30,1,1,14,1,1,30};
    static const unsigned char four[7] = {2,6,10,18,31,2,2};
    static const unsigned char five[7] = {31,16,30,1,1,17,14};
    static const unsigned char six[7] = {6,8,16,30,17,17,14};
    static const unsigned char seven[7] = {31,1,2,4,8,8,8};
    static const unsigned char eight[7] = {14,17,17,14,17,17,14};
    static const unsigned char nine[7] = {14,17,17,15,1,2,12};
    static const unsigned char A[7] = {14,17,17,31,17,17,17};
    static const unsigned char B[7] = {30,17,17,30,17,17,30};
    static const unsigned char C[7] = {14,17,16,16,16,17,14};
    static const unsigned char D[7] = {30,17,17,17,17,17,30};
    static const unsigned char E[7] = {31,16,16,30,16,16,31};
    static const unsigned char F[7] = {31,16,16,30,16,16,16};
    static const unsigned char G[7] = {14,17,16,23,17,17,15};
    static const unsigned char H[7] = {17,17,17,31,17,17,17};
    static const unsigned char I[7] = {14,4,4,4,4,4,14};
    static const unsigned char J[7] = {7,2,2,2,18,18,12};
    static const unsigned char K[7] = {17,18,20,24,20,18,17};
    static const unsigned char L[7] = {16,16,16,16,16,16,31};
    static const unsigned char M[7] = {17,27,21,21,17,17,17};
    static const unsigned char N[7] = {17,25,21,19,17,17,17};
    static const unsigned char O[7] = {14,17,17,17,17,17,14};
    static const unsigned char P[7] = {30,17,17,30,16,16,16};
    static const unsigned char Q[7] = {14,17,17,17,21,18,13};
    static const unsigned char R[7] = {30,17,17,30,20,18,17};
    static const unsigned char S[7] = {15,16,16,14,1,1,30};
    static const unsigned char T[7] = {31,4,4,4,4,4,4};
    static const unsigned char U[7] = {17,17,17,17,17,17,14};
    static const unsigned char V[7] = {17,17,17,17,17,10,4};
    static const unsigned char W[7] = {17,17,17,21,21,21,10};
    static const unsigned char X[7] = {17,17,10,4,10,17,17};
    static const unsigned char Y[7] = {17,17,10,4,4,4,4};
    static const unsigned char Z[7] = {31,1,2,4,8,16,31};
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    switch (ch) {
    case ' ': return sp; case ':': return colon; case '-': return dash; case '.': return dot; case '/': return slash;
    case '0': return zero; case '1': return one; case '2': return two; case '3': return three; case '4': return four;
    case '5': return five; case '6': return six; case '7': return seven; case '8': return eight; case '9': return nine;
    case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D; case 'E': return E; case 'F': return F;
    case 'G': return G; case 'H': return H; case 'I': return I; case 'J': return J; case 'K': return K; case 'L': return L;
    case 'M': return M; case 'N': return N; case 'O': return O; case 'P': return P; case 'Q': return Q; case 'R': return R;
    case 'S': return S; case 'T': return T; case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X;
    case 'Y': return Y; case 'Z': return Z;
    default: return font5x7_unknown;
    }
}

void cv1k_video_clear_rgb(struct cv1k_video *video, cv1k_u32 rgb)
{
    cv1k_u32 x;
    cv1k_u32 y;
    if (video == NULL || video->screen_rgb == NULL) return;
    for (y = 0UL; y < CV1K_SCREEN_H; y++) for (x = 0UL; x < CV1K_SCREEN_W; x++) video->screen_rgb[y * CV1K_FRAMEBUFFER_W + x] = rgb;
}

void cv1k_video_rect_rgb(struct cv1k_video *video, cv1k_u32 x, cv1k_u32 y, cv1k_u32 w, cv1k_u32 h, cv1k_u32 rgb)
{
    cv1k_u32 px;
    cv1k_u32 py;
    if (video == NULL || video->screen_rgb == NULL) return;
    for (py = y; py < y + h && py < CV1K_SCREEN_H; py++) for (px = x; px < x + w && px < CV1K_SCREEN_W; px++) video->screen_rgb[py * CV1K_FRAMEBUFFER_W + px] = rgb;
}

void cv1k_video_text_rgb(struct cv1k_video *video, cv1k_u32 x, cv1k_u32 y, const char *text, cv1k_u32 rgb, cv1k_u32 scale)
{
    cv1k_u32 cx;
    int ci;
    if (video == NULL || text == NULL || video->screen_rgb == NULL) return;
    if (scale == 0UL) scale = 1UL;
    cx = x;
    while (*text != '\0') {
        const unsigned char *g;
        cv1k_u32 row;
        cv1k_u32 col;
        cv1k_u32 sx;
        cv1k_u32 sy;
        ci = (int)(unsigned char)*text;
        g = font5x7_get(ci);
        for (row = 0UL; row < 7UL; row++) {
            for (col = 0UL; col < 5UL; col++) {
                if ((g[row] & (1U << (4U - col))) != 0U) {
                    for (sy = 0UL; sy < scale; sy++) for (sx = 0UL; sx < scale; sx++) {
                        cv1k_u32 px;
                        cv1k_u32 py;
                        px = cx + col * scale + sx;
                        py = y + row * scale + sy;
                        if (px < CV1K_SCREEN_W && py < CV1K_SCREEN_H) video->screen_rgb[py * CV1K_FRAMEBUFFER_W + px] = rgb;
                    }
                }
            }
        }
        cx += 6UL * scale;
        text++;
    }
}
