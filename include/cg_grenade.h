#ifndef CG_GRENADE_H
#define CG_GRENADE_H

#include "q_shared.h"

void init_grenade(void);

void del_grenade(void);

// Runs once per client frame, after the defrag cgame has predicted the player state.
void run_grenade(void);

// Handles +mdd_grenade_<feature> / -mdd_grenade_<feature>; returns qtrue if cmd was consumed.
qboolean grenade_console_command(char const* cmd);

// for tab completion
#define GRENADE_CONSOLE_COMMANDS                                                                                       \
  "+mdd_grenade_chase", "-mdd_grenade_chase", "+mdd_grenade_aim", "-mdd_grenade_aim", "+mdd_grenade_jump",             \
    "-mdd_grenade_jump", "+mdd_grenade_shoot", "-mdd_grenade_shoot"

#endif // CG_GRENADE_H
