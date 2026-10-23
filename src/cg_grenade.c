/*
  ==============================
  Grenade prediction tools.

  Everything here is driven purely through the engine's own console commands
  (+forward, +moveup, +attack, +left, ...) and the cl_yawspeed/cl_pitchspeed
  cvars, so no engine modification is needed. A cgame cannot write usercmds
  directly; the only way to generate input is to press the engine's virtual
  buttons, which is what this file does.

  Timing model (verified against the engine source):
    Com_Frame: Cbuf_Execute() -> CL_Frame: CL_SendCmd() -> CL_SetCGameTime() -> SCR_UpdateScreen()
  So a console command sent while the cgame is running in frame N is executed
  at the start of frame N+1, right before the usercmd of frame N+1 is built.
  That usercmd gets serverTime = cl.serverTime from frame N = cg.time of the
  frame in which we sent the command.
  ==============================
*/
#include "cg_grenade.h"

#include "bg_local.h"
#include "cg_cvar.h"
#include "cg_input.h"
#include "cg_local.h"
#include "cg_utils.h"
#include "g_local.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NADE_FUSE_TIME       2500 // g_missile.c: bolt->nextthink = level.time + 2500
#define MISSILE_PRESTEP_TIME 50   // g_missile.c: trTime = level.time - MISSILE_PRESTEP_TIME
#define SIM_STEP             8    // server frame length (sv_fps 125) used to replay G_RunMissile

#define MAX_TRACKED_NADES 10

// chase tuning
#define CHASE_AXIS_TOLERANCE  1.5f // stop pressing a key once the predicted stop position is this close (per axis)
#define CHASE_WALK_DISTANCE   48.f // walk instead of run when closer than this
#define CHASE_MAX_STOP_FRAMES 200

// aim tuning
#define AIM_TOLERANCE               0.25f // degrees, per axis
#define AIM_GAIN                    0.8f // fraction of the remaining error to remove per frame (<1 avoids overshoot on frame jitter)
#define AIM_MAX_PITCH               87.f
#define AIM_MIN_HORIZONTAL_DISTANCE 4.f // below this, yaw is undefined: leave yaw alone

// jump/shoot tuning: cg.time doesn't step evenly (CL_AdjustTimeDelta nudges it 1-2ms per snapshot and frames
// alternate e.g. 8/9ms), so allow acting this many ms early rather than skipping the window entirely
#define TIMING_SLACK 2

static vmCvar_t grenade_chase;
static vmCvar_t grenade_aim;
static vmCvar_t grenade_jump;
static vmCvar_t grenade_shoot;
static vmCvar_t grenade_last;
static vmCvar_t grenade_sync;
static vmCvar_t grenade_sync_offset;
static vmCvar_t grenade_lead;
static vmCvar_t grenade_jump_lead;
static vmCvar_t grenade_debug;

static cvarTable_t grenade_cvars[] = {
  { &grenade_chase, "mdd_grenade_chase", "0", CVAR_ARCHIVE_ND },
  { &grenade_aim, "mdd_grenade_aim", "0", CVAR_ARCHIVE_ND },
  { &grenade_jump, "mdd_grenade_jump", "0", CVAR_ARCHIVE_ND },
  { &grenade_shoot, "mdd_grenade_shoot", "0", CVAR_ARCHIVE_ND },
  { &grenade_last, "mdd_grenade_last", "0", CVAR_ARCHIVE_ND },
  { &grenade_sync, "mdd_grenade_sync", "0", CVAR_ARCHIVE_ND },
  { &grenade_sync_offset, "mdd_grenade_sync_offset", "0", CVAR_ARCHIVE_ND },
  { &grenade_lead, "mdd_grenade_lead", "8", CVAR_ARCHIVE_ND },
  { &grenade_jump_lead, "mdd_grenade_jump_lead", "24", CVAR_ARCHIVE_ND },
  { &grenade_debug, "mdd_grenade_debug", "0", CVAR_ARCHIVE_ND },
};

//
// +mdd_grenade_* / -mdd_grenade_* console commands (bindable, e.g. bind shift +mdd_grenade_aim)
// A feature is active while its cvar is non-zero OR its + command is held.
//
typedef enum
{
  F_CHASE,
  F_AIM,
  F_JUMP,
  F_SHOOT,
  F_SYNC,
  F_COUNT
} feature_t;

