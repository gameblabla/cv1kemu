/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Experimental OpenGL ES 2.0 video renderer for the CV1000 sandbox.
 *
 * The software CV1000 blitter remains authoritative.  This module now owns a
 * tiled VRAM texture/FBO cache so the SDL3/OpenGLES2 path can present directly
 * from emulated VRAM tiles instead of uploading a complete oriented bitmap every
 * frame.  The tile FBOs are intentionally exposed only inside this backend for
 * future GPU blitter work; unsupported cases still fall back to software VRAM.
 */
#include "video_gles2.h"
#include "platform.h"
#include "mame_cv1k_derived.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Minimal GLES2 declarations. */
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef unsigned int GLboolean;
typedef unsigned int GLsizeiptr_unsigned_unused;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLE_STRIP 0x0005
#define GL_FLOAT 0x1406
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_BLEND 0x0BE2

#ifndef CV1K_GLES2_TILE_PIXELS
#define CV1K_GLES2_TILE_PIXELS (CV1K_VRAM_TILE_W * CV1K_VRAM_TILE_H)
#endif

typedef void (*PFNGLACTIVETEXTUREPROC)(GLenum texture);
typedef void (*PFNGLATTACHSHADERPROC)(GLuint program, GLuint shader);
typedef void (*PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void (*PFNGLBINDTEXTUREPROC)(GLenum target, GLuint texture);
typedef void (*PFNGLBUFFERDATAPROC)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum target);
typedef void (*PFNGLCLEARPROC)(GLbitfield mask);
typedef void (*PFNGLCLEARCOLORPROC)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
typedef void (*PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef GLuint (*PFNGLCREATEPROGRAMPROC)(void);
typedef GLuint (*PFNGLCREATESHADERPROC)(GLenum type);
typedef void (*PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint *buffers);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint *framebuffers);
typedef void (*PFNGLDELETEPROGRAMPROC)(GLuint program);
typedef void (*PFNGLDELETESHADERPROC)(GLuint shader);
typedef void (*PFNGLDELETETEXTURESPROC)(GLsizei n, const GLuint *textures);
typedef void (*PFNGLDISABLEPROC)(GLenum cap);
typedef void (*PFNGLDISABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef void (*PFNGLDRAWARRAYSPROC)(GLenum mode, GLint first, GLsizei count);
typedef void (*PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef void (*PFNGLGENBUFFERSPROC)(GLsizei n, GLuint *buffers);
typedef void (*PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint *framebuffers);
typedef void (*PFNGLGENTEXTURESPROC)(GLsizei n, GLuint *textures);
typedef GLint (*PFNGLGETATTRIBLOCATIONPROC)(GLuint program, const GLchar *name);
typedef void (*PFNGLGETPROGRAMIVPROC)(GLuint program, GLenum pname, GLint *params);
typedef void (*PFNGLGETPROGRAMINFOLOGPROC)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void (*PFNGLGETSHADERIVPROC)(GLuint shader, GLenum pname, GLint *params);
typedef void (*PFNGLGETSHADERINFOLOGPROC)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLint (*PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const GLchar *name);
typedef void (*PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void (*PFNGLSHADERSOURCEPROC)(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
typedef void (*PFNGLTEXIMAGE2DPROC)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
typedef void (*PFNGLTEXPARAMETERIPROC)(GLenum target, GLenum pname, GLint param);
typedef void (*PFNGLTEXSUBIMAGE2DPROC)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
typedef void (*PFNGLUNIFORM1IPROC)(GLint location, GLint v0);
typedef void (*PFNGLUSEPROGRAMPROC)(GLuint program);
typedef void (*PFNGLVERTEXATTRIBPOINTERPROC)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
typedef void (*PFNGLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);

struct cv1k_gles2_api {
    PFNGLACTIVETEXTUREPROC ActiveTexture;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLBINDTEXTUREPROC BindTexture;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLCLEARPROC Clear;
    PFNGLCLEARCOLORPROC ClearColor;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLDELETETEXTURESPROC DeleteTextures;
    PFNGLDISABLEPROC Disable;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;
    PFNGLDRAWARRAYSPROC DrawArrays;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLGENTEXTURESPROC GenTextures;
    PFNGLGETATTRIBLOCATIONPROC GetAttribLocation;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLTEXIMAGE2DPROC TexImage2D;
    PFNGLTEXPARAMETERIPROC TexParameteri;
    PFNGLTEXSUBIMAGE2DPROC TexSubImage2D;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLVIEWPORTPROC Viewport;
};

struct cv1k_gles2_tile {
    GLuint tex;
    GLuint fbo;
    cv1k_u32 generation;
    cv1k_u8 allocated;
    cv1k_u8 fbo_complete;
    cv1k_u8 gpu_valid;
};

struct cv1k_gles2_renderer {
    struct cv1k_gles2_api gl;
    cv1k_u32 display_w;
    cv1k_u32 display_h;
    cv1k_u8 *rgba;
    cv1k_u8 *tile_rgba;
    GLuint tex;
    GLuint vbo;
    GLuint program;
    GLint a_pos;
    GLint a_uv;
    GLint u_tex;
    GLuint blit_program;
    GLint blit_a_pos;
    GLint blit_a_uv;
    GLint blit_u_tex;
    GLint blit_u_alpha_test;
    int use_tile_cache;
    int gpu_blitter_enabled;
    struct cv1k_video *bound_video;
    struct cv1k_gles2_tile tiles[CV1K_VRAM_TILE_COUNT];
    cv1k_u32 tiles_allocated;
    cv1k_u32 tile_uploads;
    cv1k_u32 tile_draws;
    cv1k_u32 gpu_uploads;
    cv1k_u32 gpu_draws;
    cv1k_u32 gpu_fallbacks;
    cv1k_u32 fbo_complete_tiles;
    cv1k_u32 fbo_incomplete_tiles;
    char error[256];
};

#define LOAD_GL_FIELD_TYPE(r, getproc, userdata, field, type, name) do { \
    (r)->gl.field = (type)(void *)(getproc)((name), (userdata)); \
    if ((r)->gl.field == NULL) { snprintf((r)->error, sizeof((r)->error), "missing GLES2 function %s", (name)); return 0; } \
} while (0)

static int load_api(struct cv1k_gles2_renderer *r, cv1k_gles2_getproc_fn getproc, void *userdata)
{
    if (r == NULL || getproc == NULL) return 0;
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, ActiveTexture, PFNGLACTIVETEXTUREPROC, "glActiveTexture");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, AttachShader, PFNGLATTACHSHADERPROC, "glAttachShader");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, BindBuffer, PFNGLBINDBUFFERPROC, "glBindBuffer");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC, "glBindFramebuffer");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, BindTexture, PFNGLBINDTEXTUREPROC, "glBindTexture");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, BufferData, PFNGLBUFFERDATAPROC, "glBufferData");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC, "glCheckFramebufferStatus");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, Clear, PFNGLCLEARPROC, "glClear");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, ClearColor, PFNGLCLEARCOLORPROC, "glClearColor");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, CompileShader, PFNGLCOMPILESHADERPROC, "glCompileShader");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, CreateProgram, PFNGLCREATEPROGRAMPROC, "glCreateProgram");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, CreateShader, PFNGLCREATESHADERPROC, "glCreateShader");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, DeleteBuffers, PFNGLDELETEBUFFERSPROC, "glDeleteBuffers");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, DeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC, "glDeleteFramebuffers");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, DeleteProgram, PFNGLDELETEPROGRAMPROC, "glDeleteProgram");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, DeleteShader, PFNGLDELETESHADERPROC, "glDeleteShader");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, DeleteTextures, PFNGLDELETETEXTURESPROC, "glDeleteTextures");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, Disable, PFNGLDISABLEPROC, "glDisable");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, DisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC, "glDisableVertexAttribArray");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, DrawArrays, PFNGLDRAWARRAYSPROC, "glDrawArrays");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC, "glEnableVertexAttribArray");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC, "glFramebufferTexture2D");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GenBuffers, PFNGLGENBUFFERSPROC, "glGenBuffers");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC, "glGenFramebuffers");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GenTextures, PFNGLGENTEXTURESPROC, "glGenTextures");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GetAttribLocation, PFNGLGETATTRIBLOCATIONPROC, "glGetAttribLocation");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GetProgramiv, PFNGLGETPROGRAMIVPROC, "glGetProgramiv");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC, "glGetProgramInfoLog");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GetShaderiv, PFNGLGETSHADERIVPROC, "glGetShaderiv");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC, "glGetShaderInfoLog");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC, "glGetUniformLocation");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, LinkProgram, PFNGLLINKPROGRAMPROC, "glLinkProgram");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, ShaderSource, PFNGLSHADERSOURCEPROC, "glShaderSource");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, TexImage2D, PFNGLTEXIMAGE2DPROC, "glTexImage2D");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, TexParameteri, PFNGLTEXPARAMETERIPROC, "glTexParameteri");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, TexSubImage2D, PFNGLTEXSUBIMAGE2DPROC, "glTexSubImage2D");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, Uniform1i, PFNGLUNIFORM1IPROC, "glUniform1i");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, UseProgram, PFNGLUSEPROGRAMPROC, "glUseProgram");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC, "glVertexAttribPointer");
    LOAD_GL_FIELD_TYPE(r, getproc, userdata, Viewport, PFNGLVIEWPORTPROC, "glViewport");
    return 1;
}
#undef LOAD_GL_FIELD_TYPE

