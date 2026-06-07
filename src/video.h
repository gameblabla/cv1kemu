#ifndef CV1K_VIDEO_H
#define CV1K_VIDEO_H

#include "cv1k_types.h"
#include "cv1k_config.h"

struct cv1k_video {
    cv1k_u32 regs[0x58 / 4];
    cv1k_u16 *vram1555;
    cv1k_u32 *screen_rgb;
    cv1k_u32 frame_counter;
    cv1k_u32 executed_ops;
    cv1k_u32 upload_ops;
    cv1k_u32 draw_ops;
    cv1k_u32 unknown_ops;
    cv1k_u32 clip_ops;
    cv1k_u32 last_unknown_op;
    cv1k_u32 busy_cycles_ns;
    cv1k_u32 busy_cycles_left;
    cv1k_u32 blit_idle_op_bytes;
    cv1k_u32 blit_hline_penalty_ns;
    cv1k_u32 blit_over_frame_count;
    cv1k_u32 gfx_scroll_x;
    cv1k_u32 gfx_scroll_y;
    cv1k_s32 clip_x;
    cv1k_s32 clip_y;
    cv1k_s32 clip_w;
    cv1k_s32 clip_h;
    cv1k_u32 last_list_addr;
    cv1k_u32 mmio_execs;
    cv1k_u32 last_mmio_list_addr;
    cv1k_u32 last_upload_addr;
    cv1k_u32 last_upload_x;
    cv1k_u32 last_upload_y;
    cv1k_u32 last_upload_w;
    cv1k_u32 last_upload_h;
    cv1k_u32 last_upload_pixels;
    cv1k_u32 last_upload_nonzero;
    cv1k_u32 upload_nonzero_total;
    cv1k_u32 last_draw_addr;
    cv1k_u32 last_draw_flags;
    cv1k_u32 last_draw_alpha;
    cv1k_u32 last_draw_src_x;
    cv1k_u32 last_draw_src_y;
    cv1k_s32 last_draw_dst_x;
    cv1k_s32 last_draw_dst_y;
    cv1k_u32 last_draw_w;
    cv1k_u32 last_draw_h;
    cv1k_u32 last_draw_src_nonzero;
    cv1k_u32 last_draw_written;
    cv1k_u32 last_draw_written_nonzero;
    cv1k_u32 draw_src_nonzero_total;
    cv1k_u32 draw_written_total;
    cv1k_u32 draw_written_nonzero_total;
    cv1k_u32 last_frame_nonzero;
    cv1k_u8 busy;
    cv1k_u32 fpga_firmware_pos;
    cv1k_u32 fpga_firmware_checksum;
    cv1k_u32 fpga_firmware_done;
    cv1k_s32 fpga_firmware_version;
    cv1k_u8 fpga_firmware_port;
    cv1k_u8 fpga_firmware_byte;
};

int cv1k_video_init(struct cv1k_video *video);
void cv1k_video_shutdown(struct cv1k_video *video);
void cv1k_video_reset(struct cv1k_video *video);
cv1k_u8 cv1k_video_read8(struct cv1k_video *video, cv1k_u32 offset);
void cv1k_video_tick_cycles(struct cv1k_video *video, cv1k_u32 cycles);
void cv1k_video_write8(struct cv1k_video *video, cv1k_u32 offset, cv1k_u8 data, const cv1k_u8 *ram, cv1k_u32 ram_size);
cv1k_u8 cv1k_video_fpga_read(struct cv1k_video *video);
void cv1k_video_fpga_write(struct cv1k_video *video, cv1k_u8 data);
void cv1k_video_execute_list(struct cv1k_video *video, cv1k_u32 addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops);
void cv1k_video_execute_list_bounded(struct cv1k_video *video, cv1k_u32 addr, cv1k_u32 end_addr, const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 max_ops);
void cv1k_video_frame(struct cv1k_video *video, const cv1k_u8 *ram, cv1k_u32 ram_size);
int cv1k_video_write_ppm(const struct cv1k_video *video, const char *path);
cv1k_u32 cv1k_video_vram_bytes(void);


/* Diagnostic overlay helpers used by the SDL/ROM probe frontend. */
void cv1k_video_clear_rgb(struct cv1k_video *video, cv1k_u32 rgb);
void cv1k_video_rect_rgb(struct cv1k_video *video, cv1k_u32 x, cv1k_u32 y, cv1k_u32 w, cv1k_u32 h, cv1k_u32 rgb);
void cv1k_video_text_rgb(struct cv1k_video *video, cv1k_u32 x, cv1k_u32 y, const char *text, cv1k_u32 rgb, cv1k_u32 scale);

#endif
