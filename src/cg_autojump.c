/*
  ==============================
  +mdd_autojump: while held, jump on every landing, in the landing usercmd itself (the frame a human aims for).

  The jump key goes down a little before the landing and stays down until the engine has jumped (see
  tap_jump_latched in cg_input.h), so it does not depend on predicting the exact usercmd of the landing: next
  to ledges and walls the real move slides or steps and lands a usercmd earlier or later than a straight
  line says, and with pmove_fixed one usercmd in three runs no pmove at all. Each press is down for a random
  mdd_autojump_stagger_min..mdd_autojump_stagger_max ms in total (longer if the jump takes longer to come), so
  no two jumps in a demo look machine-made.

  VQ3 needs the jump key released between jumps (PMF_JUMP_HELD, cleared by any pmove with upmove < 10). A key
  that goes down in the air while that flag is clear jumps on landing, however early it was pressed.
  ==============================
*/
#include "cg_autojump.h"

#include "bg_local.h"
#include "cg_cvar.h"
#include "cg_input.h"
#include "cg_local.h"
#include "cg_overbounce.h"
#include "cg_utils.h"

#include <math.h>

#define AJ_MIN_LEAD_CMDS 2   // press at least this many usercmds before the predicted landing
#define AJ_JUMP_TIMEOUT  300 // ms a press waits for its jump before it is let go and tried again
#define AJ_STICKY_HEIGHT 2.f // airborne but this close above walkable ground (sticky overbounce): keep trying
#define AJ_RELAND_CMDS   2   // still down from the last jump and landing again this soon: let go now

static vmCvar_t autojump_stagger_min;
static vmCvar_t autojump_stagger_max;
static vmCvar_t autojump_debug;

static cvarTable_t autojump_cvars[] = {
  { &autojump_stagger_min, "mdd_autojump_stagger_min", "8", CVAR_ARCHIVE_ND },
  { &autojump_stagger_max, "mdd_autojump_stagger_max", "64", CVAR_ARCHIVE_ND },
  { &autojump_debug, "mdd_autojump_debug", "0", CVAR_ARCHIVE_ND },
};

static qboolean held_autojump;

static int32_t  next_hold;   // total hold rolled for the next press, 0 = roll one
static qboolean waiting;     // our press is down (or queued) and its jump has not come yet
static int32_t  report_time; // cg.time of the last press we still have to report, 0 = none
static int32_t  report_hold; // hold that was rolled for it
static int32_t  report_land; // predicted ms to landing at the press (0 = already grounded)

void init_autojump(void)
{
  init_cvars(autojump_cvars, ARRAY_LEN(autojump_cvars));
  register_button("mdd_autojump", &held_autojump);
  next_hold   = 0;
  waiting     = qfalse;
  report_time = 0;
}

static qboolean walkable(trace_t const* tr)
{
  return !tr->allsolid && !tr->startsolid && tr->fraction < 1.f && tr->plane.normal[2] >= MIN_WALK_NORMAL;
}

/*
  Will the player be standing on walkable ground within `ms` of falling from the predicted state?
  Returns the estimated ms until then (0 if grounded now), or -1.

  This only decides when the key goes down, and pressing early costs nothing, so it errs on the side of yes:
    A  the straight line of the next `ms` (velocity with gravity) ends on walkable ground
    B  straight down from where we are: sliding down a wall or a ledge side, the straight line hits the wall
       first and A alone says "no landing"
    C  straight down at the end of the line, from a step height above: landing on the corner of a ledge the
       line runs into its side, and the real move steps up onto it
*/
static int32_t predict_landing(playerState_t const* ps, int32_t ms)
{
  if (ps->groundEntityNum != ENTITYNUM_NONE) return 0;
  vec3_t const mins = { -15, -15, -24 };
  vec3_t const maxs = { 15, 15, 32 };
  vec3_t       start, end;
  trace_t      tr;

  // Sticky overbounce: hovering a fraction above the floor, not grounded, hardly any fall speed. The engine
  // can't jump until the ground trace connects, so keep the key down: whichever usercmd finally lands jumps.
  VectorCopy(ps->origin, end);
  end[2] -= AJ_STICKY_HEIGHT;
  trap_CM_BoxTrace(&tr, ps->origin, end, mins, maxs, 0, MASK_PLAYERSOLID);
  if (walkable(&tr)) return 0;

  float const g      = (float)ps->gravity;
  float const t      = ms / 1000.f;
  float const vz_end = ps->velocity[2] - g * t;
  if (vz_end > 0) return -1;                                  // still rising at the end of the window
  float const dz = ps->velocity[2] * t - .5f * g * t * t - 1; // one unit extra: velocities are snapped
  if (dz >= 0) return -1;                                     // not below where we are now

  // A
  VectorMA(ps->origin, t, ps->velocity, end);
  end[2] = ps->origin[2] + dz;
  trap_CM_BoxTrace(&tr, ps->origin, end, mins, maxs, 0, MASK_PLAYERSOLID);
  if (walkable(&tr)) return (int32_t)(tr.fraction * ms);

  // B
  vec3_t down;
  VectorCopy(ps->origin, down);
  down[2] += dz;
  trap_CM_BoxTrace(&tr, ps->origin, down, mins, maxs, 0, MASK_PLAYERSOLID);
  if (walkable(&tr)) return (int32_t)(tr.fraction * ms);

  // C
  VectorCopy(end, start);
  start[2] = ps->origin[2] + STEPSIZE;
  trap_CM_BoxTrace(&tr, start, end, mins, maxs, 0, MASK_PLAYERSOLID);
  if (walkable(&tr)) return ms;

  return -1;
}

