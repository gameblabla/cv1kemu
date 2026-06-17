#include "ui_sdl12.h"
#include "savestate.h"
#include "threaded_runtime.h"
#include <stdio.h>
#include <string.h>

#ifdef CV1K_WITH_SDL12
#include <SDL.h>
#endif

#ifndef CV1K_WITH_SDL12
int cv1k_ui_sdl12_run(struct cv1k_machine *m)
{
    CV1K_UNUSED(m);
    fprintf(stderr, "SDL 1.2 frontend was not compiled. Build with: make sdl12\n");
    return 0;
}
#else

#define CV1K_SDL12_SCALE 2U
#define CV1K_SDL12_AUDIO_RING_FRAMES 4096U
#define CV1K_SDL12_AUDIO_RING_MASK (CV1K_SDL12_AUDIO_RING_FRAMES - 1U)
#define CV1K_SDL12_AUDIO_TEMP_FRAMES 1024U
#define CV1K_SDL12_AUDIO_START_FRAMES 384U
#define CV1K_SDL12_AUDIO_TARGET_FRAMES 768U
#define CV1K_SDL12_AUDIO_MAX_LATENCY_FRAMES 1152U

struct cv1k_sdl12_audio {
    struct cv1k_machine *m;
    short ring[CV1K_SDL12_AUDIO_RING_FRAMES * 2U];
    cv1k_u32 read_pos;
    cv1k_u32 write_pos;
    cv1k_u32 freq;
    cv1k_u32 accum;
    cv1k_u32 underruns;
    cv1k_u32 overruns;
    int started;
};

static int key_to_ascii(SDLKey key)
{
    if (key >= SDLK_a && key <= SDLK_z) return (int)('a' + (key - SDLK_a));
    if (key >= SDLK_0 && key <= SDLK_9) return (int)('0' + (key - SDLK_0));
    if (key == SDLK_UP) return 'w';
    if (key == SDLK_DOWN) return 's';
    if (key == SDLK_LEFT) return 'a';
    if (key == SDLK_RIGHT) return 'd';
    if (key == SDLK_SPACE) return 'j';
    if (key == SDLK_LCTRL || key == SDLK_RCTRL) return 'k';
    if (key == SDLK_LALT || key == SDLK_RALT) return 'l';
    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) return 'i';
    return 0;
}

static void joystick_button(struct cv1k_machine *m, unsigned char button, int pressed)
{
    switch (button) {
    case 0: cv1k_input_event_key(&m->input, 'j', pressed); break;
    case 1: cv1k_input_event_key(&m->input, 'k', pressed); break;
    case 2: cv1k_input_event_key(&m->input, 'l', pressed); break;
    case 3: cv1k_input_event_key(&m->input, 'i', pressed); break;
    case 6: cv1k_input_event_key(&m->input, '5', pressed); break;
    case 7: cv1k_input_event_key(&m->input, '1', pressed); break;
    case 8: cv1k_input_event_key(&m->input, '5', pressed); break;
    case 9: cv1k_input_event_key(&m->input, '1', pressed); break;
    default: break;
    }
}

static void joystick_axis(struct cv1k_machine *m, unsigned char axis, short value)
{
    const short dead = 16000;
    if (axis == 0U) {
        cv1k_input_event_key(&m->input, 'a', value < -dead);
        cv1k_input_event_key(&m->input, 'd', value > dead);
    } else if (axis == 1U) {
        cv1k_input_event_key(&m->input, 'w', value < -dead);
        cv1k_input_event_key(&m->input, 's', value > dead);
    }
}

static void joystick_hat(struct cv1k_machine *m, unsigned char value)
{
    cv1k_input_event_key(&m->input, 'w', (value & SDL_HAT_UP) != 0);
    cv1k_input_event_key(&m->input, 's', (value & SDL_HAT_DOWN) != 0);
    cv1k_input_event_key(&m->input, 'a', (value & SDL_HAT_LEFT) != 0);
    cv1k_input_event_key(&m->input, 'd', (value & SDL_HAT_RIGHT) != 0);
}


static CV1K_ALWAYS_INLINE cv1k_u32 audio_ring_fill(const struct cv1k_sdl12_audio *a)
{
    return (a->write_pos - a->read_pos) & CV1K_SDL12_AUDIO_RING_MASK;
}

