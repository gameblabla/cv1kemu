/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MAME-derived YMZ770C audio mixer for the CV1000 SDL 1.2 frontend.
 * References:
 *   MAME src/devices/sound/ymz770.cpp
 *   MAME src/devices/sound/mpeg_audio.cpp
 * MAME copyright-holders: Olivier Galibert, R. Belmont, MetalliC.
 */
#include "sound_ymz770.h"
#include "cv1k_config.h"
#include "mame_mpeg_audio.h"
#include <stdlib.h>
#include <string.h>

#define CV1K_YMZ770_IMPL_SLOTS 8
#define CV1K_YMZ770_OUTPUT_BUF 0x1000

struct cv1k_ymz770_channel_audio {
    cv1k_mame_mpeg_audio *decoder;
    short output_data[CV1K_YMZ770_OUTPUT_BUF];
    int output_remaining;
    int output_ptr;
    int atbl;
    int pptr;
};

struct cv1k_ymz770_audio_impl {
    const struct cv1k_ymz770 *key;
    const cv1k_u8 *rom;
    cv1k_u32 rom_size;
    cv1k_u32 reset_cookie;
    struct cv1k_ymz770_channel_audio ch[CV1K_YMZ770_CHANNELS];
};

static struct cv1k_ymz770_audio_impl g_impls[CV1K_YMZ770_IMPL_SLOTS];

