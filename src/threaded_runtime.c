#include "threaded_runtime.h"
#include "cv1k_config.h"
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>

#ifdef CV1K_ENABLE_THREADS
#include <threads.h>
#endif

#ifdef CV1K_ENABLE_THREADS

struct cv1k_mt_render {
    thrd_t thread;
    mtx_t mutex;
    cnd_t cond;
    int started;
    int stop;
    int job_pending;
    int job_done;
    int busy;
    cv1k_u16 *snapshot;
    cv1k_u32 *dst_rgb;
    cv1k_u32 frame_nonzero;
    cv1k_u32 done_nonzero;
};

static alignas(CV1K_CACHE_ALIGN) cv1k_u32 mt_rgb1555_table[65536];
static once_flag mt_rgb_once = ONCE_FLAG_INIT;

static void mt_build_rgb1555_table(void)
{
    cv1k_u32 i;
    for (i = 0U; i < 65536U; i++) {
        cv1k_u32 r = (i >> 10) & 0x1fU;
        cv1k_u32 g = (i >> 5) & 0x1fU;
        cv1k_u32 b = i & 0x1fU;
        mt_rgb1555_table[i] = (r << 19) | (g << 11) | (b << 3);
    }
}

static CV1K_ALWAYS_INLINE cv1k_u32 mt_rgb1555_to_rgb888(cv1k_u16 p)
{
    return mt_rgb1555_table[p];
}

static int render_thread_main(void *arg)
{
    struct cv1k_mt_render *r = (struct cv1k_mt_render *)arg;
    call_once(&mt_rgb_once, mt_build_rgb1555_table);
    for (;;) {
        cv1k_u16 *src;
        cv1k_u32 *dst;
        cv1k_u32 nonzero;
        cv1k_u32 y;

        mtx_lock(&r->mutex);
        while (!r->stop && !r->job_pending) cnd_wait(&r->cond, &r->mutex);
        if (r->stop) {
            mtx_unlock(&r->mutex);
            break;
        }
        src = r->snapshot;
        dst = r->dst_rgb;
        nonzero = r->frame_nonzero;
        r->job_pending = 0;
        r->busy = 1;
        mtx_unlock(&r->mutex);

        if (src != NULL && dst != NULL) {
            for (y = 0U; y < CV1K_SCREEN_H; y++) {
                cv1k_u32 x;
                cv1k_u32 *dstrow = dst + y * CV1K_FRAMEBUFFER_W;
                const cv1k_u16 *srcrow = src + y * CV1K_SCREEN_W;
                for (x = 0U; x < CV1K_SCREEN_W; x++) dstrow[x] = mt_rgb1555_to_rgb888(srcrow[x]);
            }
        }

        mtx_lock(&r->mutex);
        r->done_nonzero = nonzero;
        r->busy = 0;
        r->job_done = 1;
        cnd_broadcast(&r->cond);
        mtx_unlock(&r->mutex);
    }
    return 0;
}

int cv1k_mt_supported(void)
{
    return 1;
}

struct cv1k_mt_render *cv1k_mt_render_create(void)
{
    struct cv1k_mt_render *r;
    r = (struct cv1k_mt_render *)calloc(1U, sizeof(*r));
    if (r == NULL) return NULL;
    r->snapshot = (cv1k_u16 *)malloc((size_t)CV1K_SCREEN_W * (size_t)CV1K_SCREEN_H * sizeof(cv1k_u16));
    if (r->snapshot == NULL) {
        free(r);
        return NULL;
    }
    if (mtx_init(&r->mutex, mtx_plain) != thrd_success) {
        free(r->snapshot);
        free(r);
        return NULL;
    }
    if (cnd_init(&r->cond) != thrd_success) {
        mtx_destroy(&r->mutex);
        free(r->snapshot);
        free(r);
        return NULL;
    }
    if (thrd_create(&r->thread, render_thread_main, r) != thrd_success) {
        cnd_destroy(&r->cond);
        mtx_destroy(&r->mutex);
        free(r->snapshot);
        free(r);
        return NULL;
    }
    r->started = 1;
    return r;
}

void cv1k_mt_render_destroy(struct cv1k_mt_render *r)
{
    if (r == NULL) return;
    if (r->started) {
        mtx_lock(&r->mutex);
        r->stop = 1;
        cnd_broadcast(&r->cond);
        mtx_unlock(&r->mutex);
        thrd_join(r->thread, NULL);
    }
    cnd_destroy(&r->cond);
    mtx_destroy(&r->mutex);
    free(r->snapshot);
    free(r);
}

