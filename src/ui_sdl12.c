#include "ui_sdl12.h"
#include "savestate.h"
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
#define CV1K_SDL12_AUDIO_RING_FRAMES 32768U
#define CV1K_SDL12_AUDIO_TEMP_FRAMES 1024U

struct cv1k_sdl12_audio {
    struct cv1k_machine *m;
    short ring[CV1K_SDL12_AUDIO_RING_FRAMES * 2U];
    cv1k_u32 read_pos;
    cv1k_u32 write_pos;
    cv1k_u32 freq;
    cv1k_u32 accum;
    cv1k_u32 underruns;
    cv1k_u32 overruns;
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
            a->read_pos = (a->read_pos + 1U) % CV1K_SDL12_AUDIO_RING_FRAMES;
        } else {
            dst[i * 2 + 0] = 0;
            dst[i * 2 + 1] = 0;
            a->underruns++;
        }
    }
}

static void queue_audio_frames(struct cv1k_sdl12_audio *a, cv1k_u32 frames)
{
    short temp[CV1K_SDL12_AUDIO_TEMP_FRAMES * 2U];
    cv1k_u32 todo;
    cv1k_u32 i;
    cv1k_u32 next;
    if (a == NULL || a->m == NULL) return;
    while (frames > 0U) {
        todo = frames;
        if (todo > CV1K_SDL12_AUDIO_TEMP_FRAMES) todo = CV1K_SDL12_AUDIO_TEMP_FRAMES;
        cv1k_ymz770_mix_s16_stereo(&a->m->ymz, a->m->sound_rom, a->m->sound_rom_size, temp, todo);
        SDL_LockAudio();
        for (i = 0U; i < todo; i++) {
            next = (a->write_pos + 1U) % CV1K_SDL12_AUDIO_RING_FRAMES;
            if (next == a->read_pos) {
                a->read_pos = (a->read_pos + 1U) % CV1K_SDL12_AUDIO_RING_FRAMES;
                a->overruns++;
            }
            a->ring[a->write_pos * 2U + 0U] = temp[i * 2U + 0U];
            a->ring[a->write_pos * 2U + 1U] = temp[i * 2U + 1U];
            a->write_pos = next;
        }
        SDL_UnlockAudio();
        frames -= todo;
    }
}

static void queue_frame_audio(struct cv1k_sdl12_audio *a)
{
    cv1k_u32 frames;
    if (a == NULL || a->freq == 0U) return;
    a->accum += a->freq * 1000U;
    frames = a->accum / CV1K_REFRESH_MILLIHZ;
    a->accum -= frames * CV1K_REFRESH_MILLIHZ;
    queue_audio_frames(a, frames);
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

static void present_video(SDL_Surface *screen, const struct cv1k_video *video)
{
    /* ddpsdoj is a TATE (portrait) game; MAME displays it ROT270.  The raw
     * CV1000 framebuffer is 320x240 landscape, so rotate it 90 degrees CW into
     * a 240x320 portrait window: dst(dx,dy) = src((W-1)-dy, dx). */
    cv1k_u32 dx;
    cv1k_u32 dy;
    cv1k_u32 sx;
    cv1k_u32 sy;
    unsigned char *base;
    int bpp;
    if (screen == NULL || video == NULL || video->screen_rgb == NULL) return;
    if (SDL_MUSTLOCK(screen) && SDL_LockSurface(screen) != 0) return;
    base = (unsigned char *)screen->pixels;
    bpp = screen->format->BytesPerPixel;
    for (dy = 0U; dy < CV1K_SCREEN_W; dy++) {            /* dst height = src width  */
        cv1k_u32 src_col = (CV1K_SCREEN_W - 1U) - dy;
        for (sy = 0U; sy < CV1K_SDL12_SCALE; sy++) {
            unsigned char *dstrow = base + (dy * CV1K_SDL12_SCALE + sy) * (cv1k_u32)screen->pitch;
            for (dx = 0U; dx < CV1K_SCREEN_H; dx++) {    /* dst width  = src height */
                cv1k_u32 rgb = video->screen_rgb[dx * CV1K_FRAMEBUFFER_W + src_col];
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
    SDL_AudioSpec got;
    SDL_Joystick *joys[4];
    struct cv1k_sdl12_audio audio;
    int joy_count;
    int opened;
    int running;
    int i;

    if (m == NULL) return 0;
    screen = NULL;
    for (i = 0; i < 4; i++) joys[i] = NULL;
    opened = 0;
    memset(&audio, 0, sizeof(audio));
    audio.m = m;
    audio.freq = CV1K_YMZ770_CLOCK_HZ / 1024U;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_WM_SetCaption("CV1000 / ddpsdoj sandbox - SDL 1.2", NULL);
    SDL_EnableKeyRepeat(0, 0);
    SDL_ShowCursor(SDL_DISABLE);

    /* Portrait window for the rotated (ROT270) TATE display. */
    screen = SDL_SetVideoMode((int)(CV1K_SCREEN_H * CV1K_SDL12_SCALE),
                              (int)(CV1K_SCREEN_W * CV1K_SDL12_SCALE),
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
    want.samples = 512;
    want.callback = sdl12_audio_callback;
    want.userdata = &audio;
    if (SDL_OpenAudio(&want, &got) != 0) {
        fprintf(stderr, "warning: SDL_OpenAudio failed: %s\n", SDL_GetError());
        audio.freq = 0U;
    } else {
        audio.freq = (cv1k_u32)got.freq;
        SDL_PauseAudio(0);
    }

    joy_count = SDL_NumJoysticks();
    for (i = 0; i < joy_count && opened < 4; i++) {
        joys[opened] = SDL_JoystickOpen(i);
        if (joys[opened] != NULL) opened++;
    }
    if (opened > 0) SDL_JoystickEventState(SDL_ENABLE);

    running = 1;
    {
    Uint32 next_frame = SDL_GetTicks();
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

        cv1k_machine_frame(m);
        queue_frame_audio(&audio);
        present_video(screen, &m->video);

        /* Pace to the CV1000 refresh (~60.024 Hz).  Without this the loop runs
         * as fast as the host once the boot CPU load clears, making the game
         * run far too fast.  Sleep the remainder of the frame budget. */
        next_frame += 1000U / 60U;
        {
            Uint32 now = SDL_GetTicks();
            if ((Sint32)(next_frame - now) > 0) SDL_Delay(next_frame - now);
            else next_frame = now;   /* fell behind: resync, don't spiral */
        }
    }
    }

    if (audio.freq != 0U) SDL_CloseAudio();
    for (i = 0; i < opened; i++) if (joys[i] != NULL) SDL_JoystickClose(joys[i]);
    SDL_ShowCursor(SDL_ENABLE);
    SDL_Quit();
    return 1;
}
#endif
