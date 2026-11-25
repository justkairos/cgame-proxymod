#ifndef CG_OVERBOUNCE_H
#define CG_OVERBOUNCE_H

#include "q_shared.h"

void init_overbounce(void);

// Runs once per client frame, after the defrag cgame has predicted the player state.
void run_overbounce(void);

// +mdd_overbounce_jump is held / it is held and an overbounce landing is being handled (don't press jump)
qboolean overbounce_jump_held(void);
qboolean overbounce_jump_pending(void);

#endif // CG_OVERBOUNCE_H