void cv1k_mt_render_wait(struct cv1k_mt_render *r, struct cv1k_video *video)
{
    if (r == NULL) return;
    mtx_lock(&r->mutex);
    while (!r->job_done && (r->job_pending || r->busy)) cnd_wait(&r->cond, &r->mutex);
    if (r->job_done) {
        if (video != NULL) video->last_frame_nonzero = r->done_nonzero;
        r->job_done = 0;
    }
    mtx_unlock(&r->mutex);
}

int cv1k_mt_render_submit(struct cv1k_mt_render *r, struct cv1k_video *video)
{
    cv1k_u32 y;
    cv1k_u32 frame_nonzero;
    if (r == NULL || video == NULL || video->vram1555 == NULL || video->screen_rgb == NULL) return 0;
    if (video->executed_ops == 0UL) return 0;

    cv1k_mt_render_wait(r, video);

    video->frame_counter++;
    frame_nonzero = 0UL;
    for (y = 0U; y < CV1K_SCREEN_H; y++) {
        cv1k_u32 x;
        cv1k_u32 sy = (y + (video->gfx_scroll_y & (CV1K_VRAM_H - 1UL))) & (CV1K_VRAM_H - 1UL);
        cv1k_u32 sx = video->gfx_scroll_x & (CV1K_VRAM_W - 1UL);
        cv1k_u16 *dstrow = r->snapshot + y * CV1K_SCREEN_W;
        if (CV1K_LIKELY(sx + CV1K_SCREEN_W <= CV1K_VRAM_W)) {
            const cv1k_u16 *srcrow = video->vram1555 + sy * CV1K_VRAM_W + sx;
            memcpy(dstrow, srcrow, (size_t)CV1K_SCREEN_W * sizeof(cv1k_u16));
            for (x = 0U; x < CV1K_SCREEN_W; x++) frame_nonzero += ((dstrow[x] & 0x7fffU) != 0U);
        } else {
            for (x = 0U; x < CV1K_SCREEN_W; x++) {
                cv1k_u32 sxw = (sx + x) & (CV1K_VRAM_W - 1UL);
                cv1k_u16 pix = video->vram1555[sy * CV1K_VRAM_W + sxw];
                dstrow[x] = pix;
                frame_nonzero += ((pix & 0x7fffU) != 0U);
            }
        }
    }

    mtx_lock(&r->mutex);
    r->dst_rgb = video->screen_rgb;
    r->frame_nonzero = frame_nonzero;
    r->job_done = 0;
    r->busy = 0;
    r->job_pending = 1;
    cnd_broadcast(&r->cond);
    mtx_unlock(&r->mutex);
    return 1;
}

struct cv1k_mt_audio {
    thrd_t thread;
    mtx_t mutex;
    cnd_t cond;
    int started;
    int stop;
    int job_pending;
    int job_done;
    struct cv1k_ymz770 *ymz;
    const cv1k_u8 *rom;
    cv1k_u32 rom_size;
    cv1k_u32 frames;
    cv1k_u32 done_frames;
    cv1k_u32 max_frames;
    short *pcm;
};

static int audio_thread_main(void *arg)
{
    struct cv1k_mt_audio *a = (struct cv1k_mt_audio *)arg;
    for (;;) {
        struct cv1k_ymz770 *ymz;
        const cv1k_u8 *rom;
        cv1k_u32 rom_size;
        cv1k_u32 frames;
        mtx_lock(&a->mutex);
        while (!a->stop && !a->job_pending) cnd_wait(&a->cond, &a->mutex);
        if (a->stop) {
            mtx_unlock(&a->mutex);
            break;
        }
        ymz = a->ymz;
        rom = a->rom;
        rom_size = a->rom_size;
        frames = a->frames;
        a->job_pending = 0;
        mtx_unlock(&a->mutex);

        if (frames != 0U) cv1k_ymz770_mix_s16_stereo(ymz, rom, rom_size, a->pcm, frames);

        mtx_lock(&a->mutex);
        a->done_frames = frames;
        a->job_done = 1;
        cnd_broadcast(&a->cond);
        mtx_unlock(&a->mutex);
    }
    return 0;
}

