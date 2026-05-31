#include "nand.h"
#include "platform.h"
#include <string.h>

/*
 * Cave CV1000 uses MAME's SAMSUNG_K9F1G08U0M NAND device.
 * This file is an ANSI C adaptation of the command/state behavior in
 * MAME src/devices/machine/nandflash.cpp (BSD-3-Clause,
 * copyright-holders: Raphael Nabet), with the CV1000-specific diagnostics
 * retained from the sandbox.
 */

static cv1k_u32 total_pages(const struct cv1k_nand *nand)
{
    if (nand->size < CV1K_NAND_PAGE_TOTAL) return 1UL;
    return nand->size / CV1K_NAND_PAGE_TOTAL;
}

static cv1k_u32 block_size_bytes(void)
{
    return CV1K_NAND_PAGE_TOTAL * CV1K_NAND_PAGES_PER_BLOCK;
}

static int page_data_is_empty(const cv1k_u8 *p)
{
    cv1k_u32 i;
    for (i = 0UL; i < CV1K_NAND_PAGE_SIZE; i++) if (p[i] != 0xffU) return 0;
    return 1;
}

static int page_spare_is_ff(const cv1k_u8 *p)
{
    cv1k_u32 i;
    for (i = 0UL; i < CV1K_NAND_OOB_SIZE; i++) if (p[CV1K_NAND_PAGE_SIZE + i] != 0xffU) return 0;
    return 1;
}

static void sync_cursor_from_mame_state(struct cv1k_nand *nand)
{
    cv1k_u32 pages;
    pages = total_pages(nand);
    if (pages == 0UL) pages = 1UL;
    nand->page = nand->page_addr % pages;
    nand->column = nand->byte_addr % CV1K_NAND_PAGE_TOTAL;
    nand->cursor = nand->page * CV1K_NAND_PAGE_TOTAL + nand->column;
    if (nand->size != 0UL) nand->cursor %= nand->size;
    nand->addr_latch = nand->cursor;
}

int cv1k_nand_init(struct cv1k_nand *nand, cv1k_u32 size)
{
    memset(nand, 0, sizeof(*nand));
    nand->data = (cv1k_u8 *)cv1k_xmalloc(size);
    nand->page_reg = (cv1k_u8 *)cv1k_xmalloc(CV1K_NAND_PAGE_TOTAL);
    if (nand->data == NULL || nand->page_reg == NULL) {
        cv1k_free(nand->data);
        cv1k_free(nand->page_reg);
        memset(nand, 0, sizeof(*nand));
        return 0;
    }
    nand->size = size;
    nand->manufacturer = 0xecU;
    nand->device = 0xf1U;
    nand->id[0] = 0xecU;
    nand->id[1] = 0xf1U;
    nand->id[2] = 0x00U;
    nand->id[3] = 0x15U;
    nand->id[4] = 0x00U;
    nand->id_len = 4U;
    nand->ce_enabled = 1;
    memset(nand->data, 0xff, (size_t)size);
    memset(nand->page_reg, 0x00, CV1K_NAND_PAGE_TOTAL);
    cv1k_nand_reset(nand);
    return 1;
}

void cv1k_nand_shutdown(struct cv1k_nand *nand)
{
    cv1k_free(nand->data);
    cv1k_free(nand->page_reg);
    memset(nand, 0, sizeof(*nand));
}

int cv1k_nand_load(struct cv1k_nand *nand, const char *path)
{
    cv1k_u32 got;
    if (nand->data == NULL) return 0;
    memset(nand->data, 0xff, (size_t)nand->size);
    got = 0UL;
    if (!cv1k_read_file(path, nand->data, nand->size, &got)) return 0;
    CV1K_UNUSED(got);
    cv1k_nand_scan_map(nand);
    cv1k_nand_reset(nand);
    return 1;
}