static void audio_trim_latency_locked(struct cv1k_sdl12_audio *a)
{
    cv1k_u32 fill;
    fill = audio_ring_fill(a);
    if (fill > CV1K_SDL12_AUDIO_MAX_LATENCY_FRAMES) {
        a->read_pos = (a->write_pos - CV1K_SDL12_AUDIO_TARGET_FRAMES) & CV1K_SDL12_AUDIO_RING_MASK;
        a->overruns++;
    }
}

static void sdl12_audio_callback(void *userdata, Uint8 *stream, int len)
{
    struct cv1k_sdl12_audio *a;
    short *dst;
    int frames;
    int i;
    a = (struct cv1k_sdl12_audio *)userdata;
    dst = (short *)stream;
    frames = len / (int)(sizeof(short) * 2U);
    for (i = 0; i < frames; i++) {
        if (a->read_pos != a->write_pos) {
            dst[i * 2 + 0] = a->ring[a->read_pos * 2U + 0U];
            dst[i * 2 + 1] = a->ring[a->read_pos * 2U + 1U];
            a->read_pos = (a->read_pos + 1U) & CV1K_SDL12_AUDIO_RING_MASK;
        } else {
            dst[i * 2 + 0] = 0;
            dst[i * 2 + 1] = 0;
            a->underruns++;
        }
    }
}

static void queue_audio_pcm(struct cv1k_sdl12_audio *a, const short *pcm, cv1k_u32 frames)
{
    cv1k_u32 i;
    cv1k_u32 next;
    if (a == NULL || pcm == NULL || frames == 0U) return;
    SDL_LockAudio();
    for (i = 0U; i < frames; i++) {
        next = (a->write_pos + 1U) & CV1K_SDL12_AUDIO_RING_MASK;
        if (next == a->read_pos) {
            a->read_pos = (a->read_pos + 1U) & CV1K_SDL12_AUDIO_RING_MASK;
            a->overruns++;
        }
        a->ring[a->write_pos * 2U + 0U] = pcm[i * 2U + 0U];
        a->ring[a->write_pos * 2U + 1U] = pcm[i * 2U + 1U];
        a->write_pos = next;
    }
    audio_trim_latency_locked(a);
    SDL_UnlockAudio();
}

static void queue_audio_frames(struct cv1k_sdl12_audio *a, cv1k_u32 frames)
{
    short temp[CV1K_SDL12_AUDIO_TEMP_FRAMES * 2U];
    cv1k_u32 todo;
    if (a == NULL || a->m == NULL) return;
    while (frames > 0U) {
        todo = frames;
        if (todo > CV1K_SDL12_AUDIO_TEMP_FRAMES) todo = CV1K_SDL12_AUDIO_TEMP_FRAMES;
        cv1k_ymz770_mix_s16_stereo(&a->m->ymz, a->m->sound_rom, a->m->sound_rom_size, temp, todo);
        queue_audio_pcm(a, temp, todo);
        frames -= todo;
    }
}

static cv1k_u32 audio_frame_count_for_next_video_frame(struct cv1k_sdl12_audio *a)
{
    cv1k_u32 frames;
    if (a == NULL || a->freq == 0U) return 0U;
    a->accum += a->freq * 1000U;
    frames = a->accum / CV1K_REFRESH_MILLIHZ;
    a->accum -= frames * CV1K_REFRESH_MILLIHZ;
    return frames;
}

static void queue_frame_audio(struct cv1k_sdl12_audio *a)
{
    queue_audio_frames(a, audio_frame_count_for_next_video_frame(a));
}

static CV1K_ALWAYS_INLINE Uint32 map_rgb32_fast(const SDL_PixelFormat *fmt, cv1k_u32 rgb)
{
    Uint32 r;
    Uint32 g;
    Uint32 b;
    r = (Uint32)((rgb >> 16) & 0xffU);
    g = (Uint32)((rgb >> 8) & 0xffU);
    b = (Uint32)(rgb & 0xffU);
    return (Uint32)((((r >> fmt->Rloss) << fmt->Rshift) & fmt->Rmask) |
                    (((g >> fmt->Gloss) << fmt->Gshift) & fmt->Gmask) |
                    (((b >> fmt->Bloss) << fmt->Bshift) & fmt->Bmask));
}