struct cv1k_mt_audio *cv1k_mt_audio_create(cv1k_u32 max_frames)
{
    struct cv1k_mt_audio *a;
    if (max_frames == 0U) max_frames = 1024U;
    a = (struct cv1k_mt_audio *)calloc(1U, sizeof(*a));
    if (a == NULL) return NULL;
    a->pcm = (short *)malloc((size_t)max_frames * 2U * sizeof(short));
    if (a->pcm == NULL) {
        free(a);
        return NULL;
    }
    a->max_frames = max_frames;
    if (mtx_init(&a->mutex, mtx_plain) != thrd_success) {
        free(a->pcm);
        free(a);
        return NULL;
    }
    if (cnd_init(&a->cond) != thrd_success) {
        mtx_destroy(&a->mutex);
        free(a->pcm);
        free(a);
        return NULL;
    }
    if (thrd_create(&a->thread, audio_thread_main, a) != thrd_success) {
        cnd_destroy(&a->cond);
        mtx_destroy(&a->mutex);
        free(a->pcm);
        free(a);
        return NULL;
    }
    a->started = 1;
    return a;
}

void cv1k_mt_audio_destroy(struct cv1k_mt_audio *a)
{
    if (a == NULL) return;
    if (a->started) {
        mtx_lock(&a->mutex);
        a->stop = 1;
        cnd_broadcast(&a->cond);
        mtx_unlock(&a->mutex);
        thrd_join(a->thread, NULL);
    }
    cnd_destroy(&a->cond);
    mtx_destroy(&a->mutex);
    free(a->pcm);
    free(a);
}

int cv1k_mt_audio_submit(struct cv1k_mt_audio *a,
                         struct cv1k_ymz770 *ymz,
                         const cv1k_u8 *rom,
                         cv1k_u32 rom_size,
                         cv1k_u32 frames)
{
    if (a == NULL || ymz == NULL || frames == 0U || frames > a->max_frames) return 0;
    (void)cv1k_mt_audio_wait(a, NULL, NULL);
    mtx_lock(&a->mutex);
    a->ymz = ymz;
    a->rom = rom;
    a->rom_size = rom_size;
    a->frames = frames;
    a->done_frames = 0U;
    a->job_done = 0;
    a->job_pending = 1;
    cnd_broadcast(&a->cond);
    mtx_unlock(&a->mutex);
    return 1;
}

int cv1k_mt_audio_wait(struct cv1k_mt_audio *a, const short **pcm, cv1k_u32 *frames)
{
    int had;
    if (a == NULL) return 0;
    mtx_lock(&a->mutex);
    while (a->job_pending && !a->job_done) cnd_wait(&a->cond, &a->mutex);
    while (!a->job_done && a->frames != 0U) cnd_wait(&a->cond, &a->mutex);
    had = a->job_done;
    if (had) {
        if (pcm != NULL) *pcm = a->pcm;
        if (frames != NULL) *frames = a->done_frames;
        a->job_done = 0;
        a->frames = 0U;
    }
    mtx_unlock(&a->mutex);
    return had;
}

#else

int cv1k_mt_supported(void) { return 0; }
struct cv1k_mt_render *cv1k_mt_render_create(void) { return NULL; }
void cv1k_mt_render_destroy(struct cv1k_mt_render *r) { CV1K_UNUSED(r); }
int cv1k_mt_render_submit(struct cv1k_mt_render *r, struct cv1k_video *video) { CV1K_UNUSED(r); CV1K_UNUSED(video); return 0; }
void cv1k_mt_render_wait(struct cv1k_mt_render *r, struct cv1k_video *video) { CV1K_UNUSED(r); CV1K_UNUSED(video); }
struct cv1k_mt_audio *cv1k_mt_audio_create(cv1k_u32 max_frames) { CV1K_UNUSED(max_frames); return NULL; }
void cv1k_mt_audio_destroy(struct cv1k_mt_audio *a) { CV1K_UNUSED(a); }
int cv1k_mt_audio_submit(struct cv1k_mt_audio *a, struct cv1k_ymz770 *ymz, const cv1k_u8 *rom, cv1k_u32 rom_size, cv1k_u32 frames) { CV1K_UNUSED(a); CV1K_UNUSED(ymz); CV1K_UNUSED(rom); CV1K_UNUSED(rom_size); CV1K_UNUSED(frames); return 0; }
int cv1k_mt_audio_wait(struct cv1k_mt_audio *a, const short **pcm, cv1k_u32 *frames) { CV1K_UNUSED(a); if (pcm != NULL) *pcm = NULL; if (frames != NULL) *frames = 0U; return 0; }

#endif
