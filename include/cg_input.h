#ifndef CG_INPUT_H
#define CG_INPUT_H

#include "q_shared.h"

/*
  Virtual input for the automation modules.

  A cgame cannot write usercmds; the only way to generate input is to press the engine's own button
  commands (+forward, +moveup, +attack, +left, ...) through the command buffer. Every press carries a fake
  key number so the user's physical keys are never clobbered.

  Timing model (verified against the engine source):
    Com_Frame: Cbuf_Execute() -> CL_Frame: CL_SendCmd() -> CL_SetCGameTime() -> SCR_UpdateScreen()
  A console command sent while the cgame runs in frame N executes at the start of frame N+1, right before
  the usercmd of frame N+1 is built, and that usercmd gets serverTime == cg.time of frame N.
*/

typedef enum
{
  K_FORWARD,
  K_BACK,
  K_MOVELEFT,
  K_MOVERIGHT,
  K_SPEED,
  K_MOVEUP,
  K_ATTACK,
  K_LEFT,
  K_RIGHT,
  K_LOOKUP,
  K_LOOKDOWN,
  K_COUNT
} vkey_t;

// hold / release a key (no-op if already in that state)
void set_key(vkey_t k, qboolean down);

// press a key for exactly one usercmd; resolved in input_frame_end()
void tap_key(vkey_t k);

void release_move_keys(void);
void release_look_keys(void);
void release_all_keys(void);

// per-frame bookkeeping, called once around all automation modules
void input_frame_begin(void);
void input_frame_end(void);

// estimated server-time step between usercmds (ms) and real frame time (ms), both clamped to [1,200]
int32_t input_dt(void);
int32_t input_frametime(void);

// +name / -name hold buttons (e.g. "+mdd_grenade_aim"): name is the part after the sign.
// *held is set while the button is down. Also registers both commands for tab completion.
void     register_button(char const* name, qboolean* held);
qboolean input_button_command(char const* cmd);

void init_input(void);

#endif // CG_INPUT_H