static char const* const feature_names[F_COUNT] = { "chase", "aim", "jump", "shoot", "sync" };
static vmCvar_t* const   feature_cvars[F_COUNT] = { &grenade_chase,
                                                    &grenade_aim,
                                                    &grenade_jump,
                                                    &grenade_shoot,
                                                    &grenade_sync };
static qboolean          feature_held[F_COUNT];

static qboolean active(feature_t f)
{
  if (feature_cvars[f]->integer || feature_held[f]) return qtrue;
  // +mdd_grenade_sync is a jump + shoot timed to a grenade button
  return (f == F_JUMP || f == F_SHOOT) && (feature_cvars[F_SYNC]->integer || feature_held[F_SYNC]);
}

//
// movers (func_door & co) from the latest snapshot: a "grenade button" is a func_door with health, and a
// grenade explodes the moment it touches a damageable entity (G_MissileImpact) instead of bouncing. The
// snapshot doesn't tell us which movers take damage, so in sync mode the first mover a grenade touches is
// taken to be the button.
//
#define MAX_MOVERS 64
static entityState_t movers[MAX_MOVERS];
static int32_t       num_movers;

static void update_movers(snapshot_t const* snap)
{
  num_movers = 0;
  for (int32_t i = 0; i < snap->numEntities && num_movers < MAX_MOVERS; ++i)
  {
    entityState_t const* const ent = &snap->entities[i];
    if (ent->solid != SOLID_BMODEL || ent->modelindex <= 0 || ent->modelindex >= trap_CM_NumInlineModels()) continue;
    movers[num_movers++] = *ent;
  }
}

// Trace a segment against every mover (at its position at time t); returns the mover's entity number and
// overwrites *trace when one is hit before the current trace end, else -1.
static int32_t trace_movers(trace_t* trace, vec3_t const start, vec3_t const end, int32_t t)
{
  int32_t hit = -1;
  for (int32_t i = 0; i < num_movers; ++i)
  {
    vec3_t  origin, angles;
    trace_t tr;
    BG_EvaluateTrajectory(&movers[i].pos, t, origin);
    BG_EvaluateTrajectory(&movers[i].apos, t, angles);
    trap_CM_TransformedBoxTrace(
      &tr, start, end, NULL, NULL, trap_CM_InlineModel(movers[i].modelindex), MASK_SHOT, origin, angles);
    if (tr.fraction < trace->fraction)
    {
      *trace = tr;
      hit    = movers[i].number;
    }
  }
  return hit;
}

//
// keyboard-look speed cvars (cl_yawspeed, cl_pitchspeed, cl_anglespeedkey)
//
typedef struct
{
  qboolean active;
  qboolean restore_pending; // speeds were zeroed last frame, restore the user's values this frame
  char     yawspeed[MAX_CVAR_VALUE_STRING];
  char     pitchspeed[MAX_CVAR_VALUE_STRING];
  char     anglespeedkey[MAX_CVAR_VALUE_STRING];
} look_t;

static look_t look;

static void look_begin(void)
{
  if (look.active) return;
  if (!look.restore_pending)
  {
    trap_Cvar_VariableStringBuffer("cl_yawspeed", look.yawspeed, sizeof(look.yawspeed));
    trap_Cvar_VariableStringBuffer("cl_pitchspeed", look.pitchspeed, sizeof(look.pitchspeed));
    trap_Cvar_VariableStringBuffer("cl_anglespeedkey", look.anglespeedkey, sizeof(look.anglespeedkey));
  }
  look.restore_pending = qfalse;
  look.active          = qtrue;
  trap_Cvar_Set("cl_anglespeedkey", "1"); // +speed must not scale our turn rate
}

// Zero the speeds first so the half-frame key-up residual in CL_KeyState can't turn us; restore next frame.
static void look_end(void)
{
  if (!look.active) return;
  look.active          = qfalse;
  look.restore_pending = qtrue;
  release_look_keys();
  trap_Cvar_Set("cl_yawspeed", "0");
  trap_Cvar_Set("cl_pitchspeed", "0");
}

static void look_restore(void)
{
  if (!look.restore_pending) return;
  look.restore_pending = qfalse;
  trap_Cvar_Set("cl_yawspeed", look.yawspeed);
  trap_Cvar_Set("cl_pitchspeed", look.pitchspeed);
  trap_Cvar_Set("cl_anglespeedkey", look.anglespeedkey);
}

