/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Constants and helpers adapted from MAME's Cave CV1000 video device:
 *   mame-master/src/mame/cave/cv1k_v.cpp
 *   mame-master/src/mame/cave/cv1k_v.h
 *
 * MAME license: BSD-3-Clause.
 * MAME copyright-holders: David Haywood, Luca Elia, MetalliC.
 *
 * See NOTICE and docs/MAME_DERIVED.md for attribution details.
 */

#ifndef MAME_CV1K_DERIVED_H
#define MAME_CV1K_DERIVED_H

#include "cv1k_types.h"

/*
 * BSD-3-Clause MAME-derived constants and command descriptions.
 * Source: MAME Cave CV1000 driver/video files at commit
 * acad9ca235f4026b1765f62fec340f6d95b2e9ab.
 * Copyright holders named by MAME: David Haywood, Luca Elia, MetalliC.
 */

#define CV1K_BLIT_OP_DRAW   0x1000U
#define CV1K_BLIT_OP_UPLOAD 0x2000U
#define CV1K_BLIT_OP_CLIP   0xc000U
#define CV1K_BLIT_OP_MASK   0xf000U
#define CV1K_DRAW_OPERATION_SIZE_BYTES 20UL
#define CV1K_UPLOAD_HEADER_SIZE_BYTES 16UL
#define CV1K_CLIP_OPERATION_SIZE_BYTES 2UL
#define CV1K_OPERATION_CHUNK_SIZE_BYTES 64UL
#define CV1K_OPERATION_READ_CHUNK_INTERVAL_NS 700UL
#define CV1K_VRAM_CLK_NANOSEC 13UL
#define CV1K_SRAM_CLK_NANOSEC 20UL
#define CV1K_VRAM_H_LINE_PERIOD_NANOSEC 63600UL
#define CV1K_VRAM_H_LINE_DURATION_NANOSEC 2160UL
#define CV1K_FRAME_DURATION_NANOSEC 16666666UL

extern cv1k_u8 cv1k_mame_colrtable[0x20][0x40];
extern cv1k_u8 cv1k_mame_colrtable_rev[0x20][0x40];
extern cv1k_u8 cv1k_mame_colrtable_add[0x20][0x20];

void cv1k_mame_build_color_tables(void);
cv1k_u8 cv1k_mame_mul5(cv1k_u8 x, cv1k_u8 y);
cv1k_u8 cv1k_mame_mul5_rev(cv1k_u8 x, cv1k_u8 y);
cv1k_u8 cv1k_mame_add5(cv1k_u8 x, cv1k_u8 y);

#endif
