#include "ui_sdl3.h"
#include "savestate.h"
#include "threaded_runtime.h"
#include "video_gles2.h"
#include <stdio.h>
#include <string.h>
#ifdef CV1K_WITH_SDL3
#include <SDL3/SDL.h>
#endif

#ifndef CV1K_WITH_SDL3
int cv1k_ui_sdl3_run(struct cv1k_machine *m)
{
    CV1K_UNUSED(m);
    fprintf(stderr, "SDL3 frontend was not compiled. Build with: make sdl3\n");
    return 0;
}
#else

static void *sdl3_get_gl_proc(const char *name, void *userdata)
{
    CV1K_UNUSED(userdata);
    return (void *)SDL_GL_GetProcAddress(name);
}

static int key_to_ascii(SDL_Keycode key)
{
    if (key >= 'a' && key <= 'z') return (int)key;
    if (key >= '0' && key <= '9') return (int)key;
    if (key >= 'A' && key <= 'Z') return (int)(key + ('a' - 'A'));
    if (key == SDLK_UP) return 'w';
    if (key == SDLK_DOWN) return 's';
    if (key == SDLK_LEFT) return 'a';
    if (key == SDLK_RIGHT) return 'd';
    return 0;
}

static void gamepad_button(struct cv1k_machine *m, unsigned char button, int pressed)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: cv1k_input_event_key(&m->input, 'w', pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: cv1k_input_event_key(&m->input, 's', pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: cv1k_input_event_key(&m->input, 'a', pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: cv1k_input_event_key(&m->input, 'd', pressed); break;
    case SDL_GAMEPAD_BUTTON_SOUTH: cv1k_input_event_key(&m->input, 'j', pressed); break;
    case SDL_GAMEPAD_BUTTON_EAST: cv1k_input_event_key(&m->input, 'k', pressed); break;
    case SDL_GAMEPAD_BUTTON_WEST: cv1k_input_event_key(&m->input, 'l', pressed); break;
    case SDL_GAMEPAD_BUTTON_NORTH: cv1k_input_event_key(&m->input, 'i', pressed); break;
    case SDL_GAMEPAD_BUTTON_START: cv1k_input_event_key(&m->input, '1', pressed); break;
    case SDL_GAMEPAD_BUTTON_BACK: cv1k_input_event_key(&m->input, '5', pressed); break;
    default: break;
    }
}

static void gamepad_axis(struct cv1k_machine *m, unsigned char axis, short value)
{
    const short dead = 16000;
    if (axis == SDL_GAMEPAD_AXIS_LEFTX) {
        cv1k_input_event_key(&m->input, 'a', value < -dead);
        cv1k_input_event_key(&m->input, 'd', value > dead);
    } else if (axis == SDL_GAMEPAD_AXIS_LEFTY) {
        cv1k_input_event_key(&m->input, 'w', value < -dead);
        cv1k_input_event_key(&m->input, 's', value > dead);
    }
}

#define CV1K_SDL3_AUDIO_TEMP_FRAMES 1024U
#define CV1K_SDL3_AUDIO_START_FRAMES 384U
#define CV1K_SDL3_AUDIO_MAX_LATENCY_FRAMES 1152U

struct cv1k_sdl3_audio {
    struct cv1k_machine *m;
    SDL_AudioStream *stream;
    cv1k_u32 freq;
    cv1k_u32 accum;
    cv1k_u32 overruns;
    int started;
};

static void sdl3_audio_init(struct cv1k_sdl3_audio *a, struct cv1k_machine *m)
{
    SDL_AudioSpec spec;
    if (a == NULL) return;
    memset(a, 0, sizeof(*a));
    a->m = m;
    a->freq = CV1K_YMZ770_CLOCK_HZ / 1024U;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = (int)a->freq;
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (a->stream == NULL) {
        fprintf(stderr, "warning: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        a->freq = 0U;
    }
}

static void sdl3_audio_shutdown(struct cv1k_sdl3_audio *a)
{
    if (a != NULL && a->stream != NULL) {
        SDL_DestroyAudioStream(a->stream);
        a->stream = NULL;
    }
}

static void sdl3_audio_trim_and_maybe_start(struct cv1k_sdl3_audio *a)
{
    int queued;
    if (a == NULL || a->stream == NULL || a->freq == 0U) return;
    queued = SDL_GetAudioStreamQueued(a->stream);
    if (queued > (int)(CV1K_SDL3_AUDIO_MAX_LATENCY_FRAMES * 2U * (cv1k_u32)sizeof(short))) {
        SDL_ClearAudioStream(a->stream);
        a->overruns++;
        queued = 0;
    }
    if (!a->started && queued >= (int)(CV1K_SDL3_AUDIO_START_FRAMES * 2U * (cv1k_u32)sizeof(short))) {
        SDL_ResumeAudioStreamDevice(a->stream);
        a->started = 1;
    }
}

static void sdl3_audio_queue_pcm(struct cv1k_sdl3_audio *a, const short *pcm, cv1k_u32 frames)
{
    if (a == NULL || a->stream == NULL || a->freq == 0U || pcm == NULL || frames == 0U) return;
    sdl3_audio_trim_and_maybe_start(a);
    SDL_PutAudioStreamData(a->stream, pcm, (int)(frames * 2U * (cv1k_u32)sizeof(short)));
    sdl3_audio_trim_and_maybe_start(a);
}

static void sdl3_audio_queue_frames(struct cv1k_sdl3_audio *a, cv1k_u32 frames)
{
    short temp[CV1K_SDL3_AUDIO_TEMP_FRAMES * 2U];
    cv1k_u32 todo;
    if (a == NULL || a->m == NULL || a->stream == NULL || a->freq == 0U) return;

    while (frames > 0U) {
        todo = frames;
        if (todo > CV1K_SDL3_AUDIO_TEMP_FRAMES) todo = CV1K_SDL3_AUDIO_TEMP_FRAMES;
        cv1k_ymz770_mix_s16_stereo(&a->m->ymz, a->m->sound_rom, a->m->sound_rom_size, temp, todo);
        sdl3_audio_queue_pcm(a, temp, todo);
        frames -= todo;
    }
}

static cv1k_u32 sdl3_audio_frame_count_for_next_video_frame(struct cv1k_sdl3_audio *a)
{
    cv1k_u32 frames;
    if (a == NULL || a->freq == 0U) return 0U;
    a->accum += a->freq * 1000U;
    frames = a->accum / CV1K_REFRESH_MILLIHZ;
    a->accum -= frames * CV1K_REFRESH_MILLIHZ;
    return frames;
}

static void sdl3_audio_queue_frame(struct cv1k_sdl3_audio *a)
{
    sdl3_audio_queue_frames(a, sdl3_audio_frame_count_for_next_video_frame(a));
}

int cv1k_ui_sdl3_run(struct cv1k_machine *m)
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_GLContext gl_context;
    struct cv1k_gles2_renderer *gles2;
    cv1k_u32 *present_pixels;
    cv1k_u32 display_w;
    cv1k_u32 display_h;
    struct cv1k_sdl3_audio audio;
    struct cv1k_mt_render *mt_render;
    struct cv1k_mt_audio *mt_audio;
    int mt_render_active;
    int mt_audio_active;
    int use_gles2;
    Uint64 perf_freq;
    Uint64 next_frame;
    Uint64 frame_accum;
    SDL_Event e;
    int running;
    int n;
    SDL_JoystickID *pads;
    SDL_Gamepad *opened[4];
    int opened_count;
    int i;
    if (m == NULL) return 0;
    window = NULL;
    renderer = NULL;
    texture = NULL;
    gl_context = NULL;
    gles2 = NULL;
    present_pixels = NULL;
    display_w = 0U;
    display_h = 0U;
    cv1k_video_display_dimensions(m->display_rotation, &display_w, &display_h);
    memset(&audio, 0, sizeof(audio));
    mt_render = NULL;
    mt_audio = NULL;
    mt_render_active = 0;
    mt_audio_active = 0;
    use_gles2 = (m->video_renderer == CV1K_VIDEO_RENDERER_GLES2);
    perf_freq = 0;
    next_frame = 0;
    frame_accum = 0;
    for (i = 0; i < 4; i++) opened[i] = NULL;
    opened_count = 0;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    if (use_gles2) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window = SDL_CreateWindow("CV1000 sandbox - SDL3/OpenGLES2", (int)(display_w * 2U), (int)(display_h * 2U), SDL_WINDOW_OPENGL);
        if (window == NULL) {
            fprintf(stderr, "SDL_CreateWindow(OpenGLES2) failed: %s; falling back to SDL3 software-texture presenter\n", SDL_GetError());
            use_gles2 = 0;
        } else {
            gl_context = SDL_GL_CreateContext(window);
            if (gl_context == NULL || !SDL_GL_MakeCurrent(window, gl_context)) {
                fprintf(stderr, "SDL_GL_CreateContext/MakeCurrent failed: %s; falling back to SDL3 software-texture presenter\n", SDL_GetError());
                if (gl_context != NULL) SDL_GL_DestroyContext(gl_context);
                gl_context = NULL;
                SDL_DestroyWindow(window);
                window = NULL;
                use_gles2 = 0;
            } else {
                SDL_GL_SetSwapInterval(0);
                gles2 = cv1k_gles2_renderer_create(sdl3_get_gl_proc, NULL, display_w, display_h, m->gles2_tile_cache);
                if (gles2 != NULL && m->gles2_gpu_blitter) cv1k_gles2_renderer_set_gpu_blitter(gles2, &m->video, 1);
                if (gles2 == NULL) {
                    fprintf(stderr, "OpenGLES2 CV1000 presenter init failed; falling back to SDL3 software-texture presenter\n");
                    SDL_GL_DestroyContext(gl_context);
                    gl_context = NULL;
                    SDL_DestroyWindow(window);
                    window = NULL;
                    use_gles2 = 0;
                }
            }
        }
    }
    if (!use_gles2) {
        if (!SDL_CreateWindowAndRenderer("CV1000 sandbox", (int)(display_w * 2U), (int)(display_h * 2U), 0, &window, &renderer)) {
            fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
            SDL_Quit();
            return 0;
        }
        /* screen_rgb is packed 0x00RRGGBB.  Use XRGB so SDL3 renderers do not
         * treat the zero high byte as transparent alpha and present a black frame.
         */
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, (int)display_w, (int)display_h);
        if (texture == NULL) {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
        present_pixels = (cv1k_u32 *)SDL_malloc((size_t)display_w * (size_t)display_h * sizeof(cv1k_u32));
        if (present_pixels == NULL) {
            fprintf(stderr, "SDL_malloc present_pixels failed\n");
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }
    sdl3_audio_init(&audio, m);
    if (!use_gles2 && m->threaded_render && m->render_screen) {
        mt_render = cv1k_mt_render_create();
        mt_render_active = (mt_render != NULL);
        if (!mt_render_active) fprintf(stderr, "warning: threaded render worker unavailable; using single-threaded render path\n");
    }
    if (m->threaded_audio && audio.freq != 0U) {
        mt_audio = cv1k_mt_audio_create(CV1K_SDL3_AUDIO_TEMP_FRAMES);
        mt_audio_active = (mt_audio != NULL);
        if (!mt_audio_active) fprintf(stderr, "warning: threaded audio worker unavailable; using single-threaded audio path\n");
    }
    perf_freq = SDL_GetPerformanceFrequency();
    next_frame = SDL_GetPerformanceCounter();

    pads = SDL_GetGamepads(&n);
    if (pads != NULL) {
        for (i = 0; i < n && opened_count < 4; i++) {
            if (SDL_IsGamepad(pads[i])) {
                opened[opened_count] = SDL_OpenGamepad(pads[i]);
                if (opened[opened_count] != NULL) opened_count++;
            }
        }
        SDL_free(pads);
    }

    running = 1;
    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
                int pressed;
                int ascii;
                pressed = (e.type == SDL_EVENT_KEY_DOWN);
                if (e.key.key == SDLK_ESCAPE && pressed) running = 0;
                else if (e.key.key == SDLK_F5 && pressed) cv1k_save_state(m, "quick.sav");
                else if (e.key.key == SDLK_F8 && pressed) cv1k_load_state(m, "quick.sav");
                ascii = key_to_ascii(e.key.key);
                if (ascii != 0) cv1k_input_event_key(&m->input, ascii, pressed);
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
                gamepad_button(m, e.gbutton.button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                gamepad_axis(m, e.gaxis.axis, e.gaxis.value);
            }
        }
        {
            cv1k_u32 audio_frames;
            int audio_pending = 0;
            cv1k_machine_frame_advance(m, (use_gles2 || mt_render_active) ? 0 : m->render_screen);
            if (mt_render_active) {
                if (cv1k_mt_render_submit(mt_render, &m->video)) {
                    m->threaded_render_jobs++;
                } else {
                    cv1k_video_frame(&m->video, m->main_ram, m->main_ram_size);
                }
            }
            audio_frames = sdl3_audio_frame_count_for_next_video_frame(&audio);
            if (mt_audio_active && audio_frames != 0U && cv1k_mt_audio_submit(mt_audio, &m->ymz, m->sound_rom, m->sound_rom_size, audio_frames)) {
                audio_pending = 1;
                m->threaded_audio_jobs++;
            } else {
                sdl3_audio_queue_frames(&audio, audio_frames);
            }
            if (mt_render_active) cv1k_mt_render_wait(mt_render, &m->video);
            if (!use_gles2) {
                cv1k_video_make_display_xrgb8888(&m->video, m->display_rotation, present_pixels, display_w);
                SDL_UpdateTexture(texture, NULL, present_pixels, (int)(display_w * sizeof(cv1k_u32)));
            }
            if (audio_pending) {
                const short *pcm = NULL;
                cv1k_u32 got = 0U;
                if (cv1k_mt_audio_wait(mt_audio, &pcm, &got) && pcm != NULL) sdl3_audio_queue_pcm(&audio, pcm, got);
            }
        }
        if (use_gles2) {
            if (!cv1k_gles2_renderer_present(gles2, &m->video, m->display_rotation, display_w * 2U, display_h * 2U)) {
                fprintf(stderr, "warning: OpenGLES2 present failed: %s\n", cv1k_gles2_renderer_error(gles2));
            }
            SDL_GL_SwapWindow(window);
        } else {
            SDL_RenderClear(renderer);
            {
                SDL_FRect dst;
                dst.x = 0.0f;
                dst.y = 0.0f;
                dst.w = (float)(display_w * 2U);
                dst.h = (float)(display_h * 2U);
                SDL_RenderTexture(renderer, texture, NULL, &dst);
            }
            SDL_RenderPresent(renderer);
        }
        /* Rational 60.024 Hz pacing using SDL3's high-resolution timer.
         * Keep the deadline in performance-counter ticks and use a fractional
         * accumulator so the period is exactly perf_freq*1000/60024 ticks on
         * average.  SDL_DelayNS avoids the millisecond truncation of SDL_Delay.
         */
        if (perf_freq != 0U) {
            Uint64 now;
            Uint64 step;
            frame_accum += perf_freq * 1000ULL;
            step = frame_accum / (Uint64)CV1K_REFRESH_MILLIHZ;
            frame_accum -= step * (Uint64)CV1K_REFRESH_MILLIHZ;
            next_frame += step;

            now = SDL_GetPerformanceCounter();
            if (next_frame > now) {
                Uint64 delay_ticks = next_frame - now;
                Uint64 delay_ns = (delay_ticks * 1000000000ULL) / perf_freq;
                if (delay_ns != 0U) SDL_DelayNS(delay_ns);
            } else {
                next_frame = now;
                frame_accum = 0U;
            }
        }
    }

    cv1k_mt_audio_destroy(mt_audio);
    cv1k_mt_render_destroy(mt_render);
    sdl3_audio_shutdown(&audio);
    SDL_free(present_pixels);
    for (i = 0; i < opened_count; i++) if (opened[i] != NULL) SDL_CloseGamepad(opened[i]);
    cv1k_gles2_renderer_destroy(gles2);
    if (gl_context != NULL) SDL_GL_DestroyContext(gl_context);
    if (texture != NULL) SDL_DestroyTexture(texture);
    if (renderer != NULL) SDL_DestroyRenderer(renderer);
    if (window != NULL) SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
}
#endif
