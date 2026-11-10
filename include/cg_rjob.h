#ifndef CG_RJOB_H
#define CG_RJOB_H

void init_rjob(void);

// Runs once per client frame, after the defrag cgame has predicted the player state.
void run_rjob(void);

#endif // CG_RJOB_H