static GLuint compile_shader(struct cv1k_gles2_renderer *r, GLenum type, const char *src)
{
    GLuint sh;
    GLint ok;
    const GLchar *s;
    sh = r->gl.CreateShader(type);
    s = (const GLchar *)src;
    r->gl.ShaderSource(sh, 1, &s, NULL);
    r->gl.CompileShader(sh);
    ok = 0;
    r->gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLsizei got = 0;
        r->gl.GetShaderInfoLog(sh, (GLsizei)sizeof(r->error), &got, r->error);
        if (r->error[0] == '\0') snprintf(r->error, sizeof(r->error), "GLES2 shader compile failed");
        r->gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

static int build_program(struct cv1k_gles2_renderer *r)
{
    static const char *vs =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main(void) { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char *fs =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main(void) { vec4 c = texture2D(u_tex, v_uv); gl_FragColor = vec4(c.rgb, 1.0); }\n";
    GLuint vsh;
    GLuint fsh;
    GLint ok;
    vsh = compile_shader(r, GL_VERTEX_SHADER, vs);
    if (vsh == 0U) return 0;
    fsh = compile_shader(r, GL_FRAGMENT_SHADER, fs);
    if (fsh == 0U) { r->gl.DeleteShader(vsh); return 0; }
    r->program = r->gl.CreateProgram();
    r->gl.AttachShader(r->program, vsh);
    r->gl.AttachShader(r->program, fsh);
    r->gl.LinkProgram(r->program);
    r->gl.DeleteShader(vsh);
    r->gl.DeleteShader(fsh);
    ok = 0;
    r->gl.GetProgramiv(r->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLsizei got = 0;
        r->gl.GetProgramInfoLog(r->program, (GLsizei)sizeof(r->error), &got, r->error);
        if (r->error[0] == '\0') snprintf(r->error, sizeof(r->error), "GLES2 program link failed");
        return 0;
    }
    r->a_pos = r->gl.GetAttribLocation(r->program, "a_pos");
    r->a_uv = r->gl.GetAttribLocation(r->program, "a_uv");
    r->u_tex = r->gl.GetUniformLocation(r->program, "u_tex");
    if (r->a_pos < 0 || r->a_uv < 0 || r->u_tex < 0) {
        snprintf(r->error, sizeof(r->error), "GLES2 shader locations missing");
        return 0;
    }
    return 1;
}

static int build_blit_program(struct cv1k_gles2_renderer *r)
{
    static const char *vs =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main(void) { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char *fs =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform int u_alpha_test;\n"
        "void main(void) { vec4 c = texture2D(u_tex, v_uv); if (u_alpha_test != 0 && c.a < 0.5) discard; gl_FragColor = c; }\n";
    GLuint vsh;
    GLuint fsh;
    GLint ok;
    vsh = compile_shader(r, GL_VERTEX_SHADER, vs);
    if (vsh == 0U) return 0;
    fsh = compile_shader(r, GL_FRAGMENT_SHADER, fs);
    if (fsh == 0U) { r->gl.DeleteShader(vsh); return 0; }
    r->blit_program = r->gl.CreateProgram();
    r->gl.AttachShader(r->blit_program, vsh);
    r->gl.AttachShader(r->blit_program, fsh);
    r->gl.LinkProgram(r->blit_program);
    r->gl.DeleteShader(vsh);
    r->gl.DeleteShader(fsh);
    ok = 0;
    r->gl.GetProgramiv(r->blit_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLsizei got = 0;
        r->gl.GetProgramInfoLog(r->blit_program, (GLsizei)sizeof(r->error), &got, r->error);
        if (r->error[0] == '\0') snprintf(r->error, sizeof(r->error), "GLES2 blit program link failed");
        return 0;
    }
    r->blit_a_pos = r->gl.GetAttribLocation(r->blit_program, "a_pos");
    r->blit_a_uv = r->gl.GetAttribLocation(r->blit_program, "a_uv");
    r->blit_u_tex = r->gl.GetUniformLocation(r->blit_program, "u_tex");
    r->blit_u_alpha_test = r->gl.GetUniformLocation(r->blit_program, "u_alpha_test");
    if (r->blit_a_pos < 0 || r->blit_a_uv < 0 || r->blit_u_tex < 0 || r->blit_u_alpha_test < 0) {
        snprintf(r->error, sizeof(r->error), "GLES2 blit shader locations missing");
        return 0;
    }
    return 1;
}

static CV1K_ALWAYS_INLINE cv1k_u32 rgb1555_to_rgb888_local(cv1k_u16 p)
{
    cv1k_u32 r = (cv1k_u32)((p >> 10) & 0x1fU);
    cv1k_u32 g = (cv1k_u32)((p >> 5) & 0x1fU);
    cv1k_u32 b = (cv1k_u32)(p & 0x1fU);
    return (r << 19) | (g << 11) | (b << 3);
}

static cv1k_u32 raw_screen_pixel_from_vram(const struct cv1k_video *video, cv1k_u32 x, cv1k_u32 y)
{
    cv1k_u32 sx;
    cv1k_u32 sy;
    cv1k_u16 pix;
    if (video == NULL || video->vram1555 == NULL) return 0U;
    sy = (y + (video->gfx_scroll_y & (CV1K_VRAM_H - 1UL))) & (CV1K_VRAM_H - 1UL);
    sx = (x + (video->gfx_scroll_x & (CV1K_VRAM_W - 1UL))) & (CV1K_VRAM_W - 1UL);
    pix = video->vram1555[sy * CV1K_VRAM_W + sx];
    return rgb1555_to_rgb888_local(pix);
}

static cv1k_u32 oriented_screen_pixel_from_vram(const struct cv1k_video *video, int rotation, cv1k_u32 x, cv1k_u32 y)
{
    cv1k_u32 sx;
    cv1k_u32 sy;
    if (rotation == CV1K_DISPLAY_ROT_AUTO) rotation = CV1K_DISPLAY_ROT_CCW;
    switch (rotation) {
    case CV1K_DISPLAY_ROT_0: sx = x; sy = y; break;
    case CV1K_DISPLAY_ROT_CW: sx = y; sy = (CV1K_SCREEN_H - 1U) - x; break;
    case CV1K_DISPLAY_ROT_180: sx = (CV1K_SCREEN_W - 1U) - x; sy = (CV1K_SCREEN_H - 1U) - y; break;
    case CV1K_DISPLAY_ROT_CCW:
    default: sx = (CV1K_SCREEN_W - 1U) - y; sy = x; break;
    }
    return raw_screen_pixel_from_vram(video, sx, sy);
}

static void fill_rgba_from_screen_rgb(const struct cv1k_video *video, int rotation, cv1k_u8 *dst, cv1k_u32 w, cv1k_u32 h)
{
    cv1k_u32 x;
    cv1k_u32 y;
    for (y = 0U; y < h; y++) {
        for (x = 0U; x < w; x++) {
            cv1k_u32 rgb = cv1k_video_display_pixel(video, rotation, x, y);
            cv1k_u8 *p = dst + ((size_t)y * w + x) * 4U;
            p[0] = (cv1k_u8)((rgb >> 16) & 0xffU);
            p[1] = (cv1k_u8)((rgb >> 8) & 0xffU);
            p[2] = (cv1k_u8)(rgb & 0xffU);
            p[3] = 0xffU;
        }
    }
}

static void fill_rgba_from_vram(const struct cv1k_video *video, int rotation, cv1k_u8 *dst, cv1k_u32 w, cv1k_u32 h)
{
    cv1k_u32 x;
    cv1k_u32 y;
    for (y = 0U; y < h; y++) {
        for (x = 0U; x < w; x++) {
            cv1k_u32 rgb = oriented_screen_pixel_from_vram(video, rotation, x, y);
            cv1k_u8 *p = dst + ((size_t)y * w + x) * 4U;
            p[0] = (cv1k_u8)((rgb >> 16) & 0xffU);
            p[1] = (cv1k_u8)((rgb >> 8) & 0xffU);
            p[2] = (cv1k_u8)(rgb & 0xffU);
            p[3] = 0xffU;
        }
    }
}

static void fill_rgba_tile_from_vram(const struct cv1k_video *video, cv1k_u32 tx, cv1k_u32 ty, cv1k_u8 *dst)
{
    cv1k_u32 x;
    cv1k_u32 y;
    cv1k_u32 base_x = tx * CV1K_VRAM_TILE_W;
    cv1k_u32 base_y = ty * CV1K_VRAM_TILE_H;
    if (video == NULL || video->vram1555 == NULL || dst == NULL) return;
    for (y = 0U; y < CV1K_VRAM_TILE_H; y++) {
        const cv1k_u16 *src = video->vram1555 + (base_y + y) * CV1K_VRAM_W + base_x;
        cv1k_u8 *row = dst + ((size_t)y * CV1K_VRAM_TILE_W * 4U);
        for (x = 0U; x < CV1K_VRAM_TILE_W; x++) {
            cv1k_u32 rgb = rgb1555_to_rgb888_local(src[x]);
            row[x * 4U + 0U] = (cv1k_u8)((rgb >> 16) & 0xffU);
            row[x * 4U + 1U] = (cv1k_u8)((rgb >> 8) & 0xffU);
            row[x * 4U + 2U] = (cv1k_u8)(rgb & 0xffU);
            row[x * 4U + 3U] = (src[x] & 0x8000U) ? 0xffU : 0x00U;
        }
    }
}

static cv1k_u32 tile_index(cv1k_u32 tx, cv1k_u32 ty)
{
    return ty * CV1K_VRAM_TILES_X + tx;
}

static int allocate_tile(struct cv1k_gles2_renderer *r, struct cv1k_gles2_tile *t)
{
    GLenum status;
    if (r == NULL || t == NULL) return 0;
    if (t->allocated) return 1;
    r->gl.GenTextures(1, &t->tex);
    r->gl.BindTexture(GL_TEXTURE_2D, t->tex);
    r->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    r->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    r->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    r->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    r->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)CV1K_VRAM_TILE_W, (GLsizei)CV1K_VRAM_TILE_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    r->gl.GenFramebuffers(1, &t->fbo);
    r->gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    r->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    status = r->gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        t->fbo_complete = 1U;
        r->fbo_complete_tiles++;
    } else {
        t->fbo_complete = 0U;
        r->fbo_incomplete_tiles++;
        if (t->fbo != 0U) {
            r->gl.DeleteFramebuffers(1, &t->fbo);
            t->fbo = 0U;
        }
    }
    r->gl.BindFramebuffer(GL_FRAMEBUFFER, 0U);
    t->allocated = 1U;
    t->generation = 0U;
    r->tiles_allocated++;
    return 1;
}

static int ensure_tile_uploaded(struct cv1k_gles2_renderer *r, const struct cv1k_video *video, cv1k_u32 tx, cv1k_u32 ty)
{
    cv1k_u32 idx;
    cv1k_u32 gen;
    struct cv1k_gles2_tile *t;
    if (r == NULL || video == NULL || tx >= CV1K_VRAM_TILES_X || ty >= CV1K_VRAM_TILES_Y) return 0;
    idx = tile_index(tx, ty);
    t = &r->tiles[idx];
    if (!allocate_tile(r, t)) return 0;
    gen = cv1k_video_vram_tile_generation(video, tx, ty);
    if (t->generation != gen || !t->gpu_valid) {
        fill_rgba_tile_from_vram(video, tx, ty, r->tile_rgba);
        r->gl.BindTexture(GL_TEXTURE_2D, t->tex);
        r->gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)CV1K_VRAM_TILE_W, (GLsizei)CV1K_VRAM_TILE_H, GL_RGBA, GL_UNSIGNED_BYTE, r->tile_rgba);
        t->generation = gen;
        t->gpu_valid = 1U;
        r->tile_uploads++;
    }
    return 1;
}


