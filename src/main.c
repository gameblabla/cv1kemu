#include "emu.h"
#include "platform.h"
#include "ui_tui.h"
#include "ui_sdl3.h"
#include "ui_sdl12.h"
#include "savestate.h"
#include "romset.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_INPUT_SCRIPT_EVENTS 64

struct input_script_event {
    int id;
    int start;
    int duration;
};

static void usage(void)
{
    printf("cv1k-sandbox v53 ANSI C / SDL 1.2 audio-video-input frontend\n");
    printf("usage: cv1k_sandbox [options]\n");
    printf("  --model b|d           choose CV1000-B or CV1000-D RAM map\n");
    printf("  --romset path         load ddpsdoj zip or extracted directory with u2/u4/u23/u24\n");
    printf("  --boot file           load U4 boot/program flash image\n");
    printf("  --nand file           load U2 NAND image\n");
    printf("  --sound file          load U23/U24 concatenated sound data\n");
    printf("  --eeprom file         load RTC9701 EEPROM data\n");
    printf("  --ram file            preload main RAM for blitter/CPU tests\n");
    printf("  --blit hexaddr        execute a CV1000 blitter operation list in RAM\n");
    printf("  --input-map file      load text key mapping\n");
    printf("  --tap-input name,start,frames  press an input during headless run\n");
    printf("  --hold-input name     hold an input during the whole headless run\n");
    printf("  --run-frames n        run without interactive UI\n");
    printf("  --trace-steps n       print CPU PC/opcode/register trace, then exit if no run-frames\n");
    printf("  --trace-fetch         trace cached fetch opcode as well as raw RAM opcode\n");
    printf("  --aggressive-assists  enable risky DDPSDOJ boot shims past documented blockers\n");
    printf("  --dcache              enable experimental non-coherent SH data-cache model\n");
    printf("  --strict-cache-ops    honor PREF/OCB/P4 cache-control invalidation experiments\n");
    printf("  --mame-cache-meta     collect MAME SH7709S-style 16KiB/16B/4-way cache metadata\n");
    printf("  --mame-trapa          vector SH7709S TRAPA like MAME instead of diagnostic no-vector mode\n");
    printf("  --mame-speedup        emulate MAME's DDPSDOJ spin-until-interrupt speedup\n");
    printf("  --mame-full-dmatcr    use MAME's full 0x1000000 zero-DMATCR count (diagnostic)\n");
    printf("  --wide-p0-alias      map broader SH P0 virtual aliases to CV1000-D work RAM\n");
    printf("  --vblank-irq-and-tick request IRQ2 but retain synthetic vblank RAM tick\n");
    printf("  --nand-scan          print physical NAND/OOB scan summary\n");
    printf("  --dump-ram file      dump a work-RAM slice after running\n");
    printf("  --dump-nand file     dump the current NAND image after running\n");
    printf("  --dump-eeprom file   dump the current RTC9701 EEPROM data after running\n");
    printf("  --dump-ram-addr hex  source address/offset for --dump-ram, default 0x0c000000\n");
    printf("  --dump-ram-size n    byte count for --dump-ram, default 0x10000\n");
    printf("  --break-pc hex        step until PC reaches hex before optional trace/run\n");
    printf("  --break-max n         maximum instructions for --break-pc, default 10000000\n");
    printf("  --run-break-pc hex    run frame-clocked CPU until PC reaches hex\n");
    printf("  --sdl                 run the compiled SDL frontend, SDL 1.2 for make sdl12 or SDL3 for make sdl3\n");
    printf("  --sdl12               run SDL 1.2 frontend when compiled with make sdl12\n");
    printf("  --probe-title         render ddpsdoj diagnostic/probe screen\n");
    printf("  --save-state file     save state after running\n");
    printf("  --load-state file     load state before running\n");
    printf("  --dump-ppm file       dump current frame buffer\n");
    printf("  --help                show help\n");
}

