// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
/***************************************************************************

    MPEG audio support.  Only layer2 and variants for now.

***************************************************************************/

#include "mame_mpeg_audio.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int s_sample_rates[8];
static const int s_layer2_param_index[2][4][16];
static const int s_band_parameter_indexed_values[5][32][17];
static const int s_band_parameter_index_bits_count[5][32];
static const int s_total_band_counts[5];
static const int s_joint_band_counts[4];
static const struct cv1k_mame_mpeg_audio_band_info s_band_infos[18];
static const double s_scalefactors[64];
static const double s_synthesis_filter[512];
static int mpeg_do_gb_msb(const unsigned char *data, int *pos, int count);
static int mpeg_do_gb_lsb(const unsigned char *data, int *pos, int count);
static int mpeg_gb(cv1k_mame_mpeg_audio *m, int count);
static void mpeg_read_header_amm(cv1k_mame_mpeg_audio *m, bool layer25);
static void mpeg_read_header_mpeg2(cv1k_mame_mpeg_audio *m, bool layer25);
static void mpeg_read_data_mpeg2(cv1k_mame_mpeg_audio *m);
static void mpeg_decode_mpeg2(cv1k_mame_mpeg_audio *m, short *output, int *output_samples);
static int mpeg_get_band_param(cv1k_mame_mpeg_audio *m, int band);
static void mpeg_read_band_params(cv1k_mame_mpeg_audio *m);
static void mpeg_read_scfci(cv1k_mame_mpeg_audio *m);
static void mpeg_read_band_amplitude_params(cv1k_mame_mpeg_audio *m);
static void mpeg_build_amplitudes(cv1k_mame_mpeg_audio *m);
static void mpeg_read_band_value_triplet(cv1k_mame_mpeg_audio *m, int chan, int band);
static void mpeg_build_next_segments(cv1k_mame_mpeg_audio *m, int step);
static void mpeg_retrieve_subbuffer(cv1k_mame_mpeg_audio *m, int step);
static void mpeg_idct32(cv1k_mame_mpeg_audio *m, const double *input, double *output);
static void mpeg_resynthesis(const double *input, double *output);
static void mpeg_scale_and_clamp(const double *input, short *output, int step);

void cv1k_mame_mpeg_audio_init(cv1k_mame_mpeg_audio *m, const void *base, unsigned int accepted, bool lsb_first, int position_align)
{
    if (m == NULL) return;
    memset(m, 0, sizeof(*m));
    m->m_base = (const uint8_t *)base;
    m->m_accepted = (int)accepted;
    m->do_gb = lsb_first ? mpeg_do_gb_lsb : mpeg_do_gb_msb;
    m->m_position_align = position_align ? position_align - 1 : 0;

    for (int i = 0; i < 32; i++) {
        for (int j = 0; j < 32; j++)
            m->m_cos_cache[i][j] = cos(i * (2 * j + 1) * M_PI / 64.0);
    }

    cv1k_mame_mpeg_audio_clear(m);
}

cv1k_mame_mpeg_audio *cv1k_mame_mpeg_audio_create(const void *base, unsigned int accepted, bool lsb_first, int position_align)
{
    cv1k_mame_mpeg_audio *m = (cv1k_mame_mpeg_audio *)calloc(1, sizeof(*m));
    if (m != NULL) cv1k_mame_mpeg_audio_init(m, base, accepted, lsb_first, position_align);
    return m;
}

void cv1k_mame_mpeg_audio_destroy(cv1k_mame_mpeg_audio *m)
{
    free(m);
}

void cv1k_mame_mpeg_audio_clear(cv1k_mame_mpeg_audio *m)
{
    if (m == NULL) return;
    memset(m->m_audio_buffer, 0, sizeof(m->m_audio_buffer));
    m->m_audio_buffer_pos[0] = 16 * 32;
    m->m_audio_buffer_pos[1] = 16 * 32;
}

static int mpeg_gb(cv1k_mame_mpeg_audio *m, int count)
{
    if (m->m_current_pos + count > m->m_current_limit) {
        m->m_limit_hit = true;
        m->m_current_pos = m->m_current_limit;
        return 0;
    }
    return m->do_gb(m->m_base, &m->m_current_pos, count);
}