static CV1K_ALWAYS_INLINE cv1k_u16 gles_read_ram16(const cv1k_u8 *ram, cv1k_u32 ram_size, cv1k_u32 addr)
{
    if (ram == NULL || ram_size < 2U) return 0xffffU;
    if (addr + 1U < ram_size) return cv1k_be16(&ram[addr]);
    addr %= ram_size;
    if (addr + 1U < ram_size) return cv1k_be16(&ram[addr]);
    return (cv1k_u16)(((cv1k_u16)ram[addr] << 8) | (cv1k_u16)ram[0]);
}

static CV1K_ALWAYS_INLINE void gles_store_1555_rgba(cv1k_u16 pix, cv1k_u8 *dst)
{
    cv1k_u32 rgb = rgb1555_to_rgb888_local(pix);
    dst[0] = (cv1k_u8)((rgb >> 16) & 0xffU);
    dst[1] = (cv1k_u8)((rgb >> 8) & 0xffU);
    dst[2] = (cv1k_u8)(rgb & 0xffU);
    dst[3] = (pix & 0x8000U) ? 0xffU : 0x00U;
}

static int rects_intersect_u32(cv1k_u32 ax, cv1k_u32 ay, cv1k_u32 aw, cv1k_u32 ah, cv1k_u32 bx, cv1k_u32 by, cv1k_u32 bw, cv1k_u32 bh)
{
    if (aw == 0U || ah == 0U || bw == 0U || bh == 0U) return 0;
    if (ax >= bx + bw || bx >= ax + aw) return 0;
    if (ay >= by + bh || by >= ay + ah) return 0;
    return 1;
}