void cv1k_nand_reset(struct cv1k_nand *nand)
{
    nand->cursor = 0UL;
    nand->page = 0UL;
    nand->column = 0UL;
    nand->addr_latch = 0UL;
    nand->program_base = 0UL;
    nand->read_area_offset = 0UL;
    memset(nand->address_bytes, 0, sizeof(nand->address_bytes));
    nand->address_count = 0U;
    nand->address_expected = 0U;
    nand->random_read_pending = 0U;
    nand->prev_command = nand->command;
    nand->command = 0xffU;
    nand->id_index = 0U;
    nand->busy = 0U;
    nand->status = 0xc0U;
    nand->accumulated_status = 0U;
    nand->mode = CV1K_NAND_SM_INIT;
    nand->pointer_mode = CV1K_NAND_PTR_A;
    nand->addr_load_ptr = 0U;
    nand->mode_3065 = 0U;
    nand->page_addr = 0UL;
    nand->byte_addr = 0UL;
    nand->program_byte_count = 0UL;
    nand->last_read_page = 0UL;
    nand->last_read_block = 0UL;
    nand->last_read_column = 0UL;
    if (nand->page_reg != NULL) memset(nand->page_reg, 0x00, CV1K_NAND_PAGE_TOTAL);
}

void cv1k_nand_set_id(struct cv1k_nand *nand, cv1k_u8 manufacturer, cv1k_u8 device)
{
    nand->manufacturer = manufacturer;
    nand->device = device;
    nand->id[0] = manufacturer;
    nand->id[1] = device;
}

cv1k_u8 cv1k_nand_data_r(struct cv1k_nand *nand)
{
    cv1k_u8 reply;
    cv1k_u32 off;
    reply = 0U;
    if (nand == NULL || nand->data == NULL || nand->size == 0UL) return 0xffU;

    switch (nand->mode) {
    case CV1K_NAND_SM_READ:
    case CV1K_NAND_SM_RANDOM_DATA_OUTPUT:
        if (!nand->mode_3065) {
            if (nand->byte_addr < CV1K_NAND_PAGE_TOTAL) {
                if (nand->page_addr < total_pages(nand)) {
                    off = nand->page_addr * CV1K_NAND_PAGE_TOTAL + nand->byte_addr;
                    reply = nand->data[off % nand->size];
                    if (nand->data_only_reads && nand->byte_addr >= CV1K_NAND_PAGE_SIZE) reply = 0xffU;
                } else {
                    reply = 0xffU;
                }
            } else {
                reply = 0xffU;
            }
        } else {
            reply = 0xffU;
        }
        nand->last_read_page = nand->page_addr % total_pages(nand);
        nand->last_read_block = nand->last_read_page / CV1K_NAND_PAGES_PER_BLOCK;
        nand->last_read_column = nand->byte_addr;
        if (nand->byte_addr >= CV1K_NAND_PAGE_SIZE && nand->byte_addr < CV1K_NAND_PAGE_TOTAL) nand->spare_reads++;
        nand->byte_addr++;
        sync_cursor_from_mame_state(nand);
        nand->reads++;
        return reply;

    case CV1K_NAND_SM_READSTATUS:
        nand->reads++;
        nand->status_reads++;
        return (cv1k_u8)(nand->status & 0xc1U);

    case CV1K_NAND_SM_READID:
        if (nand->byte_addr < nand->id_len) reply = nand->id[nand->byte_addr];
        else reply = 0x00U;
        nand->byte_addr++;
        nand->id_index = (cv1k_u8)(nand->byte_addr & 0xffU);
        nand->reads++;
        nand->id_reads++;
        return reply;

    default:
        /* MAME logs this as an unexpected data-port read.  Keep the return
         * value deterministic and non-ready-looking for the standalone trace.
         */
        nand->reads++;
        return 0x00U;
    }
}