static void compact_line(const struct cv1k_romset_report *rr, char *a, char *b, char *c)
{
    if (rr == NULL) {
        strcpy(a, "NO ROMSET REPORT");
        strcpy(b, "");
        strcpy(c, "");
        return;
    }
    sprintf(a, "ROM %s  CRC %s", rr->set_name, rr->ok ? "OK" : "BAD");
    sprintf(b, "U4 %08lx U2 %08lx", (unsigned long)rr->entry[1].actual_crc, (unsigned long)rr->entry[0].actual_crc);
    sprintf(c, "U23 %08lx U24 %08lx", (unsigned long)rr->entry[2].actual_crc, (unsigned long)rr->entry[3].actual_crc);
}



static cv1k_u32 parse_u32_arg(const char *text)
{
    int i;
    int has_hex;
    cv1k_u32 v;
    if (text == NULL) return 0UL;
    has_hex = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) return (cv1k_u32)strtoul(text, NULL, 0);
    for (i = 0; text[i] != '\0'; i++) {
        if ((text[i] >= 'a' && text[i] <= 'f') || (text[i] >= 'A' && text[i] <= 'F')) has_hex = 1;
    }
    if (has_hex || i == 8) v = (cv1k_u32)strtoul(text, NULL, 16);
    else v = (cv1k_u32)strtoul(text, NULL, 0);
    return v;
}

static int parse_input_event(const char *text, struct input_script_event *ev)
{
    char name[64];
    int start;
    int duration;
    int id;
    if (text == NULL || ev == NULL) return 0;
    name[0] = '\0';
    start = 0;
    duration = 0;
    if (sscanf(text, "%63[^,],%d,%d", name, &start, &duration) != 3) return 0;
    id = cv1k_input_id_from_name(name);
    if (id < 0 || start < 0 || duration <= 0) return 0;
    ev->id = id;
    ev->start = start;
    ev->duration = duration;
    return 1;
}

static void apply_input_script(struct cv1k_input *input, const struct input_script_event *events, int count, int frame)
{
    int i;
    if (input == NULL || events == NULL || count <= 0) return;
    memset(input->state, 0, sizeof(input->state));
    for (i = 0; i < count; i++) {
        if (frame >= events[i].start && frame < events[i].start + events[i].duration) input->state[events[i].id] = 1U;
    }
}

static void debug_frame_clocked_until(struct cv1k_machine *m, cv1k_u32 target_pc, cv1k_u32 max_frames)
{
    cv1k_u32 f;
    cv1k_u32 s;
    cv1k_u32 total;
    total = 0UL;
    for (f = 0UL; f < max_frames && m->cpu.illegal_count == 0UL; f++) {
        cv1k_u32 cycles_before;
        int speedup_skip_once;
        int speedup_did_tick;
        speedup_skip_once = (m->mame_speedup && (m->cpu.pc == 0x0c1d1346UL || m->cpu.pc == 0x0c1d1348UL));
        speedup_did_tick = 0;
        cycles_before = m->cpu.cycles;
        for (s = 0UL; s < 20000UL && m->cpu.illegal_count == 0UL; s++) {
            if (m->cpu.pc == target_pc) {
                printf("run-break-pc hit pc=%08lx frame=%lu step=%lu total=%lu cycles=%lu\n",
                    (unsigned long)m->cpu.pc, (unsigned long)m->frames, (unsigned long)s, (unsigned long)total, (unsigned long)m->cpu.cycles);
                return;
            }
            /* use the normal machine step path, including documented boot assists */
            if (m->mame_speedup && (m->cpu.pc == 0x0c1d1346UL || m->cpu.pc == 0x0c1d1348UL)) {
                if (!speedup_skip_once) {
                    cv1k_u32 v;
                    v = cv1k_bus_read32(&m->bus, 0x0c002310UL);
                    cv1k_bus_write32(&m->bus, 0x0c002310UL, v + 1UL);
                    m->cpu.r[0] = cv1k_bus_read32(&m->bus, 0x0c002310UL);
                    m->mame_speedup_spins++;
                    speedup_did_tick = 1;
                    break;
                }
            } else {
                speedup_skip_once = 0;
            }
            cv1k_machine_step(m);
            if (!(m->mame_speedup && (m->cpu.pc == 0x0c1d1346UL || m->cpu.pc == 0x0c1d1348UL))) speedup_skip_once = 0;
            total++;
        }
        if (m->irq2_enabled) {
            if (((cycles_before / CV1K_CYCLES_PER_VBLANK) != (m->cpu.cycles / CV1K_CYCLES_PER_VBLANK)) ||
                (speedup_did_tick && m->video.executed_ops != 0UL && ((m->mame_speedup_spins & 0x3ffUL) == 0UL))) {
                cv1k_u16 iprc;
                int irq_pri;
                iprc = (cv1k_u16)(((cv1k_u16)m->sh_io[0x16] << 8) | (cv1k_u16)m->sh_io[0x17]);
                irq_pri = (int)((iprc >> 8) & 0x0fU);
                sh7709s_request_irq_line(&m->cpu, 2, irq_pri);
                if (m->vblank_irq_and_tick && !speedup_did_tick) {
                    cv1k_u32 v;
                    v = cv1k_bus_read32(&m->bus, 0x0c002310UL);
                    cv1k_bus_write32(&m->bus, 0x0c002310UL, v + 1UL);
                }
            }
        } else if (!speedup_did_tick) {
            cv1k_u32 v;
            v = cv1k_bus_read32(&m->bus, 0x0c002310UL);
            cv1k_bus_write32(&m->bus, 0x0c002310UL, v + 1UL);
        }
        cv1k_video_frame(&m->video, m->main_ram, m->main_ram_size);
        m->frames++;
    }
    printf("run-break-pc not-hit pc=%08lx frames=%lu total=%lu illegal=%lu\n",
        (unsigned long)m->cpu.pc, (unsigned long)m->frames, (unsigned long)total, (unsigned long)m->cpu.illegal_count);
}

