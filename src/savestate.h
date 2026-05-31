#ifndef CV1K_SAVESTATE_H
#define CV1K_SAVESTATE_H

#include "emu.h"

int cv1k_save_state(struct cv1k_machine *m, const char *path);
int cv1k_load_state(struct cv1k_machine *m, const char *path);

#endif