void cv1k_nand_data_w(struct cv1k_nand *nand, cv1k_u8 data)
{
    if (nand == NULL || nand->data == NULL || nand->size == 0UL) return;

    switch (nand->mode) {
    case CV1K_NAND_SM_PROGRAM:
    case CV1K_NAND_SM_RANDOM_DATA_INPUT:
        if (nand->page_reg != NULL && nand->program_byte_count < CV1K_NAND_PAGE_TOTAL) {
            nand->page_reg[nand->byte_addr % CV1K_NAND_PAGE_TOTAL] = data;
        }
        nand->program_byte_count++;
        nand->byte_addr++;
        if (nand->byte_addr == CV1K_NAND_PAGE_TOTAL) {
            nand->byte_addr = (nand->pointer_mode != CV1K_NAND_PTR_C) ? 0UL : CV1K_NAND_PAGE_SIZE;
        }
        sync_cursor_from_mame_state(nand);
        nand->writes++;
        break;
    default:
        break;
    }
}

void cv1k_nand_command_w(struct cv1k_nand *nand, cv1k_u8 data)
{
    cv1k_u32 base;
    cv1k_u32 i;
    if (nand == NULL) return;
    nand->prev_command = nand->command;
    nand->command = data;
    nand->command_counts[(cv1k_u32)data]++;
    nand->busy = 0U;

    switch (data) {
    case 0xffU:
        nand->mode = CV1K_NAND_SM_INIT;
        nand->pointer_mode = CV1K_NAND_PTR_A;
        nand->status = (cv1k_u8)((nand->status & 0x80U) | 0x40U);
        nand->accumulated_status = 0U;
        nand->mode_3065 = 0U;
        nand->addr_load_ptr = 0U;
        break;

    case 0x00U:
        nand->mode = CV1K_NAND_SM_READ;
        nand->pointer_mode = CV1K_NAND_PTR_A;
        nand->addr_load_ptr = 0U;
        nand->read_area_offset = 0UL;
        nand->random_read_pending = 0U;
        break;

    case 0x01U:
        /* MAME rejects upper-half select for large-page NAND. */
        nand->mode = CV1K_NAND_SM_INIT;
        break;

    case 0x50U:
        /* MAME rejects legacy spare-area select for large-page NAND. */
        nand->mode = CV1K_NAND_SM_INIT;
        break;

    case 0x80U:
        nand->mode = CV1K_NAND_SM_PROGRAM;
        nand->addr_load_ptr = 0U;
        nand->program_byte_count = 0UL;
        if (nand->page_reg != NULL) memset(nand->page_reg, 0xff, CV1K_NAND_PAGE_TOTAL);
        break;

    case 0x10U:
    case 0x15U:
        if (nand->mode == CV1K_NAND_SM_PROGRAM || nand->mode == CV1K_NAND_SM_RANDOM_DATA_INPUT) {
            nand->status = (cv1k_u8)((nand->status & 0x80U) | nand->accumulated_status);
            if (nand->page_reg != NULL && nand->page_addr < total_pages(nand)) {
                base = nand->page_addr * CV1K_NAND_PAGE_TOTAL;
                for (i = 0UL; i < CV1K_NAND_PAGE_TOTAL && base + i < nand->size; i++) {
                    nand->data[base + i] = (cv1k_u8)(nand->data[base + i] & nand->page_reg[i]);
                }
            }
            nand->status |= 0x40U;
            if (data == 0x15U) nand->accumulated_status = (cv1k_u8)(nand->status & 0x1fU);
            else nand->accumulated_status = 0U;
            nand->mode = CV1K_NAND_SM_INIT;
        } else {
            nand->mode = CV1K_NAND_SM_INIT;
        }
        break;

    case 0x60U:
        nand->mode = CV1K_NAND_SM_ERASE;
        nand->page_addr = 0UL;
        nand->addr_load_ptr = 0U;
        break;

    case 0xd0U:
        if (nand->mode == CV1K_NAND_SM_ERASE) {
            base = (nand->page_addr & ~(CV1K_NAND_PAGES_PER_BLOCK - 1UL)) * CV1K_NAND_PAGE_TOTAL;
            if (base < nand->size) {
                cv1k_u32 n;
                n = block_size_bytes();
                if (base + n > nand->size) n = nand->size - base;
                memset(&nand->data[base], 0xff, (size_t)n);
                nand->erases++;
            }
            nand->status = (cv1k_u8)((nand->status & 0x80U) | 0x40U);
            nand->mode = CV1K_NAND_SM_INIT;
            if (nand->pointer_mode == CV1K_NAND_PTR_B) nand->pointer_mode = CV1K_NAND_PTR_A;
        } else {
            nand->mode = CV1K_NAND_SM_INIT;
        }
        break;

    case 0x70U:
        nand->mode = CV1K_NAND_SM_READSTATUS;
        break;

    case 0x90U:
        nand->mode = CV1K_NAND_SM_READID;
        nand->addr_load_ptr = 0U;
        nand->byte_addr = 0UL;
        nand->id_index = 0U;
        break;

    case 0x30U:
        if (nand->mode == CV1K_NAND_SM_READ && nand->addr_load_ptr >= 4U) {
            /* K9F1G08U0M has two column cycles and two row cycles.  MAME does
             * not change mode on 30h; it simply accepts the second read cycle
             * once enough address cycles have been supplied.
             */
            sync_cursor_from_mame_state(nand);
        } else {
            nand->mode = CV1K_NAND_SM_INIT;
        }
        break;

    case 0x65U:
        if (nand->mode == CV1K_NAND_SM_30) nand->mode_3065 = 1U;
        else nand->mode = CV1K_NAND_SM_INIT;
        break;

    case 0x05U:
        if (nand->mode == CV1K_NAND_SM_READ || nand->mode == CV1K_NAND_SM_RANDOM_DATA_OUTPUT) {
            nand->mode = CV1K_NAND_SM_RANDOM_DATA_OUTPUT;
            nand->addr_load_ptr = 0U;
            nand->random_read_pending = 1U;
            nand->random_reads++;
        } else {
            nand->mode = CV1K_NAND_SM_INIT;
        }
        break;

    case 0xe0U:
        if (nand->mode == CV1K_NAND_SM_RANDOM_DATA_OUTPUT) sync_cursor_from_mame_state(nand);
        else nand->mode = CV1K_NAND_SM_INIT;
        nand->random_read_pending = 0U;
        break;

    case 0x85U:
        if (nand->mode == CV1K_NAND_SM_PROGRAM || nand->mode == CV1K_NAND_SM_RANDOM_DATA_INPUT) {
            nand->mode = CV1K_NAND_SM_RANDOM_DATA_INPUT;
            nand->addr_load_ptr = 0U;
            nand->program_byte_count = 0UL;
        } else {
            nand->mode = CV1K_NAND_SM_INIT;
        }
        break;

    default:
        nand->mode = CV1K_NAND_SM_INIT;
        break;
    }
}