//
// tracked grenades
//
typedef struct
{
  int32_t  id; // entity number, -1 if free
  int32_t  explode_time;
  qboolean seen;

  // cache key of the trajectory the final position was computed from
  trType_t trType;
  int32_t  trTime;
  vec3_t   trBase;
  vec3_t   final_pos;
  qboolean final_valid;

  // sync mode: first mover (grenade button) the grenade touches, -1 if none before the fuse runs out
  int32_t button_ent;
  int32_t button_time;
  vec3_t  button_pos;

  qboolean jumped;
  qboolean shot;
} tracked_nade_t;

static tracked_nade_t tracked[MAX_TRACKED_NADES];

static void reset_tracked(void)
{
  for (int32_t i = 0; i < MAX_TRACKED_NADES; ++i) tracked[i].id = -1;
}

static tracked_nade_t* find_tracked(int32_t id)
{
  for (int32_t i = 0; i < MAX_TRACKED_NADES; ++i)
  {
    if (tracked[i].id == id) return &tracked[i];
  }
  return NULL;
}

static tracked_nade_t* track(int32_t id, int32_t explode_time)
{
  for (int32_t i = 0; i < MAX_TRACKED_NADES; ++i)
  {
    if (tracked[i].id != -1) continue;
    memset(&tracked[i], 0, sizeof(tracked[i]));
    tracked[i].id           = id;
    tracked[i].explode_time = explode_time;
    tracked[i].seen         = qtrue;
    return &tracked[i];
  }
  return NULL;
}

/*
=================
predict_final_position

Replays G_RunMissile/G_BounceMissile for a grenade (EF_BOUNCE_HALF, TR_GRAVITY)
against the world until explode_time and returns where it will be when it blows.
=================
*/
static void predict_final_position(
  trajectory_t const* pos,
  int32_t             explode_time,
  vec3_t              out,
  int32_t*            button_ent,
  int32_t*            button_time,
  vec3_t              button_pos)
{
  if (button_ent) *button_ent = -1;
  if (pos->trType == TR_STATIONARY)
  {
    // came to rest: G_SetOrigin(ent, trace->endpos)
    VectorCopy(pos->trBase, out);
    return;
  }
  if (pos->trType != TR_GRAVITY)
  {
    BG_EvaluateTrajectory(pos, explode_time, out);
    return;
  }

  trajectory_t tr = *pos;
  vec3_t       cur;
  VectorCopy(tr.trBase, cur);

  int32_t prev = tr.trTime;
  for (int32_t t = tr.trTime + SIM_STEP;; t += SIM_STEP)
  {
    qboolean const last = t >= explode_time;
    if (last) t = explode_time;

    vec3_t  org;
    trace_t trace;
    BG_EvaluateTrajectory(&tr, t, org);
    trap_CM_BoxTrace(&trace, cur, org, NULL, NULL, 0, MASK_SHOT);
    int32_t const mover = trace_movers(&trace, cur, org, t);
    VectorCopy(trace.endpos, cur);

    if (trace.fraction != 1.f)
    {
      // G_BounceMissile
      vec3_t        velocity;
      int32_t const hitTime = prev + (int32_t)((t - prev) * trace.fraction);
      if (mover != -1 && button_ent && *button_ent == -1)
      {
        // a grenade button: G_MissileImpact explodes it right here instead of bouncing
        *button_ent  = mover;
        *button_time = hitTime;
        VectorCopy(cur, button_pos);
      }
      BG_EvaluateTrajectoryDelta(&tr, hitTime, velocity);
      float const dot = DotProduct(velocity, trace.plane.normal);
      VectorMA(velocity, -2 * dot, trace.plane.normal, tr.trDelta);

      // EF_BOUNCE_HALF
      VectorScale(tr.trDelta, .65f, tr.trDelta);
      if (trace.plane.normal[2] > .2f && VectorLength(tr.trDelta) < 40)
      {
        // check for stop
        VectorCopy(cur, out);
        return;
      }

      VectorAdd(cur, trace.plane.normal, cur);
      VectorCopy(cur, tr.trBase);
      tr.trTime = t;
    }

    if (last) break;
    prev = t;
  }

  VectorCopy(cur, out);
}