static void put_pixel(SDL_Surface *s, unsigned char *p, cv1k_u32 rgb)
{
    Uint32 mapped;
    mapped = SDL_MapRGB(s->format, (Uint8)((rgb >> 16) & 0xffU), (Uint8)((rgb >> 8) & 0xffU), (Uint8)(rgb & 0xffU));
    switch (s->format->BytesPerPixel) {
    case 2:
        *(Uint16 *)p = (Uint16)mapped;
        break;
    case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        p[0] = (unsigned char)((mapped >> 16) & 0xffU);
        p[1] = (unsigned char)((mapped >> 8) & 0xffU);
        p[2] = (unsigned char)(mapped & 0xffU);
#else
        p[0] = (unsigned char)(mapped & 0xffU);
        p[1] = (unsigned char)((mapped >> 8) & 0xffU);
        p[2] = (unsigned char)((mapped >> 16) & 0xffU);
#endif
        break;
    default:
        *(Uint32 *)p = mapped;
        break;
    }
}

static void present_video32_scaled2(SDL_Surface *screen, const struct cv1k_video *video, int rotation)
{
    cv1k_u32 dx;
    cv1k_u32 dy;
    cv1k_u32 dst_w;
    cv1k_u32 dst_h;
    unsigned char *base;
    const SDL_PixelFormat *fmt;
    if (video == NULL || video->screen_rgb == NULL) return;
    if (rotation == CV1K_DISPLAY_ROT_AUTO) rotation = CV1K_DISPLAY_ROT_CCW;
    cv1k_video_display_dimensions(rotation, &dst_w, &dst_h);
    base = (unsigned char *)screen->pixels;
    fmt = screen->format;
#define PUT2X2(rgb_expr) do { \
        Uint32 mapped = map_rgb32_fast(fmt, (rgb_expr)); \
        cv1k_u32 x2 = dx * 2U; \
        dst0[x2 + 0U] = mapped; \
        dst0[x2 + 1U] = mapped; \
        dst1[x2 + 0U] = mapped; \
        dst1[x2 + 1U] = mapped; \
    } while (0)
    switch (rotation) {
    case CV1K_DISPLAY_ROT_0:
        for (dy = 0U; dy < CV1K_SCREEN_H; dy++) {
            const cv1k_u32 *src = video->screen_rgb + dy * CV1K_FRAMEBUFFER_W;
            Uint32 *dst0 = (Uint32 *)(void *)(base + (dy * 2U + 0U) * (cv1k_u32)screen->pitch);
            Uint32 *dst1 = (Uint32 *)(void *)(base + (dy * 2U + 1U) * (cv1k_u32)screen->pitch);
            for (dx = 0U; dx < CV1K_SCREEN_W; dx++) { PUT2X2(src[dx]); }
        }
        break;
    case CV1K_DISPLAY_ROT_CW:
        for (dy = 0U; dy < CV1K_SCREEN_W; dy++) {
            Uint32 *dst0 = (Uint32 *)(void *)(base + (dy * 2U + 0U) * (cv1k_u32)screen->pitch);
            Uint32 *dst1 = (Uint32 *)(void *)(base + (dy * 2U + 1U) * (cv1k_u32)screen->pitch);
            for (dx = 0U; dx < CV1K_SCREEN_H; dx++) { PUT2X2(video->screen_rgb[((CV1K_SCREEN_H - 1U) - dx) * CV1K_FRAMEBUFFER_W + dy]); }
        }
        break;
    case CV1K_DISPLAY_ROT_180:
        for (dy = 0U; dy < CV1K_SCREEN_H; dy++) {
            const cv1k_u32 *src = video->screen_rgb + ((CV1K_SCREEN_H - 1U) - dy) * CV1K_FRAMEBUFFER_W;
            Uint32 *dst0 = (Uint32 *)(void *)(base + (dy * 2U + 0U) * (cv1k_u32)screen->pitch);
            Uint32 *dst1 = (Uint32 *)(void *)(base + (dy * 2U + 1U) * (cv1k_u32)screen->pitch);
            for (dx = 0U; dx < CV1K_SCREEN_W; dx++) { PUT2X2(src[(CV1K_SCREEN_W - 1U) - dx]); }
        }
        break;
    case CV1K_DISPLAY_ROT_CCW:
    default:
        for (dy = 0U; dy < CV1K_SCREEN_W; dy++) {
            cv1k_u32 sx = (CV1K_SCREEN_W - 1U) - dy;
            Uint32 *dst0 = (Uint32 *)(void *)(base + (dy * 2U + 0U) * (cv1k_u32)screen->pitch);
            Uint32 *dst1 = (Uint32 *)(void *)(base + (dy * 2U + 1U) * (cv1k_u32)screen->pitch);
            for (dx = 0U; dx < CV1K_SCREEN_H; dx++) { PUT2X2(video->screen_rgb[dx * CV1K_FRAMEBUFFER_W + sx]); }
        }
        break;
    }