static void sync_vram_rect_from_software(struct cv1k_gles2_renderer *r, const struct cv1k_video *video, cv1k_u32 x, cv1k_u32 y, cv1k_u32 w, cv1k_u32 h)
{
    cv1k_u32 tx0;
    cv1k_u32 ty0;
    cv1k_u32 tx1;
    cv1k_u32 ty1;
    cv1k_u32 tx;
    cv1k_u32 ty;
    if (r == NULL || video == NULL || w == 0U || h == 0U || x >= CV1K_VRAM_W || y >= CV1K_VRAM_H) return;
    if (w > CV1K_VRAM_W - x) w = CV1K_VRAM_W - x;
    if (h > CV1K_VRAM_H - y) h = CV1K_VRAM_H - y;
    tx0 = x / CV1K_VRAM_TILE_W;
    ty0 = y / CV1K_VRAM_TILE_H;
    tx1 = (x + w - 1U) / CV1K_VRAM_TILE_W;
    ty1 = (y + h - 1U) / CV1K_VRAM_TILE_H;
    for (ty = ty0; ty <= ty1; ty++) {
        for (tx = tx0; tx <= tx1; tx++) (void)ensure_tile_uploaded(r, video, tx, ty);
    }
}

static int gles2_gpu_upload_cb(void *userdata, const struct cv1k_video *video, const struct cv1k_video_gpu_upload_cmd *cmd, const cv1k_u8 *ram, cv1k_u32 ram_size)
{
    struct cv1k_gles2_renderer *r = (struct cv1k_gles2_renderer *)userdata;
    cv1k_u32 y;
    if (r == NULL || video == NULL || cmd == NULL || !r->gpu_blitter_enabled || r->tile_rgba == NULL) return 0;
    if (cmd->w == 0U || cmd->h == 0U || cmd->dst_x >= CV1K_VRAM_W || cmd->dst_y >= CV1K_VRAM_H) return 0;
    if (cmd->w > CV1K_VRAM_W - cmd->dst_x || cmd->h > CV1K_VRAM_H - cmd->dst_y) {
        r->gpu_fallbacks++;
        return 0;
    }

    y = 0U;
    while (y < cmd->h) {
        cv1k_u32 cur_y = cmd->dst_y + y;
        cv1k_u32 ty = cur_y / CV1K_VRAM_TILE_H;
        cv1k_u32 local_y = cur_y & (CV1K_VRAM_TILE_H - 1U);
        cv1k_u32 seg_h = cmd->h - y;
        cv1k_u32 x = 0U;
        if (seg_h > CV1K_VRAM_TILE_H - local_y) seg_h = CV1K_VRAM_TILE_H - local_y;
        while (x < cmd->w) {
            cv1k_u32 cur_x = cmd->dst_x + x;
            cv1k_u32 tx = cur_x / CV1K_VRAM_TILE_W;
            cv1k_u32 local_x = cur_x & (CV1K_VRAM_TILE_W - 1U);
            cv1k_u32 seg_w = cmd->w - x;
            cv1k_u32 sy;
            cv1k_u32 sx;
            cv1k_u32 idx;
            struct cv1k_gles2_tile *t;
            if (seg_w > CV1K_VRAM_TILE_W - local_x) seg_w = CV1K_VRAM_TILE_W - local_x;
            idx = tile_index(tx, ty);
            t = &r->tiles[idx];
            if (!allocate_tile(r, t)) return 0;
            if (!t->gpu_valid && !(local_x == 0U && local_y == 0U && seg_w == CV1K_VRAM_TILE_W && seg_h == CV1K_VRAM_TILE_H)) {
                sync_vram_rect_from_software(r, video, cur_x, cur_y, seg_w, seg_h);
                r->gpu_fallbacks++;
                return 0;
            }
            for (sy = 0U; sy < seg_h; sy++) {
                cv1k_u8 *row = r->tile_rgba + (size_t)sy * seg_w * 4U;
                for (sx = 0U; sx < seg_w; sx++) {
                    cv1k_u32 pixel_index = (y + sy) * cmd->w + (x + sx);
                    cv1k_u32 pos = cmd->addr + CV1K_UPLOAD_HEADER_SIZE_BYTES + pixel_index * 2U;
                    gles_store_1555_rgba(gles_read_ram16(ram, ram_size, pos), row + sx * 4U);
                }
            }
            r->gl.BindTexture(GL_TEXTURE_2D, t->tex);
            r->gl.TexSubImage2D(GL_TEXTURE_2D, 0, (GLint)local_x, (GLint)local_y, (GLsizei)seg_w, (GLsizei)seg_h, GL_RGBA, GL_UNSIGNED_BYTE, r->tile_rgba);
            t->generation = cv1k_video_vram_tile_generation(video, tx, ty);
            t->gpu_valid = 1U;
            r->gpu_uploads++;
            x += seg_w;
        }
        y += seg_h;
    }
    return 1;
}