bool cv1k_mame_mpeg_audio_decode_buffer(cv1k_mame_mpeg_audio *m, int *pos, int limit, short *output,
                                         int *output_samples, int *sample_rate, int *channels, int atbl)
{
    if (m == NULL || pos == NULL || output == NULL || output_samples == NULL || sample_rate == NULL || channels == NULL)
        return false;
    if (limit - *pos < 16)
        return false;

    m->m_current_pos = *pos;
    m->m_current_limit = limit;
    m->m_cbr_param_index = atbl;
    m->m_limit_hit = false;
    unsigned short sync = (unsigned short)m->do_gb(m->m_base, &m->m_current_pos, 12);

retry_sync:
    while (sync != 0xfff && m->m_current_pos < limit)
        sync = (unsigned short)(((sync << 1) | m->do_gb(m->m_base, &m->m_current_pos, 1)) & 0xfff);

    if (limit - m->m_current_pos < 4)
        return false;

    int layer = 0;
    int variant = mpeg_gb(m, 3);
    switch (variant) {
    case 2:
        if (m->m_accepted & CV1K_MAME_MPEG_AUDIO_L2_5)
            layer = 2;
        else if (m->m_accepted & CV1K_MAME_MPEG_AUDIO_AMM)
            layer = 4;
        break;
    case 5:
        if (m->m_accepted & CV1K_MAME_MPEG_AUDIO_L3)
            layer = 3;
        break;
    case 6:
        if (m->m_accepted & (CV1K_MAME_MPEG_AUDIO_L2 | CV1K_MAME_MPEG_AUDIO_L2_5))
            layer = 2;
        else if (m->m_accepted & CV1K_MAME_MPEG_AUDIO_AMM)
            layer = 4;
        break;
    case 7:
        if (m->m_accepted & CV1K_MAME_MPEG_AUDIO_L1)
            layer = 1;
        break;
    }

    if (!layer) {
        m->m_current_pos -= 3;
        sync = (unsigned short)(((sync << 1) | m->do_gb(m->m_base, &m->m_current_pos, 1)) & 0xfff);
        goto retry_sync;
    }

    switch (layer) {
    case 1:
        abort();
    case 2:
        mpeg_read_header_mpeg2(m, variant == 2);
        if (m->m_limit_hit) return false;
        mpeg_read_data_mpeg2(m);
        if (m->m_limit_hit) return false;
        mpeg_decode_mpeg2(m, output, output_samples);
        break;
    case 3:
        abort();
    case 4:
        mpeg_read_header_amm(m, variant == 2);
        if (m->m_limit_hit) return false;
        mpeg_read_data_mpeg2(m);
        if (m->m_limit_hit) return false;
        if (m->m_last_frame_number)
            mpeg_decode_mpeg2(m, output, output_samples);
        else
            *output_samples = 0;
        break;
    }

    if (m->m_position_align)
        m->m_current_pos = (m->m_current_pos + m->m_position_align) & ~m->m_position_align;

    *pos = m->m_current_pos;
    *sample_rate = s_sample_rates[m->m_sampling_rate];
    *channels = m->m_channel_count;
    return true;
}

static void mpeg_read_header_amm(cv1k_mame_mpeg_audio *m, bool layer25)
{
	int ammsl = mpeg_gb(m, 1); // 1 = older AMM variant, CBR and constant frame size
	int full_packets_count = mpeg_gb(m, 4); // max 12
	int srate_index = mpeg_gb(m, 2); // max 2
	m->m_sampling_rate = srate_index + 4 * layer25;
	int last_packet_frame_id = mpeg_gb(m, 2); // max 2
	int stereo_mode = mpeg_gb(m, 2);
	int stereo_mode_ext = mpeg_gb(m, 2);
	if (ammsl)
	{
		m->m_last_frame_number = 36;
		m->m_param_index = m->m_cbr_param_index; // CBR, m->m_param_index came from sample pointer top bits
		mpeg_gb(m, 3);
	}
	else
	{
		m->m_last_frame_number = 3*full_packets_count + last_packet_frame_id;
		m->m_param_index = mpeg_gb(m, 3);
	}
	mpeg_gb(m, 1); // must be zero

	m->m_channel_count = stereo_mode != 3 ? 2 : 1;

	m->m_total_bands = s_total_band_counts[m->m_param_index];
	m->m_joint_bands = m->m_total_bands;
	if(stereo_mode == 1) // joint stereo
		m->m_joint_bands = s_joint_band_counts[stereo_mode_ext];
	if(m->m_joint_bands > m->m_total_bands)
		m->m_joint_bands = m->m_total_bands;
}

