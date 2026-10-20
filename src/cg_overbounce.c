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

#include <math.h>

#define OB_MIN_FALL_SPEED  200.f // z velocity must be below -this while grounded to count as an OB landing
#define OB_PENDING_TIMEOUT 100   // ms; give up if the OB frame never shows up
#define OB_REGROUND_TIMEOUT                                                                                            \
  150 // ms; after a tall OB the leftover upward z (0.001 * V^2 / h) can exceed the
      // 10 ups "jumping away" threshold of PM_GroundTrace for a frame or two; wait
      // for the ground flag to come back before jumping

static vmCvar_t overbounce_debug;

static cvarTable_t overbounce_cvars[] = {
  { &overbounce_debug, "mdd_overbounce_debug", "0", CVAR_ARCHIVE_ND },
};

static qboolean held_overbounce_jump;
static qboolean held_overbounce_zero;

static int32_t  prev_ground;
static float    prev_vz;
static qboolean pending;
static float    pending_vz;
static int32_t  pending_time;
static qboolean await_ground; // OB speed obtained but not grounded yet (tall OB), jump once we are
static int32_t  await_time;

void init_overbounce(void)
{
  init_cvars(overbounce_cvars, ARRAY_LEN(overbounce_cvars));
  register_button("mdd_overbounce_jump", &held_overbounce_jump);
  register_button("mdd_overbounce_zero", &held_overbounce_zero);

  prev_ground = ENTITYNUM_NONE;
  prev_vz     = 0;
  pending     = qfalse;
}

/*
=================
zero_horizontal

+mdd_overbounce_zero: kill horizontal velocity in the air with movement keys so the landing is a vertical OB.

A vertical OB needs the horizontal velocity to be exactly 0 at the OB frame: PM_Friction zeroes it when the
speed is < 1 and only then does the clip + rescale in PM_WalkMove send all of the fall speed straight up.
Velocities are snapped to integers after every pmove, so the last step must change a component of 1 by less
than 1.5 (so it rounds to 0) - air acceleration is 1 * frametime * wishspeed per frame, i.e. 2.56 running
(overshoots forever: 1 -> -2 -> 1 ...), 1.29 walking (1 -> -0.29 -> 0). CPM's airstop accel is 2.5x that,
so there the final approach additionally ducks (wishspeed * 0.25).
=================
*/
static qboolean zero_active; // we are holding movement keys for +mdd_overbounce_zero

static void zero_horizontal(playerState_t const* ps)
{
  if (!held_overbounce_zero || ps->groundEntityNum != ENTITYNUM_NONE)
  {
    if (zero_active) release_move_keys();
    zero_active = qfalse;
    return;
  }
  zero_active = qtrue;

  vec2_t const v     = { ps->velocity[0], ps->velocity[1] };
  float const  speed = VectorLength2(v);
  if (speed < .5f)
  {
    release_move_keys(); // snapped to exactly (0, 0)
    return;
  }

  qboolean const cpm = (ps->pm_flags & PMF_PROMODE) != 0;

  // wish direction = against the velocity, expressed in the view frame (PM_AirMove projects to the flat plane)
  float const  yaw     = DEG2RAD(ps->viewangles[YAW]);
  vec2_t const forward = { cosf(yaw), sinf(yaw) };
  vec2_t const right   = { sinf(yaw), -cosf(yaw) };
  float const  df      = -(v[0] * forward[0] + v[1] * forward[1]) / speed;
  float const  dr      = -(v[0] * right[0] + v[1] * right[1]) / speed;

  // 8-way quantization (sin 22.5 deg)
  qboolean f = df > .3827f, b = df < -.3827f, r = dr > .3827f, l = dr < -.3827f;

  // CPM: side keys alone get the 70-accel/30-wishspeed strafe treatment which overshoots near zero, so close in
  // prefer forward/back which use the (duckable, walkable) airstop acceleration
  if (cpm && speed < 20 && (f || b)) r = l = qfalse;

  set_key(K_FORWARD, f);
  set_key(K_BACK, b);
  set_key(K_MOVERIGHT, r);
  set_key(K_MOVELEFT, l);

  qboolean const want_walk = speed < (cpm ? 10.f : 4.f);
  set_key(K_SPEED, want_walk == (cvar_getInteger("cl_run") != 0)); // CL_KeyMove: walking = !(in_speed ^ cl_run)
  set_key(K_MOVEDOWN, cpm && speed < 10.f);
}

void run_overbounce(void)
{
  update_cvars(overbounce_cvars, ARRAY_LEN(overbounce_cvars));

  playerState_t const* const ps        = getPs();
  qboolean const             on_ground = ps->groundEntityNum != ENTITYNUM_NONE;
  float const                vz        = ps->velocity[2];

  qboolean const inactive = cg.demoPlayback || ps->pm_type != PM_NORMAL || (ps->pm_flags & PMF_FOLLOW);

  if (inactive)
  {
    if (zero_active) release_move_keys();
    zero_active = qfalse;
  }
  else
  {
    zero_horizontal(ps);
  }

  if (inactive)
  {
    pending      = qfalse;
    await_ground = qfalse;
  }
  else if (pending)
  {
    if (vz != pending_vz)
    {
      // cmd2 ran: velocity got converted
      pending = qfalse;
      if (!held_overbounce_jump)
      {
        if (overbounce_debug.integer) trap_Print(vaf("^3[overbounce] OB at %i, key not held\n", cg.time));
      }
      else if (on_ground)
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
      else
      {
        await_ground = qtrue;
        await_time   = cg.time;
        if (overbounce_debug.integer)
          trap_Print(
            vaf("^3[overbounce] OB at %i, not grounded (vz %.0f), waiting for ground\n", cg.time, ps->velocity[2]));
      }
    }
    else if (cg.time - pending_time > OB_PENDING_TIMEOUT)
    {
      pending = qfalse;
    }
  }
  else if (await_ground)
  {
    if (on_ground && held_overbounce_jump)
    {
      tap_key(K_MOVEUP);
      await_ground = qfalse;
      if (overbounce_debug.integer)
        trap_Print(vaf(
          "^3[overbounce] jump at %i (regrounded after %i ms): speed %.0f (%.0f %.0f %.0f)\n",
          cg.time,
          cg.time - await_time,
          VectorLength(ps->velocity),
          ps->velocity[0],
          ps->velocity[1],
          ps->velocity[2]));
    }
    else if (!held_overbounce_jump || cg.time - await_time > OB_REGROUND_TIMEOUT)
    {
      await_ground = qfalse;
      if (overbounce_debug.integer) trap_Print(vaf("^3[overbounce] gave up waiting for ground at %i\n", cg.time));
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
