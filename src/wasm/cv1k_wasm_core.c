#include "emu.h"
#include "romset.h"
#include "platform.h"
#include "cv1k_frontend.h"
#include "video.h"
#include "cv1k_config.h"
#include "sh3_jit/cv1k_ir.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CV1K_WASM_STATUS_EMPTY   0u
#define CV1K_WASM_STATUS_READY   1u
#define CV1K_WASM_STATUS_RUNNING 2u
#define CV1K_WASM_STATUS_ERROR   5u

#define CV1K_WASM_ERR_NONE       0u
#define CV1K_WASM_ERR_ALLOC      1u
#define CV1K_WASM_ERR_BAD_ROM    2u
#define CV1K_WASM_ERR_CPU        3u
#define CV1K_WASM_ERR_STATE      4u

#define CV1K_WASM_ROM_PATH       "/cv1k_romset.zip"
#define CV1K_WASM_AUDIO_MAX      CV1K_FRONTEND_AUDIO_MAX_FRAMES
#define CV1K_WASM_MAX_W          320u
#define CV1K_WASM_MAX_H          320u

#if defined(__clang__) || defined(__GNUC__)
#define CV1K_WASM_EXPORT(name) __attribute__((export_name(name)))
#else
#define CV1K_WASM_EXPORT(name)
#endif

static struct cv1k_machine g_machine;
static struct cv1k_romset_report g_report;
static int g_machine_ready;
static uint32_t g_status = CV1K_WASM_STATUS_EMPTY;
static uint32_t g_error = CV1K_WASM_ERR_NONE;
static char g_error_text[256];
static uint8_t g_frame_rgba[CV1K_WASM_MAX_W * CV1K_WASM_MAX_H * 4u];
static int16_t g_audio[CV1K_WASM_AUDIO_MAX * 2u];
static uint32_t g_audio_frames;
static uint32_t g_audio_rate = 48000u;
static uint32_t g_audio_accum;
static uint32_t g_input_mask;
static uint32_t g_display_w = CV1K_SCREEN_W;
static uint32_t g_display_h = CV1K_SCREEN_H;
static uint32_t g_present_rotation = CV1K_DISPLAY_ROT_CCW;
static uint32_t g_frame_count;
static uint32_t g_wasm_jit_on = 1u;
static uint32_t g_audio_enabled = 1u;
extern const char *cv1k_wasm_jit_backend_name(void);
extern void cv1k_wasm_jit_stats(cv1k_u32 *blocks, cv1k_u32 *hits, cv1k_u32 *fallbacks, cv1k_u32 *invalidations);

static void set_error(uint32_t code, const char *msg)
{
    g_error = code;
    if (msg != NULL && msg[0] != '\0') snprintf(g_error_text, sizeof(g_error_text), "%s", msg);
    else g_error_text[0] = '\0';
    if (code != CV1K_WASM_ERR_NONE) g_status = CV1K_WASM_STATUS_ERROR;
}

static void clear_error(void)
{
    g_error = CV1K_WASM_ERR_NONE;
    g_error_text[0] = '\0';
    if (g_machine_ready) g_status = CV1K_WASM_STATUS_READY;
}

static void destroy_machine(void)
{
    if (g_machine_ready) cv1k_machine_shutdown(&g_machine);
    memset(&g_machine, 0, sizeof(g_machine));
    g_machine_ready = 0;
    g_status = CV1K_WASM_STATUS_EMPTY;
    g_input_mask = 0;
    g_audio_accum = 0;
    g_audio_frames = 0;
    g_frame_count = 0;
#ifdef CV1K_ENABLE_VFS
    cv1k_vfs_clear();
#endif
}

