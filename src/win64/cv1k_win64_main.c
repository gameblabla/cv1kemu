#include "emu.h"
#include "romset.h"
#include "platform.h"
#include "cv1k_frontend.h"
#include "video.h"
#include "savestate.h"
#include "sh3_jit/cv1k_ir.h"
#include "sh3_jit/cv1k_sh3_c23_jit.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CV1K_WIN64_WITH_SDL3
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#endif

#define CV1K_WIN_CLASS "CV1KEmuWin32Frontend"
#define CV1K_AUDIO_RATE 48000U
#define CV1K_AUDIO_BUFFERS 4U
#define CV1K_AUDIO_BUFFER_FRAMES 2048U
#define CV1K_CONTROL_FILE "cv1kemu_win32_controls.cfg"

/* ---- Gamepad binding encoding (same scheme as Qt6 frontend) ---- */
#define GP_NONE           0
#define GP_BUTTON_BASE    0x10000
#define GP_AXIS_BASE      0x20000
#define GP_PAD_SHIFT      12
#define GP_PAD_MASK       0x0f
#define GP_BUTTON_MASK    0x0fff
#define GP_AXIS_MASK      0x07ff
#define GP_AXIS_DEADZONE  16000

static int gp_encode_button(int pad, int button)
{
    if (pad < 0) pad = 0;
    if (pad > GP_PAD_MASK) pad = GP_PAD_MASK;
    return GP_BUTTON_BASE | ((pad & GP_PAD_MASK) << GP_PAD_SHIFT) | (button & GP_BUTTON_MASK);
}

static int gp_encode_axis(int pad, int axis, int positive)
{
    if (pad < 0) pad = 0;
    if (pad > GP_PAD_MASK) pad = GP_PAD_MASK;
    return GP_AXIS_BASE | ((pad & GP_PAD_MASK) << GP_PAD_SHIFT) | ((axis & 0x3ff) << 1) | (positive ? 1 : 0);
}

static int gp_is_button(int code) { return (code & 0xf0000) == GP_BUTTON_BASE; }
static int gp_is_axis(int code) { return (code & 0xf0000) == GP_AXIS_BASE; }
static int gp_pad(int code) { return (code >> GP_PAD_SHIFT) & GP_PAD_MASK; }
static int gp_button(int code) { return code & GP_BUTTON_MASK; }
static int gp_axis(int code) { return (code & GP_AXIS_MASK) >> 1; }
static int gp_axis_positive(int code) { return (code & 1) != 0; }

static const char *gp_button_name(int code)
{
    if (code == GP_NONE) return "Unmapped";
#ifdef CV1K_WIN64_WITH_SDL3
    {
        int pad = gp_pad(code) + 1;
        if (gp_is_button(code)) {
            int b = gp_button(code);
            const char *btn_names[] = { "South","East","West","North","Back","Guide","Start",
                "LStick","RStick","LShoulder","RShoulder","DUp","DDown","DLeft","DRight" };
            static char buf[64];
            if (b >= 0 && b < (int)(sizeof(btn_names)/sizeof(btn_names[0])))
                snprintf(buf, sizeof(buf), "Pad%d %s", pad, btn_names[b]);
            else
                snprintf(buf, sizeof(buf), "Pad%d Btn%d", pad, b);
            return buf;
        }
        if (gp_is_axis(code)) {
            static char buf[64];
            const char *axis_names[] = { "LX","LY","RX","RY","LTrigger","RTrigger" };
            int a = gp_axis(code);
            const char *an = (a >= 0 && a < 6) ? axis_names[a] : "?";
            snprintf(buf, sizeof(buf), "Pad%d %s%s", pad, an, gp_axis_positive(code) ? "+" : "-");
            return buf;
        }
    }
#endif
    return "Unmapped";
}

/* Menu command IDs */
enum {
    ID_FILE_OPEN_ROM = 1001,
    ID_FILE_SAVE_STATE,
    ID_FILE_LOAD_STATE,
    ID_FILE_SAVE_STATE_AS,
    ID_FILE_LOAD_STATE_FROM,
    ID_FILE_SLOT_0,                 /* 1005..1014 = slots 0..9 */
    ID_FILE_RESET,
    ID_FILE_EXIT,
    ID_EMULATION_PAUSE,
    ID_EMULATION_FRAME_ADVANCE,
    ID_EMULATION_IR_JIT,
    ID_VIEW_FULLSCREEN,
    ID_VIEW_SCALE_KEEP_ASPECT,
    ID_VIEW_SCALE_FULL_STRETCH,
    ID_VIEW_SCALE_INTEGER,
    ID_OPTIONS_CONTROLS,
    ID_OPTIONS_SETTINGS,
    ID_HELP_ABOUT
};

#define ID_FILE_SLOT_LAST (ID_FILE_SLOT_0 + 9)

typedef struct cv1k_win_audio {
    HWAVEOUT wave;
    WAVEHDR hdr[CV1K_AUDIO_BUFFERS];
    short pcm[CV1K_AUDIO_BUFFERS][CV1K_AUDIO_BUFFER_FRAMES * 2U];
    cv1k_u32 next;
    cv1k_u32 accum;
    int ready;
} cv1k_win_audio;

typedef struct cv1k_win_app {
    struct cv1k_machine m;
    struct cv1k_romset_report rr;
    HWND hwnd;
    HMENU menu;
    HMENU slot_submenu;
    HMENU scale_submenu;
    HBITMAP dib;
    HDC memdc;
    cv1k_u32 *pixels;
    cv1k_u32 display_w;
    cv1k_u32 display_h;
    int scale;
    int scale_mode;        /* 0=keep aspect, 1=full stretch, 2=integer */
    int running;
    int loaded;
    int paused;
    int fullscreen;
    int use_ir_jit;
    int current_slot;
    int capture_input_id;  /* -1 = not capturing, else input id */
    cv1k_u32 input_mask;
    int vkmap[CV1K_INPUT_COUNT];
    int gpmap[CV1K_INPUT_COUNT];  /* gamepad binding codes */
    cv1k_u32 gp_input_mask;
    int gp_capture_id;           /* -1 = not capturing, else input id */
    int gp_ready;
#ifdef CV1K_WIN64_WITH_SDL3
    SDL_Gamepad *gp_controllers[4];
    SDL_JoystickID gp_instance_ids[4];
#endif
    cv1k_win_audio audio;
    LARGE_INTEGER qpf;
    LARGE_INTEGER next_frame;
    uint64_t frame_accum;
    DWORD window_style;    /* saved style before fullscreen */
    WINDOWPLACEMENT saved_placement;
    HBRUSH bg_brush;
    char save_dir[MAX_PATH];
} cv1k_win_app;

static cv1k_win_app g_app;

/* ------------------------------------------------------------------ */
/* String helpers                                                     */
/* ------------------------------------------------------------------ */

static int str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int parse_vk_token(const char *s)
{
    if (s == NULL || s[0] == '\0') return 0;
    if ((s[0] >= '0' && s[0] <= '9')) return (int)strtoul(s, NULL, 0);
    if (strlen(s) == 1U) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return (int)c;
    }
    if (strncmp(s, "VK_", 3U) == 0) s += 3;
    if (str_eq_ci(s, "UP")) return VK_UP;
    if (str_eq_ci(s, "DOWN")) return VK_DOWN;
    if (str_eq_ci(s, "LEFT")) return VK_LEFT;
    if (str_eq_ci(s, "RIGHT")) return VK_RIGHT;
    if (str_eq_ci(s, "SPACE")) return VK_SPACE;
    if (str_eq_ci(s, "RETURN") || str_eq_ci(s, "ENTER")) return VK_RETURN;
    if (str_eq_ci(s, "SHIFT")) return VK_SHIFT;
    if (str_eq_ci(s, "CONTROL") || str_eq_ci(s, "CTRL")) return VK_CONTROL;
    if (str_eq_ci(s, "MENU") || str_eq_ci(s, "ALT")) return VK_MENU;
    if (s[0] == 'F' && s[1] >= '1' && s[1] <= '9') return VK_F1 + atoi(s + 1) - 1;
    return 0;
}