#undef PUT2X2
}

static void present_video(SDL_Surface *screen, const struct cv1k_video *video, int rotation)
{
    cv1k_u32 dx;
    cv1k_u32 dy;
    cv1k_u32 sx;
    cv1k_u32 sy;
    cv1k_u32 dst_w;
    cv1k_u32 dst_h;
    unsigned char *base;
    int bpp;
    if (screen == NULL || video == NULL || video->screen_rgb == NULL) return;
    cv1k_video_display_dimensions(rotation, &dst_w, &dst_h);
    if (SDL_MUSTLOCK(screen) && SDL_LockSurface(screen) != 0) return;
    if (screen->format != NULL && screen->format->BytesPerPixel == 4 && CV1K_SDL12_SCALE == 2U) {
        present_video32_scaled2(screen, video, rotation);
        if (SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);
        SDL_Flip(screen);
        return;
    }
    base = (unsigned char *)screen->pixels;
    bpp = screen->format->BytesPerPixel;
    for (dy = 0U; dy < dst_h; dy++) {
        for (sy = 0U; sy < CV1K_SDL12_SCALE; sy++) {
            unsigned char *dstrow = base + (dy * CV1K_SDL12_SCALE + sy) * (cv1k_u32)screen->pitch;
            for (dx = 0U; dx < dst_w; dx++) {
                cv1k_u32 rgb = cv1k_video_display_pixel(video, rotation, dx, dy);
                for (sx = 0U; sx < CV1K_SDL12_SCALE; sx++) {
                    put_pixel(screen, dstrow + (dx * CV1K_SDL12_SCALE + sx) * (cv1k_u32)bpp, rgb);
                }
            }
        }
    }
    if (SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);
    SDL_Flip(screen);
}