static void reset_loaded_machine(void)
{
    if (!g_machine_ready) return;
    cv1k_machine_reset(&g_machine);
    if (g_machine.boot_rom != NULL && g_machine.boot_rom_size >= 8UL) {
        g_machine.cpu.pc = 0xa0000000UL;
        g_machine.cpu.r[15] = ((cv1k_u32)g_machine.boot_rom[4] << 24) |
                              ((cv1k_u32)g_machine.boot_rom[5] << 16) |
                              ((cv1k_u32)g_machine.boot_rom[6] << 8) |
                               (cv1k_u32)g_machine.boot_rom[7];
        g_machine.cpu.sgr = g_machine.cpu.r[15];
    }
    g_machine.cpu.idle_pc0 = g_machine.idle_pc0;
    g_machine.cpu.idle_pc1 = g_machine.idle_pc1;
    cv1k_ir_reset();
}

static int ensure_machine(uint32_t model)
{
    int cvmodel;
    if (g_machine_ready) return 1;
    cvmodel = model ? CV1K_MODEL_D : CV1K_MODEL_B;
    if (!cv1k_machine_init(&g_machine, cvmodel)) {
        set_error(CV1K_WASM_ERR_ALLOC, "cv1k_machine_init failed");
        return 0;
    }
    g_machine_ready = 1;
    cv1k_frontend_machine_defaults(&g_machine);
    cv1k_ir_enable(g_wasm_jit_on ? 1 : 0);
    g_machine.ir_jit = g_wasm_jit_on ? 1 : 0;
    g_display_w = CV1K_SCREEN_W;
    g_display_h = CV1K_SCREEN_H;
    cv1k_romset_report_clear(&g_report);
    clear_error();
    return 1;
}

static void stage_frame_rgba(void)
{
    /* Browser/WASM always stages the raw 320x240 CV1000 framebuffer.  Rotation
     * is a presentation concern handled by CSS/canvas compositing; doing it here
     * turns every frame into a cache-hostile transpose and also makes internal
     * screenshots harder to compare against emulator state. */
    g_display_w = CV1K_SCREEN_W;
    g_display_h = CV1K_SCREEN_H;
    cv1k_frontend_make_rgba8888(&g_machine.video, CV1K_DISPLAY_ROT_0, g_frame_rgba, g_display_w * 4u);
}

static void append_frame_audio(void)
{
    uint32_t frames;
    if (!g_audio_enabled || g_audio_rate == 0u || g_audio_frames >= CV1K_WASM_AUDIO_MAX) return;
    frames = cv1k_frontend_audio_frames_for_video(g_audio_rate, &g_audio_accum);
    if (frames > CV1K_WASM_AUDIO_MAX - g_audio_frames) frames = CV1K_WASM_AUDIO_MAX - g_audio_frames;
    if (frames == 0u) return;
    cv1k_ymz770_mix_s16_stereo(&g_machine.ymz, g_machine.sound_rom, g_machine.sound_rom_size,
                               g_audio + (g_audio_frames * 2u), frames);
    g_audio_frames += frames;
}

static uint32_t run_one_frame(uint32_t input_mask, int stage_video, int stage_audio)
{
    if (!g_machine_ready || g_status != CV1K_WASM_STATUS_RUNNING) return 0u;
    g_input_mask = input_mask;
    cv1k_frontend_apply_input_mask(&g_machine.input, input_mask);
    cv1k_machine_frame_advance(&g_machine, 1);
    if (g_machine.cpu.illegal_count != 0UL) {
        set_error(CV1K_WASM_ERR_CPU, "SH-3 illegal instruction");
        return 0u;
    }
    if (stage_video) stage_frame_rgba();
    if (stage_audio) append_frame_audio();
    g_frame_count++;
    return 1u;
}

CV1K_WASM_EXPORT("cv1k_wasm_version")
uint32_t cv1k_wasm_version(void) { return 0x00010000u; }

CV1K_WASM_EXPORT("cv1k_wasm_malloc")
uint32_t cv1k_wasm_malloc(uint32_t size) { return (uint32_t)(uintptr_t)malloc(size ? (size_t)size : 1u); }

