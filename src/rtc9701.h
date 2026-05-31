#ifndef RTC9701_H
#define RTC9701_H

#include "cv1k_types.h"

#define CV1K_RTC9701_STATE_CMD_WAIT 0U
#define CV1K_RTC9701_STATE_RTC_READ 1U
#define CV1K_RTC9701_STATE_RTC_WRITE 2U
#define CV1K_RTC9701_STATE_EEPROM_READ 3U
#define CV1K_RTC9701_STATE_EEPROM_WRITE 4U
#define CV1K_RTC9701_STATE_AFTER_WRITE_ENABLE 5U

struct cv1k_rtc9701 {
    cv1k_u16 eeprom[256];
    cv1k_u8 latch;
    cv1k_u8 reset_line;
    cv1k_u8 clock_line;
    cv1k_u8 state;
    cv1k_u8 cmd_stream_pos;
    cv1k_u8 current_cmd;
    cv1k_u8 address_pos;
    cv1k_u16 current_address;
    cv1k_u16 current_data;
    cv1k_u8 data_pos;
    cv1k_u8 sec;
    cv1k_u8 min;
    cv1k_u8 hour;
    cv1k_u8 day;
    cv1k_u8 wday;
    cv1k_u8 month;
    cv1k_u8 year;
    cv1k_u32 last_unix;
};

void cv1k_rtc9701_reset(struct cv1k_rtc9701 *rtc);
cv1k_u8 cv1k_rtc9701_read_bit(struct cv1k_rtc9701 *rtc);
void cv1k_rtc9701_write_lines(struct cv1k_rtc9701 *rtc, cv1k_u8 data, cv1k_u32 now_unix);
int cv1k_rtc9701_save_eeprom(struct cv1k_rtc9701 *rtc, const char *path);
int cv1k_rtc9701_load_eeprom(struct cv1k_rtc9701 *rtc, const char *path);

#endif
