#ifndef CV1K_VIDEO_GLES2_H
#define CV1K_VIDEO_GLES2_H

#include "cv1k_types.h"
#include "video.h"

struct cv1k_gles2_renderer;

typedef void *(*cv1k_gles2_getproc_fn)(const char *name, void *userdata);

struct cv1k_gles2_renderer *cv1k_gles2_renderer_create(cv1k_gles2_getproc_fn getproc, void *userdata, cv1k_u32 display_w, cv1k_u32 display_h, int use_tile_cache);
void cv1k_gles2_renderer_set_gpu_blitter(struct cv1k_gles2_renderer *r, struct cv1k_video *video, int enable);
void cv1k_gles2_renderer_destroy(struct cv1k_gles2_renderer *r);
int cv1k_gles2_renderer_present(struct cv1k_gles2_renderer *r, const struct cv1k_video *video, int rotation, cv1k_u32 window_w, cv1k_u32 window_h);
const char *cv1k_gles2_renderer_error(const struct cv1k_gles2_renderer *r);
cv1k_u32 cv1k_gles2_renderer_tile_uploads(const struct cv1k_gles2_renderer *r);
cv1k_u32 cv1k_gles2_renderer_tiles_allocated(const struct cv1k_gles2_renderer *r);
cv1k_u32 cv1k_gles2_renderer_gpu_uploads(const struct cv1k_gles2_renderer *r);
cv1k_u32 cv1k_gles2_renderer_gpu_draws(const struct cv1k_gles2_renderer *r);
cv1k_u32 cv1k_gles2_renderer_gpu_fallbacks(const struct cv1k_gles2_renderer *r);

#endif
