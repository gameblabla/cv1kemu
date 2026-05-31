#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    p = malloc((size_t)size);
    if (p != NULL) {
        memset(p, 0, (size_t)size);
    }
    return p;
}

void cv1k_free(void *p)
{
    if (p != NULL) free(p);
}

cv1k_u32 cv1k_file_size(const char *path)
{
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