int cv1k_ui_sdl12_run(struct cv1k_machine *m)
{
    SDL_Surface *screen;
    SDL_Event e;
    SDL_AudioSpec want;
    SDL_Joystick *joys[4];
    struct cv1k_sdl12_audio audio;
    struct cv1k_mt_render *mt_render;
    struct cv1k_mt_audio *mt_audio;
    int mt_render_active;
    int mt_audio_active;
    int joy_count;
    int opened;
    int running;
    int i;
    cv1k_u32 display_w;
    cv1k_u32 display_h;

    if (m == NULL) return 0;
    cv1k_video_display_dimensions(m->display_rotation, &display_w, &display_h);
    screen = NULL;
    for (i = 0; i < 4; i++) joys[i] = NULL;
    opened = 0;
    memset(&audio, 0, sizeof(audio));
    mt_render = NULL;
    mt_audio = NULL;
    mt_render_active = 0;
    mt_audio_active = 0;
    audio.m = m;
    audio.freq = CV1K_YMZ770_CLOCK_HZ / 1024U;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_WM_SetCaption("CV1000 sandbox - SDL 1.2", NULL);
    SDL_EnableKeyRepeat(0, 0);
    SDL_ShowCursor(SDL_DISABLE);

    screen = SDL_SetVideoMode((int)(display_w * CV1K_SDL12_SCALE),
                              (int)(display_h * CV1K_SDL12_SCALE),
                              32,
                              SDL_SWSURFACE | SDL_DOUBLEBUF);
    if (screen == NULL) {
        fprintf(stderr, "SDL_SetVideoMode failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }

    memset(&want, 0, sizeof(want));
    want.freq = (int)audio.freq;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 256;
    want.callback = sdl12_audio_callback;
    want.userdata = &audio;
    /* Pass NULL for the obtained spec so SDL 1.2 converts to the requested
     * 16 kHz S16 stereo stream if the host device needs a different native
     * format.  The YMZ770 mixer itself is clocked at CV1K_YMZ770_CLOCK_HZ/1024.
     */
    if (SDL_OpenAudio(&want, NULL) != 0) {
        fprintf(stderr, "warning: SDL_OpenAudio failed: %s\n", SDL_GetError());
        audio.freq = 0U;
    }

    if (m->threaded_render && m->render_screen) {
        mt_render = cv1k_mt_render_create();
        mt_render_active = (mt_render != NULL);
        if (!mt_render_active) fprintf(stderr, "warning: threaded render worker unavailable; using single-threaded render path\n");
    }
    if (m->threaded_audio && audio.freq != 0U) {
        mt_audio = cv1k_mt_audio_create(CV1K_SDL12_AUDIO_TEMP_FRAMES);
        mt_audio_active = (mt_audio != NULL);
        if (!mt_audio_active) fprintf(stderr, "warning: threaded audio worker unavailable; using single-threaded audio path\n");
    }

    joy_count = SDL_NumJoysticks();
    for (i = 0; i < joy_count && opened < 4; i++) {
        joys[opened] = SDL_JoystickOpen(i);
        if (joys[opened] != NULL) opened++;
    }
    if (opened > 0) SDL_JoystickEventState(SDL_ENABLE);

    running = 1;
    {
    unsigned long long next_frame = (unsigned long long)SDL_GetTicks() * (unsigned long long)CV1K_REFRESH_MILLIHZ;
    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                int pressed;
                int ascii;
                pressed = (e.type == SDL_KEYDOWN);
                if (e.key.keysym.sym == SDLK_ESCAPE && pressed) running = 0;
                else if (e.key.keysym.sym == SDLK_F5 && pressed) cv1k_save_state(m, "quick.sav");
                else if (e.key.keysym.sym == SDLK_F8 && pressed) cv1k_load_state(m, "quick.sav");
                ascii = key_to_ascii(e.key.keysym.sym);
                if (ascii != 0) cv1k_input_event_key(&m->input, ascii, pressed);
            } else if (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) {
                joystick_button(m, e.jbutton.button, e.type == SDL_JOYBUTTONDOWN);
            } else if (e.type == SDL_JOYAXISMOTION) {
                joystick_axis(m, e.jaxis.axis, e.jaxis.value);
            } else if (e.type == SDL_JOYHATMOTION) {
                joystick_hat(m, e.jhat.value);
            }
        }

        {
            cv1k_u32 audio_frames = 0U;
            int audio_pending = 0;
            cv1k_machine_frame_advance(m, mt_render_active ? 0 : m->render_screen);
            if (mt_render_active) {
                if (cv1k_mt_render_submit(mt_render, &m->video)) {
                    m->threaded_render_jobs++;
                } else {
                    cv1k_video_frame(&m->video, m->main_ram, m->main_ram_size);
                }
            }
            audio_frames = audio_frame_count_for_next_video_frame(&audio);
            if (mt_audio_active && audio_frames != 0U && cv1k_mt_audio_submit(mt_audio, &m->ymz, m->sound_rom, m->sound_rom_size, audio_frames)) {
                audio_pending = 1;
                m->threaded_audio_jobs++;
            } else {
                queue_audio_frames(&audio, audio_frames);
            }
            if (mt_render_active) cv1k_mt_render_wait(mt_render, &m->video);
            present_video(screen, &m->video, m->display_rotation);
            if (audio_pending) {
                const short *pcm = NULL;
                cv1k_u32 got = 0U;
                if (cv1k_mt_audio_wait(mt_audio, &pcm, &got) && pcm != NULL) queue_audio_pcm(&audio, pcm, got);
            }
            if (audio.freq != 0U && !audio.started) {
                SDL_LockAudio();
                if (audio_ring_fill(&audio) >= CV1K_SDL12_AUDIO_START_FRAMES) {
                    audio.started = 1;
                    SDL_PauseAudio(0);
                }
                SDL_UnlockAudio();
            }
        }

        /* Pace to the CV1000 refresh (~60.024 Hz).  The older integer
         * 1000/60 delay ran at 62.5 Hz, which slowly accumulated audio backlog
         * and made voice samples appear late.  Use a rational millisecond
         * deadline so the sleep cadence alternates 16/17 ms around 16.660 ms.
         */
        next_frame += 1000000ULL;
        {
            unsigned long long now_scaled = (unsigned long long)SDL_GetTicks() * (unsigned long long)CV1K_REFRESH_MILLIHZ;
            if (next_frame > now_scaled) {
                unsigned long long delay_scaled = next_frame - now_scaled;
                Uint32 delay_ms = (Uint32)(delay_scaled / (unsigned long long)CV1K_REFRESH_MILLIHZ);
                if (delay_ms != 0U) SDL_Delay(delay_ms);
            } else {
                next_frame = now_scaled;   /* fell behind: resync, don't spiral */
            }
        }
    }
    }

    cv1k_mt_audio_destroy(mt_audio);
    cv1k_mt_render_destroy(mt_render);
    if (audio.freq != 0U) SDL_CloseAudio();
    for (i = 0; i < opened; i++) if (joys[i] != NULL) SDL_JoystickClose(joys[i]);
    SDL_ShowCursor(SDL_ENABLE);
    SDL_Quit();
    return 1;
}
#endif