static void write_fbo_vertex(struct cv1k_gles2_renderer *r, GLfloat *out, cv1k_u32 slot, GLfloat dst_x, GLfloat dst_y, GLfloat u, GLfloat v)
{
    out[slot * 4U + 0U] = (dst_x / (GLfloat)CV1K_VRAM_TILE_W) * 2.0f - 1.0f;
    out[slot * 4U + 1U] = 1.0f - (dst_y / (GLfloat)CV1K_VRAM_TILE_H) * 2.0f;
    out[slot * 4U + 2U] = u;
    out[slot * 4U + 3U] = v;
    CV1K_UNUSED(r);
}

static int gles2_draw_tile_segment(struct cv1k_gles2_renderer *r, const struct cv1k_video *video, cv1k_u32 sx, cv1k_u32 sy, cv1k_u32 dx, cv1k_u32 dy, cv1k_u32 w, cv1k_u32 h, int alpha_test)
{
    cv1k_u32 stx;
    cv1k_u32 sty;
    cv1k_u32 dtx;
    cv1k_u32 dty;
    cv1k_u32 sidx;
    cv1k_u32 didx;
    cv1k_u32 slx;
    cv1k_u32 sly;
    cv1k_u32 dlx;
    cv1k_u32 dly;
    GLfloat u0;
    GLfloat v0;
    GLfloat u1;
    GLfloat v1;
    GLfloat verts[16];
    struct cv1k_gles2_tile *src;
    struct cv1k_gles2_tile *dst;
    if (w == 0U || h == 0U) return 1;
    stx = sx / CV1K_VRAM_TILE_W;
    sty = sy / CV1K_VRAM_TILE_H;
    dtx = dx / CV1K_VRAM_TILE_W;
    dty = dy / CV1K_VRAM_TILE_H;
    sidx = tile_index(stx, sty);
    didx = tile_index(dtx, dty);
    if (sidx == didx) return 0;
    if (!ensure_tile_uploaded(r, video, stx, sty)) return 0;
    src = &r->tiles[sidx];
    dst = &r->tiles[didx];
    if (!allocate_tile(r, dst) || !dst->fbo_complete || !dst->gpu_valid) return 0;
    slx = sx & (CV1K_VRAM_TILE_W - 1U);
    sly = sy & (CV1K_VRAM_TILE_H - 1U);
    dlx = dx & (CV1K_VRAM_TILE_W - 1U);
    dly = dy & (CV1K_VRAM_TILE_H - 1U);
    u0 = (GLfloat)slx / (GLfloat)CV1K_VRAM_TILE_W;
    v0 = (GLfloat)sly / (GLfloat)CV1K_VRAM_TILE_H;
    u1 = (GLfloat)(slx + w) / (GLfloat)CV1K_VRAM_TILE_W;
    v1 = (GLfloat)(sly + h) / (GLfloat)CV1K_VRAM_TILE_H;
    write_fbo_vertex(r, verts, 0U, (GLfloat)dlx, (GLfloat)dly, u0, v0);
    write_fbo_vertex(r, verts, 1U, (GLfloat)(dlx + w), (GLfloat)dly, u1, v0);
    write_fbo_vertex(r, verts, 2U, (GLfloat)dlx, (GLfloat)(dly + h), u0, v1);
    write_fbo_vertex(r, verts, 3U, (GLfloat)(dlx + w), (GLfloat)(dly + h), u1, v1);

    r->gl.BindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
    r->gl.Viewport(0, 0, (GLsizei)CV1K_VRAM_TILE_W, (GLsizei)CV1K_VRAM_TILE_H);
    r->gl.UseProgram(r->blit_program);
    r->gl.Uniform1i(r->blit_u_tex, 0);
    r->gl.Uniform1i(r->blit_u_alpha_test, alpha_test ? 1 : 0);
    r->gl.ActiveTexture(GL_TEXTURE0);
    r->gl.BindTexture(GL_TEXTURE_2D, src->tex);
    r->gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    r->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(verts), verts, GL_DYNAMIC_DRAW);
    r->gl.EnableVertexAttribArray((GLuint)r->blit_a_pos);
    r->gl.EnableVertexAttribArray((GLuint)r->blit_a_uv);
    r->gl.VertexAttribPointer((GLuint)r->blit_a_pos, 2, GL_FLOAT, GL_FALSE, (GLsizei)(4U * sizeof(GLfloat)), (const void *)0);
    r->gl.VertexAttribPointer((GLuint)r->blit_a_uv, 2, GL_FLOAT, GL_FALSE, (GLsizei)(4U * sizeof(GLfloat)), (const void *)(2U * sizeof(GLfloat)));
    r->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    r->gl.DisableVertexAttribArray((GLuint)r->blit_a_pos);
    r->gl.DisableVertexAttribArray((GLuint)r->blit_a_uv);
    dst->generation = cv1k_video_vram_tile_generation(video, dtx, dty);
    dst->gpu_valid = 1U;
    return 1;
}