// PM_SetWaterLevel: 0 dry, 1 feet in water, 2 waist deep (swimming), 3 head under. World brushes only.
static int32_t water_level(playerState_t const* ps)
{
  vec3_t point;
  VectorCopy(ps->origin, point);
  point[2] = ps->origin[2] + MINS_Z + 1;
  if (!(trap_CM_PointContents(point, 0) & MASK_WATER)) return 0;
  int32_t const sample2 = ps->viewheight - MINS_Z;
  int32_t const sample1 = sample2 / 2;
  point[2]              = ps->origin[2] + MINS_Z + sample1;
  if (!(trap_CM_PointContents(point, 0) & MASK_WATER)) return 1;
  point[2] = ps->origin[2] + MINS_Z + sample2;
  return trap_CM_PointContents(point, 0) & MASK_WATER ? 3 : 2;
}

void run_autojump(void)
{
  update_cvars(autojump_cvars, ARRAY_LEN(autojump_cvars));

  if (cg.demoPlayback) return;
  playerState_t const* const ps = getPs();
  if (ps->clientNum != cg.clientNum) return;

  // Free-moving states - Flight powerup, free spectator, noclip, swimming - go up only while jump is held, so
  // hold it for as long as the key is down and the state lasts. Swimming is water (or lava, slime) from the
  // waist up, where the engine runs PM_WaterMove instead of walking or falling; with only the feet in it the
  // normal landing jump applies.
  static qboolean holding;
  qboolean const  swimming = ps->pm_type == PM_NORMAL && water_level(ps) >= 2;
  qboolean const  free_fly = ps->pm_type == PM_SPECTATOR || ps->pm_type == PM_NOCLIP || swimming ||
                            (ps->pm_type == PM_NORMAL && ps->powerups[PW_FLIGHT] > cg.time);
  qboolean const want_hold = held_autojump && free_fly && !(ps->pm_flags & PMF_FOLLOW);
  if (want_hold != holding)
  {
    holding = want_hold;
    if (holding)
      hold_jump(); // takes over a landing press that is still down, which would otherwise let go on its own
    else
      set_key(K_MOVEUP, qfalse);
    if (autojump_debug.integer)
      trap_Print(vaf(
        "^3[autojump] %s +moveup at %i (%s)\n",
        holding ? "holding" : "released",
        cg.time,
        ps->pm_type == PM_SPECTATOR ? "spectator"
        : ps->pm_type == PM_NOCLIP  ? "noclip"
        : swimming                  ? "swimming"
                                    : "flight / left the water"));
  }
  if (holding) return;

  if (ps->pm_type != PM_NORMAL || (ps->pm_flags & PMF_FOLLOW)) return;

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

  if (!jump_latched()) waiting = qfalse;
  if (!held_autojump)
  {
    // key let go while a press is still waiting for its landing: no jump after the key is up
    if (waiting) release_jump();
    waiting = qfalse;
    return;
  }

  int32_t const dt = input_dt();
  if (key_is_held(K_MOVEUP))
  {
    // Down since the last jump and about to land again (low ceiling, tiny hop): the engine wants the key up
    // for one pmove before it jumps again, so let go now instead of finishing the rolled hold.
    if (!jump_latched() && !jump_was_released() && predict_landing(ps, AJ_RELAND_CMDS * dt) >= 0) release_jump();
    return;
  }
  if (jump_latched()) return;       // press queued for this frame
  if (!jump_was_released()) return; // the release is not through yet, or the player holds the real jump key

  // With +mdd_overbounce_jump held as well, leave overbounce landings to it: a jump key that is down when an
  // overbounce lands cancels it. So nothing is pressed ahead of the landing, and nothing while an overbounce
  // is pending.
  qboolean const ob_held = overbounce_jump_held();
  if (overbounce_jump_pending()) return;

  if (!next_hold) next_hold = random_range(autojump_stagger_min.integer, autojump_stagger_max.integer);
  if (next_hold < 1) next_hold = 1;
  // press this long before the landing: half the hold, but early enough to be safe
  int32_t lead = next_hold / 2;
  if (lead < AJ_MIN_LEAD_CMDS * dt) lead = AJ_MIN_LEAD_CMDS * dt;

  int32_t const land = ob_held ? (ps->groundEntityNum != ENTITYNUM_NONE ? 0 : -1) : predict_landing(ps, lead);
  if (land < 0) return;

  tap_jump_latched(next_hold, AJ_JUMP_TIMEOUT);
  waiting     = qtrue;
  report_time = cg.time;
  report_hold = next_hold;
  report_land = land;
  next_hold   = 0;
  if (autojump_debug.integer)
    trap_Print(vaf(
      "^3[autojump] jump at %i: %s (landing in %i ms), vz %.0f, hold %i ms\n",
      cg.time,
      land == 0 ? (ps->groundEntityNum != ENTITYNUM_NONE ? "on ground" : "hovering just above ground") : "airborne",
      land,
      ps->velocity[2],
      report_hold));
}