static int clamp_s32(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static short clamp_s16(int v)
{
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (short)v;
}

static cv1k_u8 get_rom_byte(const cv1k_u8 *rom, cv1k_u32 rom_size, cv1k_u32 offset)
{
    if (rom == NULL || rom_size == 0U) return 0xffU;
    return rom[offset % rom_size];
}

static cv1k_u32 get_phrase_offs(const cv1k_u8 *rom, cv1k_u32 rom_size, int phrase)
{
    cv1k_u32 base = (cv1k_u32)phrase * 4U;
    return ((cv1k_u32)get_rom_byte(rom, rom_size, base + 1U) << 16) |
           ((cv1k_u32)get_rom_byte(rom, rom_size, base + 2U) << 8) |
            (cv1k_u32)get_rom_byte(rom, rom_size, base + 3U);
}

static cv1k_u32 get_seq_offs(const cv1k_u8 *rom, cv1k_u32 rom_size, int seq)
{
    cv1k_u32 base = (cv1k_u32)seq * 4U + 0x400U;
    return ((cv1k_u32)get_rom_byte(rom, rom_size, base + 1U) << 16) |
           ((cv1k_u32)get_rom_byte(rom, rom_size, base + 2U) << 8) |
            (cv1k_u32)get_rom_byte(rom, rom_size, base + 3U);
}

static void impl_free_decoders(struct cv1k_ymz770_audio_impl *impl)
{
    unsigned int i;
    if (impl == NULL) return;
    for (i = 0U; i < CV1K_YMZ770_CHANNELS; i++) {
        cv1k_mame_mpeg_audio_destroy(impl->ch[i].decoder);
        impl->ch[i].decoder = NULL;
        impl->ch[i].output_remaining = 0;
        impl->ch[i].output_ptr = 0;
        impl->ch[i].atbl = 0;
        impl->ch[i].pptr = 0;
    }
}

static struct cv1k_ymz770_audio_impl *get_impl(struct cv1k_ymz770 *ymz, const cv1k_u8 *rom, cv1k_u32 rom_size)
{
    unsigned int i;
    struct cv1k_ymz770_audio_impl *empty;
    empty = NULL;
    for (i = 0U; i < CV1K_YMZ770_IMPL_SLOTS; i++) {
        if (g_impls[i].key == ymz) {
            if (g_impls[i].rom != rom || g_impls[i].rom_size != rom_size || g_impls[i].reset_cookie > ymz->writes) {
                impl_free_decoders(&g_impls[i]);
                g_impls[i].rom = rom;
                g_impls[i].rom_size = rom_size;
                g_impls[i].reset_cookie = ymz->writes;
            }
            return &g_impls[i];
        }
        if (empty == NULL && g_impls[i].key == NULL) empty = &g_impls[i];
    }
    if (empty == NULL) {
        empty = &g_impls[0];
        impl_free_decoders(empty);
        memset(empty, 0, sizeof(*empty));
    }
    empty->key = ymz;
    empty->rom = rom;
    empty->rom_size = rom_size;
    empty->reset_cookie = ymz != NULL ? ymz->writes : 0U;
    return empty;
}

static void ensure_decoders(struct cv1k_ymz770_audio_impl *impl, const cv1k_u8 *rom)
{
    unsigned int i;
    if (impl == NULL || rom == NULL) return;
    for (i = 0U; i < CV1K_YMZ770_CHANNELS; i++) {
        if (impl->ch[i].decoder == NULL) {
            impl->ch[i].decoder = cv1k_mame_mpeg_audio_create(rom, CV1K_MAME_MPEG_AUDIO_AMM, false, 0);
        }
    }
}

static void clear_channel_decoder(struct cv1k_ymz770_audio_impl *impl, unsigned int ch)
{
    if (impl == NULL || ch >= CV1K_YMZ770_CHANNELS) return;
    if (impl->ch[ch].decoder != NULL) cv1k_mame_mpeg_audio_clear(impl->ch[ch].decoder);
    impl->ch[ch].output_remaining = 0;
    impl->ch[ch].output_ptr = 0;
}

static void internal_reg_write_from_seq(struct cv1k_ymz770 *ymz, const cv1k_u8 *rom, cv1k_u32 rom_size, cv1k_u8 reg, cv1k_u8 data)
{
    cv1k_u32 ch;
    cv1k_u32 i;
    if (ymz == NULL) return;
    ymz->regs[reg] = data;
    ymz->reg_writes++;

    if (reg < 0x40U) {
        switch (reg) {
        case 0x00U:
            ymz->mute = (cv1k_u8)(data & 1U);
            ymz->doen = (cv1k_u8)((data >> 1) & 1U);
            break;
        case 0x01U:
            ymz->vlma = data;
            break;
        case 0x02U:
            ymz->bsl = (cv1k_u8)(data & 7U);
            ymz->cpl = (cv1k_u8)((data >> 4) & 7U);
            break;
        default:
            break;
        }
        return;
    }

    if (reg < 0x60U) {
        ch = (reg >> 2) & 7U;
        switch (reg & 3U) {
        case 0U:
            ymz->channels[ch].phrase = data;
            break;
        case 1U:
            ymz->channels[ch].volume = 128UL << 17;
            ymz->channels[ch].volume2 = data;
            break;
        case 2U:
            ymz->channels[ch].pan = (cv1k_u16)((cv1k_u16)data << 3);
            break;
        case 3U:
            if ((data & 6U) == 2U || ((data & 6U) == 6U && !ymz->channels[ch].playing)) {
                ymz->channels[ch].pending = 1U;
                ymz->channels[ch].playing = 1U;
                ymz->channels[ch].paused = 0U;
                ymz->channels[ch].last_block = 0U;
                ymz->keyons++;
            } else if ((data & 6U) == 0U) {
                if (ymz->channels[ch].playing) ymz->keyoffs++;
                ymz->channels[ch].playing = 0U;
                ymz->channels[ch].pending = 0U;
                ymz->channels[ch].last_block = 0U;
            }
            ymz->channels[ch].loop = (cv1k_u8)((data & 1U) ? 255U : 0U);
            break;
        default:
            break;
        }
        return;
    }

    if (reg >= 0x80U) {
        ch = (reg >> 4) & 7U;
        switch (reg & 0x0fU) {
        case 0x00U:
            ymz->sequences[ch].sequence = data;
            break;
        case 0x01U:
            if ((data & 6U) == 2U || ((data & 6U) == 6U && !ymz->sequences[ch].playing)) {
                ymz->sequences[ch].offset = get_seq_offs(rom, rom_size, ymz->sequences[ch].sequence);
                ymz->sequences[ch].delay = 0UL;
                ymz->sequences[ch].playing = 1U;
                ymz->sequences[ch].paused = 0U;
            } else if ((data & 6U) == 0U && ymz->sequences[ch].playing) {
                ymz->sequences[ch].playing = 0U;
                for (i = 0UL; i < CV1K_YMZ770_CHANNELS; i++) {
                    if (ymz->sequences[ch].stopchan & (1U << i)) {
                        ymz->channels[i].playing = 0U;
                        ymz->channels[i].pending = 0U;
                        ymz->channels[i].last_block = 0U;
                    }
                }
            }
            ymz->sequences[ch].loop = (cv1k_u8)(data & 1U);
            break;
        case 0x02U:
            ymz->sequences[ch].timer = (cv1k_u16)((ymz->sequences[ch].timer & 0x00ffU) | ((cv1k_u16)data << 8));
            break;
        case 0x03U:
            ymz->sequences[ch].timer = (cv1k_u16)((ymz->sequences[ch].timer & 0xff00U) | data);
            break;
        case 0x06U:
            ymz->sequences[ch].stopchan = data;
            break;
        default:
            break;
        }
    }
}

static void run_sequencer(struct cv1k_ymz770 *ymz, const cv1k_u8 *rom, cv1k_u32 rom_size)
{
    unsigned int s;
    unsigned int ch;
    for (s = 0U; s < CV1K_YMZ770_SEQUENCES; s++) {
        struct cv1k_ymz770_sequence *seq = &ymz->sequences[s];
        if (!seq->playing || seq->paused) continue;
        if (seq->offset == 0U) seq->offset = get_seq_offs(rom, rom_size, seq->sequence);
        if (seq->delay > 0U) {
            seq->delay--;
        } else {
            cv1k_u8 reg = get_rom_byte(rom, rom_size, seq->offset++);
            cv1k_u8 data = get_rom_byte(rom, rom_size, seq->offset++);
            switch (reg) {
            case 0x0fU:
                for (ch = 0U; ch < CV1K_YMZ770_CHANNELS; ch++) {
                    if (seq->stopchan & (1U << ch)) {
                        ymz->channels[ch].playing = 0U;
                        ymz->channels[ch].pending = 0U;
                        ymz->channels[ch].last_block = 0U;
                    }
                }
                if (seq->loop) seq->offset = get_seq_offs(rom, rom_size, seq->sequence);
                else seq->playing = 0U;
                break;
            case 0x0eU:
                seq->delay = (cv1k_u32)seq->timer * 32U + 31U;
                break;
            default:
                internal_reg_write_from_seq(ymz, rom, rom_size, reg, data);
                break;
            }
        }
    }
}

void cv1k_ymz770_mix_s16_stereo(struct cv1k_ymz770 *ymz,
                                            const cv1k_u8 *rom,
                                            cv1k_u32 rom_size,
                                            short *stereo,
                                            cv1k_u32 samples)
{
    struct cv1k_ymz770_audio_impl *impl;
    cv1k_u32 i;
    if (stereo == NULL) return;
    if (ymz == NULL || rom == NULL || rom_size == 0U) {
        memset(stereo, 0, (size_t)samples * 2U * sizeof(short));
        return;
    }

    impl = get_impl(ymz, rom, rom_size);
    ensure_decoders(impl, rom);

    for (i = 0U; i < samples; i++) {
        int mixl = 0;
        int mixr = 0;
        unsigned int c;

        run_sequencer(ymz, rom, rom_size);

        for (c = 0U; c < CV1K_YMZ770_CHANNELS; c++) {
            struct cv1k_ymz770_channel *ch = &ymz->channels[c];
            struct cv1k_ymz770_channel_audio *ach = &impl->ch[c];

            /* MAME's YMZ770 model deliberately "force finish[es] current block"
             * after a key-off: a channel with buffered decoded MPEG/AMM samples
             * keeps emitting until that 1152-sample block drains.  On CV1000
             * this is audible on Muchi Muchi Pork's title-screen music: the
             * sequencer writes CH0 control 0x43=0x00 at the end of the jingle,
             * but the software mixer leaks the already-decoded tail for up to
             * ~72 ms at 16 kHz.  Hardware key-off is edge/level immediate for
             * the playback channel, so discard queued decode data as soon as a
             * channel is off, or when a new key-on is pending over an old buffer. */
            if (ach->output_remaining > 0 && (!ch->playing || ch->pending)) {
                clear_channel_decoder(impl, c);
            }

            if (ach->output_remaining == 0 && ch->playing && !ch->paused) {
retry_block:
                if (ch->last_block) {
                    if (ch->loop) {
                        if (ch->loop != 255U) ch->loop--;
                        ch->pending = 1U;
                    } else {
                        ch->playing = 0U;
                        ach->output_remaining = 0;
                        clear_channel_decoder(impl, c);
                    }
                }

                if (ch->playing) {
                    if (ch->pending) {
                        int phrase = (int)ch->phrase;
                        cv1k_u32 table = (cv1k_u32)phrase * 4U;
                        ach->atbl = (int)((get_rom_byte(rom, rom_size, table) >> 4) & 7U);
                        ach->pptr = (int)(8U * get_phrase_offs(rom, rom_size, phrase));
                        clear_channel_decoder(impl, c);
                        ch->pending = 0U;
                    }

                    if (ach->decoder != NULL) {
                        int sample_rate = (int)(CV1K_YMZ770_CLOCK_HZ / 1024U);
                        int channel_count = 1;
                        int out_samples = 0;
                        if (!cv1k_mame_mpeg_audio_decode_buffer(ach->decoder, &ach->pptr, (int)(rom_size * 8U), ach->output_data, &out_samples, &sample_rate, &channel_count, ach->atbl) || out_samples == 0) {
                            ch->playing = ch->last_block ? 0U : 1U;
                            ch->last_block = 1U;
                            ach->output_remaining = 0;
                            goto retry_block;
                        }
                        ach->output_remaining = out_samples;
                        ach->output_ptr = 0;
                        ch->last_block = (cv1k_u8)(out_samples < 1152 ? 1U : 0U);
                    }
                }
            }

            if (ach->output_remaining > 0) {
                int smpl = ach->output_data[ach->output_ptr++];
                int pan = (int)ch->pan;
                smpl = (smpl * (int)(ch->volume >> 17)) >> 7;
                smpl = (smpl * (int)ch->volume2) >> 7;
                if (pan < 0) pan = 0;
                if (pan > 128) pan = 128;
                mixr += (smpl * pan) >> 7;
                mixl += (smpl * (128 - pan)) >> 7;
                ach->output_remaining--;
                if (ach->output_remaining == 0 && !ch->playing) clear_channel_decoder(impl, c);
            }
        }

        if (ymz->mute) {
            mixl = 0;
            mixr = 0;
        } else {
            int shift;
            mixl *= (int)ymz->vlma;
            mixr *= (int)ymz->vlma;
            shift = 7 - (int)(ymz->bsl & 7U);
            if (shift > 0) {
                mixl >>= shift;
                mixr >>= shift;
            }
            switch (ymz->cpl) {
            case 3U:
                mixl = clamp_s32(mixl, -24576, 24576);
                mixr = clamp_s32(mixr, -24576, 24576);
                break;
            case 2U:
                mixl = clamp_s32(mixl, -28672, 28672);
                mixr = clamp_s32(mixr, -28672, 28672);
                break;
            case 1U:
                mixl = clamp_s32(mixl, -32768, 32767);
                mixr = clamp_s32(mixr, -32768, 32767);
                break;
            default:
                break;
            }
        }

        stereo[i * 2U + 0U] = clamp_s16(mixl);
        stereo[i * 2U + 1U] = clamp_s16(mixr);
    }
    ymz->generated_samples += samples;
}