/*
=================
update_tracked

Mirrors the bookkeeping of the nade timer, but computes the explode time from
the trajectory (exact) instead of the first snapshot it was seen in.
=================
*/
static void update_tracked(snapshot_t const* snap, playerState_t const* ps)
{
  for (int32_t i = 0; i < MAX_TRACKED_NADES; ++i)
  {
    if (tracked[i].id == -1) continue;
    if (tracked[i].explode_time <= snap->serverTime)
      tracked[i].id = -1; // exploded
    else
      tracked[i].seen = qfalse;
  }

  for (int32_t i = 0; i < snap->numEntities; ++i)
  {
    entityState_t const* const ent = &snap->entities[i];
    if (ent->eType != ET_MISSILE || ent->weapon != WP_GRENADE_LAUNCHER || ent->clientNum != ps->clientNum) continue;

    tracked_nade_t* n = find_tracked(ent->number);
    if (n && ent->pos.trType == TR_GRAVITY && ent->pos.trTime + MISSILE_PRESTEP_TIME >= n->explode_time)
    {
      // a brand new grenade reusing the entity slot: it was fired after the old one exploded
      // (a bounce of the old one would have trTime < its explode time)
      n->id = -1;
      n     = NULL;
    }
    if (!n)
    {
      // Explode time is level.time_at_fire + 2500. The original trajectory has trTime = fire - 50, but every
      // bounce resets trTime to the bounce time, so trTime + 2550 is only exact before the first bounce; the
      // first snapshot we see it in is at most a few ms after the fire. Take the tighter of the two upper bounds.
      int32_t explode_time = snap->serverTime + NADE_FUSE_TIME;
      if (ent->pos.trType == TR_GRAVITY)
      {
        int32_t const from_trajectory = ent->pos.trTime + MISSILE_PRESTEP_TIME + NADE_FUSE_TIME;
        if (from_trajectory < explode_time) explode_time = from_trajectory;
      }
      n = track(ent->number, explode_time);
      if (grenade_debug.integer)
        trap_Print(vaf(
          "^3[grenade] tracking ent %i, explode at %i (in %i ms), cg.time %i\n",
          ent->number,
          explode_time,
          explode_time - cg.time,
          cg.time));
    }
    if (!n) continue; // no free slot

    n->seen = qtrue;

    if (
      !n->final_valid || n->trType != ent->pos.trType || n->trTime != ent->pos.trTime ||
      !VectorCompare(n->trBase, ent->pos.trBase))
    {
      n->trType = ent->pos.trType;
      n->trTime = ent->pos.trTime;
      VectorCopy(ent->pos.trBase, n->trBase);
      predict_final_position(&ent->pos, n->explode_time, n->final_pos, &n->button_ent, &n->button_time, n->button_pos);
      if (grenade_debug.integer && n->button_ent != -1)
        trap_Print(vaf(
          "^3[grenade] ent %i touches mover %i at %i (in %i ms)\n",
          ent->number,
          n->button_ent,
          n->button_time,
          n->button_time - cg.time));
      n->final_valid = qtrue;
    }
  }

  for (int32_t i = 0; i < MAX_TRACKED_NADES; ++i)
  {
    if (tracked[i].id != -1 && !tracked[i].seen) tracked[i].id = -1; // gone (detonated early)
  }
}

static tracked_nade_t* select_target(qboolean need_button)
{
  tracked_nade_t* best = NULL;
  for (int32_t i = 0; i < MAX_TRACKED_NADES; ++i)
  {
    tracked_nade_t* n = &tracked[i];
    if (n->id == -1 || !n->final_valid) continue;
    if (need_button && n->button_ent == -1) continue;
    if (!best)
      best = n;
    else if (grenade_last.integer ? n->explode_time > best->explode_time : n->explode_time < best->explode_time)
      best = n;
  }
  return best;
}

//
// chase
//

// Horizontal distance the player would still travel if all movement keys were released now
// (PM_Friction, pm_friction/pm_stopspeed, 8ms frames).
static void predict_stop_offset(vec3_t const velocity, vec2_t out)
{
  vec2_t v = { velocity[0], velocity[1] };
  out[0] = out[1] = 0;
  for (int32_t frame = 0; frame < CHASE_MAX_STOP_FRAMES; ++frame)
  {
    float const speed = VectorLength2(v);
    if (speed < 1) break;
    float const control  = speed < pm_stopspeed ? pm_stopspeed : speed;
    float const drop     = control * pm_friction * pm_frametime;
    float       newspeed = speed - drop;
    if (newspeed < 0) newspeed = 0;
    v[0] *= newspeed / speed;
    v[1] *= newspeed / speed;
    out[0] += v[0] * pm_frametime;
    out[1] += v[1] * pm_frametime;
  }
}