static int gles2_gpu_draw_cb(void *userdata, const struct cv1k_video *video, const struct cv1k_video_gpu_draw_cmd *cmd)
{
    struct cv1k_gles2_renderer *r = (struct cv1k_gles2_renderer *)userdata;
    cv1k_u8 src_alpha;
    cv1k_u8 dst_alpha;
    cv1k_u8 src_mode;
    cv1k_u8 dst_mode;
    int blend_enabled;
    int tint_enabled;
    int alpha_test;
    cv1k_u32 px0;
    cv1k_u32 py0;
    cv1k_u32 copy_w;
    cv1k_u32 copy_h;
    cv1k_u32 x;
    cv1k_u32 y;
    if (r == NULL || video == NULL || cmd == NULL || !r->gpu_blitter_enabled) return 0;
    if (cmd->written == 0U || cmd->vis_l >= cmd->vis_r || cmd->vis_t >= cmd->vis_b) return 1;
    src_alpha = (cv1k_u8)(((cmd->alphaw >> 8) & 0xffU) >> 3);
    dst_alpha = (cv1k_u8)((cmd->alphaw & 0xffU) >> 3);
    src_mode = (cv1k_u8)((cmd->flags >> 4) & 7U);
    dst_mode = (cv1k_u8)(cmd->flags & 7U);
    blend_enabled = ((cmd->flags & 0x0200U) != 0U) ? 1 : 0;
    if (src_mode == 0U && src_alpha == 0x1fU && dst_mode == 4U && dst_alpha == 0x1fU) blend_enabled = 0;
    tint_enabled = (((cmd->mul_r >> 2) != 0x20U) || ((cmd->mul_g >> 2) != 0x20U) || ((cmd->mul_b >> 2) != 0x20U)) ? 1 : 0;
    alpha_test = ((cmd->flags & 0x0100U) != 0U) ? 1 : 0;
    px0 = (cv1k_u32)(cmd->vis_l - cmd->dst_x);
    py0 = (cv1k_u32)(cmd->vis_t - cmd->dst_y);
    copy_w = (cv1k_u32)(cmd->vis_r - cmd->vis_l);
    copy_h = (cv1k_u32)(cmd->vis_b - cmd->vis_t);

    if (blend_enabled || tint_enabled || (cmd->flags & 0x0c00U) != 0U) {
        sync_vram_rect_from_software(r, video, (cv1k_u32)cmd->vis_l, (cv1k_u32)cmd->vis_t, copy_w, copy_h);
        r->gpu_fallbacks++;
        return 0;
    }
    if (cmd->src_x + px0 + copy_w > CV1K_VRAM_W || cmd->src_y + py0 + copy_h > CV1K_VRAM_H) {
        sync_vram_rect_from_software(r, video, (cv1k_u32)cmd->vis_l, (cv1k_u32)cmd->vis_t, copy_w, copy_h);
        r->gpu_fallbacks++;
        return 0;
    }
    if (rects_intersect_u32(cmd->src_x + px0, cmd->src_y + py0, copy_w, copy_h, (cv1k_u32)cmd->vis_l, (cv1k_u32)cmd->vis_t, copy_w, copy_h)) {
        sync_vram_rect_from_software(r, video, (cv1k_u32)cmd->vis_l, (cv1k_u32)cmd->vis_t, copy_w, copy_h);
        r->gpu_fallbacks++;
        return 0;
    }

    y = 0U;
    while (y < copy_h) {
        cv1k_u32 sy = cmd->src_y + py0 + y;
        cv1k_u32 dy = (cv1k_u32)cmd->vis_t + y;
        cv1k_u32 seg_h = copy_h - y;
        cv1k_u32 dst_h_left = CV1K_VRAM_TILE_H - (dy & (CV1K_VRAM_TILE_H - 1U));
        cv1k_u32 src_h_left = CV1K_VRAM_TILE_H - (sy & (CV1K_VRAM_TILE_H - 1U));
        if (seg_h > dst_h_left) seg_h = dst_h_left;
        if (seg_h > src_h_left) seg_h = src_h_left;
        x = 0U;
        while (x < copy_w) {
            cv1k_u32 sx = cmd->src_x + px0 + x;
            cv1k_u32 dx = (cv1k_u32)cmd->vis_l + x;
            cv1k_u32 seg_w = copy_w - x;
            cv1k_u32 dst_w_left = CV1K_VRAM_TILE_W - (dx & (CV1K_VRAM_TILE_W - 1U));
            cv1k_u32 src_w_left = CV1K_VRAM_TILE_W - (sx & (CV1K_VRAM_TILE_W - 1U));
            cv1k_u32 didx;
            if (seg_w > dst_w_left) seg_w = dst_w_left;
            if (seg_w > src_w_left) seg_w = src_w_left;
            didx = tile_index(dx / CV1K_VRAM_TILE_W, dy / CV1K_VRAM_TILE_H);
            if (!r->tiles[didx].allocated || !r->tiles[didx].gpu_valid) {
                sync_vram_rect_from_software(r, video, (cv1k_u32)cmd->vis_l, (cv1k_u32)cmd->vis_t, copy_w, copy_h);
                r->gpu_fallbacks++;
                return 0;
            }
            if (!gles2_draw_tile_segment(r, video, sx, sy, dx, dy, seg_w, seg_h, alpha_test)) {
                sync_vram_rect_from_software(r, video, (cv1k_u32)cmd->vis_l, (cv1k_u32)cmd->vis_t, copy_w, copy_h);
                r->gpu_fallbacks++;
                return 0;
            }
            x += seg_w;
        }
        y += seg_h;
    }
    r->gpu_draws++;
    return 1;
}

static void gles2_gpu_invalidate_cb(void *userdata)
{
    struct cv1k_gles2_renderer *r = (struct cv1k_gles2_renderer *)userdata;
    cv1k_u32 i;
    if (r == NULL) return;
    for (i = 0U; i < CV1K_VRAM_TILE_COUNT; i++) {
        r->tiles[i].generation = 0U;
        r->tiles[i].gpu_valid = 0U;
    }
}

static const struct cv1k_video_gpu_ops gles2_gpu_ops = {
    gles2_gpu_upload_cb,
    gles2_gpu_draw_cb,
    gles2_gpu_invalidate_cb
};

static void raw_to_display_point(int rotation, GLfloat rx, GLfloat ry, GLfloat *dx, GLfloat *dy)
{
    if (rotation == CV1K_DISPLAY_ROT_AUTO) rotation = CV1K_DISPLAY_ROT_CCW;
    switch (rotation) {
    case CV1K_DISPLAY_ROT_0:
        *dx = rx;
        *dy = ry;
        break;
    case CV1K_DISPLAY_ROT_CW:
        *dx = (GLfloat)CV1K_SCREEN_H - ry;
        *dy = rx;
        break;
    case CV1K_DISPLAY_ROT_180:
        *dx = (GLfloat)CV1K_SCREEN_W - rx;
        *dy = (GLfloat)CV1K_SCREEN_H - ry;
        break;
    case CV1K_DISPLAY_ROT_CCW:
    default:
        *dx = ry;
        *dy = (GLfloat)CV1K_SCREEN_W - rx;
        break;
    }
}

static void write_vertex(struct cv1k_gles2_renderer *r, GLfloat *out, cv1k_u32 slot, int rotation, GLfloat raw_x, GLfloat raw_y, GLfloat u, GLfloat v)
{
    GLfloat dx;
    GLfloat dy;
    raw_to_display_point(rotation, raw_x, raw_y, &dx, &dy);
    out[slot * 4U + 0U] = (dx / (GLfloat)r->display_w) * 2.0f - 1.0f;
    out[slot * 4U + 1U] = 1.0f - (dy / (GLfloat)r->display_h) * 2.0f;
    out[slot * 4U + 2U] = u;
    out[slot * 4U + 3U] = v;
}

