/*
 * Epson RTC9701 serial RTC/EEPROM model for the CV1000 sandbox.
 *
 * This ANSI C port follows MAME's BSD-3-Clause rtc9701_device state machine
 * (src/devices/machine/rtc9701.cpp, copyright Angelo Salese and David Haywood)
 * but is intentionally trimmed to the CV1000 serial line interface used by
 * cv1k.cpp.  See NOTICE and docs/MAME_DERIVED.md for attribution.
 */
#include "rtc9701.h"
#include "platform.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static cv1k_u8 bcd(cv1k_u32 x)
{
    return (cv1k_u8)(((x / 10UL) << 4) | (x % 10UL));
}

static void rtc9701_load_time(struct cv1k_rtc9701 *rtc, cv1k_u32 now_unix)
{
    time_t raw;
    struct tm *tmv;
    raw = (time_t)now_unix;
    tmv = localtime(&raw);
    if (tmv == NULL) return;
    rtc->sec = bcd((cv1k_u32)tmv->tm_sec);
    rtc->min = bcd((cv1k_u32)tmv->tm_min);
    rtc->hour = bcd((cv1k_u32)tmv->tm_hour);
    rtc->day = bcd((cv1k_u32)tmv->tm_mday);
    rtc->month = bcd((cv1k_u32)(tmv->tm_mon + 1));
    rtc->year = bcd((cv1k_u32)((tmv->tm_year + 1900) % 100));
    if (tmv->tm_wday == 0) rtc->wday = 0x01U;
    else rtc->wday = (cv1k_u8)(1U << (tmv->tm_wday - 1));
}

static cv1k_u8 rtc9701_rtc_read(struct cv1k_rtc9701 *rtc, cv1k_u8 offset)
{
    switch (offset & 7U) {
    case 0U: return rtc->sec;
    case 1U: return rtc->min;
    case 2U: return rtc->hour;
    case 3U: return rtc->wday;
    case 4U: return rtc->day;
    case 5U: return rtc->month;
    case 6U: return rtc->year;
    default: return 0x20U;
    }
}

static void rtc9701_rtc_write(struct cv1k_rtc9701 *rtc, cv1k_u8 offset, cv1k_u8 data)
{
    switch (offset & 7U) {
    case 0U: rtc->sec = data; break;
    case 1U: rtc->min = data; break;
    case 2U: rtc->hour = data; break;
    case 3U: rtc->wday = data; break;
    case 4U: rtc->day = data; break;
    case 5U: rtc->month = data; break;
    case 6U: rtc->year = data; break;
    default: break;
    }
}

static void rtc9701_reset_stream(struct cv1k_rtc9701 *rtc)
{
    rtc->state = CV1K_RTC9701_STATE_CMD_WAIT;
    rtc->cmd_stream_pos = 0U;
    rtc->current_cmd = 0U;
    rtc->address_pos = 0U;
    rtc->current_address = 0U;
    rtc->current_data = 0U;
    rtc->data_pos = 0U;
}

void cv1k_rtc9701_reset(struct cv1k_rtc9701 *rtc)
{
    cv1k_u32 now;
    cv1k_u32 i;
    memset(rtc, 0, sizeof(*rtc));
    for (i = 0UL; i < 256UL; i++) rtc->eeprom[i] = 0xffffU;
    now = cv1k_now_unix();
    rtc9701_load_time(rtc, now);
    rtc9701_reset_stream(rtc);
    rtc->last_unix = now;
}

cv1k_u8 cv1k_rtc9701_read_bit(struct cv1k_rtc9701 *rtc)
{
    if (rtc->state == CV1K_RTC9701_STATE_RTC_READ || rtc->state == CV1K_RTC9701_STATE_EEPROM_READ) {
        if (rtc->data_pos == 0U) return 0U;
        return (cv1k_u8)((rtc->current_data >> (rtc->data_pos - 1U)) & 1U);
    }
    return 0U;
}