CV1K_WASM_EXPORT("cv1k_wasm_free")
void cv1k_wasm_free(uint32_t ptr) { if (ptr) free((void *)(uintptr_t)ptr); }

CV1K_WASM_EXPORT("cv1k_wasm_reset_heap")
void cv1k_wasm_reset_heap(void)
{
    destroy_machine();
    memset(g_frame_rgba, 0, sizeof(g_frame_rgba));
    memset(g_audio, 0, sizeof(g_audio));
    g_error = CV1K_WASM_ERR_NONE;
    g_error_text[0] = '\0';
}

CV1K_WASM_EXPORT("cv1k_wasm_init")
uint32_t cv1k_wasm_init(uint32_t sample_rate, uint32_t model_d)
{
    (void)sample_rate;
    destroy_machine();
    /* Keep the emulated YMZ stream at the board-native frontend rate.  Browser
     * output resampling is done in cv1k-audio-worklet.js / ScriptProcessor, not
     * by changing the emulator mixer clock. */
    g_audio_rate = 16000u;
    return ensure_machine(model_d ? 1u : 0u) ? 1u : 0u;
}

CV1K_WASM_EXPORT("cv1k_wasm_load_romset")
uint32_t cv1k_wasm_load_romset(uint32_t data_ptr, uint32_t size)
{
    if (!ensure_machine(1u) || data_ptr == 0u || size == 0u) return 0u;
#ifdef CV1K_ENABLE_VFS
    cv1k_vfs_remove_file(CV1K_WASM_ROM_PATH);
    if (!cv1k_vfs_add_file_copy(CV1K_WASM_ROM_PATH, (const uint8_t *)(uintptr_t)data_ptr, size)) {
        free((void *)(uintptr_t)data_ptr);
        set_error(CV1K_WASM_ERR_BAD_ROM, "ROM VFS staging failed");
        return 0u;
    }
#endif
    free((void *)(uintptr_t)data_ptr);
    cv1k_romset_report_clear(&g_report);
    if (!cv1k_romset_load_ddpsdoj(&g_machine, CV1K_WASM_ROM_PATH, &g_report) || !g_report.ok) {
        set_error(CV1K_WASM_ERR_BAD_ROM, g_report.message[0] ? g_report.message : "ROM set load failed");
        return 0u;
    }
    cv1k_frontend_machine_defaults(&g_machine);
    cv1k_ir_enable(g_wasm_jit_on ? 1 : 0);
    g_machine.ir_jit = g_wasm_jit_on ? 1 : 0;
    g_present_rotation = (uint32_t)g_report.display_rotation;
    g_machine.display_rotation = CV1K_DISPLAY_ROT_0;
    g_display_w = CV1K_SCREEN_W;
    g_display_h = CV1K_SCREEN_H;
    clear_error();
    return 1u;
}

CV1K_WASM_EXPORT("cv1k_wasm_start")
uint32_t cv1k_wasm_start(void)
{
    if (!g_machine_ready) return 0u;
    cv1k_frontend_machine_defaults(&g_machine);
    reset_loaded_machine();
    cv1k_ir_enable(g_wasm_jit_on ? 1 : 0);
    g_machine.ir_jit = g_wasm_jit_on ? 1 : 0;
    g_present_rotation = (uint32_t)g_report.display_rotation;
    g_machine.display_rotation = CV1K_DISPLAY_ROT_0;
    g_audio_accum = 0u;
    g_audio_frames = 0u;
    g_frame_count = 0u;
    g_input_mask = 0u;
    g_status = CV1K_WASM_STATUS_RUNNING;
    g_error = CV1K_WASM_ERR_NONE;
    g_error_text[0] = '\0';
    return 1u;
}

CV1K_WASM_EXPORT("cv1k_wasm_soft_reset")
uint32_t cv1k_wasm_soft_reset(void)
{
    if (!g_machine_ready) return 0u;
    cv1k_machine_reset(&g_machine);
    return cv1k_wasm_start();
}

