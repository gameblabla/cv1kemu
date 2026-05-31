#include "ui_tui.h"
#include "savestate.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_menu(void)
{
    printf("\n");
    printf("============================================================\n");
    printf(" CV1000 Sandbox             Main | States | Controls | Debug\n");
    printf("============================================================\n");
    printf(" run <n>        run n frames          step <n>       run n CPU instructions\n");
    printf(" save <file>    save state            load <file>    load state\n");
    printf(" ppm <file>     dump frame PPM        blit <addr>     execute RAM blit list\n");
    printf(" press <key>    tap mapped key        savemap <file> write controls\n");
    printf(" map            show controls         remap <id> <k> change control\n");
    printf(" status         show machine state    reset          reset emulation\n");
    printf(" help           show this menu        quit           exit\n");
    printf("============================================================\n");
}

static void show_map(struct cv1k_machine *m)
{
    int i;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        printf("%2d %-10s -> %c\n", i, cv1k_input_name(i), m->input.keymap[i] ? m->input.keymap[i] : '-');
    }
}

int cv1k_ui_tui_run(struct cv1k_machine *m)
{
    char line[256];
    char cmd[64];
    char arg1[160];
    char arg2[64];
    int n;
    int i;
    char status[4096];

    print_menu();
    for (;;) {
        printf("cv1k> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        cmd[0] = 0;
        arg1[0] = 0;
        arg2[0] = 0;
        n = sscanf(line, "%63s %159s %63s", cmd, arg1, arg2);
        if (n <= 0) continue;

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            break;
        } else if (strcmp(cmd, "help") == 0) {
            print_menu();
        } else if (strcmp(cmd, "status") == 0) {
            cv1k_machine_status(m, status, (cv1k_u32)sizeof(status));
            printf("%s\n", status);
        } else if (strcmp(cmd, "reset") == 0) {
            cv1k_machine_reset(m);
            printf("reset complete\n");
        } else if (strcmp(cmd, "run") == 0) {
            n = (arg1[0] != 0) ? atoi(arg1) : 1;
            if (n < 1) n = 1;
            for (i = 0; i < n; i++) cv1k_machine_frame(m);
            printf("ran %d frame(s)\n", n);
        } else if (strcmp(cmd, "step") == 0) {
            n = (arg1[0] != 0) ? atoi(arg1) : 1;
            if (n < 1) n = 1;
            sh7709s_run(&m->cpu, &m->bus, (cv1k_u32)n);
            printf("stepped %d instruction(s)\n", n);
        } else if (strcmp(cmd, "save") == 0) {
            if (arg1[0] == 0) printf("usage: save file.ss\n");
            else printf(cv1k_save_state(m, arg1) ? "saved\n" : "save failed\n");
        } else if (strcmp(cmd, "load") == 0) {
            if (arg1[0] == 0) printf("usage: load file.ss\n");
            else printf(cv1k_load_state(m, arg1) ? "loaded\n" : "load failed\n");
        } else if (strcmp(cmd, "blit") == 0) {
            if (arg1[0] == 0) printf("usage: blit 0xADDR\n");
            else {
                cv1k_machine_blit(m, (cv1k_u32)strtoul(arg1, NULL, 0));
                cv1k_machine_frame(m);
                printf("blitter list executed\n");
            }
        } else if (strcmp(cmd, "ppm") == 0) {
            if (arg1[0] == 0) printf("usage: ppm frame.ppm\n");
            else printf(cv1k_video_write_ppm(&m->video, arg1) ? "wrote ppm\n" : "ppm failed\n");
        } else if (strcmp(cmd, "press") == 0) {
            if (arg1[0] == 0) printf("usage: press key\n");
            else {
                cv1k_input_event_key(&m->input, arg1[0], 1);
                cv1k_machine_frame(m);
                cv1k_input_event_key(&m->input, arg1[0], 0);
                printf("pressed %c\n", arg1[0]);
            }
        } else if (strcmp(cmd, "map") == 0) {
            show_map(m);
        } else if (strcmp(cmd, "savemap") == 0) {
            if (arg1[0] == 0) printf("usage: savemap file.cfg\n");
            else printf(cv1k_input_save_map(&m->input, arg1) ? "mapping saved\n" : "mapping save failed\n");
        } else if (strcmp(cmd, "remap") == 0) {
            if (arg1[0] == 0 || arg2[0] == 0) printf("usage: remap <id> <key>\n");
            else {
                cv1k_input_set_key(&m->input, atoi(arg1), arg2[0]);
                printf("remapped\n");
            }
        } else {
            printf("unknown command: %s\n", cmd);
        }
    }
    return 0;
}