static void mpeg_read_header_mpeg2(cv1k_mame_mpeg_audio *m, bool layer25)
{
	(void)layer25;
	int prot = mpeg_gb(m, 1);
	int bitrate_index = mpeg_gb(m, 4);
	m->m_sampling_rate = mpeg_gb(m, 2);
	mpeg_gb(m, 1); // padding
	mpeg_gb(m, 1);
	m->m_last_frame_number = 36;
	int stereo_mode = mpeg_gb(m, 2);
	int stereo_mode_ext = mpeg_gb(m, 2);
	mpeg_gb(m, 2); // copyright, original
	mpeg_gb(m, 2); // emphasis
	if(!prot)
		mpeg_gb(m, 16); // crc

	m->m_channel_count = stereo_mode != 3 ? 2 : 1;

	m->m_param_index = s_layer2_param_index[m->m_channel_count-1][m->m_sampling_rate][bitrate_index];
	assert(m->m_param_index != -1);

	m->m_total_bands = s_total_band_counts[m->m_param_index];
	m->m_joint_bands = m->m_total_bands;
	if(stereo_mode == 1) // joint stereo
		m->m_joint_bands = s_joint_band_counts[stereo_mode_ext];
	if(m->m_joint_bands > m->m_total_bands)
		m->m_joint_bands = m->m_total_bands;
}

static void mpeg_read_data_mpeg2(cv1k_mame_mpeg_audio *m)
{
	mpeg_read_band_params(m);
	mpeg_read_scfci(m);
	mpeg_read_band_amplitude_params(m);
}

static void mpeg_decode_mpeg2(cv1k_mame_mpeg_audio *m, short *output, int *output_samples)
{
	*output_samples = 0;
	mpeg_build_amplitudes(m);

	// Supposed to stop at m->m_last_frame_number when it's not 12*3+2 = 38
	int frame_number = 0;
	for(int upper_step = 0; upper_step<3; upper_step++)
		for(int middle_step = 0; middle_step < 4; middle_step++) {
			mpeg_build_next_segments(m, upper_step);
			for(int lower_step = 0; lower_step < 3; lower_step++) {
				mpeg_retrieve_subbuffer(m, lower_step);

				for(int chan=0; chan<m->m_channel_count; chan++) {
					double resynthesis_buffer[32];
					mpeg_idct32(m, m->m_subbuffer[chan], m->m_audio_buffer[chan] + m->m_audio_buffer_pos[chan]);
					mpeg_resynthesis(m->m_audio_buffer[chan] + m->m_audio_buffer_pos[chan] + 16, resynthesis_buffer);
					mpeg_scale_and_clamp(resynthesis_buffer, output + chan, m->m_channel_count);
					m->m_audio_buffer_pos[chan] -= 32;
					if(m->m_audio_buffer_pos[chan]<0) {
						memmove(m->m_audio_buffer[chan]+17*32, m->m_audio_buffer[chan], 15*32*sizeof(m->m_audio_buffer[chan][0]));
						m->m_audio_buffer_pos[chan] = 16*32;
					}
				}
				output += 32*m->m_channel_count;
				*output_samples += 32;
				frame_number++;
				if(frame_number == m->m_last_frame_number)
					return;
			}
		}
}

static const int s_sample_rates[8] = { 44100, 48000, 32000, 0, 22050, 24000, 16000, 0 };