static const char *vk_to_name(int vk, char *buf, size_t buf_size)
{
    if (vk == 0) { snprintf(buf, buf_size, "(none)"); return buf; }
    if (vk == VK_UP) { snprintf(buf, buf_size, "Up"); return buf; }
    if (vk == VK_DOWN) { snprintf(buf, buf_size, "Down"); return buf; }
    if (vk == VK_LEFT) { snprintf(buf, buf_size, "Left"); return buf; }
    if (vk == VK_RIGHT) { snprintf(buf, buf_size, "Right"); return buf; }
    if (vk == VK_SPACE) { snprintf(buf, buf_size, "Space"); return buf; }
    if (vk == VK_RETURN) { snprintf(buf, buf_size, "Return"); return buf; }
    if (vk == VK_SHIFT) { snprintf(buf, buf_size, "Shift"); return buf; }
    if (vk == VK_CONTROL) { snprintf(buf, buf_size, "Ctrl"); return buf; }
    if (vk == VK_MENU) { snprintf(buf, buf_size, "Alt"); return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(buf, buf_size, "F%d", vk - VK_F1 + 1); return buf; }
    if (vk >= 'A' && vk <= 'Z') { snprintf(buf, buf_size, "%c", vk); return buf; }
    if (vk >= '0' && vk <= '9') { snprintf(buf, buf_size, "%c", vk); return buf; }
    snprintf(buf, buf_size, "VK_0x%02X", vk);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Controls                                                           */
/* ------------------------------------------------------------------ */

static void default_controls(cv1k_win_app *app)
{
    int i;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        app->vkmap[i] = 0;
        app->gpmap[i] = GP_NONE;
    }
    app->vkmap[CV1K_IN_P1_UP] = VK_UP;
    app->vkmap[CV1K_IN_P1_DOWN] = VK_DOWN;
    app->vkmap[CV1K_IN_P1_LEFT] = VK_LEFT;
    app->vkmap[CV1K_IN_P1_RIGHT] = VK_RIGHT;
    app->vkmap[CV1K_IN_P1_B1] = 'Z';
    app->vkmap[CV1K_IN_P1_B2] = 'X';
    app->vkmap[CV1K_IN_P1_B3] = 'C';
    app->vkmap[CV1K_IN_P1_B4] = 'V';
    app->vkmap[CV1K_IN_P1_START] = '1';
    app->vkmap[CV1K_IN_COIN1] = '5';
    app->vkmap[CV1K_IN_P2_UP] = 'I';
    app->vkmap[CV1K_IN_P2_DOWN] = 'K';
    app->vkmap[CV1K_IN_P2_LEFT] = 'J';
    app->vkmap[CV1K_IN_P2_RIGHT] = 'L';
    app->vkmap[CV1K_IN_P2_B1] = 'A';
    app->vkmap[CV1K_IN_P2_B2] = 'S';
    app->vkmap[CV1K_IN_P2_B3] = 'D';
    app->vkmap[CV1K_IN_P2_B4] = 'F';
    app->vkmap[CV1K_IN_P2_START] = '2';
    app->vkmap[CV1K_IN_COIN2] = '6';
    app->vkmap[CV1K_IN_SERVICE1] = '9';
    app->vkmap[CV1K_IN_SERVICE2] = 'T';
    app->vkmap[CV1K_IN_SERVICE3] = 'Y';

#ifdef CV1K_WIN64_WITH_SDL3
    /* Default gamepad bindings matching the Qt6 SDL3 frontend */
    app->gpmap[CV1K_IN_P1_UP]    = gp_encode_button(0, SDL_GAMEPAD_BUTTON_DPAD_UP);
    app->gpmap[CV1K_IN_P1_DOWN]  = gp_encode_button(0, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    app->gpmap[CV1K_IN_P1_LEFT]  = gp_encode_button(0, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    app->gpmap[CV1K_IN_P1_RIGHT] = gp_encode_button(0, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    app->gpmap[CV1K_IN_P1_B1]    = gp_encode_button(0, SDL_GAMEPAD_BUTTON_SOUTH);
    app->gpmap[CV1K_IN_P1_B2]    = gp_encode_button(0, SDL_GAMEPAD_BUTTON_EAST);
    app->gpmap[CV1K_IN_P1_B3]    = gp_encode_button(0, SDL_GAMEPAD_BUTTON_WEST);
    app->gpmap[CV1K_IN_P1_B4]    = gp_encode_button(0, SDL_GAMEPAD_BUTTON_NORTH);
    app->gpmap[CV1K_IN_P1_START] = gp_encode_button(0, SDL_GAMEPAD_BUTTON_START);
    app->gpmap[CV1K_IN_COIN1]   = gp_encode_button(0, SDL_GAMEPAD_BUTTON_BACK);
    app->gpmap[CV1K_IN_P2_UP]    = gp_encode_button(1, SDL_GAMEPAD_BUTTON_DPAD_UP);
    app->gpmap[CV1K_IN_P2_DOWN]  = gp_encode_button(1, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    app->gpmap[CV1K_IN_P2_LEFT]  = gp_encode_button(1, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    app->gpmap[CV1K_IN_P2_RIGHT] = gp_encode_button(1, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    app->gpmap[CV1K_IN_P2_B1]    = gp_encode_button(1, SDL_GAMEPAD_BUTTON_SOUTH);
    app->gpmap[CV1K_IN_P2_B2]    = gp_encode_button(1, SDL_GAMEPAD_BUTTON_EAST);
    app->gpmap[CV1K_IN_P2_B3]    = gp_encode_button(1, SDL_GAMEPAD_BUTTON_WEST);
    app->gpmap[CV1K_IN_P2_B4]    = gp_encode_button(1, SDL_GAMEPAD_BUTTON_NORTH);
    app->gpmap[CV1K_IN_P2_START] = gp_encode_button(1, SDL_GAMEPAD_BUTTON_START);
    app->gpmap[CV1K_IN_COIN2]   = gp_encode_button(1, SDL_GAMEPAD_BUTTON_BACK);
#endif
}

static void save_controls_template(const cv1k_win_app *app)
{
    FILE *f = fopen(CV1K_CONTROL_FILE, "w");
    int i;
    if (f == NULL) return;
    fprintf(f, "# CV1KEmu Win32 controls. Values are Win32 virtual-key names, single characters, or numeric VK codes.\n");
    fprintf(f, "# Edit this file and restart the frontend. F2 writes this template.\n");
    fprintf(f, "# Gamepad lines use gp_btn/pad/button or gp_axis/pad/axis/[+/-] format.\n");
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        fprintf(f, "%s_key=0x%02x\n", cv1k_input_name(i), app->vkmap[i]);
        fprintf(f, "%s_gp=%d\n", cv1k_input_name(i), app->gpmap[i]);
    }
    fclose(f);
}

static void load_controls(cv1k_win_app *app)
{
    FILE *f;
    char line[160];
    char name[80];
    char value[80];
    default_controls(app);
    f = fopen(CV1K_CONTROL_FILE, "r");
    if (f == NULL) {
        save_controls_template(app);
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        int id;
        char *underscore;
        if (line[0] == '#') continue;
        name[0] = value[0] = '\0';
        if (sscanf(line, " %79[^=]=%79s", name, value) == 2) {
            /* Strip _key or _gp suffix to find the input name */
            underscore = strrchr(name, '_');
            if (underscore == NULL) continue;
            if (str_eq_ci(underscore, "_key")) {
                *underscore = '\0';
                id = cv1k_input_id_from_name(name);
                if (id >= 0) app->vkmap[id] = parse_vk_token(value);
            } else if (str_eq_ci(underscore, "_gp")) {
                *underscore = '\0';
                id = cv1k_input_id_from_name(name);
                if (id >= 0) app->gpmap[id] = atoi(value);
            }
        }
    }
    fclose(f);
}

static void sync_input_mask(cv1k_win_app *app)
{
    cv1k_frontend_apply_input_mask(&app->m.input, app->input_mask | app->gp_input_mask);
}

static void update_input_vk(cv1k_win_app *app, WPARAM vk, int pressed)
{
    int i;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        if (app->vkmap[i] == (int)vk && app->vkmap[i] != 0) {
            if (pressed) app->input_mask |= (1UL << i);
            else app->input_mask &= ~(1UL << i);
        }
    }
    sync_input_mask(app);
}

/* ------------------------------------------------------------------ */
/* SDL3 gamepad input                                                  */
/* ------------------------------------------------------------------ */

#ifdef CV1K_WIN64_WITH_SDL3

static void sdl_reopen_gamepads(cv1k_win_app *app)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (app->gp_controllers[i]) {
            SDL_CloseGamepad(app->gp_controllers[i]);
            app->gp_controllers[i] = NULL;
            app->gp_instance_ids[i] = 0;
        }
    }
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        int opened = 0;
        if (ids) {
            for (i = 0; i < count && opened < 4; i++) {
                if (SDL_IsGamepad(ids[i])) {
                    SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
                    if (pad) {
                        app->gp_controllers[opened] = pad;
                        app->gp_instance_ids[opened] = ids[i];
                        opened++;
                    }
                }
            }
            SDL_free(ids);
        }
    }
}

static int sdl_gamepad_index_from_instance(cv1k_win_app *app, SDL_JoystickID instance)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (app->gp_controllers[i] && app->gp_instance_ids[i] == instance) return i;
    }
    return 0;
}

