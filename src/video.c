#include "video.h"
#include "platform.h"
#include "mame_cv1k_derived.h"
#include <stdio.h>
#include <string.h>

static cv1k_u32 rgb1555_to_rgb888(cv1k_u16 p)
{
    cv1k_u32 r;
    cv1k_u32 g;
    cv1k_u32 b;
    r = (cv1k_u32)((p >> 10) & 0x1fU);
    g = (cv1k_u32)((p >> 5) & 0x1fU);
    b = (cv1k_u32)(p & 0x1fU);
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
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

static cv1k_u8 blend_component(cv1k_u8 s, cv1k_u8 d, cv1k_u8 alpha, int mode)
{
    cv1k_u8 scaled;
    scaled = cv1k_mame_mul5(s, (cv1k_u8)((alpha >> 3) & 0x3fU));
    switch (mode & 7) {
    case 0: return cv1k_mame_add5(d, scaled);       /* +alpha */
    case 1: return cv1k_mame_add5(d, s);            /* +source */
    case 2: return cv1k_mame_add5(d, d);            /* +destination */
    case 4: return (d > scaled) ? (cv1k_u8)(d - scaled) : 0U;
    case 5: return (d > s) ? (cv1k_u8)(d - s) : 0U;
    case 6: return 0U;
    default: return s;
    }
}

static cv1k_u16 blend_pixel(cv1k_u16 src, cv1k_u16 dst, cv1k_u8 src_alpha, int src_mode, int dst_mode)
{
    cv1k_u8 sr;
    cv1k_u8 sg;
    cv1k_u8 sb;
    cv1k_u8 dr;
    cv1k_u8 dg;
    cv1k_u8 db;
    cv1k_u8 rr;
    cv1k_u8 rg;
    cv1k_u8 rb;
    CV1K_UNUSED(dst_mode);
    sr = (cv1k_u8)((src >> 10) & 0x1fU);
    sg = (cv1k_u8)((src >> 5) & 0x1fU);
    sb = (cv1k_u8)(src & 0x1fU);
    dr = (cv1k_u8)((dst >> 10) & 0x1fU);
    dg = (cv1k_u8)((dst >> 5) & 0x1fU);
    db = (cv1k_u8)(dst & 0x1fU);
    rr = blend_component(sr, dr, src_alpha, src_mode);
    rg = blend_component(sg, dg, src_alpha, src_mode);
    rb = blend_component(sb, db, src_alpha, src_mode);
    return make1555(rr, rg, rb, 1U);
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

int cv1k_video_init(struct cv1k_video *video)
{
    memset(video, 0, sizeof(*video));
    cv1k_mame_build_color_tables();
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
    video->blit_idle_op_bytes = 0UL;
    video->blit_hline_penalty_ns = 0UL;
    video->blit_over_frame_count = 0UL;
    video->gfx_scroll_x = 0UL;
    video->gfx_scroll_y = 0UL;
    video->clip_x = 0UL;
    video->clip_y = 0UL;
    video->clip_w = CV1K_SCREEN_W;
    video->clip_h = CV1K_SCREEN_H;
    video->busy = 0U;
    video->fpga_firmware_pos = 0UL;
    video->fpga_firmware_checksum = 0UL;
    video->fpga_firmware_done = 0UL;
    video->fpga_firmware_version = -1L;
    video->fpga_firmware_port = 0U;
    video->fpga_firmware_byte = 0U;
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

static cv1k_u32 cv1k_video_read32_mame(const struct cv1k_video *video, cv1k_u32 regoff)
{
    /* ANSI C adaptation of MAME cv1k_blitter_device::blitter_r.
     * The MAME device only returns defined values for ready/status, two
     * handshake registers, and the DSW port.  DSW defaults to low-nibble
     * switches off and high bits active/unknown (0xfffffff0).  Other offsets
     * fall through to zero after logging; returning shadow registers here was
     * a v27 scaffold convenience, but it is not MAME-compatible.
     */
    switch (regoff & ~3UL) {
    case 0x10UL: return video->busy ? 0x00000000UL : 0x00000010UL;
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
    v = cv1k_video_read32_mame(video, offset);
    shift = (3UL - (offset & 3UL)) * 8UL;
    return (cv1k_u8)((v >> shift) & 0xffUL);
}



static void set_exec_clip_from_regs(struct cv1k_video *video)
{
    /* MAME resets the active clip at the start of both shadow-copy and
     * execution passes from the shadowed 0x40/0x44 clip registers, expanded
     * by the 32-pixel CV1000 margin.  Keep the coordinates saturating for the
     * ANSI C VRAM array instead of allowing unsigned underflow.
     */
    cv1k_u32 x;
    cv1k_u32 y;
    x = video->regs[0x40UL >> 2] & 0x1fffUL;
    y = video->regs[0x44UL >> 2] & 0x0fffUL;
    video->clip_x = (x >= CV1K_CLIP_MARGIN) ? (x - CV1K_CLIP_MARGIN) : 0UL;
    video->clip_y = (y >= CV1K_CLIP_MARGIN) ? (y - CV1K_CLIP_MARGIN) : 0UL;
    video->clip_w = CV1K_SCREEN_W + (CV1K_CLIP_MARGIN * 2UL);
    video->clip_h = CV1K_SCREEN_H + (CV1K_CLIP_MARGIN * 2UL);
    if (video->clip_x + video->clip_w > CV1K_VRAM_W) video->clip_w = CV1K_VRAM_W - video->clip_x;
    if (video->clip_y + video->clip_h > CV1K_VRAM_H) video->clip_h = CV1K_VRAM_H - video->clip_y;
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
        video->clip_x = 0UL;
        video->clip_y = 0UL;
        video->clip_w = CV1K_VRAM_W;
        video->clip_h = CV1K_VRAM_H;
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

static cv1k_u32 calculate_vram_accesses_mame(cv1k_u32 start_x, cv1k_u32 start_y, cv1k_u32 dimx, cv1k_u32 dimy)
{
    cv1k_u32 x_rows;
    cv1k_u32 num_vram_rows;
    cv1k_u32 x_pixels;
    cv1k_u32 y_pixels;
    cv1k_u32 chunk;
    x_rows = 0UL;
    num_vram_rows = 0UL;
    for (x_pixels = dimx; x_pixels > 0UL; ) {
        chunk = (x_pixels < 32UL) ? x_pixels : 32UL;
        x_rows++;
        if (((start_x & 31UL) + chunk) > 32UL) x_rows++;
        if (x_pixels <= 32UL) break;
        x_pixels -= 32UL;
    }
    for (y_pixels = dimy; y_pixels > 0UL; ) {
        chunk = (y_pixels < 32UL) ? y_pixels : 32UL;
        num_vram_rows += x_rows;
        if (((start_y & 31UL) + chunk) > 32UL) num_vram_rows += x_rows;
        if (y_pixels <= 32UL) break;
        y_pixels -= 32UL;
    }
    return num_vram_rows;
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

static cv1k_u32 execute_upload(struct cv1k_video *video, cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    cv1k_u32 dst_x;
    cv1k_u32 dst_y;
    cv1k_u32 w;
    cv1k_u32 h;
    cv1k_u32 px;
    cv1k_u32 py;
    cv1k_u32 pos;
    cv1k_u16 pen;
    dst_x = (cv1k_u32)(read_ram16(ram, ram_size, addr + 8UL) & 0x1fffU);
    dst_y = (cv1k_u32)(read_ram16(ram, ram_size, addr + 10UL) & 0x0fffU);
    w = (cv1k_u32)(read_ram16(ram, ram_size, addr + 12UL) & 0x1fffU) + 1UL;
    h = (cv1k_u32)(read_ram16(ram, ram_size, addr + 14UL) & 0x0fffU) + 1UL;
    pos = addr + CV1K_UPLOAD_HEADER_SIZE_BYTES;
    if (w > CV1K_VRAM_W) w = CV1K_VRAM_W;
    if (h > CV1K_VRAM_H) h = CV1K_VRAM_H;
    for (py = 0UL; py < h; py++) {
        for (px = 0UL; px < w; px++) {
            pen = read_ram16(ram, ram_size, pos);
            pos += 2UL;
            if (video->vram1555 != NULL) video->vram1555[vram_index(dst_x + px, dst_y + py)] = pen;
        }
    }
    video->upload_ops++;
    video->busy_cycles_ns += (((CV1K_UPLOAD_HEADER_SIZE_BYTES + w * h * 2UL) / 4UL) * CV1K_SRAM_CLK_NANOSEC);
    video->blit_idle_op_bytes = 0UL;
    return pos;
}

static cv1k_u32 execute_draw(struct cv1k_video *video, cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size)
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
    cv1k_u8 src_mode;
    cv1k_u8 dst_mode;
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
    src_alpha = (cv1k_u8)((alphaw >> 8) & 0xffU);
    src_mode = (cv1k_u8)((flags >> 4) & 7U);
    dst_mode = (cv1k_u8)(flags & 7U);
    if (w > CV1K_VRAM_W) w = CV1K_VRAM_W;
    if (h > CV1K_VRAM_H) h = CV1K_VRAM_H;
    for (py = 0UL; py < h; py++) {
        sy = ((flags & 0x0400U) != 0U) ? (src_y + (h - 1UL - py)) : (src_y + py);
        dy = dst_y + (cv1k_s32)py;
        if (dy < 0 || (cv1k_u32)dy >= CV1K_VRAM_H) continue;
        for (px = 0UL; px < w; px++) {
            sx = ((flags & 0x0800U) != 0U) ? (src_x + (w - 1UL - px)) : (src_x + px);
            dx = dst_x + (cv1k_s32)px;
            if (dx < 0 || (cv1k_u32)dx >= CV1K_VRAM_W) continue;
            if ((cv1k_u32)dx < video->clip_x || (cv1k_u32)dy < video->clip_y) continue;
            if ((cv1k_u32)dx >= video->clip_x + video->clip_w || (cv1k_u32)dy >= video->clip_y + video->clip_h) continue;
            src = video->vram1555[vram_index(sx, sy)];
            if ((flags & 0x0100U) != 0U && (src & 0x8000U) == 0U) continue;
            src = apply_tint(src, mul_r, mul_g, mul_b);
            if ((flags & 0x0200U) != 0U) {
                dst = video->vram1555[vram_index((cv1k_u32)dx, (cv1k_u32)dy)];
                src = blend_pixel(src, dst, src_alpha, (int)src_mode, (int)dst_mode);
            }
            video->vram1555[vram_index((cv1k_u32)dx, (cv1k_u32)dy)] = src;
        }
    }
    /* MAME estimates draw time from clipped source/destination VRAM row
     * accesses and 4-pixel DDR transfers.  The pixel loop above is still the
     * sandbox renderer, but the timing/accounting path now follows
     * cv1k_blitter_device::gfx_draw_shadow_copy() more closely.
     */
    clipped_w = w;
    clipped_h = h;
    if (dst_x < (cv1k_s32)video->clip_x) {
        cv1k_u32 cut;
        cut = video->clip_x - (cv1k_u32)dst_x;
        clipped_w = (cut >= clipped_w) ? 0UL : (clipped_w - cut);
    }
    if (dst_y < (cv1k_s32)video->clip_y) {
        cv1k_u32 cuty;
        cuty = video->clip_y - (cv1k_u32)dst_y;
        clipped_h = (cuty >= clipped_h) ? 0UL : (clipped_h - cuty);
    }
    if (dst_x >= 0 && (cv1k_u32)dst_x + clipped_w > video->clip_x + video->clip_w) {
        clipped_w = ((cv1k_u32)dst_x >= video->clip_x + video->clip_w) ? 0UL : (video->clip_x + video->clip_w - (cv1k_u32)dst_x);
    }
    if (dst_y >= 0 && (cv1k_u32)dst_y + clipped_h > video->clip_y + video->clip_h) {
        clipped_h = ((cv1k_u32)dst_y >= video->clip_y + video->clip_h) ? 0UL : (video->clip_y + video->clip_h - (cv1k_u32)dst_y);
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

static void cv1k_video_execute_list_from(struct cv1k_video *video, cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops)
{
    cv1k_u32 i;
    cv1k_u16 op;
    if (ram == NULL || ram_size < 2UL) return;
    video->busy = 1U;
    video->busy_cycles_ns = 0UL;
    video->blit_idle_op_bytes = 0UL;
    set_exec_clip_from_regs(video);
    for (i = 0UL; i < max_ops; i++) {
        op = read_ram16(ram, ram_size, addr);
        if (op == 0x0000U || op == 0xffffU) break;
        video->executed_ops++;
        switch (op & CV1K_BLIT_OP_MASK) {
        case CV1K_BLIT_OP_UPLOAD:
            addr = execute_upload(video, addr, ram, ram_size);
            break;
        case CV1K_BLIT_OP_DRAW:
            addr = execute_draw(video, addr, ram, ram_size);
            break;
        case CV1K_BLIT_OP_CLIP:
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
    video->busy = 0U;
}

void cv1k_video_execute_list(struct cv1k_video *video, cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops)
{
    cv1k_u8 *shadow;
    /* MAME snapshots the blit command stream into m_ram16_copy before queuing
     * the worker thread.  The sandbox executes synchronously, but using a
     * read-only RAM snapshot prevents self-modifying command/data writes from
     * changing an in-flight list and matches the device-level contract more
     * closely.  Full-RAM snapshotting is deliberately simple and ANSI C.
     */
    if (ram == NULL || ram_size < 2UL) return;
    shadow = (cv1k_u8 *)cv1k_xmalloc(ram_size);
    if (shadow != NULL) {
        memcpy(shadow, ram, (size_t)ram_size);
        cv1k_video_execute_list_from(video, addr, shadow, ram_size, max_ops);
        cv1k_free(shadow);
    } else {
        cv1k_video_execute_list_from(video, addr, ram, ram_size, max_ops);
    }
}

void cv1k_video_write8(struct cv1k_video *video, cv1k_u32 offset, cv1k_u8 data, const cv1k_u8 *ram, cv1k_u32 ram_size)
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
        addr = video->regs[0x08UL >> 2] & 0x00ffffffUL;
        cv1k_video_execute_list(video, addr, ram, ram_size, 4096UL);
    } else if (regbase == 0x14UL || regbase == 0x18UL) {
        video->gfx_scroll_x = video->regs[0x14UL >> 2] & 0x1fffUL;
        video->gfx_scroll_y = video->regs[0x18UL >> 2] & 0x0fffUL;
    } else if (regbase == 0x40UL || regbase == 0x44UL) {
        set_exec_clip_from_regs(video);
    }
}

void cv1k_video_frame(struct cv1k_video *video, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_u32 sx;
    cv1k_u32 sy;
    cv1k_u16 pix;
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
        return;
    }

    for (y = 0UL; y < CV1K_SCREEN_H; y++) {
        sy = (y - video->gfx_scroll_y) & (CV1K_VRAM_H - 1UL);
        for (x = 0UL; x < CV1K_SCREEN_W; x++) {
            sx = (x - video->gfx_scroll_x) & (CV1K_VRAM_W - 1UL);
            pix = video->vram1555[vram_index(sx, sy)];
            video->screen_rgb[y * CV1K_FRAMEBUFFER_W + x] = rgb1555_to_rgb888(pix);
        }
    }
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
