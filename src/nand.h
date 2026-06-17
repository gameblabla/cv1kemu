#ifndef CV1K_NAND_H
#define CV1K_NAND_H

#include "cv1k_types.h"
#include "cv1k_config.h"

#define CV1K_NAND_SM_INIT 0U
#define CV1K_NAND_SM_READ 1U
#define CV1K_NAND_SM_PROGRAM 2U
#define CV1K_NAND_SM_ERASE 3U
#define CV1K_NAND_SM_READSTATUS 4U
#define CV1K_NAND_SM_READID 5U
#define CV1K_NAND_SM_30 6U
#define CV1K_NAND_SM_RANDOM_DATA_INPUT 7U
#define CV1K_NAND_SM_RANDOM_DATA_OUTPUT 8U

#define CV1K_NAND_PTR_A 0U
#define CV1K_NAND_PTR_B 1U
#define CV1K_NAND_PTR_C 2U

struct cv1k_nand {
    cv1k_u8 *data;
    cv1k_u8 *page_reg;
    cv1k_u32 size;
    cv1k_u32 cursor;
    cv1k_u32 page;
    cv1k_u32 column;
    cv1k_u32 addr_latch;
    cv1k_u32 program_base;
    cv1k_u32 read_area_offset;
    cv1k_u8 address_bytes[5];
    cv1k_u8 address_count;
    cv1k_u8 address_expected;
    cv1k_u8 random_read_pending;
    cv1k_u8 command;
    cv1k_u8 prev_command;
    cv1k_u8 id_index;
    cv1k_u8 busy;
    cv1k_u8 status;
    cv1k_u8 accumulated_status;
    cv1k_u8 manufacturer;
    cv1k_u8 device;
    cv1k_u8 id[5];
    cv1k_u8 id_len;
    cv1k_u8 mode;
    cv1k_u8 pointer_mode;
    cv1k_u8 addr_load_ptr;
    cv1k_u8 mode_3065;
    cv1k_u32 page_addr;
    cv1k_u32 byte_addr;
    cv1k_u32 program_byte_count;
    cv1k_u32 reads;
    cv1k_u32 writes;
    cv1k_u32 erases;
    cv1k_u32 random_reads;
    cv1k_u32 spare_reads;
    cv1k_u32 status_reads;
    cv1k_u32 id_reads;
    cv1k_u32 command_counts[256];
    cv1k_u32 map_blocks;
    cv1k_u32 map_empty_blocks;
    cv1k_u32 map_oob_marked_blocks;
    cv1k_u32 map_spare_non_ff_pages;
    cv1k_u32 last_read_page;
    cv1k_u32 last_read_block;
    cv1k_u32 last_read_column;
    int data_only_reads;
    int ce_enabled;
};

int cv1k_nand_init(struct cv1k_nand *nand, cv1k_u32 size);
void cv1k_nand_shutdown(struct cv1k_nand *nand);
int cv1k_nand_load(struct cv1k_nand *nand, const char *path);
void cv1k_nand_reset(struct cv1k_nand *nand);
cv1k_u8 cv1k_nand_data_r(struct cv1k_nand *nand);
int cv1k_nand_data_read_bulk(struct cv1k_nand *nand, cv1k_u8 *dst, cv1k_u32 count);
void cv1k_nand_data_w(struct cv1k_nand *nand, cv1k_u8 data);
void cv1k_nand_command_w(struct cv1k_nand *nand, cv1k_u8 data);
void cv1k_nand_address_w(struct cv1k_nand *nand, cv1k_u8 data);
cv1k_u8 cv1k_nand_is_busy(const struct cv1k_nand *nand);
void cv1k_nand_set_id(struct cv1k_nand *nand, cv1k_u8 manufacturer, cv1k_u8 device);
void cv1k_nand_set_data_only_reads(struct cv1k_nand *nand, int enabled);
void cv1k_nand_set_ce(struct cv1k_nand *nand, int enabled);
void cv1k_nand_scan_map(struct cv1k_nand *nand);
cv1k_u32 cv1k_nand_current_page(const struct cv1k_nand *nand);
cv1k_u32 cv1k_nand_current_block(const struct cv1k_nand *nand);

#endif