void cv1k_nand_address_w(struct cv1k_nand *nand, cv1k_u8 data)
{
    if (nand == NULL) return;
    if (nand->address_count < 5U) {
        nand->address_bytes[nand->address_count] = data;
        nand->address_count++;
    }

    switch (nand->mode) {
    case CV1K_NAND_SM_READ:
    case CV1K_NAND_SM_PROGRAM:
        if (nand->addr_load_ptr == 0U) nand->page_addr = 0UL;
        if (nand->addr_load_ptr < 2U) {
            nand->byte_addr &= ~(0xffUL << (nand->addr_load_ptr * 8U));
            nand->byte_addr |= ((cv1k_u32)data << (nand->addr_load_ptr * 8U));
        } else if (nand->addr_load_ptr < 4U) {
            nand->page_addr &= ~(0xffUL << ((nand->addr_load_ptr - 2U) * 8U));
            nand->page_addr |= ((cv1k_u32)data << ((nand->addr_load_ptr - 2U) * 8U));
        }
        nand->addr_load_ptr++;
        sync_cursor_from_mame_state(nand);
        break;

    case CV1K_NAND_SM_ERASE:
        if (nand->addr_load_ptr < 2U) {
            nand->page_addr &= ~(0xffUL << (nand->addr_load_ptr * 8U));
            nand->page_addr |= ((cv1k_u32)data << (nand->addr_load_ptr * 8U));
        }
        nand->addr_load_ptr++;
        sync_cursor_from_mame_state(nand);
        break;

    case CV1K_NAND_SM_RANDOM_DATA_INPUT:
    case CV1K_NAND_SM_RANDOM_DATA_OUTPUT:
        if (nand->addr_load_ptr < 2U) {
            nand->byte_addr &= ~(0xffUL << (nand->addr_load_ptr * 8U));
            nand->byte_addr |= ((cv1k_u32)data << (nand->addr_load_ptr * 8U));
        }
        nand->addr_load_ptr++;
        sync_cursor_from_mame_state(nand);
        break;

    case CV1K_NAND_SM_READID:
        if (nand->addr_load_ptr == 0U) nand->byte_addr = data;
        nand->addr_load_ptr++;
        nand->id_index = (cv1k_u8)(nand->byte_addr & 0xffU);
        break;

    default:
        break;
    }
}