static void chase(playerState_t const* ps, vec3_t const target)
{
  qboolean const on_ground = ps->groundEntityNum != ENTITYNUM_NONE && !(ps->pm_flags & PMF_TIME_KNOCKBACK);

  vec2_t err = { target[0] - ps->origin[0], target[1] - ps->origin[1] };
  if (on_ground)
  {
    vec2_t stop;
    predict_stop_offset(ps->velocity, stop);
    err[0] -= stop[0];
    err[1] -= stop[1];
  }

  // express the error in the player's view frame (same vectors PM_WalkMove uses)
  float const  yaw     = DEG2RAD(ps->viewangles[YAW]);
  vec2_t const forward = { cosf(yaw), sinf(yaw) };
  vec2_t const right   = { sinf(yaw), -cosf(yaw) };
  float const  df      = err[0] * forward[0] + err[1] * forward[1];
  float const  dr      = err[0] * right[0] + err[1] * right[1];

  set_key(K_FORWARD, df > CHASE_AXIS_TOLERANCE);
  set_key(K_BACK, df < -CHASE_AXIS_TOLERANCE);
  set_key(K_MOVERIGHT, dr > CHASE_AXIS_TOLERANCE);
  set_key(K_MOVELEFT, dr < -CHASE_AXIS_TOLERANCE);

  // walk when close (in the air a lower wishspeed only lowers air acceleration, so keep running there)
  qboolean const want_walk = on_ground && VectorLength2(err) < CHASE_WALK_DISTANCE;
  // CL_KeyMove: walking = !(in_speed.active ^ cl_run)
  set_key(K_SPEED, want_walk == (cvar_getInteger("cl_run") != 0));
}

//
// aim
//
static void aim(playerState_t const* ps, vec3_t const target, int32_t frametime_ms)
{
  vec3_t eye;
  VectorCopy(ps->origin, eye);
  eye[2] += ps->viewheight;

  vec3_t dir;
  VectorSubtract(target, eye, dir);
  float const horizontal = VectorLength2(dir);

  float const want_pitch = RAD2DEG(-atan2f(dir[2], horizontal));
  float const want_yaw   = RAD2DEG(atan2f(dir[1], dir[0]));

  float pitch_err = AngleNormalize180(want_pitch - ps->viewangles[PITCH]);
  float yaw_err   = AngleNormalize180(want_yaw - ps->viewangles[YAW]);

  // PM_UpdateViewAngles clamps the pitch, don't fight it
  if (want_pitch > AIM_MAX_PITCH || want_pitch < -AIM_MAX_PITCH) pitch_err = 0;
  if (horizontal < AIM_MIN_HORIZONTAL_DISTANCE) yaw_err = 0;

  look_begin();

  // CL_AdjustAngles: viewangles[YAW]   -= 0.001 * frametime * cl_yawspeed   * KeyState(+right)   (+= for +left)
  //                  viewangles[PITCH] -= 0.001 * frametime * cl_pitchspeed * KeyState(+lookup)  (+= for +lookdown)
  // KeyState is 1 for a key held by console command, so choose the speed that removes AIM_GAIN of the error.
  float const per_degree = 1.f / (.001f * (float)frametime_ms);

  if (fabsf(yaw_err) > AIM_TOLERANCE)
  {
    trap_Cvar_Set("cl_yawspeed", vaf("%f", fabsf(yaw_err) * AIM_GAIN * per_degree));
    set_key(K_LEFT, yaw_err > 0);
    set_key(K_RIGHT, yaw_err < 0);
  }
  else
  {
    trap_Cvar_Set("cl_yawspeed", "0");
    set_key(K_LEFT, qfalse);
    set_key(K_RIGHT, qfalse);
  }

  if (fabsf(pitch_err) > AIM_TOLERANCE)
  {
    trap_Cvar_Set("cl_pitchspeed", vaf("%f", fabsf(pitch_err) * AIM_GAIN * per_degree));
    set_key(K_LOOKDOWN, pitch_err > 0);
    set_key(K_LOOKUP, pitch_err < 0);
  }
  else
  {
    trap_Cvar_Set("cl_pitchspeed", "0");
    set_key(K_LOOKUP, qfalse);
    set_key(K_LOOKDOWN, qfalse);
  }
}

