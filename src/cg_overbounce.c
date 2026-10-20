/*
  ==============================
  +mdd_overbounce_jump: jump on the usercmd right after an overbounce converted your fall speed.

  How an overbounce happens (bg_pmove.c):
    cmd1 PM_AirMove ends with the player inside the 0.25u gap above the floor without the slide trace
         touching it. The trailing PM_GroundTrace marks the player grounded but never clips velocity,
         so velocity[2] is still the full fall speed (minus one frame of gravity).      -> "pending"
    cmd2 PM_WalkMove: PM_CheckJump (a jump here kills the OB), friction, accelerate, then
         vel = VectorLength(v); PM_ClipVelocity(v, groundnormal); VectorNormalize(v); VectorScale(v, vel)
         i.e. the fall speed is kept but redirected along the floor.                      -> "OB speed"
    cmd3 jump with the OB speed.

  We detect "pending" from the predicted player state (grounded, large negative z velocity that still
  decreased by gravity since last frame - a normal or slope landing always raises it), wait until the
  velocity changes (cmd2 ran) and tap jump so it lands in cmd3.
  ==============================
*/
#include "cg_overbounce.h"

#include "cg_cvar.h"
#include "cg_input.h"
#include "cg_local.h"
#include "cg_utils.h"

#define OB_MIN_FALL_SPEED  200.f // z velocity must be below -this while grounded to count as an OB landing
#define OB_PENDING_TIMEOUT 100   // ms; give up if the OB frame never shows up

static vmCvar_t overbounce_debug;

static cvarTable_t overbounce_cvars[] = {
  { &overbounce_debug, "mdd_overbounce_debug", "0", CVAR_ARCHIVE_ND },
};

static qboolean held_overbounce_jump;

static int32_t  prev_ground;
static float    prev_vz;
static qboolean pending;
static float    pending_vz;
static int32_t  pending_time;

void init_overbounce(void)
{
  init_cvars(overbounce_cvars, ARRAY_LEN(overbounce_cvars));
  register_button("mdd_overbounce_jump", &held_overbounce_jump);

  prev_ground = ENTITYNUM_NONE;
  prev_vz     = 0;
  pending     = qfalse;
}

void run_overbounce(void)
{
  update_cvars(overbounce_cvars, ARRAY_LEN(overbounce_cvars));

  playerState_t const* const ps        = getPs();
  qboolean const             on_ground = ps->groundEntityNum != ENTITYNUM_NONE;
  float const                vz        = ps->velocity[2];

  if (cg.demoPlayback || ps->pm_type != PM_NORMAL || (ps->pm_flags & PMF_FOLLOW))
  {
    pending = qfalse;
  }
  else if (pending)
  {
    if (vz != pending_vz)
    {
      // cmd2 ran: velocity got converted
      if (on_ground && held_overbounce_jump)
      {
        tap_key(K_MOVEUP);
        if (overbounce_debug.integer)
          trap_Print(vaf(
            "^3[overbounce] jump at %i: OB speed %.0f (%.0f %.0f %.0f)\n",
            cg.time,
            VectorLength(ps->velocity),
            ps->velocity[0],
            ps->velocity[1],
            ps->velocity[2]));
      }
      else if (overbounce_debug.integer)
      {
        trap_Print(vaf(
          "^3[overbounce] %s at %i, no jump (%s)\n",
          on_ground ? "horizontal OB" : "vertical OB",
          cg.time,
          held_overbounce_jump ? "left the ground" : "key not held"));
      }
      pending = qfalse;
    }
    else if (cg.time - pending_time > OB_PENDING_TIMEOUT)
    {
      pending = qfalse;
    }
  }
  else if (on_ground && prev_ground == ENTITYNUM_NONE && vz < -OB_MIN_FALL_SPEED && vz < prev_vz)
  {
    // landed inside the ground gap without the velocity being clipped: an overbounce is about to happen
    pending      = qtrue;
    pending_vz   = vz;
    pending_time = cg.time;
    if (overbounce_debug.integer)
      trap_Print(vaf("^3[overbounce] pending at %i: grounded with vz %.0f (prev %.0f)\n", cg.time, vz, prev_vz));
  }

  prev_ground = ps->groundEntityNum;
  prev_vz     = vz;
}
