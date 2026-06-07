#ifndef SOUND_YMZ770_H
#define SOUND_YMZ770_H

#include "cv1k_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CV1K_YMZ770_FIFO_SIZE 256U
#define CV1K_YMZ770_CHANNELS 8U
#define CV1K_YMZ770_SEQUENCES 8U

struct cv1k_ymz770_channel {
    cv1k_u8 phrase;
    cv1k_u32 volume;
    cv1k_u8 volume2;
    cv1k_u16 pan;
    cv1k_u8 loop;
    cv1k_u8 pending;
    cv1k_u8 playing;
    cv1k_u8 paused;
    cv1k_u8 last_block;
};

struct cv1k_ymz770_sequence {
    cv1k_u8 sequence;
    cv1k_u16 timer;
    cv1k_u8 stopchan;
    cv1k_u8 loop;
    cv1k_u8 playing;
    cv1k_u8 paused;
    cv1k_u32 offset;
    cv1k_u32 delay;
};

struct cv1k_ymz770 {
    cv1k_u8 regs[256];
    cv1k_u8 fifo[CV1K_YMZ770_FIFO_SIZE];
    cv1k_u32 write_pos;
    cv1k_u32 read_pos;
    cv1k_u32 writes;
    cv1k_u32 generated_samples;
    cv1k_u8 cur_reg;
    cv1k_u8 mute;
    cv1k_u8 doen;
    cv1k_u8 vlma;
    cv1k_u8 bsl;
    cv1k_u8 cpl;
    cv1k_u32 reg_writes;
    cv1k_u32 keyons;
    cv1k_u32 keyoffs;
    struct cv1k_ymz770_channel channels[CV1K_YMZ770_CHANNELS];
    struct cv1k_ymz770_sequence sequences[CV1K_YMZ770_SEQUENCES];
};

void cv1k_ymz770_reset(struct cv1k_ymz770 *ymz);
void cv1k_ymz770_write(struct cv1k_ymz770 *ymz, cv1k_u32 offset, cv1k_u8 data);
void cv1k_ymz770_mix_s16(struct cv1k_ymz770 *ymz, short *mono, cv1k_u32 samples);

/* MAME-derived YMZ770C/AMM path used by the SDL 1.2 backend.  It is kept as
 * a separate API so the existing ANSI C/headless build does not require C++.
 * Output is interleaved signed 16-bit stereo at the YMZ770 sample clock.
 */
void cv1k_ymz770_mix_s16_stereo(struct cv1k_ymz770 *ymz,
                                const cv1k_u8 *rom,
                                cv1k_u32 rom_size,
                                short *stereo,
                                cv1k_u32 samples);

#ifdef __cplusplus
}
#endif

#endif