static void put_u32le(unsigned char *p, cv1k_u32 v)
{
    p[0] = (unsigned char)(v & 0xffU);
    p[1] = (unsigned char)((v >> 8) & 0xffU);
    p[2] = (unsigned char)((v >> 16) & 0xffU);
    p[3] = (unsigned char)((v >> 24) & 0xffU);
}

/* Write a 44-byte canonical PCM WAV header for 16-bit stereo. */
static void write_wav_header(FILE *f, cv1k_u32 sample_rate, cv1k_u32 channels, cv1k_u32 frames)
{
    unsigned char h[44];
    cv1k_u32 byte_rate = sample_rate * channels * 2UL;
    cv1k_u32 data_bytes = frames * channels * 2UL;
    memcpy(h, "RIFF", 4);
    put_u32le(h + 4, 36UL + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_u32le(h + 16, 16UL);
    h[20] = 1; h[21] = 0;                       /* PCM */
    h[22] = (unsigned char)channels; h[23] = 0;
    put_u32le(h + 24, sample_rate);
    put_u32le(h + 28, byte_rate);
    h[32] = (unsigned char)(channels * 2UL); h[33] = 0; /* block align */
    h[34] = 16; h[35] = 0;                      /* bits per sample */
    memcpy(h + 36, "data", 4);
    put_u32le(h + 40, data_bytes);
    fwrite(h, 1, 44, f);
}

int main(int argc, char **argv)
{
    struct cv1k_machine m;
    struct cv1k_romset_report rr;
    int have_report;
    int model;
    int i;
    int run_frames;
    int sdl_requested;
    int sdl12_requested;
    int probe_requested;
    int trace_steps;
    int trace_fetch;
    int irq2_enabled;
    int aggressive_assists;
    int dcache_requested;
    int strict_cache_ops;
    int mame_cache_meta;
    int mame_trapa;
    int mame_speedup;
    int mame_full_dmatcr;
    int mame_tmu_irq;
    int wide_p0_alias;
    int compact_400_alias;
    int nand_data_only;
    int dma_cache_sync;
    int vblank_irq_and_tick;
    int nand_scan;
    const char *dump_ram;
    const char *dump_nand;
    const char *dump_eeprom;
    cv1k_u32 dump_ram_addr;
    cv1k_u32 dump_ram_size;
    int break_requested;
    cv1k_u32 break_pc;
    cv1k_u32 break_max;
    int run_break_requested;
    cv1k_u32 run_break_pc;
    const char *save_state;
    const char *load_state;
    const char *dump_ppm;
    const char *dump_audio;
    struct input_script_event input_script[MAX_INPUT_SCRIPT_EVENTS];
    int input_script_count;
    cv1k_u32 blit_addr;
    int blit_requested;
    char status[4096];
    char l1[96];
    char l2[96];
    char l3[96];

    if (!cv1k_platform_check()) {
        fprintf(stderr, "unsupported C integer layout\n");
        return 2;
    }

    model = CV1K_MODEL_D;
    run_frames = -1;
    sdl_requested = 0;
    sdl12_requested = 0;
    probe_requested = 0;
    trace_steps = 0;
    trace_fetch = 0;
    /* The MAME-derived SH-3 core needs the real vblank IRQ2 and benefits from
     * the idle-loop skip, so both are on by default now (a proper emulator,
     * not a debug timeslice).  Use --no-irq2 to override for diagnostics. */
    irq2_enabled = 1;
    aggressive_assists = 0;
    dcache_requested = 0;
    strict_cache_ops = 0;
    mame_cache_meta = 0;
    mame_trapa = 0;
    mame_speedup = 1;
    mame_full_dmatcr = 0;
    /* TMU underflow interrupt drives the game's sound engine; the multi-source
     * INTC now lets it coexist with the vblank IRQ, so enable it by default. */
    mame_tmu_irq = 1;
    wide_p0_alias = 0;
    compact_400_alias = 0;
    nand_data_only = 0;
    dma_cache_sync = 0;
    vblank_irq_and_tick = 0;
    nand_scan = 0;
    dump_ram = NULL;
    dump_nand = NULL;
    dump_eeprom = NULL;
    dump_ram_addr = CV1K_ADDR_WORK_RAM;
    dump_ram_size = 0x10000UL;
    break_requested = 0;
    break_pc = 0UL;
    break_max = 10000000UL;
    run_break_requested = 0;
    run_break_pc = 0UL;
    save_state = NULL;
    load_state = NULL;
    dump_ppm = NULL;
    dump_audio = NULL;
    input_script_count = 0;
    blit_addr = 0UL;
    blit_requested = 0;
    have_report = 0;
    cv1k_romset_report_clear(&rr);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            i++;
            if (argv[i][0] == 'd' || argv[i][0] == 'D') model = CV1K_MODEL_D;
            else model = CV1K_MODEL_B;
        } else if (strcmp(argv[i], "--sdl") == 0) {
            sdl_requested = 1;
        } else if (strcmp(argv[i], "--sdl12") == 0) {
            sdl12_requested = 1;
        } else if (strcmp(argv[i], "--probe-title") == 0) {
            probe_requested = 1;
        }
    }

    if (!cv1k_machine_init(&m, model)) {
        fprintf(stderr, "failed to initialize machine\n");
        return 2;
    }
    m.irq2_enabled = irq2_enabled;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--boot") == 0 && i + 1 < argc) {
            i++;
            if (!cv1k_machine_load_boot(&m, argv[i])) fprintf(stderr, "warning: failed to load boot: %s\n", argv[i]);
        } else if (strcmp(argv[i], "--romset") == 0 && i + 1 < argc) {
            i++;
            if (!cv1k_romset_load_ddpsdoj(&m, argv[i], &rr)) fprintf(stderr, "warning: ddpsdoj romset load not clean: %s\n", rr.message);
            have_report = 1;
        } else if (strcmp(argv[i], "--nand") == 0 && i + 1 < argc) {
            i++;
            if (!cv1k_machine_load_nand(&m, argv[i])) fprintf(stderr, "warning: failed to load nand: %s\n", argv[i]);
        } else if (strcmp(argv[i], "--sound") == 0 && i + 1 < argc) {
            i++;
            if (!cv1k_machine_load_sound(&m, argv[i])) fprintf(stderr, "warning: failed to load sound: %s\n", argv[i]);
        } else if (strcmp(argv[i], "--eeprom") == 0 && i + 1 < argc) {
            i++;
            if (!cv1k_rtc9701_load_eeprom(&m.rtc, argv[i])) fprintf(stderr, "warning: failed to load eeprom: %s\n", argv[i]);
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            i++;
            if (!cv1k_machine_load_ram(&m, argv[i])) fprintf(stderr, "warning: failed to load RAM: %s\n", argv[i]);
        } else if (strcmp(argv[i], "--input-map") == 0 && i + 1 < argc) {
            i++;
            if (!cv1k_input_load_map(&m.input, argv[i])) fprintf(stderr, "warning: failed to load input map: %s\n", argv[i]);
        } else if (strcmp(argv[i], "--tap-input") == 0 && i + 1 < argc) {
            i++;
            if (input_script_count >= MAX_INPUT_SCRIPT_EVENTS || !parse_input_event(argv[i], &input_script[input_script_count])) {
                fprintf(stderr, "warning: bad input event: %s\n", argv[i]);
            } else {
                input_script_count++;
            }
        } else if (strcmp(argv[i], "--hold-input") == 0 && i + 1 < argc) {
            i++;
            if (input_script_count >= MAX_INPUT_SCRIPT_EVENTS) {
                fprintf(stderr, "warning: too many input events\n");
            } else {
                int id;
                id = cv1k_input_id_from_name(argv[i]);
                if (id < 0) {
                    fprintf(stderr, "warning: bad input name: %s\n", argv[i]);
                } else {
                    input_script[input_script_count].id = id;
                    input_script[input_script_count].start = 0;
                    input_script[input_script_count].duration = 0x7fffffff;
                    input_script_count++;
                }
            }
        } else if (strcmp(argv[i], "--run-frames") == 0 && i + 1 < argc) {
            i++;
            run_frames = atoi(argv[i]);
        } else if (strcmp(argv[i], "--irq2") == 0) {
            irq2_enabled = 1;
        } else if (strcmp(argv[i], "--no-irq2") == 0) {
            irq2_enabled = 0;
        } else if (strcmp(argv[i], "--no-speedup") == 0) {
            mame_speedup = 0;
        } else if (strcmp(argv[i], "--aggressive-assists") == 0) {
            aggressive_assists = 1;
        } else if (strcmp(argv[i], "--dcache") == 0) {
            dcache_requested = 1;
        } else if (strcmp(argv[i], "--strict-cache-ops") == 0) {
            strict_cache_ops = 1;
        } else if (strcmp(argv[i], "--mame-cache-meta") == 0) {
            mame_cache_meta = 1;
        } else if (strcmp(argv[i], "--mame-trapa") == 0) {
            mame_trapa = 1;
        } else if (strcmp(argv[i], "--mame-speedup") == 0) {
            mame_speedup = 1;
            mame_trapa = 1;
        } else if (strcmp(argv[i], "--mame-full-dmatcr") == 0) {
            mame_full_dmatcr = 1;
        } else if (strcmp(argv[i], "--mame-tmu-irq") == 0) {
            mame_tmu_irq = 1;
        } else if (strcmp(argv[i], "--no-tmu-irq") == 0) {
            mame_tmu_irq = 0;
        } else if (strcmp(argv[i], "--dump-audio") == 0 && i + 1 < argc) {
            i++;
            dump_audio = argv[i];
        } else if (strcmp(argv[i], "--wide-p0-alias") == 0) {
            wide_p0_alias = 1;
        } else if (strcmp(argv[i], "--compact-400-alias") == 0) {
            compact_400_alias = 1;
        } else if (strcmp(argv[i], "--nand-data-only") == 0) {
            nand_data_only = 1;
        } else if (strcmp(argv[i], "--dma-cache-sync") == 0) {
            dma_cache_sync = 1;
        } else if (strcmp(argv[i], "--vblank-irq-and-tick") == 0) {
            irq2_enabled = 1;
            vblank_irq_and_tick = 1;
        } else if (strcmp(argv[i], "--nand-scan") == 0) {
            nand_scan = 1;
        } else if (strcmp(argv[i], "--dump-ram") == 0 && i + 1 < argc) {
            i++;
            dump_ram = argv[i];
        } else if (strcmp(argv[i], "--dump-nand") == 0 && i + 1 < argc) {
            i++;
            dump_nand = argv[i];
        } else if (strcmp(argv[i], "--dump-eeprom") == 0 && i + 1 < argc) {
            i++;
            dump_eeprom = argv[i];
        } else if (strcmp(argv[i], "--dump-ram-addr") == 0 && i + 1 < argc) {
            i++;
            dump_ram_addr = parse_u32_arg(argv[i]);
        } else if (strcmp(argv[i], "--dump-ram-size") == 0 && i + 1 < argc) {
            i++;
            dump_ram_size = parse_u32_arg(argv[i]);
        } else if (strcmp(argv[i], "--trace-steps") == 0 && i + 1 < argc) {
            i++;
            trace_steps = atoi(argv[i]);
        } else if (strcmp(argv[i], "--trace-fetch") == 0) {
            trace_fetch = 1;
        } else if (strcmp(argv[i], "--break-pc") == 0 && i + 1 < argc) {
            i++;
            break_pc = parse_u32_arg(argv[i]);
            break_requested = 1;
        } else if (strcmp(argv[i], "--break-max") == 0 && i + 1 < argc) {
            i++;
            break_max = parse_u32_arg(argv[i]);
        } else if (strcmp(argv[i], "--run-break-pc") == 0 && i + 1 < argc) {
            i++;
            run_break_pc = parse_u32_arg(argv[i]);
            run_break_requested = 1;
        } else if (strcmp(argv[i], "--save-state") == 0 && i + 1 < argc) {
            i++;
            save_state = argv[i];
        } else if (strcmp(argv[i], "--load-state") == 0 && i + 1 < argc) {
            i++;
            load_state = argv[i];
        } else if (strcmp(argv[i], "--dump-ppm") == 0 && i + 1 < argc) {
            i++;
            dump_ppm = argv[i];
        } else if (strcmp(argv[i], "--blit") == 0 && i + 1 < argc) {
            i++;
            blit_addr = parse_u32_arg(argv[i]);
            blit_requested = 1;
        }
    }

    m.irq2_enabled = irq2_enabled;
    m.aggressive_boot_assists = aggressive_assists;
    m.dcache_enabled = dcache_requested;
    m.strict_cache_ops = strict_cache_ops;
    m.mame_cache_meta = mame_cache_meta;
    m.mame_trapa = mame_trapa;
    m.mame_speedup = mame_speedup;
    m.mame_full_dmatcr = mame_full_dmatcr;
    m.mame_tmu_irq = mame_tmu_irq;
    m.wide_p0_alias = wide_p0_alias;
    m.compact_400_alias = compact_400_alias;
    m.dma_cache_sync = dma_cache_sync;
    m.vblank_irq_and_tick = vblank_irq_and_tick;
    cv1k_nand_set_data_only_reads(&m.nand, nand_data_only);

    if (nand_scan) {
        printf("nand-scan blocks=%lu empty=%lu oob_marked=%lu spare_non_ff_pages=%lu size=%lu\n",
            (unsigned long)m.nand.map_blocks,
            (unsigned long)m.nand.map_empty_blocks,
            (unsigned long)m.nand.map_oob_marked_blocks,
            (unsigned long)m.nand.map_spare_non_ff_pages,
            (unsigned long)m.nand.size);
    }

    if (load_state != NULL) {
        if (!cv1k_load_state(&m, load_state)) fprintf(stderr, "warning: state load failed: %s\n", load_state);
        /* Loading a snapshot restores emulated machine state, but command-line
         * execution controls are frontend/debug options.  Re-apply them after
         * load so a saved title state can still be tested with the same
         * MAME-derived cache, IRQ, speedup, and alias modes requested on the
         * current command line.
         */
        m.irq2_enabled = irq2_enabled;
        m.aggressive_boot_assists = aggressive_assists;
        m.dcache_enabled = dcache_requested;
        m.strict_cache_ops = strict_cache_ops;
        m.mame_cache_meta = mame_cache_meta;
        m.mame_trapa = mame_trapa;
        m.mame_speedup = mame_speedup;
        m.mame_full_dmatcr = mame_full_dmatcr;
        m.mame_tmu_irq = mame_tmu_irq;
        m.wide_p0_alias = wide_p0_alias;
        m.compact_400_alias = compact_400_alias;
        m.dma_cache_sync = dma_cache_sync;
        m.vblank_irq_and_tick = vblank_irq_and_tick;
        cv1k_nand_set_data_only_reads(&m.nand, nand_data_only);
    }

    if (run_break_requested) {
        cv1k_u32 maxf;
        maxf = (run_frames > 0) ? (cv1k_u32)run_frames : 20000UL;
        debug_frame_clocked_until(&m, run_break_pc, maxf);
        run_frames = 0;
    }

    if (break_requested) {
        cv1k_u32 bi;
        for (bi = 0UL; bi < break_max && m.cpu.pc != break_pc && m.cpu.illegal_count == 0UL; bi++) {
            cv1k_machine_step(&m);
        }
        if (m.cpu.pc == break_pc) printf("break-pc hit pc=%08lx after=%lu cycles=%lu\n", (unsigned long)m.cpu.pc, (unsigned long)bi, (unsigned long)m.cpu.cycles);
        else printf("break-pc not-hit pc=%08lx after=%lu illegal=%lu\n", (unsigned long)m.cpu.pc, (unsigned long)bi, (unsigned long)m.cpu.illegal_count);
    }

    if (trace_steps > 0) {
        int ti;
        for (ti = 0; ti < trace_steps; ti++) {
            cv1k_u32 pc0;
            cv1k_u16 op0;
            pc0 = m.cpu.pc;
            op0 = trace_fetch ? cv1k_bus_fetch16(&m.bus, pc0) : cv1k_bus_read16(&m.bus, pc0);
            printf("T %06d pc=%08lx op=%04lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx r6=%08lx r7=%08lx r8=%08lx r9=%08lx ra=%08lx rb=%08lx rc=%08lx rd=%08lx re=%08lx rf=%08lx sr=%08lx\n",
                ti, (unsigned long)pc0, (unsigned long)op0,
                (unsigned long)m.cpu.r[0], (unsigned long)m.cpu.r[1], (unsigned long)m.cpu.r[2], (unsigned long)m.cpu.r[3],
                (unsigned long)m.cpu.r[4], (unsigned long)m.cpu.r[5], (unsigned long)m.cpu.r[6], (unsigned long)m.cpu.r[7],
                (unsigned long)m.cpu.r[8], (unsigned long)m.cpu.r[9], (unsigned long)m.cpu.r[10], (unsigned long)m.cpu.r[11],
                (unsigned long)m.cpu.r[12], (unsigned long)m.cpu.r[13], (unsigned long)m.cpu.r[14], (unsigned long)m.cpu.r[15],
                (unsigned long)m.cpu.sr);
            cv1k_machine_step(&m);
        }
        if (run_frames < 0) run_frames = 0;
    }

    if (blit_requested) {
        cv1k_machine_blit(&m, blit_addr);
        cv1k_machine_frame(&m);
    }

    if (probe_requested) {
        compact_line(have_report ? &rr : NULL, l1, l2, l3);
        cv1k_machine_render_probe(&m, l1, l2, l3);
    }

    if (sdl_requested || sdl12_requested) {
        if (have_report) {
            compact_line(&rr, l1, l2, l3);
            cv1k_machine_render_probe(&m, l1, l2, l3);
        }
#ifdef CV1K_DEFAULT_SDL12
        cv1k_ui_sdl12_run(&m);
#else
        if (sdl12_requested) cv1k_ui_sdl12_run(&m);
        else cv1k_ui_sdl3_run(&m);
#endif
    } else if (run_frames >= 0) {
        FILE *af = NULL;
        cv1k_u32 a_accum = 0UL;
        cv1k_u32 a_total = 0UL;
        if (dump_audio != NULL) {
            af = fopen(dump_audio, "wb");
            if (af != NULL) write_wav_header(af, CV1K_YMZ770_CLOCK_HZ / 1024UL, 2UL, 0UL);
        }
        for (i = 0; i < run_frames; i++) {
            apply_input_script(&m.input, input_script, input_script_count, i);
            cv1k_machine_frame(&m);
            if (af != NULL) {
                short abuf[2048];
                cv1k_u32 n;
                a_accum += (CV1K_YMZ770_CLOCK_HZ / 1024UL) * 1000UL;
                n = a_accum / CV1K_REFRESH_MILLIHZ;
                a_accum -= n * CV1K_REFRESH_MILLIHZ;
                if (n > 1024UL) n = 1024UL;
                cv1k_ymz770_mix_s16_stereo(&m.ymz, m.sound_rom, m.sound_rom_size, abuf, n);
                fwrite(abuf, sizeof(short) * 2U, n, af);
                a_total += n;
            }
        }
        if (af != NULL) {
            fseek(af, 0L, SEEK_SET);
            write_wav_header(af, CV1K_YMZ770_CLOCK_HZ / 1024UL, 2UL, a_total);
            fclose(af);
        }
        if (probe_requested) {
            compact_line(have_report ? &rr : NULL, l1, l2, l3);
            cv1k_machine_render_probe(&m, l1, l2, l3);
        }
        cv1k_machine_status(&m, status, (cv1k_u32)sizeof(status));
        printf("%s\n", status);
        if (have_report) {
            cv1k_romset_report_text(&rr, status, (cv1k_u32)sizeof(status));
            printf("%s", status);
        }
    } else {
        cv1k_ui_tui_run(&m);
    }

    if (dump_ppm != NULL) {
        cv1k_video_frame(&m.video, m.main_ram, m.main_ram_size);
        if (!cv1k_video_write_ppm(&m.video, dump_ppm)) fprintf(stderr, "warning: PPM dump failed: %s\n", dump_ppm);
    }

    if (dump_ram != NULL) {
        FILE *rf;
        cv1k_u32 roff;
        cv1k_u32 rsize;
        roff = dump_ram_addr;
        if (roff >= CV1K_ADDR_WORK_RAM && roff < CV1K_ADDR_WORK_RAM + m.main_ram_size) roff -= CV1K_ADDR_WORK_RAM;
        if (roff >= m.main_ram_size) {
            fprintf(stderr, "warning: RAM dump address out of range: %08lx\n", (unsigned long)dump_ram_addr);
        } else {
            rsize = dump_ram_size;
            if (roff + rsize < roff || roff + rsize > m.main_ram_size) rsize = m.main_ram_size - roff;
            rf = fopen(dump_ram, "wb");
            if (rf == NULL || fwrite(m.main_ram + roff, 1U, (size_t)rsize, rf) != (size_t)rsize) fprintf(stderr, "warning: RAM dump failed: %s\n", dump_ram);
            if (rf != NULL) fclose(rf);
        }
    }

    if (dump_nand != NULL) {
        FILE *nf;
        nf = fopen(dump_nand, "wb");
        if (nf == NULL || fwrite(m.nand.data, 1U, (size_t)m.nand.size, nf) != (size_t)m.nand.size) fprintf(stderr, "warning: NAND dump failed: %s\n", dump_nand);
        if (nf != NULL) fclose(nf);
    }

    if (dump_eeprom != NULL) {
        if (!cv1k_rtc9701_save_eeprom(&m.rtc, dump_eeprom)) fprintf(stderr, "warning: EEPROM dump failed: %s\n", dump_eeprom);
    }

    if (save_state != NULL) {
        if (!cv1k_save_state(&m, save_state)) fprintf(stderr, "warning: state save failed: %s\n", save_state);
    }

    cv1k_machine_shutdown(&m);
    return 0;
}
