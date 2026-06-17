// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
/***************************************************************************

    MPEG audio support.  Only layer2 and variants for now.
    Standalone C23 copy adapted from MAME src/devices/sound/mpeg_audio.*

***************************************************************************/

#ifndef CV1K_MAME_MPEG_AUDIO_H
#define CV1K_MAME_MPEG_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

enum {
    CV1K_MAME_MPEG_AUDIO_L1   = 1,
    CV1K_MAME_MPEG_AUDIO_L2   = 2,
    CV1K_MAME_MPEG_AUDIO_L2_5 = 4,
    CV1K_MAME_MPEG_AUDIO_L3   = 8,
    CV1K_MAME_MPEG_AUDIO_AMM  = 16
};

struct cv1k_mame_mpeg_audio_band_info {
    int modulo;
    double s1;
    int bits, cube_bits;
    int s4, s5;
    double range, s7, scale, offset;
};

typedef struct cv1k_mame_mpeg_audio {
    const uint8_t *m_base;
    int m_accepted, m_position_align;

    int m_sampling_rate, m_last_frame_number;
    int m_param_index, m_cbr_param_index;

    int m_channel_count, m_total_bands, m_joint_bands;

    int m_band_param[2][32];
    int m_scfsi[2][32];
    int m_scf[2][3][32];
    double m_amp_values[2][3][32];
    double m_bdata[2][3][32];
    double m_subbuffer[2][32];
    double m_audio_buffer[2][32 * 32];
    int m_audio_buffer_pos[2];
    double m_cos_cache[32][32];

    int m_current_pos, m_current_limit;
    bool m_limit_hit;

    int (*do_gb)(const unsigned char *data, int *pos, int count);
} cv1k_mame_mpeg_audio;

cv1k_mame_mpeg_audio *cv1k_mame_mpeg_audio_create(const void *base,
                                                   unsigned int accepted,
                                                   bool lsb_first,
                                                   int position_align);
void cv1k_mame_mpeg_audio_destroy(cv1k_mame_mpeg_audio *m);
void cv1k_mame_mpeg_audio_init(cv1k_mame_mpeg_audio *m,
                                const void *base,
                                unsigned int accepted,
                                bool lsb_first,
                                int position_align);
void cv1k_mame_mpeg_audio_clear(cv1k_mame_mpeg_audio *m);

bool cv1k_mame_mpeg_audio_decode_buffer(cv1k_mame_mpeg_audio *m,
                                         int *pos,
                                         int limit,
                                         short *output,
                                         int *output_samples,
                                         int *sample_rate,
                                         int *channels,
                                         int atbl);

#endif /* CV1K_MAME_MPEG_AUDIO_H */
