/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Silent Matroska/ZMBV frame recorder for CV1K headless video checks.
 *
 * The EBML/Matroska packet writer and all-keyframe ZMBV packet layout are
 * adapted from the GP32emu media recorder supplied with this test request.
 * This version intentionally omits audio, threading, and GP32-specific frame
 * conversion so it can be used by the CV1K command-line validation path.
 */
#include "zmbv_recorder.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CV1K_ZMBV_BLOCK 16U
#define CV1K_ZMBV_FMT_32BPP 8U
#define CV1K_ZMBV_KEYFRAME 1U

struct cv1k_membuf {
    cv1k_u8 *p;
    size_t n;
    size_t cap;
};

struct cv1k_zmbv_recorder {
    FILE *f;
    cv1k_u32 width;
    cv1k_u32 height;
    cv1k_u32 refresh_millihz;
    uint64_t current_cluster_ms;
    int cluster_open;
    char err[256];
    cv1k_u8 *zwork;
    size_t zwork_cap;
    cv1k_u8 *zcomp;
    size_t zcomp_cap;
};

static void seterr_buf(char *err, size_t err_len, const char *msg)
{
    if (err != NULL && err_len != 0U) snprintf(err, err_len, "%s", msg != NULL ? msg : "unknown ZMBV recorder error");
}

static void rec_seterr(struct cv1k_zmbv_recorder *rec, const char *msg)
{
    if (rec != NULL) snprintf(rec->err, sizeof(rec->err), "%s", msg != NULL ? msg : "unknown ZMBV recorder error");
}

const char *cv1k_zmbv_recorder_error(const struct cv1k_zmbv_recorder *rec)
{
    return (rec != NULL && rec->err[0] != '\0') ? rec->err : "ZMBV recorder error";
}

static void put_u16be(FILE *f, cv1k_u16 v)
{
    fputc((int)((v >> 8) & 255U), f);
    fputc((int)(v & 255U), f);
}

static void put_u32be(FILE *f, cv1k_u32 v)
{
    put_u16be(f, (cv1k_u16)(v >> 16));
    put_u16be(f, (cv1k_u16)v);
}

static void put_u64be(FILE *f, uint64_t v)
{
    put_u32be(f, (cv1k_u32)(v >> 32));
    put_u32be(f, (cv1k_u32)v);
}