cv1k_u8 cv1k_nand_is_busy(const struct cv1k_nand *nand)
{
    return (nand->status & 0x40U) == 0U;
}

void cv1k_nand_set_data_only_reads(struct cv1k_nand *nand, int enabled)
{
    nand->data_only_reads = enabled ? 1 : 0;
}

void cv1k_nand_set_ce(struct cv1k_nand *nand, int enabled)
{
    nand->ce_enabled = enabled ? 1 : 0;
}

void cv1k_nand_scan_map(struct cv1k_nand *nand)
{
    cv1k_u32 pages;
    cv1k_u32 blocks;
    cv1k_u32 b;
    cv1k_u32 p;
    cv1k_u32 base;
    int block_empty;
    int block_marked;
    if (nand == NULL || nand->data == NULL || nand->size < CV1K_NAND_PAGE_TOTAL) return;
    pages = total_pages(nand);
    blocks = pages / CV1K_NAND_PAGES_PER_BLOCK;
    nand->map_blocks = blocks;
    nand->map_empty_blocks = 0UL;
    nand->map_oob_marked_blocks = 0UL;
    nand->map_spare_non_ff_pages = 0UL;
    for (b = 0UL; b < blocks; b++) {
        block_empty = 1;
        block_marked = 0;
        for (p = 0UL; p < CV1K_NAND_PAGES_PER_BLOCK; p++) {
            base = (b * CV1K_NAND_PAGES_PER_BLOCK + p) * CV1K_NAND_PAGE_TOTAL;
            if (base + CV1K_NAND_PAGE_TOTAL > nand->size) break;
            if (!page_data_is_empty(&nand->data[base])) block_empty = 0;
            if (!page_spare_is_ff(&nand->data[base])) nand->map_spare_non_ff_pages++;
            if (p < 2UL) {
                const cv1k_u8 *oob;
                oob = &nand->data[base + CV1K_NAND_PAGE_SIZE];
                if (oob[0] != 0xffU || oob[5] != 0xffU) block_marked = 1;
            }
        }
        if (block_empty) nand->map_empty_blocks++;
        if (block_marked) nand->map_oob_marked_blocks++;
    }
}

cv1k_u32 cv1k_nand_current_page(const struct cv1k_nand *nand)
{
    if (nand == NULL || nand->size == 0UL) return 0UL;
    return nand->page_addr % total_pages(nand);
}

cv1k_u32 cv1k_nand_current_block(const struct cv1k_nand *nand)
{
    return cv1k_nand_current_page(nand) / CV1K_NAND_PAGES_PER_BLOCK;
}
