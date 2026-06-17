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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CV1K_WIN_CLASS "CV1KWin64Frontend"
#define CV1K_AUDIO_RATE 48000U
#define CV1K_AUDIO_BUFFERS 4U
#define CV1K_AUDIO_BUFFER_FRAMES 2048U
#define CV1K_CONTROL_FILE "cv1k_win64_controls.cfg"

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
    HBITMAP dib;
    HDC memdc;
    cv1k_u32 *pixels;
    cv1k_u32 display_w;
    cv1k_u32 display_h;
    int scale;
    int running;
    int loaded;
    cv1k_u32 input_mask;
    int vkmap[CV1K_INPUT_COUNT];
    cv1k_win_audio audio;
    LARGE_INTEGER qpf;
    LARGE_INTEGER next_frame;
    uint64_t frame_accum;
} cv1k_win_app;

static cv1k_win_app g_app;

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

static void default_controls(cv1k_win_app *app)
{
    int i;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) app->vkmap[i] = 0;
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
}

static void save_controls_template(const cv1k_win_app *app)
{
    FILE *f = fopen(CV1K_CONTROL_FILE, "w");
    int i;
    if (f == NULL) return;
    fprintf(f, "# CV1000 Win64 controls. Values are Win32 virtual-key names, single characters, or numeric VK codes.\n");
    fprintf(f, "# Edit this file and restart the frontend. F2 writes this template.\n");
    for (i = 0; i < CV1K_INPUT_COUNT; i++) fprintf(f, "%s=0x%02x\n", cv1k_input_name(i), app->vkmap[i]);
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
        if (line[0] == '#') continue;
        name[0] = value[0] = '\0';
        if (sscanf(line, " %79[^=]=%79s", name, value) == 2) {
            id = cv1k_input_id_from_name(name);
            if (id >= 0) app->vkmap[id] = parse_vk_token(value);
        }
    }
    fclose(f);
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
    cv1k_frontend_apply_input_mask(&app->m.input, app->input_mask);
}

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
    r.left = 0; r.top = 0; r.right = (LONG)(app->display_w * (cv1k_u32)app->scale); r.bottom = (LONG)(app->display_h * (cv1k_u32)app->scale);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(app->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void run_frame(cv1k_win_app *app)
{
    if (!app->loaded) return;
    cv1k_frontend_apply_input_mask(&app->m.input, app->input_mask);
    cv1k_machine_frame_advance(&app->m, 1);
    audio_submit_frame(app);
    cv1k_video_make_display_xrgb8888(&app->m.video, app->m.display_rotation, app->pixels, app->display_w);
    InvalidateRect(app->hwnd, NULL, FALSE);
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
    ofn.lpstrFilter = "CV1000 ROM zip\0*.zip\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = path_size;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static int load_rom(cv1k_win_app *app, const char *path)
{
    char title[512];
    if (path == NULL || path[0] == '\0') return 0;
    cv1k_romset_report_clear(&app->rr);
    if (!cv1k_romset_load_ddpsdoj(&app->m, path, &app->rr) || !app->rr.ok) {
        MessageBoxA(app->hwnd, app->rr.message[0] ? app->rr.message : "ROM load failed", "CV1000", MB_ICONERROR);
        return 0;
    }
    cv1k_frontend_machine_defaults(&app->m);
    app->m.ir_jit = 1;
    cv1k_ir_enable(1);
    app->m.display_rotation = app->rr.display_rotation;
    app->loaded = 1;
    resize_to_display(app);
    snprintf(title, sizeof(title), "CV1000 Win64 - %s", app->rr.set_name);
    SetWindowTextA(app->hwnd, title);
    return 1;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    cv1k_win_app *app = &g_app;
    switch (msg) {
    case WM_CREATE: return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
        if (wp == VK_F2) { save_controls_template(app); MessageBoxA(hwnd, "Control template written to cv1k_win64_controls.cfg", "CV1000", MB_OK); return 0; }
        if (wp == VK_F5) { cv1k_save_state(&app->m, "quick.sav"); return 0; }
        if (wp == VK_F8) { cv1k_load_state(&app->m, "quick.sav"); return 0; }
        update_input_vk(app, wp, 1);
        return 0;
    case WM_KEYUP:
        update_input_vk(app, wp, 0);
        return 0;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            if (app->memdc != NULL && app->pixels != NULL) StretchBlt(dc, 0, 0, r.right - r.left, r.bottom - r.top, app->memdc, 0, 0, (int)app->display_w, (int)app->display_h, SRCCOPY);
            EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc;
    MSG msg;
    char rom_path[MAX_PATH * 2];
    char cmd_copy[4096];
    char *rom_arg;
    (void)prev;
    memset(&g_app, 0, sizeof(g_app));
    g_app.scale = 2;
    load_controls(&g_app);
    if (!cv1k_platform_check() || !cv1k_machine_init(&g_app.m, CV1K_MODEL_D)) return 2;
    cv1k_frontend_machine_defaults(&g_app.m);
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = CV1K_WIN_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);
    g_app.hwnd = CreateWindowExA(0, CV1K_WIN_CLASS, "CV1000 Win64", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 480, 640, NULL, NULL, inst, NULL);
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
        run_frame(&g_app);
        pace(&g_app);
    }
    audio_shutdown(&g_app.audio);
    if (g_app.memdc != NULL) DeleteDC(g_app.memdc);
    if (g_app.dib != NULL) DeleteObject(g_app.dib);
    cv1k_machine_shutdown(&g_app.m);
    return 0;
}
