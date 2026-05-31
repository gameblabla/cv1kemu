#include "input.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *names[CV1K_INPUT_COUNT] = {
    "p1_up", "p1_down", "p1_left", "p1_right", "p1_b1", "p1_b2", "p1_b3", "p1_b4", "p1_start",
    "p2_up", "p2_down", "p2_left", "p2_right", "p2_b1", "p2_b2", "p2_b3", "p2_b4", "coin1"
};

void cv1k_input_default(struct cv1k_input *in)
{
    int i;
    memset(in, 0, sizeof(*in));
    for (i = 0; i < CV1K_INPUT_COUNT; i++) in->keymap[i] = 0;
    in->keymap[CV1K_IN_P1_UP] = 'w';
    in->keymap[CV1K_IN_P1_DOWN] = 's';
    in->keymap[CV1K_IN_P1_LEFT] = 'a';
    in->keymap[CV1K_IN_P1_RIGHT] = 'd';
    in->keymap[CV1K_IN_P1_B1] = 'j';
    in->keymap[CV1K_IN_P1_B2] = 'k';
    in->keymap[CV1K_IN_P1_B3] = 'l';
    in->keymap[CV1K_IN_P1_B4] = 'i';
    in->keymap[CV1K_IN_P1_START] = '1';
    in->keymap[CV1K_IN_COIN1] = '5';
}

void cv1k_input_set_key(struct cv1k_input *in, int id, int ascii_key)
{
    if (id >= 0 && id < CV1K_INPUT_COUNT) in->keymap[id] = ascii_key;
}

void cv1k_input_event_key(struct cv1k_input *in, int ascii_key, int pressed)
{
    int i;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        if (in->keymap[i] == ascii_key) in->state[i] = (cv1k_u8)(pressed ? 1U : 0U);
    }
}

static cv1k_u8 low_if_pressed(cv1k_u8 port, cv1k_u8 bit, cv1k_u8 pressed)
{
    if (pressed) return (cv1k_u8)(port & (cv1k_u8)(~bit));
    return port;
}

cv1k_u8 cv1k_input_port_c(const struct cv1k_input *in)
{
    cv1k_u8 p;
    p = 0xffU;
    p = low_if_pressed(p, 0x04U, in->state[CV1K_IN_COIN1]);
    p = low_if_pressed(p, 0x10U, in->state[CV1K_IN_P1_START]);
    return p;
}

cv1k_u8 cv1k_input_port_d(const struct cv1k_input *in)
{
    cv1k_u8 p;
    p = 0xffU;
    p = low_if_pressed(p, 0x01U, in->state[CV1K_IN_P1_UP]);
    p = low_if_pressed(p, 0x02U, in->state[CV1K_IN_P1_DOWN]);
    p = low_if_pressed(p, 0x04U, in->state[CV1K_IN_P1_LEFT]);
    p = low_if_pressed(p, 0x08U, in->state[CV1K_IN_P1_RIGHT]);
    p = low_if_pressed(p, 0x10U, in->state[CV1K_IN_P1_B1]);
    p = low_if_pressed(p, 0x20U, in->state[CV1K_IN_P1_B2]);
    p = low_if_pressed(p, 0x40U, in->state[CV1K_IN_P1_B3]);
    p = low_if_pressed(p, 0x80U, in->state[CV1K_IN_P1_B4]);
    return p;
}

cv1k_u8 cv1k_input_port_f(const struct cv1k_input *in)
{
    CV1K_UNUSED(in);
    return 0xffU;
}

cv1k_u8 cv1k_input_port_l(const struct cv1k_input *in)
{
    cv1k_u8 p;
    p = 0xffU;
    p = low_if_pressed(p, 0x01U, in->state[CV1K_IN_P2_UP]);
    p = low_if_pressed(p, 0x02U, in->state[CV1K_IN_P2_DOWN]);
    p = low_if_pressed(p, 0x04U, in->state[CV1K_IN_P2_LEFT]);
    p = low_if_pressed(p, 0x08U, in->state[CV1K_IN_P2_RIGHT]);
    p = low_if_pressed(p, 0x10U, in->state[CV1K_IN_P2_B1]);
    p = low_if_pressed(p, 0x20U, in->state[CV1K_IN_P2_B2]);
    p = low_if_pressed(p, 0x40U, in->state[CV1K_IN_P2_B3]);
    p = low_if_pressed(p, 0x80U, in->state[CV1K_IN_P2_B4]);
    return p;
}

const char *cv1k_input_name(int id)
{
    if (id < 0 || id >= CV1K_INPUT_COUNT) return "unknown";
    return names[id];
}

static int find_name(const char *s)
{
    int i;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        if (strcmp(s, names[i]) == 0) return i;
    }
    return -1;
}

int cv1k_input_load_map(struct cv1k_input *in, const char *path)
{
    FILE *f;
    char line[128];
    char a[64];
    char b[64];
    int id;
    f = fopen(path, "r");
    if (f == NULL) return 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "%63[^=]=%63s", a, b) == 2) {
            id = find_name(a);
            if (id >= 0) cv1k_input_set_key(in, id, (int)b[0]);
        }
    }
    fclose(f);
    return 1;
}

int cv1k_input_save_map(const struct cv1k_input *in, const char *path)
{
    FILE *f;
    int i;
    f = fopen(path, "w");
    if (f == NULL) return 0;
    for (i = 0; i < CV1K_INPUT_COUNT; i++) {
        fprintf(f, "%s=%c\n", names[i], in->keymap[i] ? in->keymap[i] : '-');
    }
    fclose(f);
    return 1;
}
