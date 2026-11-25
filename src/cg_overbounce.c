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
#include <stdio.h>

#define OB_MIN_FALL_SPEED  200.f // z velocity must be below -this while grounded to count as an OB landing
#define OB_PENDING_TIMEOUT 100   // ms; give up if the OB frame never shows up
#define OB_JUMP_TIMEOUT    200   // ms a jump press waits on the ground for its jump
// After an OB the clip leaves an upward z of 0.001 * V^2 / h (V fall speed, h horizontal speed). Above 10 ups
// PM_GroundTrace takes that as "jumping away" and the player hops: for 2 * vz / gravity seconds, so the faster
// the fall the longer. The jump key is pressed during the hop and held until the landing jumps.
#define OB_REGROUND_MARGIN 250  // ms on top of the computed hop time
#define OB_REGROUND_MAX    5000 // ms

static vmCvar_t overbounce_debug;

static cvarTable_t overbounce_cvars[] = {
  { &overbounce_debug, "mdd_overbounce_debug", "0", CVAR_ARCHIVE_ND },
};

static qboolean held_overbounce_jump;
static qboolean held_overbounce_zero;

static int32_t  prev_ground;
static float    prev_vz;
static float    prev_h;     // horizontal speed
static float    prev_speed; // total speed
static qboolean pending;
static float    pending_vz;
static int32_t  pending_time;
static qboolean await_ground; // OB speed obtained but hopping (see OB_REGROUND_MARGIN): jump on the landing
static int32_t  await_time;
static int32_t  await_timeout;

void init_overbounce(void)
{
  init_cvars(overbounce_cvars, ARRAY_LEN(overbounce_cvars));
  register_button("mdd_overbounce_jump", &held_overbounce_jump);
  register_button("mdd_overbounce_zero", &held_overbounce_zero);

  prev_ground  = ENTITYNUM_NONE;
  prev_vz      = 0;
  prev_h       = 0;
  prev_speed   = 0;
  pending      = qfalse;
  await_ground = qfalse;
}

qboolean overbounce_jump_held(void)
{
  return held_overbounce_jump;
}

qboolean overbounce_jump_pending(void)
{
  return held_overbounce_jump && (pending || await_ground);
}

// The overbounce has happened (the velocity is converted): jump with it as soon as the engine allows.
static void jump_with_ob(playerState_t const* ps, qboolean on_ground, char const* how)
{
  if (!held_overbounce_jump)
  {
    if (overbounce_debug.integer) trap_Print(vaf("^3[overbounce] OB at %i (%s), key not held\n", cg.time, how));
    return;
  }
  int32_t timeout = OB_JUMP_TIMEOUT;
  if (!on_ground)
  {
    // hopping: press now, the key is still down when the hop lands
    float const g   = ps->gravity > 0 ? (float)ps->gravity : 800.f;
    float const hop = ps->velocity[2] > 0 ? 2000.f * ps->velocity[2] / g : 0;
    timeout         = (int32_t)hop + OB_REGROUND_MARGIN;
    if (timeout > OB_REGROUND_MAX) timeout = OB_REGROUND_MAX;
    await_ground  = qtrue;
    await_time    = cg.time;
    await_timeout = timeout;
  }
  if (!key_is_held(K_MOVEUP) && jump_was_released()) tap_jump_latched(input_stagger(), timeout);
  if (overbounce_debug.integer)
  {
    char where[64];
    if (on_ground)
      snprintf(where, sizeof(where), "on the ground");
    else
      snprintf(where, sizeof(where), "hopping, landing expected within %i ms", timeout);
    trap_Print(vaf(
      "^3[overbounce] OB at %i (%s): speed %.0f (%.0f %.0f %.0f), %s, jump key %s\n",
      cg.time,
      how,
      VectorLength(ps->velocity),
      ps->velocity[0],
      ps->velocity[1],
      ps->velocity[2],
      where,
      jump_latched() ? "pressed" : "NOT pressed (jump key already down)"));
  }
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

  float const h     = VectorLength2(ps->velocity);
  float const speed = VectorLength(ps->velocity);

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
      jump_with_ob(ps, on_ground, "seen pending");
    }
    else if (cg.time - pending_time > OB_PENDING_TIMEOUT)
    {
      pending = qfalse;
    }
  }
  else if (await_ground)
  {
    // the key was pressed during the hop; this only tracks it for autojump and the debug output
    qboolean const jumped = !jump_was_released();
    if (jumped || !held_overbounce_jump || cg.time - await_time > await_timeout)
    {
      await_ground = qfalse;
      if (!jumped) release_jump();
      if (overbounce_debug.integer)
        trap_Print(vaf(
          "^3[overbounce] %s at %i, %i ms after the OB: speed %.0f\n",
          jumped ? "jumped" : "gave up waiting for the landing",
          cg.time,
          cg.time - await_time,
          speed));
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
  else if (
    prev_ground == ENTITYNUM_NONE && prev_vz < -OB_MIN_FALL_SPEED && vz > .5f * prev_vz &&
    h > prev_h + .5f * -prev_vz && fabsf(speed - prev_speed) < .1f * prev_speed + 50.f)
  {
    // The pending state was never visible: with pmove_fixed one usercmd in three runs two pmoves, and when
    // those are the landing and the conversion, the first thing we see is the result - airborne and falling
    // a frame ago, now the fall speed is gone and the same speed points along the floor. (A normal landing
    // loses the fall speed, a knockback does not keep the total.)
    jump_with_ob(ps, on_ground, "conversion already done");
  }

  prev_ground = ps->groundEntityNum;
  prev_vz     = vz;
  prev_h      = h;
  prev_speed  = speed;
}
