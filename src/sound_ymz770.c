/*
 * Yamaha YMZ770C register-path scaffold for the CV1000 sandbox.
 *
 * This does not attempt AMM/MPEG audio decoding.  It ports the MAME YMZ770
 * register-select/data semantics and playback/sequencer bookkeeping into
 * ANSI C so CV1000 software observes a stateful sound device instead of a
 * raw 8-byte latch.  MAME source: src/devices/sound/ymz770.cpp, BSD-3-Clause,
 * copyright Olivier Galibert, R. Belmont, and MetalliC.
 */
#include "sound_ymz770.h"
#include <string.h>

static void ymz770_internal_reg_write(struct cv1k_ymz770 *ymz, cv1k_u8 reg, cv1k_u8 data)
{
    cv1k_u32 ch;
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
                ymz->sequences[ch].offset = 0UL;
                ymz->sequences[ch].delay = 0UL;
                ymz->sequences[ch].playing = 1U;
                ymz->sequences[ch].paused = 0U;
            } else if ((data & 6U) == 0U && ymz->sequences[ch].playing) {
                cv1k_u32 i;
                ymz->sequences[ch].playing = 0U;
                for (i = 0UL; i < CV1K_YMZ770_CHANNELS; i++) {
                    if (ymz->sequences[ch].stopchan & (1U << i)) ymz->channels[i].playing = 0U;
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

void cv1k_ymz770_reset(struct cv1k_ymz770 *ymz)
{
    cv1k_u32 i;
    memset(ymz, 0, sizeof(*ymz));
    /* MAME ymz770_device::device_reset() starts playback pan centered.
     * Volume remains zero until the game writes the channel volume register.
     */
    for (i = 0UL; i < CV1K_YMZ770_CHANNELS; i++) {
        ymz->channels[i].pan = 64U;
        ymz->channels[i].volume = 0UL;
        ymz->channels[i].volume2 = 0U;
    }
}

void cv1k_ymz770_write(struct cv1k_ymz770 *ymz, cv1k_u32 offset, cv1k_u8 data)
{
    offset &= 7UL;
    ymz->fifo[ymz->write_pos % CV1K_YMZ770_FIFO_SIZE] = data;
    ymz->write_pos = (ymz->write_pos + 1UL) % CV1K_YMZ770_FIFO_SIZE;
    ymz->writes++;

    if (offset & 1UL) {
        ymz770_internal_reg_write(ymz, ymz->cur_reg, data);
    } else {
        ymz->cur_reg = data;
        ymz->regs[0xffU] = data;
    }
}

void cv1k_ymz770_mix_s16(struct cv1k_ymz770 *ymz, short *mono, cv1k_u32 samples)
{
    cv1k_u32 i;
    for (i = 0UL; i < samples; i++) mono[i] = 0;
    ymz->generated_samples += samples;
}
