#ifndef CV1K_ROMSET_H
#define CV1K_ROMSET_H

#include "emu.h"

#define CV1K_ROM_ENTRY_COUNT 4

struct cv1k_rom_entry_report {
    char name[16];
    cv1k_u32 expected_crc;
    cv1k_u32 actual_crc;
    cv1k_u32 source_size;
    cv1k_u32 loaded_size;
    int present;
    int crc_ok;
    int ignored_tail;
};

struct cv1k_romset_report {
    char set_name[32];
    char description[160];
    char source_path[256];
    struct cv1k_rom_entry_report entry[CV1K_ROM_ENTRY_COUNT];
    cv1k_u32 boot_loaded;
    cv1k_u32 nand_loaded;
    cv1k_u32 sound_loaded;
    cv1k_u32 idle_pc;
    int display_rotation;
    int model;
    int ok;
    int used_zip;
    char message[256];
};

void cv1k_romset_report_clear(struct cv1k_romset_report *r);
int cv1k_romset_load_ddpsdoj(struct cv1k_machine *m, const char *path, struct cv1k_romset_report *report);
void cv1k_romset_report_text(const struct cv1k_romset_report *r, char *out, cv1k_u32 out_size);

#endif
