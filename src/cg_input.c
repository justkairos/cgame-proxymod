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
// see it), so every tapped key is held for a random mdd_input_stagger_min..max ms before release. Jump and
// attack are paired: when one is pressed while the other is still down from a tap, its release is scheduled
// a random stagger after the other's release, so the four events of any jump + shot (jumpshoot, rocketsync,
// grenade jump/shoot, ...) never coincide and the (press gap, first hold, release gap) pattern, rounded to
// the usercmd grid, is re-rolled against the last INPUT_HISTORY patterns so no two look alike in a demo.
static vmCvar_t stagger_min;
static vmCvar_t stagger_max;
static vmCvar_t input_debug;

static cvarTable_t input_cvars[] = {
  { &stagger_min, "mdd_input_stagger_min", "8", CVAR_ARCHIVE_ND },
  { &stagger_max, "mdd_input_stagger_max", "64", CVAR_ARCHIVE_ND },
  { &input_debug, "mdd_input_debug", "0", CVAR_ARCHIVE_ND },
};

static int32_t dt = 8, frametime_ms = 8;

#define INPUT_HISTORY 256
static int32_t history[INPUT_HISTORY][3];
static int32_t history_num, history_next;

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

// A random stagger from mdd_input_stagger_min..max, in whole usercmds: nothing can happen between usercmds,
// so the range is mapped to a count of cmds (rounding up) and every count is equally likely. The value
// returned is the ms that makes a release land exactly that many cmds after the press, whatever the frame
// jitter: (n - 1) * dt + 1 is reached by frame n and not by frame n - 1.
int32_t input_stagger(void)
{
  int32_t lo = (stagger_min.integer + dt - 1) / dt, hi = (stagger_max.integer + dt - 1) / dt;
  if (lo < 1) lo = 1;
  if (hi < lo) hi = lo;
  return (random_range(lo, hi) - 1) * dt + 1;
}

static int32_t roll_stagger(void)
{
  return input_stagger();
}

static qboolean key_held[K_COUNT];                        // currently pressed through set_key
static int32_t  key_down_at[K_COUNT], key_up_at[K_COUNT]; // cg.time (= usercmd serverTime) of the last press / release
static int32_t  tap_at[K_COUNT];                          // > 0: press once cg.time reaches it
static int32_t  tap_hold[K_COUNT];                        // hold time to use for that press, 0 = random stagger
static int32_t  tap_until[K_COUNT];                       // > 0: pressed by a tap, release once cg.time reaches it

static int32_t last_ms, last_time;

// the jump/attack pair currently in flight, for the debug report once both are released
static qboolean pair_active;
static vkey_t   pair_first, pair_second;
static int32_t  pair_press; // cg.time of the second press
static int32_t  pair_rolled_gap;

static int32_t to_cmds(int32_t ms)
{
  return (ms + dt - 1) / dt;
}

static qboolean history_seen(int32_t a, int32_t b, int32_t c)
{
  for (int32_t i = 0; i < history_num; ++i)
    if (history[i][0] == a && history[i][1] == b && history[i][2] == c) return qtrue;
  return qfalse;
}

static void history_push(int32_t a, int32_t b, int32_t c)
{
  history[history_next][0] = a;
  history[history_next][1] = b;
  history[history_next][2] = c;
  history_next             = (history_next + 1) % INPUT_HISTORY;
  if (history_num < INPUT_HISTORY) ++history_num;
}

static vkey_t partner_of(vkey_t k)
{
  return k == K_MOVEUP ? K_ATTACK : k == K_ATTACK ? K_MOVEUP : K_COUNT;
}

// Press a tapped key now and decide how long it stays down.
#define PAIR_WINDOW 150 // ms: an attack this soon after a jump (or vice versa) is one jump + shot pattern

