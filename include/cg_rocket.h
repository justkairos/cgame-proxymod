#ifndef CG_ROCKET_H
#define CG_ROCKET_H

void init_rocket(void);

// Runs once per client frame, after the defrag cgame has predicted the player state.
void run_rocket(void);

#endif // CG_ROCKET_H
