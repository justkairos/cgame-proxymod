/*
  ==============================
  +mdd_autojump: while held, press jump whenever you are on the ground or about to land, so the jump goes
  out in the landing usercmd itself (the frame a human aims for). Each press is held for a random
  mdd_autojump_stagger_min..mdd_autojump_stagger_max ms so no two jumps in a demo look machine-made.

  VQ3 needs the jump key released between jumps (PMF_JUMP_HELD, cleared by any usercmd with upmove < 10),
  which the hold/release of every tap provides. A press that is still held when you land still jumps, as
  long as a release happened since the previous jump.
  ==============================
*/
#include "cg_autojump.h"

#include "bg_local.h"
#include "cg_cvar.h"
#include "cg_input.h"
#include "cg_local.h"
#include "cg_utils.h"

#include <math.h>

#define AJ_LANDING_LOOKAHEAD_CMDS 1 // press when the landing is predicted within this many usercmds

static vmCvar_t autojump_stagger_min;
static vmCvar_t autojump_stagger_max;
static vmCvar_t autojump_debug;

static cvarTable_t autojump_cvars[] = {
  { &autojump_stagger_min, "mdd_autojump_stagger_min", "8", CVAR_ARCHIVE_ND },
  { &autojump_stagger_max, "mdd_autojump_stagger_max", "64", CVAR_ARCHIVE_ND },
  { &autojump_debug, "mdd_autojump_debug", "0", CVAR_ARCHIVE_ND },
};

static qboolean held_autojump;

static qboolean armed;       // a jump may be sent (we have been airborne, or just started)
static int32_t  report_time; // cg.time of the last press we still have to report, 0 = none
static int32_t  report_hold; // hold that was rolled for it
static int32_t  report_land; // predicted ms to landing at the press (0 = already grounded)

void init_autojump(void)
{
  init_cvars(autojump_cvars, ARRAY_LEN(autojump_cvars));
  register_button("mdd_autojump", &held_autojump);
  armed       = qtrue;
  report_time = 0;
}

// Will the player's box touch walkable ground within `ms` of free fall from the predicted state?
// Returns the ms until contact (0 if grounded now), or -1.
static int32_t predict_landing(playerState_t const* ps, int32_t ms)
{
  if (ps->groundEntityNum != ENTITYNUM_NONE) return 0;
  vec3_t const mins = { -15, -15, -24 };
  vec3_t const maxs = { 15, 15, 32 };
  vec3_t       end;
  float const  g = (float)ps->gravity;
  float const  t = ms / 1000.f;
  // one usercmd of PM_AirMove: velocity gets the full frame of gravity before the slide trace
  float const vz = ps->velocity[2] - g * t;
  if (vz > 0) return -1;
  VectorMA(ps->origin, t, ps->velocity, end);
  end[2] = ps->origin[2] + vz * t;
  trace_t tr;
  trap_CM_BoxTrace(&tr, ps->origin, end, mins, maxs, 0, MASK_PLAYERSOLID);
  if (tr.fraction == 1.f || tr.plane.normal[2] < MIN_WALK_NORMAL) return -1;
  return (int32_t)(tr.fraction * ms);
}

void run_autojump(void)
{
  update_cvars(autojump_cvars, ARRAY_LEN(autojump_cvars));

  if (cg.demoPlayback) return;
  playerState_t const* const ps = getPs();
  if (ps->pm_type != PM_NORMAL || (ps->pm_flags & PMF_FOLLOW) || ps->clientNum != cg.clientNum) return;

  // report the previous press once the key is back up
  if (report_time && !key_is_held(K_MOVEUP) && key_up_time(K_MOVEUP) >= report_time)
  {
    if (autojump_debug.integer)
      trap_Print(vaf(
        "^3[autojump] +moveup %i  -moveup %i  held %i ms (rolled %i), landing predicted in %i ms, now %s\n",
        key_down_time(K_MOVEUP),
        key_up_time(K_MOVEUP),
        key_up_time(K_MOVEUP) - key_down_time(K_MOVEUP),
        report_hold,
        report_land,
        ps->groundEntityNum != ENTITYNUM_NONE ? "grounded" : "airborne"));
    report_time = 0;
  }

  qboolean const on_ground = ps->groundEntityNum != ENTITYNUM_NONE;
  if (!on_ground) armed = qtrue; // every flight re-arms

  if (!held_autojump) return;
  if (!armed) return;
  if (key_is_held(K_MOVEUP)) return; // previous tap still down

  int32_t const dt   = input_dt();
  int32_t const land = predict_landing(ps, AJ_LANDING_LOOKAHEAD_CMDS * dt);
  if (land < 0) return;

  int32_t hold = random_range(autojump_stagger_min.integer, autojump_stagger_max.integer);
  if (hold < land + dt) hold = land + dt; // must still be down when the landing usercmd runs
  if (hold < 1) hold = 1;
  tap_key_at(K_MOVEUP, 0, hold);
  armed       = qfalse;
  report_time = cg.time;
  report_hold = hold;
  report_land = land;
  if (autojump_debug.integer)
    trap_Print(vaf(
      "^3[autojump] jump at %i: %s (landing in %i ms), vz %.0f, hold %i ms\n",
      cg.time,
      land == 0 ? "on ground" : "airborne",
      land,
      ps->velocity[2],
      hold));
}