static void sdl_set_gamepad_mapped(cv1k_win_app *app, int pad, int code, int pressed)
{
    int i;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        if (app->gpmap[i] == code && gp_pad(code) == pad) {
            if (pressed) app->gp_input_mask |= (1UL << i);
            else app->gp_input_mask &= ~(1UL << i);
        }
    }
    sync_input_mask(app);
}

static void sdl_update_gamepad_mask(cv1k_win_app *app)
{
    cv1k_u32 new_mask = 0;
    int i;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        int code = app->gpmap[i];
        int pad = gp_pad(code);
        if (pad < 0 || pad >= 4 || !app->gp_controllers[pad]) continue;
        if (gp_is_button(code)) {
            int btn = gp_button(code);
            if (btn >= 0 && btn < SDL_GAMEPAD_BUTTON_COUNT &&
                SDL_GetGamepadButton(app->gp_controllers[pad], (SDL_GamepadButton)btn)) {
                new_mask |= (1UL << i);
            }
        } else if (gp_is_axis(code)) {
            int axis = gp_axis(code);
            if (axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT) {
                short v = (short)SDL_GetGamepadAxis(app->gp_controllers[pad], (SDL_GamepadAxis)axis);
                int active = gp_axis_positive(code) ? (v > GP_AXIS_DEADZONE) : (v < -GP_AXIS_DEADZONE);
                if (active) new_mask |= (1UL << i);
            }
        }
    }
    if (new_mask != app->gp_input_mask) {
        app->gp_input_mask = new_mask;
        sync_input_mask(app);
    }
}

static void sdl_poll_gamepads(cv1k_win_app *app)
{
    SDL_Event e;
    if (!app->gp_ready) return;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
            sdl_reopen_gamepads(app);
            app->gp_input_mask = 0;
            sync_input_mask(app);
            continue;
        }
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            int pad = sdl_gamepad_index_from_instance(app, e.gbutton.which);
            int code = gp_encode_button(pad, e.gbutton.button);
            if (app->gp_capture_id >= 0 && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                app->gpmap[app->gp_capture_id] = code;
                app->gp_capture_id = -1;
                continue;
            }
            sdl_set_gamepad_mapped(app, pad, code, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            int pad = sdl_gamepad_index_from_instance(app, e.gaxis.which);
            short v = (short)e.gaxis.value;
            if (app->gp_capture_id >= 0 && (v > GP_AXIS_DEADZONE || v < -GP_AXIS_DEADZONE)) {
                app->gpmap[app->gp_capture_id] = gp_encode_axis(pad, e.gaxis.axis, v > 0);
                app->gp_capture_id = -1;
                continue;
            }
        }
    }
    SDL_UpdateGamepads();
    sdl_update_gamepad_mask(app);
}

static void sdl_init_gamepad(cv1k_win_app *app)
{
    /* Force DirectInput backend: disable RawInput and HIDAPI, enable DirectInput.
     * This helps with older arcade sticks and DInput-only gamepads. */
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Correlate XInput data so XInput controllers still work alongside DInput */
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, "1");

    if (SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        app->gp_ready = 1;
        sdl_reopen_gamepads(app);
    } else {
        app->gp_ready = 0;
    }
    app->gp_capture_id = -1;
}

static void sdl_shutdown_gamepad(cv1k_win_app *app)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (app->gp_controllers[i]) {
            SDL_CloseGamepad(app->gp_controllers[i]);
            app->gp_controllers[i] = NULL;
        }
    }
    if (app->gp_ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
    app->gp_ready = 0;
}

#endif /* CV1K_WIN64_WITH_SDL3 */

/* ------------------------------------------------------------------ */
/* Save directory                                                     */
/* ------------------------------------------------------------------ */

static void init_save_dir(cv1k_win_app *app)
{
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, path))) {
        snprintf(app->save_dir, sizeof(app->save_dir), "%s\\cv1k", path);
        CreateDirectoryA(app->save_dir, NULL);
    } else {
        GetModuleFileNameA(NULL, path, sizeof(path));
        char *slash = strrchr(path, '\\');
        if (slash) *slash = '\0';
        snprintf(app->save_dir, sizeof(app->save_dir), "%s", path);
    }
}

static void state_path_for_slot(cv1k_win_app *app, int slot, char *out, size_t out_size)
{
    if (slot < 0) slot = 0;
    if (slot > 9) slot = 9;
    snprintf(out, out_size, "%s\\slot%d.sav", app->save_dir, slot);
}

/* ------------------------------------------------------------------ */
/* Audio                                                              */
/* ------------------------------------------------------------------ */

static void audio_shutdown(cv1k_win_audio *a)
{
    unsigned i;
    if (a == NULL || !a->ready) return;
    waveOutReset(a->wave);
    for (i = 0; i < CV1K_AUDIO_BUFFERS; i++) waveOutUnprepareHeader(a->wave, &a->hdr[i], sizeof(WAVEHDR));
    waveOutClose(a->wave);
    memset(a, 0, sizeof(*a));
}