static int mb_reserve(struct cv1k_membuf *b, size_t add)
{
    size_t need;
    size_t cap;
    cv1k_u8 *p;
    if (b == NULL || add > (size_t)-1 - b->n) return 0;
    need = b->n + add;
    if (need <= b->cap) return 1;
    cap = b->cap != 0U ? b->cap * 2U : 256U;
    while (cap < need) {
        if (cap > (size_t)-1 / 2U) { cap = need; break; }
        cap *= 2U;
    }
    p = (cv1k_u8 *)realloc(b->p, cap);
    if (p == NULL) return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static int mb_put(struct cv1k_membuf *b, const void *p, size_t n)
{
    if (!mb_reserve(b, n)) return 0;
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 1;
}

static int mb_u8(struct cv1k_membuf *b, cv1k_u8 v)
{
    return mb_put(b, &v, 1U);
}

static int mb_u16le(struct cv1k_membuf *b, cv1k_u16 v)
{
    cv1k_u8 x[2];
    x[0] = (cv1k_u8)v;
    x[1] = (cv1k_u8)(v >> 8);
    return mb_put(b, x, 2U);
}

static int mb_u32le(struct cv1k_membuf *b, cv1k_u32 v)
{
    return mb_u16le(b, (cv1k_u16)v) && mb_u16le(b, (cv1k_u16)(v >> 16));
}

static int mb_u32be(struct cv1k_membuf *b, cv1k_u32 v)
{
    cv1k_u8 x[4];
    x[0] = (cv1k_u8)(v >> 24);
    x[1] = (cv1k_u8)(v >> 16);
    x[2] = (cv1k_u8)(v >> 8);
    x[3] = (cv1k_u8)v;
    return mb_put(b, x, 4U);
}

static int mb_u64be(struct cv1k_membuf *b, uint64_t v)
{
    return mb_u32be(b, (cv1k_u32)(v >> 32)) && mb_u32be(b, (cv1k_u32)v);
}

static void ebml_id(FILE *f, cv1k_u32 id)
{
    if (id > 0x00ffffffU) {
        fputc((int)(id >> 24), f);
        fputc((int)(id >> 16), f);
        fputc((int)(id >> 8), f);
        fputc((int)id, f);
    } else if (id > 0x0000ffffU) {
        fputc((int)(id >> 16), f);
        fputc((int)(id >> 8), f);
        fputc((int)id, f);
    } else if (id > 0x000000ffU) {
        fputc((int)(id >> 8), f);
        fputc((int)id, f);
    } else {
        fputc((int)id, f);
    }
}

static void ebml_size(FILE *f, uint64_t n)
{
    if (n < 0x7fULL) {
        fputc((int)(0x80U | (cv1k_u8)n), f);
    } else if (n < 0x3fffULL) {
        cv1k_u16 v = (cv1k_u16)(0x4000U | n);
        put_u16be(f, v);
    } else if (n < 0x1fffffULL) {
        fputc((int)(0x20U | (cv1k_u8)(n >> 16)), f);
        put_u16be(f, (cv1k_u16)n);
    } else if (n < 0x0fffffffULL) {
        put_u32be(f, (cv1k_u32)(0x10000000U | n));
    } else if (n < 0x07ffffffffULL) {
        fputc((int)(0x08U | (cv1k_u8)(n >> 32)), f);
        put_u32be(f, (cv1k_u32)n);
    } else if (n < 0x03ffffffffffULL) {
        fputc((int)(0x04U | (cv1k_u8)(n >> 40)), f);
        fputc((int)(n >> 32), f);
        put_u32be(f, (cv1k_u32)n);
    } else if (n < 0x01ffffffffffffULL) {
        fputc((int)(0x02U | (cv1k_u8)(n >> 48)), f);
        fputc((int)(n >> 40), f);
        fputc((int)(n >> 32), f);
        put_u32be(f, (cv1k_u32)n);
    } else {
        put_u64be(f, 0x0100000000000000ULL | n);
    }
}

static void ebml_unknown_size8(FILE *f)
{
    static const cv1k_u8 unknown[8] = {0x01U,0xffU,0xffU,0xffU,0xffU,0xffU,0xffU,0xffU};
    fwrite(unknown, 1U, sizeof(unknown), f);
}

static void ebml_elem(FILE *f, cv1k_u32 id, const void *p, size_t n)
{
    ebml_id(f, id);
    ebml_size(f, (uint64_t)n);
    fwrite(p, 1U, n, f);
}

static void ebml_uint(FILE *f, cv1k_u32 id, uint64_t v)
{
    cv1k_u8 b[8];
    size_t n = 1U;
    int i;
    for (i = 0; i < 8; i++) b[i] = (cv1k_u8)(v >> ((7 - i) * 8));
    while (n < 8U && b[8U - n - 1U] != 0U) n++;
    ebml_elem(f, id, b + 8U - n, n);
}

static int mb_ebml_id(struct cv1k_membuf *b, cv1k_u32 id)
{
    if (id > 0x00ffffffU) return mb_u32be(b, id);
    if (id > 0x0000ffffU) {
        cv1k_u8 x[3];
        x[0] = (cv1k_u8)(id >> 16);
        x[1] = (cv1k_u8)(id >> 8);
        x[2] = (cv1k_u8)id;
        return mb_put(b, x, 3U);
    }
    if (id > 0x000000ffU) {
        cv1k_u8 x[2];
        x[0] = (cv1k_u8)(id >> 8);
        x[1] = (cv1k_u8)id;
        return mb_put(b, x, 2U);
    }
    return mb_u8(b, (cv1k_u8)id);
}

static int mb_ebml_size(struct cv1k_membuf *b, uint64_t n)
{
    if (n < 0x7fULL) return mb_u8(b, (cv1k_u8)(0x80U | n));
    if (n < 0x3fffULL) {
        cv1k_u16 v = (cv1k_u16)(0x4000U | n);
        cv1k_u8 x[2];
        x[0] = (cv1k_u8)(v >> 8);
        x[1] = (cv1k_u8)v;
        return mb_put(b, x, 2U);
    }
    if (n < 0x1fffffULL) {
        cv1k_u8 x[3];
        x[0] = (cv1k_u8)(0x20U | (n >> 16));
        x[1] = (cv1k_u8)(n >> 8);
        x[2] = (cv1k_u8)n;
        return mb_put(b, x, 3U);
    }
    if (n < 0x0fffffffULL) return mb_u32be(b, (cv1k_u32)(0x10000000U | n));
    return mb_u64be(b, 0x0100000000000000ULL | n);
}

static int mb_ebml_elem(struct cv1k_membuf *b, cv1k_u32 id, const void *p, size_t n)
{
    return mb_ebml_id(b, id) && mb_ebml_size(b, (uint64_t)n) && mb_put(b, p, n);
}

static int mb_ebml_uint(struct cv1k_membuf *b, cv1k_u32 id, uint64_t v)
{
    cv1k_u8 x[8];
    size_t n = 1U;
    int i;
    for (i = 0; i < 8; i++) x[i] = (cv1k_u8)(v >> ((7 - i) * 8));
    while (n < 8U && x[8U - n - 1U] != 0U) n++;
    return mb_ebml_elem(b, id, x + 8U - n, n);
}

static int mb_ebml_str(struct cv1k_membuf *b, cv1k_u32 id, const char *s)
{
    return mb_ebml_elem(b, id, s, strlen(s));
}

static int mb_ebml_master(struct cv1k_membuf *b, cv1k_u32 id, const struct cv1k_membuf *child)
{
    return mb_ebml_id(b, id) && mb_ebml_size(b, (uint64_t)child->n) && mb_put(b, child->p, child->n);
}

static int write_header(struct cv1k_zmbv_recorder *rec)
{
    FILE *f = rec->f;
    struct cv1k_membuf ebml = {0}, info = {0}, tracks = {0}, te = {0}, video = {0}, priv = {0};
    uint64_t default_duration_ns;
    int ok = 0;

    if (!mb_ebml_uint(&ebml, 0x4286U, 1U) ||
        !mb_ebml_uint(&ebml, 0x42F7U, 1U) ||
        !mb_ebml_uint(&ebml, 0x42F2U, 4U) ||
        !mb_ebml_uint(&ebml, 0x42F3U, 8U) ||
        !mb_ebml_str(&ebml, 0x4282U, "matroska") ||
        !mb_ebml_uint(&ebml, 0x4287U, 4U) ||
        !mb_ebml_uint(&ebml, 0x4285U, 2U)) goto done;
    ebml_id(f, 0x1A45DFA3U);
    ebml_size(f, (uint64_t)ebml.n);
    fwrite(ebml.p, 1U, ebml.n, f);

    ebml_id(f, 0x18538067U);
    ebml_unknown_size8(f);
    if (!mb_ebml_uint(&info, 0x2AD7B1U, 1000000U) ||
        !mb_ebml_str(&info, 0x4D80U, "cv1k_sandbox") ||
        !mb_ebml_str(&info, 0x5741U, "CV1K silent ZMBV recorder")) goto done;
    ebml_id(f, 0x1549A966U);
    ebml_size(f, (uint64_t)info.n);
    fwrite(info.p, 1U, info.n, f);

    /* BITMAPINFOHEADER codec private for V_MS/VFW/FOURCC ZMBV. */
    if (!mb_u32le(&priv, 40U) ||
        !mb_u32le(&priv, rec->width) ||
        !mb_u32le(&priv, rec->height) ||
        !mb_u16le(&priv, 1U) ||
        !mb_u16le(&priv, 32U) ||
        !mb_put(&priv, "ZMBV", 4U) ||
        !mb_u32le(&priv, rec->width * rec->height * 4U) ||
        !mb_u32le(&priv, 2835U) || !mb_u32le(&priv, 2835U) ||
        !mb_u32le(&priv, 0U) || !mb_u32le(&priv, 0U)) goto done;

    default_duration_ns = rec->refresh_millihz != 0U ? (1000000000000ULL / (uint64_t)rec->refresh_millihz) : 16666667ULL;
    if (!mb_ebml_uint(&video, 0xB0U, rec->width) || !mb_ebml_uint(&video, 0xBAU, rec->height)) goto done;
    if (!mb_ebml_uint(&te, 0xD7U, 1U) ||
        !mb_ebml_uint(&te, 0x73C5U, 1U) ||
        !mb_ebml_uint(&te, 0x83U, 1U) ||
        !mb_ebml_uint(&te, 0x23E383U, default_duration_ns) ||
        !mb_ebml_str(&te, 0x86U, "V_MS/VFW/FOURCC") ||
        !mb_ebml_elem(&te, 0x63A2U, priv.p, priv.n) ||
        !mb_ebml_master(&te, 0xE0U, &video)) goto done;
    if (!mb_ebml_master(&tracks, 0xAEU, &te)) goto done;
    ebml_id(f, 0x1654AE6BU);
    ebml_size(f, (uint64_t)tracks.n);
    fwrite(tracks.p, 1U, tracks.n, f);
    ok = ferror(f) == 0;

done:
    free(ebml.p);
    free(info.p);
    free(tracks.p);
    free(te.p);
    free(video.p);
    free(priv.p);
    return ok;
}

static int ensure_cluster(struct cv1k_zmbv_recorder *rec, uint64_t timestamp_ms)
{
    if (!rec->cluster_open || timestamp_ms < rec->current_cluster_ms || timestamp_ms - rec->current_cluster_ms > 30000ULL) {
        ebml_id(rec->f, 0x1F43B675U);
        ebml_unknown_size8(rec->f);
        rec->current_cluster_ms = timestamp_ms;
        ebml_uint(rec->f, 0xE7U, timestamp_ms);
        rec->cluster_open = 1;
    }
    return ferror(rec->f) == 0;
}

static int write_simple_block(struct cv1k_zmbv_recorder *rec, cv1k_u8 track, uint64_t timestamp_ms, cv1k_u8 flags, const void *data, size_t bytes)
{
    int64_t rel;
    if (!ensure_cluster(rec, timestamp_ms)) return 0;
    rel = (int64_t)timestamp_ms - (int64_t)rec->current_cluster_ms;
    if (rel < -32768 || rel > 32767) {
        rec_seterr(rec, "Matroska relative timestamp overflow");
        return 0;
    }
    ebml_id(rec->f, 0xA3U);
    ebml_size(rec->f, bytes + 4U);
    fputc((int)(0x80U | track), rec->f);
    put_u16be(rec->f, (cv1k_u16)(int16_t)rel);
    fputc((int)flags, rec->f);
    fwrite(data, 1U, bytes, rec->f);
    return ferror(rec->f) == 0;
}

static int ensure_zbuf(struct cv1k_zmbv_recorder *rec, size_t raw)
{
    size_t comp = (size_t)compressBound((uLong)raw);
    cv1k_u8 *p;
    if (rec->zwork_cap < raw) {
        p = (cv1k_u8 *)realloc(rec->zwork, raw);
        if (p == NULL) return 0;
        rec->zwork = p;
        rec->zwork_cap = raw;
    }
    if (rec->zcomp_cap < comp) {
        p = (cv1k_u8 *)realloc(rec->zcomp, comp);
        if (p == NULL) return 0;
        rec->zcomp = p;
        rec->zcomp_cap = comp;
    }
    return 1;
}

struct cv1k_zmbv_recorder *cv1k_zmbv_recorder_open(const char *path, cv1k_u32 width, cv1k_u32 height, cv1k_u32 refresh_millihz, char *err, size_t err_len)
{
    struct cv1k_zmbv_recorder *rec;
    if (path == NULL || path[0] == '\0' || width == 0U || height == 0U) {
        seterr_buf(err, err_len, "invalid ZMBV recorder path or dimensions");
        return NULL;
    }
    rec = (struct cv1k_zmbv_recorder *)calloc(1U, sizeof(*rec));
    if (rec == NULL) {
        seterr_buf(err, err_len, "out of memory opening ZMBV recorder");
        return NULL;
    }
    rec->width = width;
    rec->height = height;
    rec->refresh_millihz = refresh_millihz != 0U ? refresh_millihz : 60000U;
    rec->f = fopen(path, "wb");
    if (rec->f == NULL) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "ZMBV recording open failed: %s", strerror(errno));
        seterr_buf(err, err_len, tmp);
        free(rec);
        return NULL;
    }
    if (!write_header(rec)) {
        fclose(rec->f);
        free(rec);
        seterr_buf(err, err_len, "failed to write ZMBV Matroska header");
        return NULL;
    }
    return rec;
}