CV1K_WASM_EXPORT("cv1k_wasm_frame")
uint32_t cv1k_wasm_frame(uint32_t input_mask)
{
    g_audio_frames = 0u;
    return run_one_frame(input_mask, 1, 1);
}

CV1K_WASM_EXPORT("cv1k_wasm_run_frames")
uint32_t cv1k_wasm_run_frames(uint32_t input_mask, uint32_t frame_count, uint32_t stage_last)
{
    uint32_t done = 0u;
    if (!g_machine_ready || g_status != CV1K_WASM_STATUS_RUNNING || frame_count == 0u) return 0u;
    if (frame_count > 8u) frame_count = 8u;
    g_audio_frames = 0u;
    for (uint32_t i = 0u; i < frame_count; i++) {
        int video = stage_last ? (i + 1u == frame_count) : 0;
        if (!run_one_frame(input_mask, video, 1)) break;
        done++;
        if (g_audio_frames >= CV1K_WASM_AUDIO_MAX - 1024u) break;
    }
    return done;
}

CV1K_WASM_EXPORT("cv1k_wasm_set_input")
void cv1k_wasm_set_input(uint32_t input_mask)
{
    g_input_mask = input_mask;
    if (g_machine_ready) cv1k_frontend_apply_input_mask(&g_machine.input, input_mask);
}

CV1K_WASM_EXPORT("cv1k_wasm_set_rotation")
void cv1k_wasm_set_rotation(uint32_t rotation)
{
    if (rotation <= (uint32_t)CV1K_DISPLAY_ROT_CCW) g_present_rotation = rotation;
    g_display_w = CV1K_SCREEN_W;
    g_display_h = CV1K_SCREEN_H;
    if (g_machine_ready) g_machine.display_rotation = CV1K_DISPLAY_ROT_0;
}

CV1K_WASM_EXPORT("cv1k_wasm_get_rotation") uint32_t cv1k_wasm_get_rotation(void) { return g_present_rotation; }

