#ifndef CG_REPLAY_H
#define CG_REPLAY_H

#include "q_shared.h"

void init_replay(void);
void del_replay(void);

// Runs once per client frame, after the defrag cgame has predicted the player state.
void run_replay(void);

// console commands: mdd_replay_record <name>, mdd_replay_telerecord <name>, mdd_replay_stoprecord,
//                   mdd_replay_start <name>, mdd_replay_telestart <name>, mdd_replay_stop
qboolean replay_console_command(char const* cmd);

// CG_SETUSERCMDVALUE hook: during playback the recorded weapon replaces what the defrag cgame asked for
int32_t replay_usercmd_weapon(int32_t weapon);

#endif // CG_REPLAY_H