static void draw_vram_subrect(struct cv1k_gles2_renderer *r, const struct cv1k_video *video, cv1k_u32 vram_x, cv1k_u32 vram_y, cv1k_u32 raw_x, cv1k_u32 raw_y, cv1k_u32 w, cv1k_u32 h, int rotation)
{
    cv1k_u32 tx;
    cv1k_u32 ty;
    cv1k_u32 local_x;
    cv1k_u32 local_y;
    GLfloat u0;
    GLfloat v0;
    GLfloat u1;
    GLfloat v1;
    GLfloat verts[16];
    const struct cv1k_gles2_tile *t;
    CV1K_UNUSED(video);
    if (w == 0U || h == 0U) return;
    tx = vram_x / CV1K_VRAM_TILE_W;
    ty = vram_y / CV1K_VRAM_TILE_H;
    local_x = vram_x & (CV1K_VRAM_TILE_W - 1U);
    local_y = vram_y & (CV1K_VRAM_TILE_H - 1U);
    t = &r->tiles[tile_index(tx, ty)];
    if (!t->allocated) return;
    u0 = (GLfloat)local_x / (GLfloat)CV1K_VRAM_TILE_W;
    v0 = (GLfloat)local_y / (GLfloat)CV1K_VRAM_TILE_H;
    u1 = (GLfloat)(local_x + w) / (GLfloat)CV1K_VRAM_TILE_W;
    v1 = (GLfloat)(local_y + h) / (GLfloat)CV1K_VRAM_TILE_H;

    write_vertex(r, verts, 0U, rotation, (GLfloat)raw_x, (GLfloat)raw_y, u0, v0);
    write_vertex(r, verts, 1U, rotation, (GLfloat)(raw_x + w), (GLfloat)raw_y, u1, v0);
    write_vertex(r, verts, 2U, rotation, (GLfloat)raw_x, (GLfloat)(raw_y + h), u0, v1);
    write_vertex(r, verts, 3U, rotation, (GLfloat)(raw_x + w), (GLfloat)(raw_y + h), u1, v1);

    r->gl.BindTexture(GL_TEXTURE_2D, t->tex);
    r->gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    r->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(verts), verts, GL_DYNAMIC_DRAW);
    r->gl.VertexAttribPointer((GLuint)r->a_pos, 2, GL_FLOAT, GL_FALSE, (GLsizei)(4U * sizeof(GLfloat)), (const void *)0);
    r->gl.VertexAttribPointer((GLuint)r->a_uv, 2, GL_FLOAT, GL_FALSE, (GLsizei)(4U * sizeof(GLfloat)), (const void *)(2U * sizeof(GLfloat)));
    r->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    r->tile_draws++;
}

static int present_from_vram_tiles(struct cv1k_gles2_renderer *r, const struct cv1k_video *video, int rotation)
{
    cv1k_u32 raw_y;
    cv1k_u32 raw_x;
    cv1k_u32 sx0;
    cv1k_u32 sy0;
    if (r == NULL || video == NULL || video->vram1555 == NULL || r->tile_rgba == NULL) return 0;
    sx0 = video->gfx_scroll_x & (CV1K_VRAM_W - 1UL);
    sy0 = video->gfx_scroll_y & (CV1K_VRAM_H - 1UL);

    r->gl.BindFramebuffer(GL_FRAMEBUFFER, 0U);
    r->gl.UseProgram(r->program);
    r->gl.Uniform1i(r->u_tex, 0);
    r->gl.ActiveTexture(GL_TEXTURE0);
    r->gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    r->gl.EnableVertexAttribArray((GLuint)r->a_pos);
    r->gl.EnableVertexAttribArray((GLuint)r->a_uv);

    raw_y = 0U;
    while (raw_y < CV1K_SCREEN_H) {
        cv1k_u32 vy = (sy0 + raw_y) & (CV1K_VRAM_H - 1UL);
        cv1k_u32 seg_h = CV1K_SCREEN_H - raw_y;
        if (seg_h > CV1K_VRAM_H - vy) seg_h = CV1K_VRAM_H - vy;
        raw_x = 0U;
        while (raw_x < CV1K_SCREEN_W) {
            cv1k_u32 vx = (sx0 + raw_x) & (CV1K_VRAM_W - 1UL);
            cv1k_u32 seg_w = CV1K_SCREEN_W - raw_x;
            if (seg_w > CV1K_VRAM_W - vx) seg_w = CV1K_VRAM_W - vx;
            cv1k_u32 sub_y = 0U;
            while (sub_y < seg_h) {
                cv1k_u32 tile_h = seg_h - sub_y;
                cv1k_u32 cur_vy = vy + sub_y;
                if (tile_h > CV1K_VRAM_TILE_H - (cur_vy & (CV1K_VRAM_TILE_H - 1U))) tile_h = CV1K_VRAM_TILE_H - (cur_vy & (CV1K_VRAM_TILE_H - 1U));
                cv1k_u32 sub_x = 0U;
                while (sub_x < seg_w) {
                    cv1k_u32 tile_w = seg_w - sub_x;
                    cv1k_u32 cur_vx = vx + sub_x;
                    cv1k_u32 tx = cur_vx / CV1K_VRAM_TILE_W;
                    cv1k_u32 ty = cur_vy / CV1K_VRAM_TILE_H;
                    if (tile_w > CV1K_VRAM_TILE_W - (cur_vx & (CV1K_VRAM_TILE_W - 1U))) tile_w = CV1K_VRAM_TILE_W - (cur_vx & (CV1K_VRAM_TILE_W - 1U));
                    if (!ensure_tile_uploaded(r, video, tx, ty)) return 0;
                    draw_vram_subrect(r, video, cur_vx, cur_vy, raw_x + sub_x, raw_y + sub_y, tile_w, tile_h, rotation);
                    sub_x += tile_w;
                }
                sub_y += tile_h;
            }
            raw_x += seg_w;
        }
        raw_y += seg_h;
    }
    r->gl.DisableVertexAttribArray((GLuint)r->a_pos);
    r->gl.DisableVertexAttribArray((GLuint)r->a_uv);
    return 1;
}