void cv1k_rtc9701_write_lines(struct cv1k_rtc9701 *rtc, cv1k_u8 data, cv1k_u32 now_unix)
{
    cv1k_u8 new_latch;
    cv1k_u8 new_clock;
    cv1k_u8 new_reset;

    new_latch = (cv1k_u8)(data & 1U);
    new_clock = (cv1k_u8)((data >> 1) & 1U);
    /* MAME's CV1000 output port maps CS as IP_ACTIVE_LOW.  The RTC state
     * machine shifts only while set_cs_line receives CLEAR_LINE (0), and
     * resets the command stream while the line is non-zero.
     */
    new_reset = (cv1k_u8)((data & 4U) ? 1U : 0U);
    rtc->latch = new_latch;

    if (new_reset != 0U) {
        rtc9701_reset_stream(rtc);
    } else if (rtc->reset_line == 0U && rtc->clock_line == 0U && new_clock != 0U) {
        switch (rtc->state) {
        case CV1K_RTC9701_STATE_CMD_WAIT:
            rtc->current_cmd = (cv1k_u8)((rtc->current_cmd << 1) | (rtc->latch & 1U));
            rtc->cmd_stream_pos++;
            if (rtc->cmd_stream_pos == 4U) {
                if (rtc->current_cmd == 0x00U) rtc->state = CV1K_RTC9701_STATE_RTC_WRITE;
                else if (rtc->current_cmd == 0x02U) rtc->state = CV1K_RTC9701_STATE_EEPROM_WRITE;
                else if (rtc->current_cmd == 0x06U) rtc->state = CV1K_RTC9701_STATE_AFTER_WRITE_ENABLE;
                else if (rtc->current_cmd == 0x08U) rtc->state = CV1K_RTC9701_STATE_RTC_READ;
                else if (rtc->current_cmd == 0x0aU) rtc->state = CV1K_RTC9701_STATE_EEPROM_READ;
                rtc->cmd_stream_pos = 0U;
                rtc->current_cmd = 0U;
                rtc->address_pos = 0U;
                rtc->current_address = 0U;
                rtc->current_data = 0U;
                rtc->data_pos = 0U;
            }
            break;

        case CV1K_RTC9701_STATE_AFTER_WRITE_ENABLE:
            rtc->cmd_stream_pos++;
            if (rtc->cmd_stream_pos == 12U) rtc9701_reset_stream(rtc);
            break;

        case CV1K_RTC9701_STATE_RTC_WRITE:
            rtc->cmd_stream_pos++;
            if (rtc->cmd_stream_pos <= 4U) {
                rtc->address_pos++;
                rtc->current_address = (cv1k_u16)((rtc->current_address << 1) | (rtc->latch & 1U));
            } else {
                rtc->data_pos++;
                rtc->current_data = (cv1k_u16)((rtc->current_data << 1) | (rtc->latch & 1U));
            }
            if (rtc->cmd_stream_pos == 12U) {
                rtc9701_rtc_write(rtc, (cv1k_u8)rtc->current_address, (cv1k_u8)rtc->current_data);
                rtc9701_reset_stream(rtc);
            }
            break;

        case CV1K_RTC9701_STATE_RTC_READ:
            rtc->cmd_stream_pos++;
            if (rtc->cmd_stream_pos <= 4U) {
                rtc->address_pos++;
                rtc->current_address = (cv1k_u16)((rtc->current_address << 1) | (rtc->latch & 1U));
                if (rtc->cmd_stream_pos == 4U) {
                    rtc9701_load_time(rtc, now_unix);
                    rtc->current_data = rtc9701_rtc_read(rtc, (cv1k_u8)rtc->current_address);
                    rtc->data_pos = 8U;
                }
            } else if (rtc->data_pos > 0U) {
                rtc->data_pos--;
            }
            if (rtc->cmd_stream_pos == 12U) rtc->cmd_stream_pos = 0U;
            break;

        case CV1K_RTC9701_STATE_EEPROM_WRITE:
            rtc->cmd_stream_pos++;
            if (rtc->cmd_stream_pos <= 12U) {
                rtc->address_pos++;
                rtc->current_address = (cv1k_u16)((rtc->current_address << 1) | (rtc->latch & 1U));
            } else {
                rtc->data_pos++;
                rtc->current_data = (cv1k_u16)((rtc->current_data << 1) | (rtc->latch & 1U));
            }
            if (rtc->cmd_stream_pos == 28U) {
                rtc->eeprom[(rtc->current_address >> 1) & 0xffU] = rtc->current_data;
                rtc9701_reset_stream(rtc);
            }
            break;

        case CV1K_RTC9701_STATE_EEPROM_READ:
            rtc->cmd_stream_pos++;
            if (rtc->cmd_stream_pos <= 12U) {
                rtc->address_pos++;
                rtc->current_address = (cv1k_u16)((rtc->current_address << 1) | (rtc->latch & 1U));
                if (rtc->cmd_stream_pos == 12U) {
                    rtc->current_data = rtc->eeprom[(rtc->current_address >> 1) & 0xffU];
                    rtc->data_pos = 16U;
                }
            } else if (rtc->data_pos > 0U) {
                rtc->data_pos--;
            }
            if (rtc->cmd_stream_pos == 28U) rtc->cmd_stream_pos = 0U;
            break;

        default:
            rtc9701_reset_stream(rtc);
            break;
        }
    }

    rtc->clock_line = new_clock;
    rtc->reset_line = new_reset;
    rtc->last_unix = now_unix;
}

int cv1k_rtc9701_save_eeprom(struct cv1k_rtc9701 *rtc, const char *path)
{
    cv1k_u8 buf[512];
    cv1k_u32 i;
    for (i = 0UL; i < 256UL; i++) cv1k_put_be16(&buf[i * 2UL], rtc->eeprom[i]);
    return cv1k_write_file(path, buf, 512UL);
}

int cv1k_rtc9701_load_eeprom(struct cv1k_rtc9701 *rtc, const char *path)
{
    cv1k_u8 buf[512];
    cv1k_u32 got;
    cv1k_u32 i;
    got = 0UL;
    if (!cv1k_read_file(path, buf, 512UL, &got)) return 0;
    if (got < 512UL) return 0;
    for (i = 0UL; i < 256UL; i++) rtc->eeprom[i] = cv1k_be16(&buf[i * 2UL]);
    return 1;
}
