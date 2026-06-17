#ifndef CV1K_FRONTEND_H
#define CV1K_FRONTEND_H

#include "emu.h"

/* Shared frontend button mask.  Bits intentionally match enum cv1k_input_id.
 * This gives browser, Win64, SDL, and scripted inputs one common namespace:
 * p1_up..p1_b4, p1_start, p2_up..p2_b4, coin1, coin2, p2_start,
 * service1..service3.
 */
#define CV1K_FRONTEND_BUTTON(id) (1UL << (id))
#define CV1K_FRONTEND_AUDIO_MAX_FRAMES 8192U

void cv1k_frontend_apply_input_mask(struct cv1k_input *input, cv1k_u32 mask);
cv1k_u32 cv1k_frontend_button_mask_from_name(const char *name);
cv1k_u32 cv1k_frontend_audio_frames_for_video(cv1k_u32 sample_rate, cv1k_u32 *accum);
void cv1k_frontend_make_rgba8888(const struct cv1k_video *video, int rotation, cv1k_u8 *dst_rgba, cv1k_u32 dst_pitch_bytes);
void cv1k_frontend_machine_defaults(struct cv1k_machine *m);

#endif
