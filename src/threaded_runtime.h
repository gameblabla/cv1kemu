#ifndef CV1K_THREADED_RUNTIME_H
#define CV1K_THREADED_RUNTIME_H

#include "cv1k_types.h"
#include "video.h"
#include "sound_ymz770.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cv1k_mt_render;
struct cv1k_mt_audio;

int cv1k_mt_supported(void);

struct cv1k_mt_render *cv1k_mt_render_create(void);
void cv1k_mt_render_destroy(struct cv1k_mt_render *r);
int cv1k_mt_render_submit(struct cv1k_mt_render *r, struct cv1k_video *video);
void cv1k_mt_render_wait(struct cv1k_mt_render *r, struct cv1k_video *video);

struct cv1k_mt_audio *cv1k_mt_audio_create(cv1k_u32 max_frames);
void cv1k_mt_audio_destroy(struct cv1k_mt_audio *a);
int cv1k_mt_audio_submit(struct cv1k_mt_audio *a,
                         struct cv1k_ymz770 *ymz,
                         const cv1k_u8 *rom,
                         cv1k_u32 rom_size,
                         cv1k_u32 frames);
int cv1k_mt_audio_wait(struct cv1k_mt_audio *a, const short **pcm, cv1k_u32 *frames);

#ifdef __cplusplus
}
#endif

#endif