static void press_tap(vkey_t k)
{
  int32_t const now  = cg.time;
  int32_t       hold = tap_hold[k] ? tap_hold[k] : roll_stagger();
  vkey_t const  p    = partner_of(k);
  if (p != K_COUNT && !tap_hold[k] && key_down_at[p] && now - key_down_at[p] <= PAIR_WINDOW)
  {
    int32_t const press_gap = to_cmds(now - key_down_at[p]);
    if (tap_until[p] > now)
    {
      // partner still down: release this key a random stagger after the partner's release, choosing the
      // stagger so the whole (press gap, first hold, release gap) pattern differs from every recent one
      int32_t const first_hold = to_cmds(tap_until[p] - key_down_at[p]);
      int32_t       gap        = roll_stagger();
      for (int32_t attempt = 0; attempt < 100 && history_seen(press_gap, first_hold, to_cmds(gap)); ++attempt)
        gap = roll_stagger();
      history_push(press_gap, first_hold, to_cmds(gap));
      hold            = tap_until[p] - now + gap;
      pair_rolled_gap = gap;
    }
    else
    {
      // partner already released (short hold, long press gap): the pattern is (press gap, partner hold, own
      // hold); re-roll the hold against the history the same way
      int32_t const first_hold = to_cmds(key_up_at[p] - key_down_at[p]);
      for (int32_t attempt = 0; attempt < 100 && history_seen(press_gap, first_hold, to_cmds(hold)); ++attempt)
        hold = roll_stagger();
      history_push(press_gap, first_hold, to_cmds(hold));
      pair_rolled_gap = hold;
    }
    pair_active = qtrue;
    pair_first  = p;
    pair_second = k;
    pair_press  = now;
  }
  set_key(k, qtrue);
  tap_until[k] = now + hold;
  tap_at[k]    = 0;
}

static void report_pair(void)
{
  if (!pair_active) return;
  if (key_held[pair_first] || key_held[pair_second]) return;
  if (key_up_at[pair_second] < pair_press) return;
  pair_active = qfalse;
  if (!input_debug.integer && !cvar_getInteger("mdd_jumpshoot_debug")) return;
  int32_t const fd = key_down_at[pair_first], fu = key_up_at[pair_first];
  int32_t const sd = key_down_at[pair_second], su = key_up_at[pair_second];
  trap_Print(vaf(
    "^3[input] +%s %i  +%s %i (+%i)  -%s %i (%s held %i)  -%s %i (%s held %i, %+i after -%s, rolled %i)\n",
    key_names[pair_first],
    fd,
    key_names[pair_second],
    sd,
    sd - fd,
    key_names[pair_first],
    fu,
    key_names[pair_first],
    fu - fd,
    key_names[pair_second],
    su,
    key_names[pair_second],
    su - sd,
    su - fu,
    key_names[pair_first],
    pair_rolled_gap));
}

void set_key(vkey_t k, qboolean down)
{
  if (key_held[k] == down) return;
  key_held[k] = down;
  if (down)
    key_down_at[k] = cg.time;
  else
    key_up_at[k] = cg.time;
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
  report_pair();
  for (int32_t k = 0; k < K_COUNT; ++k)
  {
    if (!tap_at[k] || cg.time < tap_at[k]) continue;
    press_tap((vkey_t)k);
    return;
  }
  for (int32_t k = 0; k < K_COUNT; ++k)
  {
    if (!tap_until[k] || cg.time < tap_until[k]) continue;
    set_key((vkey_t)k, qfalse);
    tap_until[k] = 0;
    if (input_debug.integer > 1 && !pair_active && (partner_of((vkey_t)k) == K_COUNT || !tap_at[partner_of((vkey_t)k)]))
      trap_Print(vaf(
        "^3[input] %s tapped: +%i -%i (held %i)\n", key_names[k], key_down_at[k], cg.time, cg.time - key_down_at[k]));
    return;
  }
}

qboolean key_is_held(vkey_t k)
{
  return key_held[k];
}

int32_t key_down_time(vkey_t k)
{
  return key_down_at[k];
}

int32_t key_up_time(vkey_t k)
{
  return key_up_at[k];
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
    if (cvar_getInteger("mdd_grenade_debug") && !Q_stricmp(buttons[i].name, "mdd_grenade_sync"))
      trap_Print(vaf("^3[grenade] key %s at %i\n", cmd, cg.time));
    return qtrue;
  }
  return qfalse;
}

void init_input(void)
{
  init_cvars(input_cvars, ARRAY_LEN(input_cvars));
  srand((unsigned)trap_Milliseconds());

  memset(key_held, 0, sizeof(key_held));
  history_num = history_next = 0;
  pair_active                = qfalse;
  memset(key_down_at, 0, sizeof(key_down_at));
  memset(key_up_at, 0, sizeof(key_up_at));
  memset(tap_at, 0, sizeof(tap_at));
  memset(tap_hold, 0, sizeof(tap_hold));
  memset(tap_until, 0, sizeof(tap_until));
  num_buttons = 0;
}
