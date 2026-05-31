#ifndef CV1K_PLATFORM_H
#define CV1K_PLATFORM_H

#include "cv1k_types.h"

int cv1k_platform_check(void);
void *cv1k_xmalloc(cv1k_u32 size);
void cv1k_free(void *p);
cv1k_u32 cv1k_file_size(const char *path);
int cv1k_read_file(const char *path, cv1k_u8 *dst, cv1k_u32 max_size, cv1k_u32 *out_size);
int cv1k_write_file(const char *path, const cv1k_u8 *src, cv1k_u32 size);
cv1k_u32 cv1k_now_unix(void);

cv1k_u16 cv1k_be16(const cv1k_u8 *p);
cv1k_u32 cv1k_be32(const cv1k_u8 *p);
void cv1k_put_be16(cv1k_u8 *p, cv1k_u16 v);
void cv1k_put_be32(cv1k_u8 *p, cv1k_u32 v);

#endif