static const int s_layer2_param_index[2][4][16] = {
	{
		{  1,  2,  2,  0,  0,  0,  1,  1,  1,  1,  1, -1, -1, -1, -1, -1 },
		{  0,  2,  2,  0,  0,  0,  0,  0,  0,  0,  0, -1, -1, -1, -1, -1 },
		{  1,  3,  3,  0,  0,  0,  1,  1,  1,  1,  1, -1, -1, -1, -1, -1 },
		{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
	},
	{
		{  1, -1, -1, -1,  2, -1,  2,  0,  0,  0,  1,  1,  1,  1,  1, -1 },
		{  0, -1, -1, -1,  2, -1,  2,  0,  0,  0,  0,  0,  0,  0,  0, -1 },
		{  1, -1, -1, -1,  3, -1,  3,  0,  0,  0,  1,  1,  1,  1,  1, -1 },
		{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
	}
};

static const int s_band_parameter_indexed_values[5][32][17] = {
	{
		{  0,  1,  3,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, -1, },
		{  0,  1,  3,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, -1, },
		{  0,  1,  3,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
	},
	{
		{  0,  1,  3,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, -1, },
		{  0,  1,  3,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, -1, },
		{  0,  1,  3,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 17, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  3,  4,  5,  6, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
	},
	{
		{  0,  1,  2,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
	},
	{
		{  0,  1,  2,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
	},
	{
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, -1, },
		{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0,  1,  2,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
		{  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, },
	}
};

static const int s_band_parameter_index_bits_count[5][32] = {
	{ 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 0, 0, 0, 0, 0, },
	{ 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 0, 0, },
	{ 4, 4, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
	{ 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
	{ 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, },
};

static const int s_total_band_counts[5] = { 27, 30, 8, 12, 30 };

static const int s_joint_band_counts[4] = { 4, 8, 12, 16 };

static const struct cv1k_mame_mpeg_audio_band_info s_band_infos[18] = {
	{ 0x0000,  0.00,  0,  0, 0,  0,           0,          0,                 0,         0 },
	{ 0x0003,  7.00,  2,  5, 3,  9, 1-1.0/    4, -1.0/    4,   1/(1-1.0/    4), 1.0/    2 },
	{ 0x0005, 11.00,  3,  7, 5, 25, 1-3.0/    8, -3.0/    8,   1/(1-3.0/    8), 1.0/    2 },
	{ 0x0007, 16.00,  3,  9, 0,  0, 1-1.0/    8, -1.0/    8,   1/(1-1.0/    8), 1.0/    4 },
	{ 0x0009, 20.84,  4, 10, 9, 81, 1-7.0/   16, -7.0/   16,   1/(1-7.0/   16), 1.0/    2 },
	{ 0x000f, 25.28,  4, 12, 0,  0, 1-1.0/   16, -1.0/   16,   1/(1-1.0/   16), 1.0/    8 },
	{ 0x001f, 31.59,  5, 15, 0,  0, 1-1.0/   32, -1.0/   32,   1/(1-1.0/   32), 1.0/   16 },
	{ 0x003f, 37.75,  6, 18, 0,  0, 1-1.0/   64, -1.0/   64,   1/(1-1.0/   64), 1.0/   32 },
	{ 0x007f, 43.84,  7, 21, 0,  0, 1-1.0/  128, -1.0/  128,   1/(1-1.0/  128), 1.0/   64 },
	{ 0x00ff, 49.89,  8, 24, 0,  0, 1-1.0/  256, -1.0/  256,   1/(1-1.0/  256), 1.0/  128 },
	{ 0x01ff, 55.93,  9, 27, 0,  0, 1-1.0/  512, -1.0/  512,   1/(1-1.0/  512), 1.0/  256 },
	{ 0x03ff, 61.96, 10, 30, 0,  0, 1-1.0/ 1024, -1.0/ 1024,   1/(1-1.0/ 1024), 1.0/  512 },
	{ 0x07ff, 67.98, 11, 33, 0,  0, 1-1.0/ 2048, -1.0/ 2048,   1/(1-1.0/ 2048), 1.0/ 1024 },
	{ 0x0fff, 74.01, 12, 36, 0,  0, 1-1.0/ 4096, -1.0/ 4096,   1/(1-1.0/ 4096), 1.0/ 2048 },
	{ 0x1fff, 80.03, 13, 39, 0,  0, 1-1.0/ 8192, -1.0/ 8192,   1/(1-1.0/ 8192), 1.0/ 4096 },
	{ 0x3fff, 86.05, 14, 42, 0,  0, 1-1.0/16384, -1.0/16384,   1/(1-1.0/16384), 1.0/ 8192 },
	{ 0x7fff, 92.01, 15, 45, 0,  0, 1-1.0/32768, -1.0/32768,   1/(1-1.0/32768), 1.0/16384 },
	{ 0xffff, 98.01, 16, 48, 0,  0, 1-1.0/65536, -1.0/65536,   1/(1-1.0/65536), 1.0/32768 },
};

static const double s_scalefactors[64] = {
	2.00000000000000, 1.58740105196820, 1.25992104989487, 1.00000000000000,
	0.79370052598410, 0.62996052494744, 0.50000000000000, 0.39685026299205,
	0.31498026247372, 0.25000000000000, 0.19842513149602, 0.15749013123686,
	0.12500000000000, 0.09921256574801, 0.07874506561843, 0.06250000000000,
	0.04960628287401, 0.03937253280921, 0.03125000000000, 0.02480314143700,
	0.01968626640461, 0.01562500000000, 0.01240157071850, 0.00984313320230,
	0.00781250000000, 0.00620078535925, 0.00492156660115, 0.00390625000000,
	0.00310039267963, 0.00246078330058, 0.00195312500000, 0.00155019633981,
	0.00123039165029, 0.00097656250000, 0.00077509816991, 0.00061519582514,
	0.00048828125000, 0.00038754908495, 0.00030759791257, 0.00024414062500,
	0.00019377454248, 0.00015379895629, 0.00012207031250, 0.00009688727124,
	0.00007689947814, 0.00006103515625, 0.00004844363562, 0.00003844973907,
	0.00003051757812, 0.00002422181781, 0.00001922486954, 0.00001525878906,
	0.00001211090890, 0.00000961243477, 0.00000762939453, 0.00000605545445,
	0.00000480621738, 0.00000381469727, 0.00000302772723, 0.00000240310869,
	0.00000190734863, 0.00000151386361, 0.00000120155435, 0.00000000000000
};

static const double s_synthesis_filter[512] = {
	+0.000000000, -0.000015259, -0.000015259, -0.000015259, -0.000015259, -0.000015259, -0.000015259, -0.000030518,
	-0.000030518, -0.000030518, -0.000030518, -0.000045776, -0.000045776, -0.000061035, -0.000061035, -0.000076294,
	-0.000076294, -0.000091553, -0.000106812, -0.000106812, -0.000122070, -0.000137329, -0.000152588, -0.000167847,
	-0.000198364, -0.000213623, -0.000244141, -0.000259399, -0.000289917, -0.000320435, -0.000366211, -0.000396729,
	-0.000442505, -0.000473022, -0.000534058, -0.000579834, -0.000625610, -0.000686646, -0.000747681, -0.000808716,
	-0.000885010, -0.000961304, -0.001037598, -0.001113892, -0.001205444, -0.001296997, -0.001388550, -0.001480103,
	-0.001586914, -0.001693726, -0.001785278, -0.001907349, -0.002014160, -0.002120972, -0.002243042, -0.002349854,
	-0.002456665, -0.002578735, -0.002685547, -0.002792358, -0.002899170, -0.002990723, -0.003082275, -0.003173828,
	+0.003250122, +0.003326416, +0.003387451, +0.003433228, +0.003463745, +0.003479004, +0.003479004, +0.003463745,
	+0.003417969, +0.003372192, +0.003280640, +0.003173828, +0.003051758, +0.002883911, +0.002700806, +0.002487183,
	+0.002227783, +0.001937866, +0.001617432, +0.001266479, +0.000869751, +0.000442505, -0.000030518, -0.000549316,
	-0.001098633, -0.001693726, -0.002334595, -0.003005981, -0.003723145, -0.004486084, -0.005294800, -0.006118774,
	-0.007003784, -0.007919312, -0.008865356, -0.009841919, -0.010848999, -0.011886597, -0.012939453, -0.014022827,
	-0.015121460, -0.016235352, -0.017349243, -0.018463135, -0.019577026, -0.020690918, -0.021789550, -0.022857666,
	-0.023910522, -0.024932861, -0.025909424, -0.026840210, -0.027725220, -0.028533936, -0.029281616, -0.029937744,
	-0.030532837, -0.031005860, -0.031387330, -0.031661987, -0.031814575, -0.031845093, -0.031738280, -0.031478880,
	+0.031082153, +0.030517578, +0.029785156, +0.028884888, +0.027801514, +0.026535034, +0.025085450, +0.023422241,
	+0.021575928, +0.019531250, +0.017257690, +0.014801025, +0.012115479, +0.009231567, +0.006134033, +0.002822876,
	-0.000686646, -0.004394531, -0.008316040, -0.012420654, -0.016708374, -0.021179200, -0.025817871, -0.030609130,
	-0.035552980, -0.040634155, -0.045837402, -0.051132202, -0.056533813, -0.061996460, -0.067520140, -0.073059080,
	-0.078628540, -0.084182740, -0.089706420, -0.095169070, -0.100540160, -0.105819700, -0.110946655, -0.115921020,
	-0.120697020, -0.125259400, -0.129562380, -0.133590700, -0.137298580, -0.140670780, -0.143676760, -0.146255500,
	-0.148422240, -0.150115970, -0.151306150, -0.151962280, -0.152069090, -0.151596070, -0.150497440, -0.148773200,
	-0.146362300, -0.143264770, -0.139450070, -0.134887700, -0.129577640, -0.123474120, -0.116577150, -0.108856200,
	+0.100311280, +0.090927124, +0.080688480, +0.069595340, +0.057617188, +0.044784546, +0.031082153, +0.016510010,
	+0.001068115, -0.015228271, -0.032379150, -0.050354004, -0.069168090, -0.088775635, -0.109161380, -0.130310060,
	-0.152206420, -0.174789430, -0.198059080, -0.221984860, -0.246505740, -0.271591200, -0.297210700, -0.323318480,
	-0.349868770, -0.376800540, -0.404083250, -0.431655880, -0.459472660, -0.487472530, -0.515609740, -0.543823240,
	-0.572036740, -0.600219700, -0.628295900, -0.656219500, -0.683914200, -0.711318970, -0.738372800, -0.765029900,
	-0.791214000, -0.816864000, -0.841949460, -0.866363500, -0.890090940, -0.913055400, -0.935195900, -0.956481930,
	-0.976852400, -0.996246340, -1.014617900, -1.031936600, -1.048156700, -1.063217200, -1.077117900, -1.089782700,
	-1.101211500, -1.111373900, -1.120224000, -1.127746600, -1.133926400, -1.138763400, -1.142211900, -1.144287100,
	+1.144989000, +1.144287100, +1.142211900, +1.138763400, +1.133926400, +1.127746600, +1.120224000, +1.111373900,
	+1.101211500, +1.089782700, +1.077117900, +1.063217200, +1.048156700, +1.031936600, +1.014617900, +0.996246340,
	+0.976852400, +0.956481930, +0.935195900, +0.913055400, +0.890090940, +0.866363500, +0.841949460, +0.816864000,
	+0.791214000, +0.765029900, +0.738372800, +0.711318970, +0.683914200, +0.656219500, +0.628295900, +0.600219700,
	+0.572036740, +0.543823240, +0.515609740, +0.487472530, +0.459472660, +0.431655880, +0.404083250, +0.376800540,
	+0.349868770, +0.323318480, +0.297210700, +0.271591200, +0.246505740, +0.221984860, +0.198059080, +0.174789430,
	+0.152206420, +0.130310060, +0.109161380, +0.088775635, +0.069168090, +0.050354004, +0.032379150, +0.015228271,
	-0.001068115, -0.016510010, -0.031082153, -0.044784546, -0.057617188, -0.069595340, -0.080688480, -0.090927124,
	+0.100311280, +0.108856200, +0.116577150, +0.123474120, +0.129577640, +0.134887700, +0.139450070, +0.143264770,
	+0.146362300, +0.148773200, +0.150497440, +0.151596070, +0.152069090, +0.151962280, +0.151306150, +0.150115970,
	+0.148422240, +0.146255500, +0.143676760, +0.140670780, +0.137298580, +0.133590700, +0.129562380, +0.125259400,
	+0.120697020, +0.115921020, +0.110946655, +0.105819700, +0.100540160, +0.095169070, +0.089706420, +0.084182740,
	+0.078628540, +0.073059080, +0.067520140, +0.061996460, +0.056533813, +0.051132202, +0.045837402, +0.040634155,
	+0.035552980, +0.030609130, +0.025817871, +0.021179200, +0.016708374, +0.012420654, +0.008316040, +0.004394531,
	+0.000686646, -0.002822876, -0.006134033, -0.009231567, -0.012115479, -0.014801025, -0.017257690, -0.019531250,
	-0.021575928, -0.023422241, -0.025085450, -0.026535034, -0.027801514, -0.028884888, -0.029785156, -0.030517578,
	+0.031082153, +0.031478880, +0.031738280, +0.031845093, +0.031814575, +0.031661987, +0.031387330, +0.031005860,
	+0.030532837, +0.029937744, +0.029281616, +0.028533936, +0.027725220, +0.026840210, +0.025909424, +0.024932861,
	+0.023910522, +0.022857666, +0.021789550, +0.020690918, +0.019577026, +0.018463135, +0.017349243, +0.016235352,
	+0.015121460, +0.014022827, +0.012939453, +0.011886597, +0.010848999, +0.009841919, +0.008865356, +0.007919312,
	+0.007003784, +0.006118774, +0.005294800, +0.004486084, +0.003723145, +0.003005981, +0.002334595, +0.001693726,
	+0.001098633, +0.000549316, +0.000030518, -0.000442505, -0.000869751, -0.001266479, -0.001617432, -0.001937866,
	-0.002227783, -0.002487183, -0.002700806, -0.002883911, -0.003051758, -0.003173828, -0.003280640, -0.003372192,
	-0.003417969, -0.003463745, -0.003479004, -0.003479004, -0.003463745, -0.003433228, -0.003387451, -0.003326416,
	+0.003250122, +0.003173828, +0.003082275, +0.002990723, +0.002899170, +0.002792358, +0.002685547, +0.002578735,
	+0.002456665, +0.002349854, +0.002243042, +0.002120972, +0.002014160, +0.001907349, +0.001785278, +0.001693726,
	+0.001586914, +0.001480103, +0.001388550, +0.001296997, +0.001205444, +0.001113892, +0.001037598, +0.000961304,
	+0.000885010, +0.000808716, +0.000747681, +0.000686646, +0.000625610, +0.000579834, +0.000534058, +0.000473022,
	+0.000442505, +0.000396729, +0.000366211, +0.000320435, +0.000289917, +0.000259399, +0.000244141, +0.000213623,
	+0.000198364, +0.000167847, +0.000152588, +0.000137329, +0.000122070, +0.000106812, +0.000106812, +0.000091553,
	+0.000076294, +0.000076294, +0.000061035, +0.000061035, +0.000045776, +0.000045776, +0.000030518, +0.000030518,
	+0.000030518, +0.000030518, +0.000015259, +0.000015259, +0.000015259, +0.000015259, +0.000015259, +0.000015259,
};

static int mpeg_do_gb_msb(const unsigned char *data, int *pos, int count)
{
	int v = 0;
	for(int i=0; i != count; i++) {
		v <<= 1;
		if(data[*pos >> 3] & (0x80 >> (*pos & 7)))
			v |= 1;
		(*pos)++;
	}
	return v;
}

static int mpeg_do_gb_lsb(const unsigned char *data, int *pos, int count)
{
	int v = 0;
	for(int i=0; i != count; i++) {
		v <<= 1;
		if(data[*pos >> 3] & (0x01 << (*pos & 7)))
			v |= 1;
		(*pos)++;
	}
	return v;
}

static int mpeg_get_band_param(cv1k_mame_mpeg_audio *m, int band)
{
	int bit_count = s_band_parameter_index_bits_count[m->m_param_index][band];
	int index = mpeg_gb(m, bit_count);
	return s_band_parameter_indexed_values[m->m_param_index][band][index];
}

static void mpeg_read_band_params(cv1k_mame_mpeg_audio *m)
{
	int band = 0;

	while(band < m->m_joint_bands) {
		for(int chan=0; chan < m->m_channel_count; chan++)
			m->m_band_param[chan][band] = mpeg_get_band_param(m, band);
		band++;
	}

	while(band < m->m_total_bands) {
		int val = mpeg_get_band_param(m, band);
		m->m_band_param[0][band] = val;
		m->m_band_param[1][band] = val;
		band++;
	}

	while(band < 32) {
		m->m_band_param[0][band] = 0;
		m->m_band_param[1][band] = 0;
		band++;
	}
}

static void mpeg_read_scfci(cv1k_mame_mpeg_audio *m)
{
	memset(m->m_scfsi, 0, sizeof(m->m_scfsi));
	for(int band=0; band < m->m_total_bands; band++)
		for(int chan=0; chan < m->m_channel_count; chan++)
			if(m->m_band_param[chan][band])
				m->m_scfsi[chan][band] = mpeg_gb(m, 2);
}

static void mpeg_read_band_amplitude_params(cv1k_mame_mpeg_audio *m)
{
	memset(m->m_scf, 0, sizeof(m->m_scf));
	for(int band=0; band < m->m_total_bands; band++)
		for(int chan=0; chan<m->m_channel_count; chan++)
			if(m->m_band_param[chan][band]) {
				switch(m->m_scfsi[chan][band]) {
				case 0:
					m->m_scf[chan][0][band] = mpeg_gb(m, 6);
					m->m_scf[chan][1][band] = mpeg_gb(m, 6);
					m->m_scf[chan][2][band] = mpeg_gb(m, 6);
					break;

				case 1: {
					int val = mpeg_gb(m, 6);
					m->m_scf[chan][0][band] = val;
					m->m_scf[chan][1][band] = val;
					m->m_scf[chan][2][band] = mpeg_gb(m, 6);
					break;
				}

				case 2: {
					int val = mpeg_gb(m, 6);
					m->m_scf[chan][0][band] = val;
					m->m_scf[chan][1][band] = val;
					m->m_scf[chan][2][band] = val;
					break;
				}

				case 3: {
					m->m_scf[chan][0][band] = mpeg_gb(m, 6);
					int val = mpeg_gb(m, 6);
					m->m_scf[chan][1][band] = val;
					m->m_scf[chan][2][band] = val;
					break;
				}
				}
			}
}

static void mpeg_build_amplitudes(cv1k_mame_mpeg_audio *m)
{
	memset(m->m_amp_values, 0, sizeof(m->m_amp_values));

	for(int band=0; band < m->m_total_bands; band++)
		for(int chan=0; chan<m->m_channel_count; chan++)
			if(m->m_band_param[chan][band])
				for(int step=0; step<3; step++)
					m->m_amp_values[chan][step][band] = s_scalefactors[m->m_scf[chan][step][band]];
}

static void mpeg_read_band_value_triplet(cv1k_mame_mpeg_audio *m, int chan, int band)
{
	double buffer[3];

	int band_idx = m->m_band_param[chan][band];
	switch(band_idx) {
	case 0:
		m->m_bdata[chan][0][band] = 0;
		m->m_bdata[chan][1][band] = 0;
		m->m_bdata[chan][2][band] = 0;
		return;

	case 1:
	case 2:
	case 4: {
		int modulo = s_band_infos[band_idx].modulo;
		int val = mpeg_gb(m, s_band_infos[band_idx].cube_bits);
		buffer[0] = val % modulo;
		val = val / modulo;
		buffer[1] = val % modulo;
		val = val / modulo;
		buffer[2] = val % modulo;
		break;
	}

	default: {
		int bits = s_band_infos[band_idx].bits;
		buffer[0] = mpeg_gb(m, bits);
		buffer[1] = mpeg_gb(m, bits);
		buffer[2] = mpeg_gb(m, bits);
		break;
	}
	}

	double scale = 1 << (s_band_infos[band_idx].bits - 1);

	m->m_bdata[chan][0][band] = ((buffer[0] - scale) / scale + s_band_infos[band_idx].offset) * s_band_infos[band_idx].scale;
	m->m_bdata[chan][1][band] = ((buffer[1] - scale) / scale + s_band_infos[band_idx].offset) * s_band_infos[band_idx].scale;
	m->m_bdata[chan][2][band] = ((buffer[2] - scale) / scale + s_band_infos[band_idx].offset) * s_band_infos[band_idx].scale;
}

static void mpeg_build_next_segments(cv1k_mame_mpeg_audio *m, int step)
{
	int band = 0;
	while(band < m->m_joint_bands) {
		for(int chan=0; chan<m->m_channel_count; chan++) {
			mpeg_read_band_value_triplet(m, chan, band);
			double amp = m->m_amp_values[chan][step][band];
			m->m_bdata[chan][0][band] *= amp;
			m->m_bdata[chan][1][band] *= amp;
			m->m_bdata[chan][2][band] *= amp;
		}
		band++;
	}

	while(band < m->m_joint_bands) {
		mpeg_read_band_value_triplet(m, 0, band);
		m->m_bdata[1][0][band] = m->m_bdata[0][0][band];
		m->m_bdata[1][1][band] = m->m_bdata[0][1][band];
		m->m_bdata[1][2][band] = m->m_bdata[0][2][band];

		for(int chan=0; chan<m->m_channel_count; chan++) {
			double amp = m->m_amp_values[chan][step][band];
			m->m_bdata[chan][0][band] *= amp;
			m->m_bdata[chan][1][band] *= amp;
			m->m_bdata[chan][2][band] *= amp;
		}
		band++;
	}

	while(band < 32) {
		m->m_bdata[0][0][band] = 0;
		m->m_bdata[0][1][band] = 0;
		m->m_bdata[0][2][band] = 0;
		m->m_bdata[1][0][band] = 0;
		m->m_bdata[1][1][band] = 0;
		m->m_bdata[1][2][band] = 0;
		band++;
	}
}

static void mpeg_retrieve_subbuffer(cv1k_mame_mpeg_audio *m, int step)
{
	for(int chan=0; chan<m->m_channel_count; chan++)
		memcpy(m->m_subbuffer[chan], m->m_bdata[chan][step], 32*sizeof(m->m_subbuffer[0][0]));
}

static void mpeg_idct32(cv1k_mame_mpeg_audio *m, const double *input, double *output)
{
	// Simplest idct32 ever, non-fast at all
	for (int i = 0; i < 32; i++) {
		double s = 0;
		for (int j = 0; j < 32; j++)
			s += input[j] * m->m_cos_cache[i][j];
		output[i] = s;
	}
}

static void mpeg_resynthesis(const double *input, double *output)
{
	memset(output, 0, 32*sizeof(output[0]));
	for(int j=0; j<64*8; j+=64) {
		for(int i=0; i<16; i++)
			output[i] += input[   i+j]*s_synthesis_filter[i+j] - input[32-i+j]*s_synthesis_filter[32+i+j];
		output[16] -= input[16+j]*s_synthesis_filter[32+16+j];
		for(int i=17; i<32; i++)
			output[i] -= input[32-i+j]*s_synthesis_filter[i+j] + input[   i+j]*s_synthesis_filter[32+i+j];
	}
}

static void mpeg_scale_and_clamp(const double *input, short *output, int step)
{
	for(int i=0; i<32; i++) {
		double val = input[i]*32768 + 0.5;
		short cval;
		if(val <= -32768)
			cval = -32768;
		else if(val >= 32767)
			cval = 32767;
		else
			cval = (short)((int)val);
		*output = cval;
		output += step;
	}
}
