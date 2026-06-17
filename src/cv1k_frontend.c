#include "cv1k_frontend.h"
#include "video.h"
#include "sh3_jit/cv1k_ir.h"
#include "sh3_jit/cv1k_sh3_c23_jit.h"
#include <string.h>

void cv1k_frontend_apply_input_mask(struct cv1k_input *input, cv1k_u32 mask)
{
    int i;
    if (input == NULL) return;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) input->state[i] = (cv1k_u8)((mask >> i) & 1U);
}

cv1k_u32 cv1k_frontend_button_mask_from_name(const char *name)
{
    int id;
    id = cv1k_input_id_from_name(name);
    if (id < 0 || id >= 32) return 0UL;
    return 1UL << id;
}

cv1k_u32 cv1k_frontend_audio_frames_for_video(cv1k_u32 sample_rate, cv1k_u32 *accum)
{
    cv1k_u32 frames;
    if (sample_rate == 0UL || accum == NULL) return 0UL;
    *accum += sample_rate * 1000UL;
    frames = *accum / CV1K_REFRESH_MILLIHZ;
    *accum -= frames * CV1K_REFRESH_MILLIHZ;
    return frames;
}

static CV1K_ALWAYS_INLINE cv1k_u32 rgba_word_from_rgb888(cv1k_u32 p)
{
    /* ImageData wants byte order R,G,B,A.  WebAssembly is little-endian, so the
     * 32-bit value stored in memory must be 0xAABBGGRR. */
    return 0xff000000U | ((p & 0x0000ffU) << 16U) | (p & 0x00ff00U) | ((p >> 16U) & 0x0000ffU);
}

void cv1k_frontend_make_rgba8888(const struct cv1k_video *video, int rotation, cv1k_u8 *dst_rgba, cv1k_u32 dst_pitch_bytes)
{
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_u32 w;
    cv1k_u32 h;
    cv1k_u32 dst_pitch;
    cv1k_u32 *dst;
    const cv1k_u32 *src;
    if (video == NULL || video->screen_rgb == NULL || dst_rgba == NULL) return;
    cv1k_video_display_dimensions(rotation, &w, &h);
    if (dst_pitch_bytes < w * 4U) return;
    dst_pitch = dst_pitch_bytes / 4U;
    dst = (cv1k_u32 *)(void *)dst_rgba;
    if (rotation == CV1K_DISPLAY_ROT_AUTO) rotation = CV1K_DISPLAY_ROT_CCW;
    switch (rotation) {
    case CV1K_DISPLAY_ROT_0:
        for (y = 0U; y < CV1K_SCREEN_H; y++) {
            const cv1k_u32 *s = video->screen_rgb + y * CV1K_FRAMEBUFFER_W;
            cv1k_u32 *d = dst + y * dst_pitch;
            for (x = 0U; x < CV1K_SCREEN_W; x++) d[x] = rgba_word_from_rgb888(s[x]);
        }
        break;
    case CV1K_DISPLAY_ROT_CW:
        for (y = 0U; y < CV1K_SCREEN_W; y++) {
            cv1k_u32 *d = dst + y * dst_pitch;
            for (x = 0U; x < CV1K_SCREEN_H; x++) {
                src = video->screen_rgb + ((CV1K_SCREEN_H - 1U) - x) * CV1K_FRAMEBUFFER_W + y;
                d[x] = rgba_word_from_rgb888(*src);
            }
        }
        break;
    case CV1K_DISPLAY_ROT_180:
        for (y = 0U; y < CV1K_SCREEN_H; y++) {
            const cv1k_u32 *s = video->screen_rgb + ((CV1K_SCREEN_H - 1U) - y) * CV1K_FRAMEBUFFER_W;
            cv1k_u32 *d = dst + y * dst_pitch;
            for (x = 0U; x < CV1K_SCREEN_W; x++) d[x] = rgba_word_from_rgb888(s[(CV1K_SCREEN_W - 1U) - x]);
        }
        break;
    case CV1K_DISPLAY_ROT_CCW:
    default:
        for (y = 0U; y < CV1K_SCREEN_W; y++) {
            cv1k_u32 sx = (CV1K_SCREEN_W - 1U) - y;
            cv1k_u32 *d = dst + y * dst_pitch;
            for (x = 0U; x < CV1K_SCREEN_H; x++) {
                src = video->screen_rgb + x * CV1K_FRAMEBUFFER_W + sx;
                d[x] = rgba_word_from_rgb888(*src);
            }
        }
        break;
    }
}

void cv1k_frontend_machine_defaults(struct cv1k_machine *m)
{
    if (m == NULL) return;
    m->irq2_enabled = 1;
    m->mame_speedup = 1;
    m->mame_tmu_irq = 1;
    m->tmu_irq_defer_frames = 0UL;
    m->render_screen = 1;
    m->display_rotation = CV1K_DISPLAY_ROT_CCW;
    m->video_renderer = CV1K_VIDEO_RENDERER_SOFTWARE;
    m->gles2_tile_cache = 0;
    m->gles2_gpu_blitter = 0;
    m->threaded_render = 0;
    m->threaded_audio = 0;
    m->ir_jit = 0;
    cv1k_ir_enable(0);
    sh7709s_c23jit_enable(0);
}
