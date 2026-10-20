#ifndef CG_OVERBOUNCE_H
#define CG_OVERBOUNCE_H

void init_overbounce(void);

// Runs once per client frame, after the defrag cgame has predicted the player state.
void run_overbounce(void);

#endif // CG_OVERBOUNCE_H