//
// jump / shoot
//
// Sync mode: when must the second shot be fired (server time) so that it lands `mdd_grenade_sync_offset` ms
// after the tracked grenade touches the button? The current weapon's projectile is simulated from the
// current view: a grenade has to touch the same button (it only explodes on contact there), a rocket /
// plasma / bfg explodes on whatever it hits first, anything else is instant. Returns -1 if the shot
// can't land (a grenade fired now would miss the button).
static int32_t sync_fire_time(tracked_nade_t const* n, playerState_t const* ps, int32_t* flight_out)
{
  gentity_t ent;
  BG_PlayerStateToEntityState(ps, &ent.s, qtrue);
  VectorCopy(ent.s.pos.trBase, ent.r.currentOrigin);
  gentity_t m;
  memset(&m, 0, sizeof(m));
  FireWeapon(ps, &m, &ent);
  // the missile functions stamp trTime = cg.time - 50, i.e. the fire command is at cg.time
  int32_t const fire_cmd = cg.time;
  int32_t       flight   = 0;
  if (m.s.pos.trType == TR_GRAVITY)
  {
    vec3_t  final, button_pos;
    int32_t button_ent, button_time;
    // it only matters whether it gets there by the time the first one blows: cheap enough to run every frame
    predict_final_position(&m.s.pos, n->button_time + 200, final, &button_ent, &button_time, button_pos);
    if (button_ent != n->button_ent) return -1;
    flight = button_time - fire_cmd;
  }
  else if (m.s.pos.trType == TR_LINEAR)
  {
    vec3_t  dest;
    trace_t tr;
    BG_EvaluateTrajectory(&m.s.pos, m.s.pos.trTime + 10000, dest);
    trap_CM_BoxTrace(&tr, m.s.pos.trBase, dest, NULL, NULL, 0, MASK_SHOT);
    trace_movers(&tr, m.s.pos.trBase, dest, n->button_time);
    float const speed = VectorLength(m.s.pos.trDelta);
    if (tr.fraction == 1.f || speed < 1.f) return -1;
    flight = (int32_t)(1000.f * Distance(m.s.pos.trBase, tr.endpos) / speed) - MISSILE_PRESTEP_TIME;
    if (flight < 0) flight = 0;
  }
  if (flight_out) *flight_out = flight;
  return n->button_time + grenade_sync_offset.integer - flight;
}

