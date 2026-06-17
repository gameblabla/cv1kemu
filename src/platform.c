#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#ifdef CV1K_ENABLE_VFS
#define CV1K_VFS_MAX_FILES 16
struct cv1k_vfs_file {
    char path[192];
    cv1k_u8 *data;
    cv1k_u32 size;
};
static struct cv1k_vfs_file g_vfs[CV1K_VFS_MAX_FILES];

static int vfs_find(const char *path)
{
    int i;
    if (path == NULL) return -1;
    for (i = 0; i < CV1K_VFS_MAX_FILES; i++) {
        if (g_vfs[i].path[0] != '\0' && strcmp(g_vfs[i].path, path) == 0) return i;
    }
    return -1;
}

static int vfs_alloc_slot(const char *path)
{
    int i;
    i = vfs_find(path);
    if (i >= 0) return i;
    for (i = 0; i < CV1K_VFS_MAX_FILES; i++) if (g_vfs[i].path[0] == '\0') return i;
    return -1;
}

void cv1k_vfs_clear(void)
{
    int i;
    for (i = 0; i < CV1K_VFS_MAX_FILES; i++) {
        free(g_vfs[i].data);
        memset(&g_vfs[i], 0, sizeof(g_vfs[i]));
    }
}

int cv1k_vfs_remove_file(const char *path)
{
    int i;
    i = vfs_find(path);
    if (i < 0) return 0;
    free(g_vfs[i].data);
    memset(&g_vfs[i], 0, sizeof(g_vfs[i]));
    return 1;
}

int cv1k_vfs_add_file_copy(const char *path, const cv1k_u8 *data, cv1k_u32 size)
{
    int slot;
    cv1k_u8 *copy;
    if (path == NULL || data == NULL || size == 0UL) return 0;
    slot = vfs_alloc_slot(path);
    if (slot < 0) return 0;
    copy = (cv1k_u8 *)malloc((size_t)size);
    if (copy == NULL) return 0;
    memcpy(copy, data, (size_t)size);
    free(g_vfs[slot].data);
    memset(&g_vfs[slot], 0, sizeof(g_vfs[slot]));
    strncpy(g_vfs[slot].path, path, sizeof(g_vfs[slot].path) - 1U);
    g_vfs[slot].data = copy;
    g_vfs[slot].size = size;
    return 1;
}
#endif


int cv1k_platform_check(void)
{
    if (sizeof(cv1k_u8) != 1U) return 0;
    if (sizeof(cv1k_u16) < 2U) return 0;
    if (sizeof(cv1k_u32) != 4U) return 0;
    return 1;
}

void *cv1k_xmalloc(cv1k_u32 size)
{
    void *p;
    size_t n;
    n = (size_t)size;
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(CV1K_WASM)
    {
        size_t align;
        size_t padded;
        align = (size_t)CV1K_CACHE_ALIGN;
        padded = (n + align - 1U) & ~(align - 1U);
        if (padded < n) return NULL;
        p = aligned_alloc(align, padded);
        if (p != NULL) {
            memset(p, 0, padded);
            return p;
        }
    }
#endif
    p = malloc(n);
    if (p != NULL) memset(p, 0, n);
    return p;
}

void cv1k_free(void *p)
{
    if (p != NULL) free(p);
}

cv1k_u32 cv1k_file_size(const char *path)
{
#ifdef CV1K_ENABLE_VFS
    int vi;
    vi = vfs_find(path);
    if (vi >= 0) return g_vfs[vi].size;
#ifdef CV1K_VFS_ONLY
    return 0UL;
#endif
#endif
    FILE *f;
    long endpos;
    f = fopen(path, "rb");
    if (f == NULL) return 0UL;
    if (fseek(f, 0L, SEEK_END) != 0) {
        fclose(f);
        return 0UL;
    }
    endpos = ftell(f);
    fclose(f);
    if (endpos < 0L) return 0UL;
    return (cv1k_u32)endpos;
}

int cv1k_read_file(const char *path, cv1k_u8 *dst, cv1k_u32 max_size, cv1k_u32 *out_size)
{
#ifdef CV1K_ENABLE_VFS
    int vi;
    vi = vfs_find(path);
    if (vi >= 0) {
        cv1k_u32 n = g_vfs[vi].size;
        if (n > max_size) n = max_size;
        if (n != 0UL && dst != NULL) memcpy(dst, g_vfs[vi].data, (size_t)n);
        if (out_size != NULL) *out_size = n;
        return 1;
    }
#ifdef CV1K_VFS_ONLY
    return 0;
#endif
#endif
    FILE *f;
    size_t n;
    f = fopen(path, "rb");
    if (f == NULL) return 0;
    n = fread(dst, 1U, (size_t)max_size, f);
    fclose(f);
    if (out_size != NULL) *out_size = (cv1k_u32)n;
    return 1;
}

int cv1k_write_file(const char *path, const cv1k_u8 *src, cv1k_u32 size)
{
#ifdef CV1K_ENABLE_VFS
    if (path != NULL && src != NULL) return cv1k_vfs_add_file_copy(path, src, size);
#ifdef CV1K_VFS_ONLY
    return 0;
#endif
#endif
    FILE *f;
    size_t n;
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    n = fwrite(src, 1U, (size_t)size, f);
    fclose(f);
    return n == (size_t)size;
}

cv1k_u32 cv1k_now_unix(void)
{
    return (cv1k_u32)time(NULL);
}

cv1k_u16 cv1k_be16(const cv1k_u8 *p)
{
    cv1k_u16 v;
    v = (cv1k_u16)(((cv1k_u16)p[0] << 8) | (cv1k_u16)p[1]);
    return v;
}

cv1k_u32 cv1k_be32(const cv1k_u8 *p)
{
    cv1k_u32 v;
    v = (((cv1k_u32)p[0]) << 24) | (((cv1k_u32)p[1]) << 16) | (((cv1k_u32)p[2]) << 8) | ((cv1k_u32)p[3]);
    return v;
}

void cv1k_put_be16(cv1k_u8 *p, cv1k_u16 v)
{
    p[0] = (cv1k_u8)((v >> 8) & 0xffU);
    p[1] = (cv1k_u8)(v & 0xffU);
}

void cv1k_put_be32(cv1k_u8 *p, cv1k_u32 v)
{
    p[0] = (cv1k_u8)((v >> 24) & 0xffUL);
    p[1] = (cv1k_u8)((v >> 16) & 0xffUL);
    p[2] = (cv1k_u8)((v >> 8) & 0xffUL);
    p[3] = (cv1k_u8)(v & 0xffUL);
}
