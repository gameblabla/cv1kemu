#ifndef CV1K_INPUT_H
#define CV1K_INPUT_H

#include "cv1k_types.h"

#define CV1K_INPUT_COUNT 18

enum cv1k_input_id {
    CV1K_IN_P1_UP = 0,
    CV1K_IN_P1_DOWN,
    CV1K_IN_P1_LEFT,
    CV1K_IN_P1_RIGHT,
    CV1K_IN_P1_B1,
    CV1K_IN_P1_B2,
    CV1K_IN_P1_B3,
    CV1K_IN_P1_B4,
    CV1K_IN_P1_START,
    CV1K_IN_P2_UP,
    CV1K_IN_P2_DOWN,
    CV1K_IN_P2_LEFT,
    CV1K_IN_P2_RIGHT,
    CV1K_IN_P2_B1,
    CV1K_IN_P2_B2,
    CV1K_IN_P2_B3,
    CV1K_IN_P2_B4,
    CV1K_IN_COIN1
};

struct cv1k_input {
    cv1k_u8 state[CV1K_INPUT_COUNT];
    int keymap[CV1K_INPUT_COUNT];
};

void cv1k_input_default(struct cv1k_input *in);
void cv1k_input_set_key(struct cv1k_input *in, int id, int ascii_key);
void cv1k_input_event_key(struct cv1k_input *in, int ascii_key, int pressed);
cv1k_u8 cv1k_input_port_c(const struct cv1k_input *in);
cv1k_u8 cv1k_input_port_d(const struct cv1k_input *in);
cv1k_u8 cv1k_input_port_f(const struct cv1k_input *in);
cv1k_u8 cv1k_input_port_l(const struct cv1k_input *in);
const char *cv1k_input_name(int id);
int cv1k_input_load_map(struct cv1k_input *in, const char *path);
int cv1k_input_save_map(const struct cv1k_input *in, const char *path);

#endif