CV1K_WASM_EXPORT("cv1k_wasm_get_status") uint32_t cv1k_wasm_get_status(void) { return g_status; }
CV1K_WASM_EXPORT("cv1k_wasm_get_error") uint32_t cv1k_wasm_get_error(void) { return g_error; }
CV1K_WASM_EXPORT("cv1k_wasm_get_error_ptr") uint32_t cv1k_wasm_get_error_ptr(void) { return (uint32_t)(uintptr_t)g_error_text; }
CV1K_WASM_EXPORT("cv1k_wasm_get_frame_count") uint32_t cv1k_wasm_get_frame_count(void) { return g_frame_count; }
CV1K_WASM_EXPORT("cv1k_wasm_get_width") uint32_t cv1k_wasm_get_width(void) { return g_display_w; }
CV1K_WASM_EXPORT("cv1k_wasm_get_height") uint32_t cv1k_wasm_get_height(void) { return g_display_h; }
CV1K_WASM_EXPORT("cv1k_wasm_get_pitch_bytes") uint32_t cv1k_wasm_get_pitch_bytes(void) { return g_display_w * 4u; }
CV1K_WASM_EXPORT("cv1k_wasm_get_framebuffer") uint32_t cv1k_wasm_get_framebuffer(void) { return (uint32_t)(uintptr_t)g_frame_rgba; }
CV1K_WASM_EXPORT("cv1k_wasm_stage_framebuffer") uint32_t cv1k_wasm_stage_framebuffer(void)
{
    if (!g_machine_ready) return 0u;
    stage_frame_rgba();
    return (uint32_t)(uintptr_t)g_frame_rgba;
}
CV1K_WASM_EXPORT("cv1k_wasm_get_framebuffer_hash") uint32_t cv1k_wasm_get_framebuffer_hash(void)
{
    uint32_t h = 2166136261u;
    uint32_t bytes = g_display_h * g_display_w * 4u;
    for (uint32_t i = 0u; i < bytes; i++) { h ^= g_frame_rgba[i]; h *= 16777619u; }
    return h;
}
CV1K_WASM_EXPORT("cv1k_wasm_get_audio_rate") uint32_t cv1k_wasm_get_audio_rate(void) { return g_audio_rate; }
CV1K_WASM_EXPORT("cv1k_wasm_get_audio_ptr") uint32_t cv1k_wasm_get_audio_ptr(void) { return (uint32_t)(uintptr_t)g_audio; }
CV1K_WASM_EXPORT("cv1k_wasm_get_audio_frames") uint32_t cv1k_wasm_get_audio_frames(void) { return g_audio_frames; }
CV1K_WASM_EXPORT("cv1k_wasm_audio_consume") void cv1k_wasm_audio_consume(void) { g_audio_frames = 0u; }
CV1K_WASM_EXPORT("cv1k_wasm_set_audio_enabled") void cv1k_wasm_set_audio_enabled(uint32_t on)
{
    g_audio_enabled = on ? 1u : 0u;
    if (!g_audio_enabled) g_audio_frames = 0u;
}
CV1K_WASM_EXPORT("cv1k_wasm_get_audio_enabled") uint32_t cv1k_wasm_get_audio_enabled(void) { return g_audio_enabled; }
CV1K_WASM_EXPORT("cv1k_wasm_get_pc") uint32_t cv1k_wasm_get_pc(void) { return g_machine_ready ? g_machine.cpu.pc : 0u; }
CV1K_WASM_EXPORT("cv1k_wasm_get_cycles") uint32_t cv1k_wasm_get_cycles(void) { return g_machine_ready ? g_machine.cpu.cycles : 0u; }
CV1K_WASM_EXPORT("cv1k_wasm_set_jit") void cv1k_wasm_set_jit(uint32_t on)
{
    g_wasm_jit_on = on ? 1u : 0u;
    cv1k_ir_enable(g_wasm_jit_on ? 1 : 0);
    if (g_machine_ready) g_machine.ir_jit = g_wasm_jit_on ? 1 : 0;
}
CV1K_WASM_EXPORT("cv1k_wasm_get_jit") uint32_t cv1k_wasm_get_jit(void) { return g_wasm_jit_on; }
CV1K_WASM_EXPORT("cv1k_wasm_get_jit_backend_ptr") uint32_t cv1k_wasm_get_jit_backend_ptr(void) { return (uint32_t)(uintptr_t)cv1k_wasm_jit_backend_name(); }
CV1K_WASM_EXPORT("cv1k_wasm_get_jit_blocks") uint32_t cv1k_wasm_get_jit_blocks(void) { cv1k_u32 b=0,h=0,f=0,i=0; cv1k_wasm_jit_stats(&b,&h,&f,&i); return b; }
CV1K_WASM_EXPORT("cv1k_wasm_get_jit_hits") uint32_t cv1k_wasm_get_jit_hits(void) { cv1k_u32 b=0,h=0,f=0,i=0; cv1k_wasm_jit_stats(&b,&h,&f,&i); return h; }
CV1K_WASM_EXPORT("cv1k_wasm_get_jit_fallbacks") uint32_t cv1k_wasm_get_jit_fallbacks(void) { cv1k_u32 b=0,h=0,f=0,i=0; cv1k_wasm_jit_stats(&b,&h,&f,&i); return f; }
CV1K_WASM_EXPORT("cv1k_wasm_button_mask") uint32_t cv1k_wasm_button_mask(uint32_t name_ptr, uint32_t name_len)
{
    char name[64];
    if (name_ptr == 0u || name_len == 0u) return 0u;
    if (name_len >= sizeof(name)) name_len = sizeof(name) - 1u;
    memcpy(name, (const void *)(uintptr_t)name_ptr, name_len);
    name[name_len] = '\0';
    return cv1k_frontend_button_mask_from_name(name);
}
