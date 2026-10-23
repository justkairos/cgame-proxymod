#include "cg_input.h"

#include "cg_cvar.h"
#include "cg_local.h"

#include <stdlib.h>
#include <string.h>

// the fake key number we pass to +cmd/-cmd so we never clobber the user's own physical key
#define KEY_ID "1000"

static char const* const key_names[K_COUNT] = {
  "forward",  "back",   "moveleft", "moveright", "speed",  "moveup",
  "movedown", "attack", "left",     "right",     "lookup", "lookdown",
};

// A tap must land in the very next usercmd, but a 1-cmd press is invisible in demos (player input huds never
// see it), so a tapped key is held for mdd_input_time ms of game time (+/- a random mdd_input_time_randomize
// so it doesn't look machine-made) before it is released.
static vmCvar_t input_time;
static vmCvar_t input_time_randomize;

static cvarTable_t input_cvars[] = {
  { &input_time, "mdd_input_time", "32", CVAR_ARCHIVE_ND },
  { &input_time_randomize, "mdd_input_time_randomize", "12", CVAR_ARCHIVE_ND },
};

static int32_t tap_hold_time(void)
{
  int32_t       hold = input_time.integer;
  int32_t const r    = abs(input_time_randomize.integer);
  if (r) hold += rand() % (2 * r + 1) - r;
  if (hold < 1) hold = 1; // the press itself always lands in the next usercmd; this only delays the release
  return hold;
}

static qboolean key_held[K_COUNT];  // currently pressed through set_key
static int32_t  tap_at[K_COUNT];    // > 0: press once cg.time reaches it
static int32_t  tap_hold[K_COUNT];  // hold time to use for that press, 0 = mdd_input_time
static int32_t  tap_until[K_COUNT]; // > 0: pressed by a tap, release once cg.time reaches it

static int32_t last_ms, last_time;
static int32_t dt = 8, frametime_ms = 8;

void set_key(vkey_t k, qboolean down)
{
  if (key_held[k] == down) return;
  key_held[k] = down;
  // Release with an uptime of 1: IN_KeyUp then does msec += uptime - downtime (hugely negative, CL_KeyState
  // clamps it to 0). Without an uptime it adds frame_msec / 2, i.e. a half-strength press in the frame after
  // the release - enough to nudge a snapped velocity of 0 back to 1.
  if (down)
    trap_SendConsoleCommand(vaf("+%s " KEY_ID "\n", key_names[k]));
  else
    trap_SendConsoleCommand(vaf("-%s " KEY_ID " 1\n", key_names[k]));
}

void tap_key(vkey_t k)
{
  tap_at[k]   = cg.time;
  tap_hold[k] = 0;
}

void tap_key_at(vkey_t k, int32_t delay, int32_t hold)
{
  tap_at[k]   = cg.time + (delay > 0 ? delay : 0);
  tap_hold[k] = hold > 0 ? hold : 0;
}

int32_t random_range(int32_t lo, int32_t hi)
{
  if (hi < lo)
  {
    int32_t const t = lo;
    lo              = hi;
    hi              = t;
  }
  return lo + rand() % (hi - lo + 1);
}

void pulse_key(vkey_t k, int32_t msec)
{
  if (msec < 1) msec = 1;
  // IN_KeyDown: downtime = 1; IN_KeyUp: msec += uptime - downtime = msec
  trap_SendConsoleCommand(vaf("+%s " KEY_ID " 1\n-%s " KEY_ID " %i\n", key_names[k], key_names[k], 1 + msec));
}

void release_move_keys(void)
{
  set_key(K_FORWARD, qfalse);
  set_key(K_BACK, qfalse);
  set_key(K_MOVELEFT, qfalse);
  set_key(K_MOVERIGHT, qfalse);
  set_key(K_SPEED, qfalse);
  set_key(K_MOVEDOWN, qfalse);
}

void release_look_keys(void)
{
  set_key(K_LEFT, qfalse);
  set_key(K_RIGHT, qfalse);
  set_key(K_LOOKUP, qfalse);
  set_key(K_LOOKDOWN, qfalse);
}

void release_all_keys(void)
{
  for (int32_t k = 0; k < K_COUNT; ++k) set_key((vkey_t)k, qfalse);
}

void input_frame_begin(void)
{
  int32_t const ms = trap_Milliseconds();
  frametime_ms     = ms - last_ms;
  dt               = cg.time - last_time;
  last_ms          = ms;
  last_time        = cg.time;
  if (frametime_ms < 1) frametime_ms = 1;
  if (frametime_ms > 200) frametime_ms = 200;
  if (dt < 1) dt = 1;
  if (dt > 200) dt = 200;

  update_cvars(input_cvars, ARRAY_LEN(input_cvars));
}

void input_frame_end(void)
{
  // Never let two tapped keys change state in the same frame (= the same usercmd): a demo must never show
  // +attack and +moveup landing or lifting together as if they were one bound command. Presses go first
  // because their timing is what the predictors computed; a release that has to wait a frame costs nothing.
  for (int32_t k = 0; k < K_COUNT; ++k)
  {
    if (!tap_at[k] || cg.time < tap_at[k]) continue;
    set_key((vkey_t)k, qtrue);
    tap_until[k] = cg.time + (tap_hold[k] ? tap_hold[k] : tap_hold_time());
    tap_at[k]    = 0;
    return;
  }
  for (int32_t k = 0; k < K_COUNT; ++k)
  {
    if (!tap_until[k] || cg.time < tap_until[k]) continue;
    set_key((vkey_t)k, qfalse);
    tap_until[k] = 0;
    return;
  }
}

int32_t input_dt(void)
{
  return dt;
}

int32_t input_frametime(void)
{
  return frametime_ms;
}

//
// hold buttons
//
#define MAX_BUTTONS 16

typedef struct
{
  char      name[32];
  qboolean* held;
} button_t;

static button_t buttons[MAX_BUTTONS];
static int32_t  num_buttons;

void register_button(char const* name, qboolean* held)
{
  if (num_buttons >= MAX_BUTTONS) return;
  // name may be a vaf() buffer, keep our own copy
  strncpy(buttons[num_buttons].name, name, sizeof(buttons[num_buttons].name) - 1);
  buttons[num_buttons].name[sizeof(buttons[num_buttons].name) - 1] = 0;
  buttons[num_buttons].held                                        = held;
  ++num_buttons;
  *held = qfalse;
  trap_AddCommand(vaf("+%s", buttons[num_buttons - 1].name));
  trap_AddCommand(vaf("-%s", buttons[num_buttons - 1].name));
}

qboolean input_button_command(char const* cmd)
{
  if (cmd[0] != '+' && cmd[0] != '-') return qfalse;
  for (int32_t i = 0; i < num_buttons; ++i)
  {
    if (Q_stricmp(cmd + 1, buttons[i].name)) continue;
    *buttons[i].held = cmd[0] == '+';
    return qtrue;
  }
  return qfalse;
}

void init_input(void)
{
  init_cvars(input_cvars, ARRAY_LEN(input_cvars));
  srand((unsigned)trap_Milliseconds());

  memset(key_held, 0, sizeof(key_held));
  memset(tap_at, 0, sizeof(tap_at));
  memset(tap_hold, 0, sizeof(tap_hold));
  memset(tap_until, 0, sizeof(tap_until));
  num_buttons = 0;
}
