#ifndef CG_AUTOJUMP_H
#define CG_AUTOJUMP_H

void init_autojump(void);

// Runs once per client frame, after the defrag cgame has predicted the player state.
void run_autojump(void);

#endif // CG_AUTOJUMP_H
