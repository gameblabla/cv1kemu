#include "ui_sdl3.h"
#include "savestate.h"
#include <stdio.h>
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

int cv1k_ui_sdl3_run(struct cv1k_machine *m)
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
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
    for (i = 0; i < 4; i++) opened[i] = NULL;
    opened_count = 0;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    if (!SDL_CreateWindowAndRenderer("CV1000 / ddpsdoj sandbox", (int)(CV1K_SCREEN_W * 2U), (int)(CV1K_SCREEN_H * 2U), 0, &window, &renderer)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)CV1K_SCREEN_W, (int)CV1K_SCREEN_H);
    if (texture == NULL) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

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
        cv1k_machine_frame(m);
        if (m->video.executed_ops == 0UL) cv1k_machine_render_probe(m, "BOOTING PARTIAL SH3 CORE", "F5 SAVE  F8 LOAD  ESC QUIT", "INPUT LIVE VIA SDL3");
        SDL_UpdateTexture(texture, NULL, m->video.screen_rgb, (int)(CV1K_FRAMEBUFFER_W * sizeof(cv1k_u32)));
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    for (i = 0; i < opened_count; i++) if (opened[i] != NULL) SDL_CloseGamepad(opened[i]);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
}
#endif