static void jump_and_shoot(tracked_nade_t* n, playerState_t const* ps, int32_t dt, qboolean sync)
{
  // The usercmd our commands land in gets serverTime == cg.time; the one after that ~cg.time + dt.
  // The server processes a usercmd roughly ping ms after its serverTime (client clock lags one-way latency,
  // the packet needs another one-way trip), so subtract the ping to act before the explosion *on the server*.
  // A late shot still fires, a late jump is lost (the blast already lifted us off the ground), so the jump
  // gets its own extra lead on top: server frame quantization, cl_maxpackets batching and ping jitter all
  // push a usercmd later, never earlier.
  int32_t const now           = cg.time;
  int32_t const explode_time  = sync ? n->button_time : n->explode_time;
  int32_t       deadline      = explode_time - grenade_lead.integer - ps->ping;
  int32_t const jump_deadline = deadline - grenade_jump_lead.integer;
  int32_t       flight        = 0;

  if (now >= explode_time) return; // too late, it already blew up

  // sync: the shot is timed by the second grenade's own flight to the button, not by a fixed lead
  if (sync && active(F_SHOOT) && !n->shot)
  {
    int32_t const fire = sync_fire_time(n, ps, &flight);
    if (fire < 0)
    {
      if (grenade_debug.integer > 1)
        trap_Print(vaf("^3[grenade] sync: a grenade fired now would miss button %i\n", n->button_ent));
      deadline = -1; // can't shoot this frame; keep re-checking as the view/position changes
    }
    else
    {
      deadline = fire - ps->ping;
    }
  }

  qboolean const want_jump  = active(F_JUMP) && !n->jumped;
  qboolean const want_shoot = active(F_SHOOT) && !n->shot;

  if (want_jump)
  {
    // with shoot enabled the jump goes one frame earlier so the shot is the last cmd before the explosion
    int32_t const frames_needed = active(F_SHOOT) && !sync ? 2 : 1;
    if (jump_deadline - now <= frames_needed * dt + TIMING_SLACK)
    {
      tap_key(K_MOVEUP);
      n->jumped = qtrue;
      if (grenade_debug.integer)
        trap_Print(vaf(
          "^3[grenade] jump: cmd time %i, explode %i (in %i ms), ping %i, lead %i+%i, dt %i, ground %s\n",
          now,
          n->explode_time,
          n->explode_time - now,
          ps->ping,
          grenade_lead.integer,
          grenade_jump_lead.integer,
          dt,
          ps->groundEntityNum != ENTITYNUM_NONE ? "yes" : "NO"));
      if (!sync) return; // shoot next frame at the earliest
    }
  }

  if (want_shoot && deadline >= 0)
  {
    // in sync mode the shot has its own schedule (the flight can be longer than the jump lead), so it doesn't
    // wait for the jump; the input layer still keeps the two presses in separate usercmds
    qboolean const jump_pending = !sync && active(F_JUMP) && !n->jumped;
    if (!jump_pending && deadline - now <= dt + TIMING_SLACK)
    {
      tap_key(K_ATTACK);
      n->shot = qtrue;
      if (grenade_debug.integer)
        trap_Print(vaf(
          "^3[grenade] %s: cmd time %i, explode %i (in %i ms), ping %i, lead %i, flight %i, dt %i, weaponTime %i\n",
          sync ? "sync shoot" : "shoot",
          now,
          explode_time,
          explode_time - now,
          ps->ping,
          grenade_lead.integer,
          flight,
          dt,
          ps->weaponTime));
    }
  }
}

//
// entry points
//
void init_grenade(void)
{
  init_cvars(grenade_cvars, ARRAY_LEN(grenade_cvars));

  for (int32_t f = 0; f < F_COUNT; ++f) register_button(vaf("mdd_grenade_%s", feature_names[f]), &feature_held[f]);
  memset(&look, 0, sizeof(look));
  reset_tracked();
}

static void idle(void)
{
  release_move_keys();
  look_end();
}

void del_grenade(void)
{
  idle();
  // can't wait a frame here: restore immediately
  look.restore_pending = look.restore_pending || look.active;
  look.active          = qfalse;
  look_restore();
}

void run_grenade(void)
{
  update_cvars(grenade_cvars, ARRAY_LEN(grenade_cvars));

  look_restore();

  if (cg.demoPlayback)
  {
    reset_tracked();
    idle();
    return;
  }

  static snapshot_t snap;
  int32_t           snapNum, serverTime;
  trap_GetCurrentSnapshotNumber(&snapNum, &serverTime);
  if (!trap_GetSnapshot(snapNum, &snap))
  {
    idle();
    return;
  }

  playerState_t const* const ps = getPs();
  if (ps->pm_type != PM_NORMAL || (ps->pm_flags & PMF_FOLLOW) || ps->clientNum != cg.clientNum)
  {
    reset_tracked();
    idle();
    return;
  }

  // Always track, even with nothing enabled: the explode time is only exact when a grenade is seen right
  // after launch (every bounce resets trTime), so a key pressed after firing must find it already tracked.
  update_movers(&snap);
  update_tracked(&snap, ps);

  qboolean const enabled = active(F_CHASE) || active(F_AIM) || active(F_JUMP) || active(F_SHOOT);
  if (!enabled)
  {
    idle();
    return;
  }

  // sync mode wants a grenade that is going to touch a mover; without one behave like plain jump/shoot
  tracked_nade_t* n = active(F_SYNC) ? select_target(qtrue) : NULL;
  if (!n) n = select_target(qfalse);
  if (!n)
  {
    idle();
    return;
  }

  // sync mode needs a grenade that is going to touch a mover; otherwise behave like plain jump/shoot
  qboolean const sync   = active(F_SYNC) && n->button_ent != -1;
  float const*   target = sync ? n->button_pos : n->final_pos;

  if (active(F_CHASE) && !n->jumped)
    chase(ps, target);
  else
    release_move_keys();

  if (active(F_AIM))
    aim(ps, target, input_frametime());
  else
    look_end();

  jump_and_shoot(n, ps, input_dt(), sync);
}
