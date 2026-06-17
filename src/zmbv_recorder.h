#ifndef CV1K_ZMBV_RECORDER_H
#define CV1K_ZMBV_RECORDER_H

#include "cv1k_types.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cv1k_zmbv_recorder;

struct cv1k_zmbv_recorder *cv1k_zmbv_recorder_open(const char *path,
                                                    cv1k_u32 width,
                                                    cv1k_u32 height,
                                                    cv1k_u32 refresh_millihz,
                                                    char *err,
                                                    size_t err_len);

int cv1k_zmbv_recorder_add_xrgb8888(struct cv1k_zmbv_recorder *rec,
                                    const cv1k_u32 *pixels,
                                    cv1k_u32 width,
                                    cv1k_u32 height,
                                    cv1k_u32 stride_pixels,
                                    uint64_t frame_index,
                                    cv1k_u32 *out_hash,
                                    cv1k_u32 *out_nonzero);

int cv1k_zmbv_recorder_close(struct cv1k_zmbv_recorder *rec);
void cv1k_zmbv_recorder_abort(struct cv1k_zmbv_recorder *rec);
const char *cv1k_zmbv_recorder_error(const struct cv1k_zmbv_recorder *rec);

#ifdef __cplusplus
}
#endif

#endif
