#ifndef CG_GRENADE_H
#define CG_GRENADE_H

#include "q_shared.h"

void init_grenade(void);

void del_grenade(void);

// Runs once per client frame, after the defrag cgame has predicted the player state.
void run_grenade(void);

#endif // CG_GRENADE_H