static int audio_init(cv1k_win_audio *a)
{
    WAVEFORMATEX fmt;
    unsigned i;
    memset(a, 0, sizeof(*a));
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = CV1K_AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * (fmt.wBitsPerSample / 8));
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    if (waveOutOpen(&a->wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return 0;
    for (i = 0; i < CV1K_AUDIO_BUFFERS; i++) {
        memset(&a->hdr[i], 0, sizeof(WAVEHDR));
        a->hdr[i].lpData = (LPSTR)a->pcm[i];
        a->hdr[i].dwBufferLength = CV1K_AUDIO_BUFFER_FRAMES * 2U * sizeof(short);
        a->hdr[i].dwFlags = WHDR_DONE;
        if (waveOutPrepareHeader(a->wave, &a->hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            audio_shutdown(a);
            return 0;
        }
    }
    a->ready = 1;
    return 1;
}

static void audio_submit_frame(cv1k_win_app *app)
{
    cv1k_win_audio *a = &app->audio;
    cv1k_u32 todo;
    WAVEHDR *h;
    if (!a->ready) return;
    todo = cv1k_frontend_audio_frames_for_video(CV1K_AUDIO_RATE, &a->accum);
    while (todo > 0UL) {
        cv1k_u32 n = todo;
        if (n > CV1K_AUDIO_BUFFER_FRAMES) n = CV1K_AUDIO_BUFFER_FRAMES;
        h = &a->hdr[a->next];
        if ((h->dwFlags & WHDR_DONE) == 0) return;
        cv1k_ymz770_mix_s16_stereo(&app->m.ymz, app->m.sound_rom, app->m.sound_rom_size, a->pcm[a->next], n);
        h->dwBufferLength = n * 2U * sizeof(short);
        h->dwFlags &= ~WHDR_DONE;
        waveOutWrite(a->wave, h, sizeof(WAVEHDR));
        a->next = (a->next + 1U) % CV1K_AUDIO_BUFFERS;
        todo -= n;
    }
}

/* ------------------------------------------------------------------ */
/* Video / display                                                    */
/* ------------------------------------------------------------------ */

static int recreate_dib(cv1k_win_app *app)
{
    BITMAPINFO bi;
    if (app->dib != NULL) DeleteObject(app->dib);
    if (app->memdc == NULL) app->memdc = CreateCompatibleDC(NULL);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)app->display_w;
    bi.bmiHeader.biHeight = -(LONG)app->display_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    app->dib = CreateDIBSection(app->memdc, &bi, DIB_RGB_COLORS, (void **)&app->pixels, NULL, 0);
    if (app->dib == NULL) return 0;
    SelectObject(app->memdc, app->dib);
    return 1;
}

static void resize_to_display(cv1k_win_app *app)
{
    RECT r;
    cv1k_video_display_dimensions(app->m.display_rotation, &app->display_w, &app->display_h);
    if (app->display_w == 0UL || app->display_h == 0UL) { app->display_w = 240UL; app->display_h = 320UL; }
    recreate_dib(app);
    if (app->fullscreen) return;
    r.left = 0; r.top = 0; r.right = (LONG)(app->display_w * (cv1k_u32)app->scale); r.bottom = (LONG)(app->display_h * (cv1k_u32)app->scale);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    SetWindowPos(app->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void present_frame(cv1k_win_app *app)
{
    if (app->pixels != NULL && app->loaded) {
        cv1k_video_make_display_xrgb8888(&app->m.video, app->m.display_rotation, app->pixels, app->display_w);
    }
    InvalidateRect(app->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Fullscreen                                                         */
/* ------------------------------------------------------------------ */

static void enter_fullscreen(cv1k_win_app *app)
{
    if (app->fullscreen) return;
    app->window_style = (DWORD)GetWindowLongA(app->hwnd, GWL_STYLE);
    app->saved_placement.length = sizeof(app->saved_placement);
    GetWindowPlacement(app->hwnd, &app->saved_placement);
    SetWindowLongA(app->hwnd, GWL_STYLE, WS_POPUP);
    SetWindowPos(app->hwnd, HWND_TOP, 0, 0,
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    app->fullscreen = 1;
    if (app->menu) SetMenu(app->hwnd, NULL);
}

static void exit_fullscreen(cv1k_win_app *app)
{
    if (!app->fullscreen) return;
    SetWindowLongA(app->hwnd, GWL_STYLE, app->window_style);
    SetWindowPlacement(app->hwnd, &app->saved_placement);
    SetWindowPos(app->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    app->fullscreen = 0;
    if (app->menu) SetMenu(app->hwnd, app->menu);
}

static void toggle_fullscreen(cv1k_win_app *app)
{
    if (app->fullscreen) exit_fullscreen(app);
    else enter_fullscreen(app);
}

/* ------------------------------------------------------------------ */
/* Frame loop                                                         */
/* ------------------------------------------------------------------ */

static void run_frame(cv1k_win_app *app)
{
    if (!app->loaded || app->paused) return;
    cv1k_frontend_apply_input_mask(&app->m.input, app->input_mask);
    cv1k_machine_frame_advance(&app->m, 1);
    audio_submit_frame(app);
    present_frame(app);
}

static void frame_advance(cv1k_win_app *app)
{
    if (!app->loaded) return;
    int was_paused = app->paused;
    app->paused = 0;
    cv1k_frontend_apply_input_mask(&app->m.input, app->input_mask);
    cv1k_machine_frame_advance(&app->m, 1);
    audio_submit_frame(app);
    present_frame(app);
    app->paused = was_paused;
}

static void pace(cv1k_win_app *app)
{
    LARGE_INTEGER now;
    uint64_t step;
    if (app->qpf.QuadPart == 0) return;
    app->frame_accum += (uint64_t)app->qpf.QuadPart * 1000ULL;
    step = app->frame_accum / (uint64_t)CV1K_REFRESH_MILLIHZ;
    app->frame_accum -= step * (uint64_t)CV1K_REFRESH_MILLIHZ;
    app->next_frame.QuadPart += (LONGLONG)step;
    QueryPerformanceCounter(&now);
    if (app->next_frame.QuadPart > now.QuadPart) {
        uint64_t ms = (uint64_t)((app->next_frame.QuadPart - now.QuadPart) * 1000LL / app->qpf.QuadPart);
        if (ms > 0ULL) Sleep((DWORD)ms);
    } else {
        app->next_frame = now;
        app->frame_accum = 0ULL;
    }
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                        */
/* ------------------------------------------------------------------ */

static char *command_line_rom(char *cmd)
{
    char *p = cmd;
    int quoted = 0;
    if (p == NULL) return NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') { quoted = 1; p++; while (*p && *p != '"') p++; if (*p == '"') p++; }
    else { while (*p && *p != ' ' && *p != '\t') p++; }
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "--romset", 8U) == 0) { p += 8; while (*p == ' ' || *p == '\t') p++; }
    if (*p == '"') { char *start = ++p; while (*p && *p != '"') p++; *p = '\0'; return start; }
    if (*p) { char *start = p; while (*p && *p != ' ' && *p != '\t') p++; *p = '\0'; return start; }
    (void)quoted;
    return NULL;
}

static int open_rom_dialog(HWND hwnd, char *path, DWORD path_size)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    path[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "CV1KEmu ROM zip\0*.zip\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = path_size;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void show_about(HWND hwnd)
{
    MessageBoxA(hwnd,
        "CV1KEmu Win32 frontend\n\n"
        "Credits:\n"
        "- gameblabla: CV1KEmu project, standalone frontends, JIT/performance work, save states, and integration.\n"
        "- MAME: original source/reference for the emulation core, including the Cave CV1000 driver/video behavior and related SH-3, NAND, YMZ770, and device logic.\n\n"
        "This frontend provides file/slot menus, per-player controls, Win32 audio/video, and selectable interpreter/IR JIT execution mode.\n\n"
        "Shortcuts:\n"
        "  F1   - About\n"
        "  F2   - Write control template\n"
        "  F5   - Save state\n"
        "  F8   - Load state\n"
        "  F10  - Frame advance\n"
        "  F11  - Toggle fullscreen\n"
        "  Alt+Enter - Toggle fullscreen\n"
        "  Esc  - Exit fullscreen (does not quit)",
        "About CV1KEmu", MB_OK | MB_ICONINFORMATION);
}

static int load_rom(cv1k_win_app *app, const char *path)
{
    char title[512];
    if (path == NULL || path[0] == '\0') return 0;
    cv1k_romset_report_clear(&app->rr);
    if (!cv1k_romset_load_ddpsdoj(&app->m, path, &app->rr) || !app->rr.ok) {
        MessageBoxA(app->hwnd, app->rr.message[0] ? app->rr.message : "ROM load failed", "CV1KEmu", MB_ICONERROR);
        return 0;
    }
    cv1k_frontend_machine_defaults(&app->m);
    app->m.ir_jit = app->use_ir_jit ? 1 : 0;
    cv1k_ir_enable(app->use_ir_jit ? 1 : 0);
    app->m.display_rotation = app->rr.display_rotation;
    app->loaded = 1;
    app->paused = 0;
    resize_to_display(app);
    snprintf(title, sizeof(title), "CV1KEmu Win32 - %s", app->rr.set_name);
    SetWindowTextA(app->hwnd, title);
    return 1;
}

static void apply_execution_mode(cv1k_win_app *app)
{
    app->m.ir_jit = app->use_ir_jit ? 1 : 0;
    cv1k_ir_enable(app->use_ir_jit ? 1 : 0);
    cv1k_ir_reset();
    CheckMenuItem(app->menu, ID_EMULATION_IR_JIT,
        app->use_ir_jit ? MF_CHECKED : MF_UNCHECKED);
}

static void reset_machine(cv1k_win_app *app)
{
    if (!app->loaded) return;
    cv1k_machine_reset(&app->m);
    apply_execution_mode(app);
    cv1k_frontend_apply_input_mask(&app->m.input, app->input_mask);
    QueryPerformanceCounter(&app->next_frame);
    app->frame_accum = 0ULL;
    present_frame(app);
}

static void save_state_to_slot(cv1k_win_app *app, int slot)
{
    char path[MAX_PATH];
    char msg[256];
    if (!app->loaded) return;
    state_path_for_slot(app, slot, path, sizeof(path));
    if (cv1k_save_state(&app->m, path)) {
        snprintf(msg, sizeof(msg), "Saved state to slot %d.", slot);
        MessageBoxA(app->hwnd, msg, "CV1KEmu", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxA(app->hwnd, "Could not save state.", "CV1KEmu", MB_OK | MB_ICONERROR);
    }
}

static void load_state_from_slot(cv1k_win_app *app, int slot)
{
    char path[MAX_PATH];
    char msg[256];
    if (!app->loaded) return;
    state_path_for_slot(app, slot, path, sizeof(path));
    if (cv1k_load_state(&app->m, path)) {
        cv1k_ir_reset();
        present_frame(app);
        snprintf(msg, sizeof(msg), "Loaded state from slot %d.", slot);
        MessageBoxA(app->hwnd, msg, "CV1KEmu", MB_OK | MB_ICONINFORMATION);
    } else {
        snprintf(msg, sizeof(msg), "Could not load state from slot %d.", slot);
        MessageBoxA(app->hwnd, msg, "CV1KEmu", MB_OK | MB_ICONWARNING);
    }
}

static void save_state_to_file(cv1k_win_app *app)
{
    char path[MAX_PATH];
    OPENFILENAMEA ofn;
    if (!app->loaded) return;
    memset(&ofn, 0, sizeof(ofn));
    path[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->hwnd;
    ofn.lpstrFilter = "CV1KEmu Save State\0*.sav\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrDefExt = "sav";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&ofn)) return;
    if (cv1k_save_state(&app->m, path)) {
        MessageBoxA(app->hwnd, "State saved.", "CV1KEmu", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxA(app->hwnd, "Could not save state file.", "CV1KEmu", MB_OK | MB_ICONERROR);
    }
}

static void load_state_from_file(cv1k_win_app *app)
{
    char path[MAX_PATH];
    OPENFILENAMEA ofn;
    if (!app->loaded) return;
    memset(&ofn, 0, sizeof(ofn));
    path[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->hwnd;
    ofn.lpstrFilter = "CV1KEmu Save State\0*.sav\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return;
    if (cv1k_load_state(&app->m, path)) {
        cv1k_ir_reset();
        present_frame(app);
        MessageBoxA(app->hwnd, "State loaded.", "CV1KEmu", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxA(app->hwnd, "Could not load state file.", "CV1KEmu", MB_OK | MB_ICONWARNING);
    }
}

static void update_slot_menu_checks(cv1k_win_app *app)
{
    int i;
    char label[64];
    for (i = 0; i < 10; i++) {
        CheckMenuItem(app->slot_submenu, ID_FILE_SLOT_0 + i,
            (i == app->current_slot) ? MF_CHECKED : MF_UNCHECKED);
    }
    snprintf(label, sizeof(label), "&Save State to Slot %d\tF5", app->current_slot);
    ModifyMenuA(app->menu, ID_FILE_SAVE_STATE, MF_BYCOMMAND | MF_STRING, ID_FILE_SAVE_STATE, label);
    snprintf(label, sizeof(label), "&Load State from Slot %d\tF8", app->current_slot);
    ModifyMenuA(app->menu, ID_FILE_LOAD_STATE, MF_BYCOMMAND | MF_STRING, ID_FILE_LOAD_STATE, label);
    DrawMenuBar(app->hwnd);
}

static void update_scale_menu_checks(cv1k_win_app *app)
{
    CheckMenuItem(app->scale_submenu, ID_VIEW_SCALE_KEEP_ASPECT,
        (app->scale_mode == 0) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(app->scale_submenu, ID_VIEW_SCALE_FULL_STRETCH,
        (app->scale_mode == 1) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(app->scale_submenu, ID_VIEW_SCALE_INTEGER,
        (app->scale_mode == 2) ? MF_CHECKED : MF_UNCHECKED);
}

/* ------------------------------------------------------------------ */
/* Controls configuration dialog                                      */
/* ------------------------------------------------------------------ */

#define CTL_DLG_LIST     1001
#define CTL_DLG_REBIND_K 1002
#define CTL_DLG_REBIND_G 1003
#define CTL_DLG_CLEAR_K  1004
#define CTL_DLG_CLEAR_G  1005
#define CTL_DLG_RESET    1006
#define CTL_DLG_CLOSE    1007
#define CTL_DLG_TIMER    9001

typedef struct {
    cv1k_win_app *app;
    int tmp_keys[CV1K_INPUT_COUNT];
    int tmp_pads[CV1K_INPUT_COUNT];
    int capture_mode;  /* 0=none, 1=key, 2=gamepad */
    int capture_id;
    HWND hlist;
    HWND hdlg;
} controls_dialog_state;

static void ctl_refresh_list(controls_dialog_state *st)
{
    int i;
    int sel = (int)SendMessageA(st->hlist, LB_GETCURSEL, 0, 0);
    SendMessageA(st->hlist, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        char line[256];
        char keyname[64];
        vk_to_name(st->tmp_keys[i], keyname, sizeof(keyname));
        snprintf(line, sizeof(line), "%-14s  Key: %-12s  Pad: %s",
                 cv1k_input_name(i), keyname, gp_button_name(st->tmp_pads[i]));
        SendMessageA(st->hlist, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0) SendMessageA(st->hlist, LB_SETCURSEL, sel, 0);
}

#ifdef CV1K_WIN64_WITH_SDL3
static void ctl_poll_gamepad_capture(controls_dialog_state *st)
{
    SDL_Event e;
    if (!st->app->gp_ready) return;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
            sdl_reopen_gamepads(st->app);
            continue;
        }
        if (st->capture_mode != 2) continue;
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            int pad = sdl_gamepad_index_from_instance(st->app, e.gbutton.which);
            st->tmp_pads[st->capture_id] = gp_encode_button(pad, e.gbutton.button);
            st->capture_mode = 0;
            SetWindowTextA(st->hdlg, "CV1KEmu Controls");
            ctl_refresh_list(st);
            return;
        }
        if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            short v = (short)e.gaxis.value;
            if (v > GP_AXIS_DEADZONE || v < -GP_AXIS_DEADZONE) {
                int pad = sdl_gamepad_index_from_instance(st->app, e.gaxis.which);
                st->tmp_pads[st->capture_id] = gp_encode_axis(pad, e.gaxis.axis, v > 0);
                st->capture_mode = 0;
                SetWindowTextA(st->hdlg, "CV1KEmu Controls");
                ctl_refresh_list(st);
                return;
            }
        }
    }
    SDL_UpdateGamepads();
}
#endif

static INT_PTR CALLBACK controls_dlg_proc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    controls_dialog_state *st = (controls_dialog_state *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        st = (controls_dialog_state *)lp;
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)st);
        st->hdlg = hdlg;
        st->hlist = GetDlgItem(hdlg, CTL_DLG_LIST);
        ctl_refresh_list(st);
        SendMessageA(st->hlist, LB_SETCURSEL, 0, 0);
        SetTimer(hdlg, CTL_DLG_TIMER, 16, NULL);
        SetFocus(st->hlist);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == CTL_DLG_TIMER) {
#ifdef CV1K_WIN64_WITH_SDL3
            ctl_poll_gamepad_capture(st);
#endif
            return TRUE;
        }
        break;
    case WM_COMMAND: {
        int sel = (int)SendMessageA(st->hlist, LB_GETCURSEL, 0, 0);
        if (sel == LB_ERR) sel = -1;
        switch (LOWORD(wp)) {
        case CTL_DLG_REBIND_K:
            if (sel < 0) break;
            st->capture_mode = 1;
            st->capture_id = sel;
            SetWindowTextA(hdlg, "CV1KEmu Controls - Press a key (Esc to cancel)...");
            SetFocus(hdlg);
            return TRUE;
        case CTL_DLG_REBIND_G:
            if (sel < 0) break;
            st->capture_mode = 2;
            st->capture_id = sel;
            SetWindowTextA(hdlg, "CV1KEmu Controls - Press gamepad btn/stick (Esc to cancel)...");
            return TRUE;
        case CTL_DLG_CLEAR_K:
            if (sel < 0) break;
            st->tmp_keys[sel] = 0;
            ctl_refresh_list(st);
            return TRUE;
        case CTL_DLG_CLEAR_G:
            if (sel < 0) break;
            st->tmp_pads[sel] = GP_NONE;
            ctl_refresh_list(st);
            return TRUE;
        case CTL_DLG_RESET:
            default_controls(st->app);
            memcpy(st->tmp_keys, st->app->vkmap, sizeof(st->tmp_keys));
            memcpy(st->tmp_pads, st->app->gpmap, sizeof(st->tmp_pads));
            ctl_refresh_list(st);
            return TRUE;
        case CTL_DLG_CLOSE:
        case IDCANCEL:
            if (st->capture_mode) {
                st->capture_mode = 0;
                SetWindowTextA(hdlg, "CV1KEmu Controls");
                return TRUE;
            }
            KillTimer(hdlg, CTL_DLG_TIMER);
            EndDialog(hdlg, 1);
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN: {
        if (st->capture_mode == 1) {
            if (wp == VK_ESCAPE) {
                st->capture_mode = 0;
                SetWindowTextA(hdlg, "CV1KEmu Controls");
                return TRUE;
            }
            st->tmp_keys[st->capture_id] = (int)wp;
            st->capture_mode = 0;
            SetWindowTextA(hdlg, "CV1KEmu Controls");
            ctl_refresh_list(st);
            return TRUE;
        }
        if (wp == VK_ESCAPE) {
            KillTimer(hdlg, CTL_DLG_TIMER);
            EndDialog(hdlg, 1);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static void configure_controls(cv1k_win_app *app)
{
    controls_dialog_state st;
    int i;

    /* Build in-memory dialog template */
    struct {
        DLGTEMPLATE tmpl;
        WORD menu;
        WORD wndclass;
        WCHAR title[32];
    } dt;

    BYTE *buf;
    int buf_size;
    int n_items = 7;

    memset(&dt, 0, sizeof(dt));
    dt.tmpl.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_MODALFRAME | DS_SETFONT;
    dt.tmpl.cx = 340;
    dt.tmpl.cy = 200;
    dt.menu = 0;
    dt.wndclass = 0;
    MultiByteToWideChar(CP_ACP, 0, "CV1KEmu Controls", -1, dt.title, 32);

    buf_size = 4096;
    buf = (BYTE *)malloc(buf_size);
    if (!buf) return;

    {
        BYTE *p = buf;
        DLGITEMTEMPLATE *di;
        int y;

        memcpy(p, &dt, sizeof(dt));
        p += sizeof(dt);

#define ALIGN_DW(ptr) do { while ((ULONG_PTR)(ptr) & 3) *(ptr)++ = 0; } while(0)
#define ADD_BTN(px, py, pcx, pcy, ctrl_id, text) do { \
            ALIGN_DW(p); \
            di = (DLGITEMTEMPLATE *)p; \
            di->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON; \
            di->dwExtendedStyle = 0; \
            di->x = (px); di->y = (py); di->cx = (pcx); di->cy = (pcy); \
            p += sizeof(DLGITEMTEMPLATE); \
            *p++ = 0x80; *p++ = 0x00; \
            { WCHAR _w[32]; int _n = MultiByteToWideChar(CP_ACP, 0, (text), -1, _w, 32); memcpy(p, _w, _n*2); p += _n*2; } \
            *p++ = 0; \
            di->id = (ctrl_id); \
        } while(0)

        /* Listbox */
        ALIGN_DW(p);
        di = (DLGITEMTEMPLATE *)p;
        di->style = WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER | LBS_HASSTRINGS;
        di->dwExtendedStyle = 0;
        di->x = 5; di->y = 5; di->cx = 330; di->cy = 140;
        p += sizeof(DLGITEMTEMPLATE);
        *p++ = 0x81; *p++ = 0x00;
        *p++ = 0; *p++ = 0;
        *p++ = 0;
        di->id = CTL_DLG_LIST;

        y = 150;
        ADD_BTN(5, y, 70, 14, CTL_DLG_REBIND_K, "Rebind Key");
        ADD_BTN(80, y, 80, 14, CTL_DLG_REBIND_G, "Rebind Pad");
        ADD_BTN(165, y, 65, 14, CTL_DLG_CLEAR_K, "Clear Key");
        ADD_BTN(235, y, 65, 14, CTL_DLG_CLEAR_G, "Clear Pad");

        y = 168;
        ADD_BTN(5, y, 80, 14, CTL_DLG_RESET, "Reset Defaults");
        ADD_BTN(265, y, 60, 14, CTL_DLG_CLOSE, "Close");

#undef ADD_BTN
#undef ALIGN_DW

        ((DLGTEMPLATE *)buf)->cdit = n_items;
    }

    st.app = app;
    memcpy(st.tmp_keys, app->vkmap, sizeof(st.tmp_keys));
    memcpy(st.tmp_pads, app->gpmap, sizeof(st.tmp_pads));
    st.capture_mode = 0;
    st.capture_id = -1;

    DialogBoxIndirectParamA(GetModuleHandleA(NULL),
        (LPCDLGTEMPLATEA)buf, app->hwnd, controls_dlg_proc, (LPARAM)&st);

    /* Save bindings after dialog closes */
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        app->vkmap[i] = st.tmp_keys[i];
        app->gpmap[i] = st.tmp_pads[i];
    }
    save_controls_template(app);

    free(buf);

    /* Re-sync input mask with new bindings */
    app->input_mask = 0;
    app->gp_input_mask = 0;
    sync_input_mask(app);
}

/* ------------------------------------------------------------------ */
/* Settings dialog                                                    */
/* ------------------------------------------------------------------ */

static void configure_settings(cv1k_win_app *app)
{
    char msg[512];
    snprintf(msg, sizeof(msg),
        "Current settings:\n\n"
        "  Execution mode: %s\n"
        "  State slot:     %d\n"
        "  Scale mode:     %s\n"
        "  Gamepad input:  %s\n\n"
        "Use the menu bar to change these:\n"
        "  Emulation > Use IR JIT\n"
        "  File > Select State Slot\n"
        "  View > Scaling\n"
        "  Options > Controls (rebind keyboard & gamepad)",
        app->use_ir_jit ? "IR JIT (fast)" : "Interpreter (accurate)",
        app->current_slot,
        app->scale_mode == 0 ? "Keep aspect ratio" :
        app->scale_mode == 1 ? "Full stretch" : "Integer scale",
#ifdef CV1K_WIN64_WITH_SDL3
        app->gp_ready ? "SDL3 (DirectInput)" : "unavailable"
#else
        "not compiled in"
#endif
        );
    MessageBoxA(app->hwnd, msg, "CV1KEmu Settings", MB_OK | MB_ICONINFORMATION);
}

/* ------------------------------------------------------------------ */
/* Menu creation                                                      */
/* ------------------------------------------------------------------ */

static void build_menu(cv1k_win_app *app)
{
    HMENU file_menu, emu_menu, view_menu, options_menu, help_menu;
    int i;

    app->menu = CreateMenu();

    /* File menu */
    file_menu = CreatePopupMenu();
    AppendMenuA(file_menu, MF_STRING, ID_FILE_OPEN_ROM, "&Open ROM...\tCtrl+O");
    AppendMenuA(file_menu, MF_SEPARATOR, 0, NULL);
    {
        char slot_label[64];
        snprintf(slot_label, sizeof(slot_label), "&Save State to Slot %d\tF5", app->current_slot);
        AppendMenuA(file_menu, MF_STRING, ID_FILE_SAVE_STATE, slot_label);
        snprintf(slot_label, sizeof(slot_label), "&Load State from Slot %d\tF8", app->current_slot);
        AppendMenuA(file_menu, MF_STRING, ID_FILE_LOAD_STATE, slot_label);
    }
    AppendMenuA(file_menu, MF_STRING, ID_FILE_SAVE_STATE_AS, "Save State &As...");
    AppendMenuA(file_menu, MF_STRING, ID_FILE_LOAD_STATE_FROM, "Load State &From...");

    app->slot_submenu = CreatePopupMenu();
    for (i = 0; i < 10; i++) {
        char label[32];
        snprintf(label, sizeof(label), "Slot &%d", i);
        AppendMenuA(app->slot_submenu, MF_STRING | (i == 0 ? MF_CHECKED : 0),
                    ID_FILE_SLOT_0 + i, label);
    }
    AppendMenuA(file_menu, MF_POPUP | MF_STRING, (UINT_PTR)app->slot_submenu, "Select State &Slot");

    AppendMenuA(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file_menu, MF_STRING, ID_FILE_RESET, "&Reset Game\tCtrl+R");
    AppendMenuA(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file_menu, MF_STRING, ID_FILE_EXIT, "E&xit\tCtrl+Q");
    AppendMenuA(app->menu, MF_POPUP | MF_STRING, (UINT_PTR)file_menu, "&File");

    /* Emulation menu */
    emu_menu = CreatePopupMenu();
    AppendMenuA(emu_menu, MF_STRING, ID_EMULATION_PAUSE, "&Pause\tP");
    AppendMenuA(emu_menu, MF_STRING, ID_EMULATION_FRAME_ADVANCE, "Frame &Advance\tF10");
    AppendMenuA(emu_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(emu_menu, MF_STRING | (app->use_ir_jit ? MF_CHECKED : 0),
                ID_EMULATION_IR_JIT, "Use &IR JIT");
    AppendMenuA(app->menu, MF_POPUP | MF_STRING, (UINT_PTR)emu_menu, "&Emulation");

    /* View menu */
    view_menu = CreatePopupMenu();
    AppendMenuA(view_menu, MF_STRING, ID_VIEW_FULLSCREEN, "&Fullscreen\tF11");

    app->scale_submenu = CreatePopupMenu();
    AppendMenuA(app->scale_submenu, MF_STRING | MF_CHECKED, ID_VIEW_SCALE_KEEP_ASPECT, "Keep aspect ratio");
    AppendMenuA(app->scale_submenu, MF_STRING, ID_VIEW_SCALE_FULL_STRETCH, "Full stretch");
    AppendMenuA(app->scale_submenu, MF_STRING, ID_VIEW_SCALE_INTEGER, "Integer scale");
    AppendMenuA(view_menu, MF_POPUP | MF_STRING, (UINT_PTR)app->scale_submenu, "&Scaling");

    AppendMenuA(app->menu, MF_POPUP | MF_STRING, (UINT_PTR)view_menu, "&View");

    /* Options menu */
    options_menu = CreatePopupMenu();
    AppendMenuA(options_menu, MF_STRING, ID_OPTIONS_CONTROLS, "&Controls...");
    AppendMenuA(options_menu, MF_STRING, ID_OPTIONS_SETTINGS, "&Settings...");
    AppendMenuA(app->menu, MF_POPUP | MF_STRING, (UINT_PTR)options_menu, "&Options");

    /* Help menu */
    help_menu = CreatePopupMenu();
    AppendMenuA(help_menu, MF_STRING, ID_HELP_ABOUT, "&About CV1KEmu\tF1");
    AppendMenuA(app->menu, MF_POPUP | MF_STRING, (UINT_PTR)help_menu, "&Help");
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                   */
/* ------------------------------------------------------------------ */

static void handle_menu_command(cv1k_win_app *app, WORD cmd)
{
    switch (cmd) {
    case ID_FILE_OPEN_ROM: {
        char path[MAX_PATH * 2];
        if (open_rom_dialog(app->hwnd, path, sizeof(path))) load_rom(app, path);
        break;
    }
    case ID_FILE_SAVE_STATE:
        save_state_to_slot(app, app->current_slot);
        break;
    case ID_FILE_LOAD_STATE:
        load_state_from_slot(app, app->current_slot);
        break;
    case ID_FILE_SAVE_STATE_AS:
        save_state_to_file(app);
        break;
    case ID_FILE_LOAD_STATE_FROM:
        load_state_from_file(app);
        break;
    case ID_FILE_RESET:
        reset_machine(app);
        break;
    case ID_FILE_EXIT:
        PostQuitMessage(0);
        break;
    case ID_EMULATION_PAUSE:
        app->paused = !app->paused;
        CheckMenuItem(app->menu, ID_EMULATION_PAUSE, app->paused ? MF_CHECKED : MF_UNCHECKED);
        break;
    case ID_EMULATION_FRAME_ADVANCE:
        frame_advance(app);
        break;
    case ID_EMULATION_IR_JIT:
        app->use_ir_jit = !app->use_ir_jit;
        apply_execution_mode(app);
        break;
    case ID_VIEW_FULLSCREEN:
        toggle_fullscreen(app);
        CheckMenuItem(app->menu, ID_VIEW_FULLSCREEN, app->fullscreen ? MF_CHECKED : MF_UNCHECKED);
        break;
    case ID_VIEW_SCALE_KEEP_ASPECT:
        app->scale_mode = 0;
        update_scale_menu_checks(app);
        InvalidateRect(app->hwnd, NULL, FALSE);
        break;
    case ID_VIEW_SCALE_FULL_STRETCH:
        app->scale_mode = 1;
        update_scale_menu_checks(app);
        InvalidateRect(app->hwnd, NULL, FALSE);
        break;
    case ID_VIEW_SCALE_INTEGER:
        app->scale_mode = 2;
        update_scale_menu_checks(app);
        InvalidateRect(app->hwnd, NULL, FALSE);
        break;
    case ID_OPTIONS_CONTROLS:
        configure_controls(app);
        break;
    case ID_OPTIONS_SETTINGS:
        configure_settings(app);
        break;
    case ID_HELP_ABOUT:
        show_about(app->hwnd);
        break;
    default:
        if (cmd >= ID_FILE_SLOT_0 && cmd <= ID_FILE_SLOT_LAST) {
            app->current_slot = cmd - ID_FILE_SLOT_0;
            update_slot_menu_checks(app);
        }
        break;
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    cv1k_win_app *app = &g_app;
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_COMMAND:
        handle_menu_command(app, LOWORD(wp));
        return 0;

    case WM_KEYDOWN:
        /* Escape: exit fullscreen, never quit */
        if (wp == VK_ESCAPE) {
            if (app->fullscreen) exit_fullscreen(app);
            return 0;
        }
        /* F1 = About */
        if (wp == VK_F1) { show_about(hwnd); return 0; }
        /* F2 = Write control template */
        if (wp == VK_F2) {
            save_controls_template(app);
            MessageBoxA(hwnd, "Control template written to cv1kemu_win32_controls.cfg", "CV1KEmu", MB_OK);
            return 0;
        }
        /* F5 = Save state, F8 = Load state */
        if (wp == VK_F5) { save_state_to_slot(app, app->current_slot); return 0; }
        if (wp == VK_F8) { load_state_from_slot(app, app->current_slot); return 0; }
        /* F10 = Frame advance */
        if (wp == VK_F10) { frame_advance(app); return 0; }
        /* F11 = Toggle fullscreen */
        if (wp == VK_F11) {
            toggle_fullscreen(app);
            CheckMenuItem(app->menu, ID_VIEW_FULLSCREEN, app->fullscreen ? MF_CHECKED : MF_UNCHECKED);
            return 0;
        }
        /* Alt+Enter = Toggle fullscreen */
        if (wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)) {
            toggle_fullscreen(app);
            CheckMenuItem(app->menu, ID_VIEW_FULLSCREEN, app->fullscreen ? MF_CHECKED : MF_UNCHECKED);
            return 0;
        }
        /* Ctrl+R = Reset */
        if (wp == 'R' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            reset_machine(app);
            return 0;
        }
        /* P = Pause toggle (without Ctrl) */
        if (wp == 'P' && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            app->paused = !app->paused;
            CheckMenuItem(app->menu, ID_EMULATION_PAUSE, app->paused ? MF_CHECKED : MF_UNCHECKED);
            return 0;
        }
        update_input_vk(app, wp, 1);
        return 0;

    case WM_KEYUP:
        update_input_vk(app, wp, 0);
        return 0;

    case WM_CHAR:
        /* Prevent beep on F10 */
        if (wp == 0) return 0;
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        if (app->memdc != NULL && app->pixels != NULL) {
            int dst_w = r.right - r.left;
            int dst_h = r.bottom - r.top;
            if (app->scale_mode == 0) {
                /* Keep aspect ratio: letterbox */
                int src_w = (int)app->display_w;
                int src_h = (int)app->display_h;
                double scale_x = (double)dst_w / (double)src_w;
                double scale_y = (double)dst_h / (double)src_h;
                double scale = scale_x < scale_y ? scale_x : scale_y;
                int out_w = (int)(src_w * scale);
                int out_h = (int)(src_h * scale);
                int off_x = (dst_w - out_w) / 2;
                int off_y = (dst_h - out_h) / 2;
                FillRect(dc, &r, app->bg_brush);
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchBlt(dc, off_x, off_y, out_w, out_h, app->memdc, 0, 0, src_w, src_h, SRCCOPY);
            } else if (app->scale_mode == 2) {
                /* Integer scale */
                int src_w = (int)app->display_w;
                int src_h = (int)app->display_h;
                int scale = 1;
                while ((scale + 1) * src_w <= dst_w && (scale + 1) * src_h <= dst_h) scale++;
                int out_w = src_w * scale;
                int out_h = src_h * scale;
                int off_x = (dst_w - out_w) / 2;
                int off_y = (dst_h - out_h) / 2;
                FillRect(dc, &r, app->bg_brush);
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchBlt(dc, off_x, off_y, out_w, out_h, app->memdc, 0, 0, src_w, src_h, SRCCOPY);
            } else {
                /* Full stretch */
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchBlt(dc, 0, 0, dst_w, dst_h, app->memdc, 0, 0, (int)app->display_w, (int)app->display_h, SRCCOPY);
            }
        } else {
            FillRect(dc, &r, app->bg_brush);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;  /* we handle background in WM_PAINT */

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        if (app->loaded && !app->fullscreen) {
            RECT r;
            r.left = 0; r.top = 0;
            r.right = (LONG)app->display_w;
            r.bottom = (LONG)app->display_h;
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

/* ------------------------------------------------------------------ */
/* WinMain                                                            */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc;
    MSG msg;
    char rom_path[MAX_PATH * 2];
    char cmd_copy[4096];
    char *rom_arg;
    (void)prev;
    (void)cmdline;
    memset(&g_app, 0, sizeof(g_app));
    g_app.scale = 2;
    g_app.scale_mode = 0;
    g_app.use_ir_jit = 1;
    g_app.current_slot = 0;
    g_app.capture_input_id = -1;
    g_app.gp_capture_id = -1;
    load_controls(&g_app);
    init_save_dir(&g_app);
    g_app.bg_brush = CreateSolidBrush(RGB(0, 0, 0));

#ifdef CV1K_WIN64_WITH_SDL3
    sdl_init_gamepad(&g_app);
#endif

    if (!cv1k_platform_check() || !cv1k_machine_init(&g_app.m, CV1K_MODEL_D)) return 2;
    cv1k_frontend_machine_defaults(&g_app.m);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = CV1K_WIN_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    build_menu(&g_app);

    g_app.window_style = WS_OVERLAPPEDWINDOW;
    g_app.hwnd = CreateWindowExA(0, CV1K_WIN_CLASS, "CV1KEmu Win32",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 480, 640,
        NULL, g_app.menu, inst, NULL);
    if (g_app.hwnd == NULL) return 2;

    audio_init(&g_app.audio);
    QueryPerformanceFrequency(&g_app.qpf);
    QueryPerformanceCounter(&g_app.next_frame);

    ShowWindow(g_app.hwnd, show);
    UpdateWindow(g_app.hwnd);

    snprintf(cmd_copy, sizeof(cmd_copy), "%s", GetCommandLineA());
    rom_arg = command_line_rom(cmd_copy);
    if (rom_arg != NULL && rom_arg[0] != '\0') load_rom(&g_app, rom_arg);
    else if (open_rom_dialog(g_app.hwnd, rom_path, sizeof(rom_path))) load_rom(&g_app, rom_path);

    g_app.running = 1;
    while (g_app.running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_app.running = 0;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_app.running) break;
#ifdef CV1K_WIN64_WITH_SDL3
        sdl_poll_gamepads(&g_app);
#endif
        run_frame(&g_app);
        pace(&g_app);
    }

    audio_shutdown(&g_app.audio);
#ifdef CV1K_WIN64_WITH_SDL3
    sdl_shutdown_gamepad(&g_app);
#endif
    if (g_app.memdc != NULL) DeleteDC(g_app.memdc);
    if (g_app.dib != NULL) DeleteObject(g_app.dib);
    if (g_app.bg_brush != NULL) DeleteObject(g_app.bg_brush);
    cv1k_machine_shutdown(&g_app.m);
    return 0;
}