struct cv1k_gles2_renderer *cv1k_gles2_renderer_create(cv1k_gles2_getproc_fn getproc, void *userdata, cv1k_u32 display_w, cv1k_u32 display_h, int use_tile_cache)
{
    struct cv1k_gles2_renderer *r;
    if (display_w == 0U || display_h == 0U || display_w > 4096U || display_h > 4096U) return NULL;
    r = (struct cv1k_gles2_renderer *)calloc(1U, sizeof(*r));
    if (r == NULL) return NULL;
    r->display_w = display_w;
    r->display_h = display_h;
    r->use_tile_cache = use_tile_cache ? 1 : 0;
    r->rgba = (cv1k_u8 *)malloc((size_t)display_w * (size_t)display_h * 4U);
    r->tile_rgba = (cv1k_u8 *)malloc((size_t)CV1K_GLES2_TILE_PIXELS * 4U);
    if (r->rgba == NULL || r->tile_rgba == NULL) {
        snprintf(r->error, sizeof(r->error), "out of memory");
        cv1k_gles2_renderer_destroy(r);
        return NULL;
    }
    if (!load_api(r, getproc, userdata) || !build_program(r) || !build_blit_program(r)) {
        cv1k_gles2_renderer_destroy(r);
        return NULL;
    }
    r->gl.Disable(GL_BLEND);
    r->gl.GenTextures(1, &r->tex);
    r->gl.ActiveTexture(GL_TEXTURE0);
    r->gl.BindTexture(GL_TEXTURE_2D, r->tex);
    r->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    r->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    r->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    r->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    r->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)display_w, (GLsizei)display_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    r->gl.GenBuffers(1, &r->vbo);
    r->gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    r->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(16U * sizeof(GLfloat)), NULL, GL_DYNAMIC_DRAW);
    return r;
}

void cv1k_gles2_renderer_set_gpu_blitter(struct cv1k_gles2_renderer *r, struct cv1k_video *video, int enable)
{
    if (r == NULL) return;
    if (r->bound_video != NULL && r->bound_video != video) cv1k_video_set_gpu_accel(r->bound_video, NULL, NULL);
    r->bound_video = enable ? video : NULL;
    r->gpu_blitter_enabled = enable ? 1 : 0;
    if (video != NULL) {
        video->vram_dirty_tracking = (r->use_tile_cache || enable) ? 1 : 0;
        if (video->vram_dirty_tracking) cv1k_video_mark_vram_dirty_all(video);
    }
    if (enable && video != NULL) {
        cv1k_video_set_gpu_accel(video, &gles2_gpu_ops, r);
    } else if (video != NULL) {
        cv1k_video_set_gpu_accel(video, NULL, NULL);
    }
}

void cv1k_gles2_renderer_destroy(struct cv1k_gles2_renderer *r)
{
    cv1k_u32 i;
    if (r == NULL) return;
    if (r->gl.DeleteTextures != NULL) {
        for (i = 0U; i < CV1K_VRAM_TILE_COUNT; i++) {
            if (r->tiles[i].tex != 0U) r->gl.DeleteTextures(1, &r->tiles[i].tex);
        }
        if (r->tex != 0U) r->gl.DeleteTextures(1, &r->tex);
    }
    if (r->gl.DeleteFramebuffers != NULL) {
        for (i = 0U; i < CV1K_VRAM_TILE_COUNT; i++) {
            if (r->tiles[i].fbo != 0U) r->gl.DeleteFramebuffers(1, &r->tiles[i].fbo);
        }
    }
    if (r->gl.DeleteBuffers != NULL && r->vbo != 0U) r->gl.DeleteBuffers(1, &r->vbo);
    if (r->gl.DeleteProgram != NULL && r->program != 0U) r->gl.DeleteProgram(r->program);
    free(r->rgba);
    free(r->tile_rgba);
    free(r);
}

static int present_full_upload(struct cv1k_gles2_renderer *r, const struct cv1k_video *video, int rotation)
{
    GLfloat verts[16] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f
    };
    if (video->executed_ops == 0U && video->screen_rgb != NULL) fill_rgba_from_screen_rgb(video, rotation, r->rgba, r->display_w, r->display_h);
    else fill_rgba_from_vram(video, rotation, r->rgba, r->display_w, r->display_h);
    r->gl.BindFramebuffer(GL_FRAMEBUFFER, 0U);
    r->gl.ActiveTexture(GL_TEXTURE0);
    r->gl.BindTexture(GL_TEXTURE_2D, r->tex);
    r->gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)r->display_w, (GLsizei)r->display_h, GL_RGBA, GL_UNSIGNED_BYTE, r->rgba);
    r->gl.UseProgram(r->program);
    r->gl.Uniform1i(r->u_tex, 0);
    r->gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    r->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(verts), verts, GL_DYNAMIC_DRAW);
    r->gl.EnableVertexAttribArray((GLuint)r->a_pos);
    r->gl.EnableVertexAttribArray((GLuint)r->a_uv);
    r->gl.VertexAttribPointer((GLuint)r->a_pos, 2, GL_FLOAT, GL_FALSE, (GLsizei)(4U * sizeof(GLfloat)), (const void *)0);
    r->gl.VertexAttribPointer((GLuint)r->a_uv, 2, GL_FLOAT, GL_FALSE, (GLsizei)(4U * sizeof(GLfloat)), (const void *)(2U * sizeof(GLfloat)));
    r->gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    r->gl.DisableVertexAttribArray((GLuint)r->a_pos);
    r->gl.DisableVertexAttribArray((GLuint)r->a_uv);
    return 1;
}

int cv1k_gles2_renderer_present(struct cv1k_gles2_renderer *r, const struct cv1k_video *video, int rotation, cv1k_u32 window_w, cv1k_u32 window_h)
{
    if (r == NULL || video == NULL || r->rgba == NULL) return 0;
    r->gl.Viewport(0, 0, (GLsizei)window_w, (GLsizei)window_h);
    r->gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    r->gl.Clear(GL_COLOR_BUFFER_BIT);
    if (r->use_tile_cache && video->executed_ops != 0U) {
        if (present_from_vram_tiles(r, video, rotation)) return 1;
        r->use_tile_cache = 0;
        snprintf(r->error, sizeof(r->error), "GLES2 tile cache failed; using full-frame upload fallback");
    }
    return present_full_upload(r, video, rotation);
}

const char *cv1k_gles2_renderer_error(const struct cv1k_gles2_renderer *r)
{
    if (r == NULL) return "GLES2 renderer unavailable";
    if (r->error[0] == '\0') return "no GLES2 error";
    return r->error;
}

cv1k_u32 cv1k_gles2_renderer_tile_uploads(const struct cv1k_gles2_renderer *r)
{
    return r == NULL ? 0U : r->tile_uploads;
}

cv1k_u32 cv1k_gles2_renderer_tiles_allocated(const struct cv1k_gles2_renderer *r)
{
    return r == NULL ? 0U : r->tiles_allocated;
}

cv1k_u32 cv1k_gles2_renderer_gpu_uploads(const struct cv1k_gles2_renderer *r)
{
    return r == NULL ? 0U : r->gpu_uploads;
}

cv1k_u32 cv1k_gles2_renderer_gpu_draws(const struct cv1k_gles2_renderer *r)
{
    return r == NULL ? 0U : r->gpu_draws;
}

cv1k_u32 cv1k_gles2_renderer_gpu_fallbacks(const struct cv1k_gles2_renderer *r)
{
    return r == NULL ? 0U : r->gpu_fallbacks;
}