int cv1k_zmbv_recorder_add_xrgb8888(struct cv1k_zmbv_recorder *rec,
                                    const cv1k_u32 *pixels,
                                    cv1k_u32 width,
                                    cv1k_u32 height,
                                    cv1k_u32 stride_pixels,
                                    uint64_t frame_index,
                                    cv1k_u32 *out_hash,
                                    cv1k_u32 *out_nonzero)
{
    size_t pixel_count;
    size_t raw;
    cv1k_u8 *packet;
    size_t packet_len;
    uLongf comp_len;
    uint64_t timestamp_ms;
    cv1k_u32 hash = 2166136261U;
    cv1k_u32 nonzero = 0U;
    cv1k_u32 x;
    cv1k_u32 y;
    int ok;

    if (rec == NULL || pixels == NULL || width != rec->width || height != rec->height || stride_pixels < width) return 0;
    pixel_count = (size_t)width * (size_t)height;
    if (width != 0U && pixel_count / width != height) {
        rec_seterr(rec, "ZMBV frame dimensions overflow");
        return 0;
    }
    raw = pixel_count * 4U;
    if (!ensure_zbuf(rec, raw)) {
        rec_seterr(rec, "out of memory compressing ZMBV frame");
        return 0;
    }
    for (y = 0U; y < height; y++) {
        const cv1k_u32 *row = pixels + (size_t)y * stride_pixels;
        cv1k_u8 *dst = rec->zwork + (size_t)y * width * 4U;
        for (x = 0U; x < width; x++) {
            cv1k_u32 p = row[x] & 0x00ffffffU;
            if (p != 0U) nonzero++;
            hash ^= p;
            hash *= 16777619U;
            dst[x * 4U + 0U] = (cv1k_u8)(p & 255U);
            dst[x * 4U + 1U] = (cv1k_u8)((p >> 8) & 255U);
            dst[x * 4U + 2U] = (cv1k_u8)((p >> 16) & 255U);
            dst[x * 4U + 3U] = 0U;
        }
    }
    comp_len = (uLongf)rec->zcomp_cap;
    if (compress2((Bytef *)rec->zcomp, &comp_len, (const Bytef *)rec->zwork, (uLong)raw, 6) != Z_OK) {
        rec_seterr(rec, "ZMBV deflate failed");
        return 0;
    }
    packet_len = 7U + (size_t)comp_len;
    packet = (cv1k_u8 *)malloc(packet_len);
    if (packet == NULL) {
        rec_seterr(rec, "out of memory writing ZMBV frame");
        return 0;
    }
    packet[0] = CV1K_ZMBV_KEYFRAME;
    packet[1] = 0U;
    packet[2] = 1U;
    packet[3] = 1U;
    packet[4] = CV1K_ZMBV_FMT_32BPP;
    packet[5] = CV1K_ZMBV_BLOCK;
    packet[6] = CV1K_ZMBV_BLOCK;
    memcpy(packet + 7U, rec->zcomp, (size_t)comp_len);
    timestamp_ms = ((uint64_t)frame_index * 1000000ULL) / (uint64_t)rec->refresh_millihz;
    ok = write_simple_block(rec, 1U, timestamp_ms, 0x80U, packet, packet_len);
    free(packet);
    if (!ok) {
        if (rec->err[0] == '\0') rec_seterr(rec, "failed to write ZMBV video block");
        return 0;
    }
    if (out_hash != NULL) *out_hash = hash;
    if (out_nonzero != NULL) *out_nonzero = nonzero;
    return 1;
}

int cv1k_zmbv_recorder_close(struct cv1k_zmbv_recorder *rec)
{
    int ok = 1;
    if (rec == NULL) return 1;
    if (rec->f != NULL) {
        ok = ferror(rec->f) == 0;
        if (fclose(rec->f) != 0) ok = 0;
    }
    free(rec->zwork);
    free(rec->zcomp);
    free(rec);
    return ok;
}

void cv1k_zmbv_recorder_abort(struct cv1k_zmbv_recorder *rec)
{
    if (rec == NULL) return;
    if (rec->f != NULL) fclose(rec->f);
    free(rec->zwork);
    free(rec->zcomp);
    free(rec);
}
