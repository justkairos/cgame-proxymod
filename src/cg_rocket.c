/*
  ==============================
  Rocket / jump automation, driven through the engine's own buttons (see cg_input.h).

  +mdd_jumpshoot   while held: as soon as you are on the ground with the weapon ready, jump and fire in
                   the same usercmd (like `bind x "+moveup;+attack"`). Re-arms every time you land again
                   while still held.
  +mdd_rocketsync  while held: time a jump + shot so the shot is the last usercmd before your next rocket
                   explodes (same timing model as the grenade tools).
  +mdd_rocketstack while held with the rocket launcher out: if one of your rockets is still in flight and
                   would hit its surface *later* than a rocket fired right now would hit yours, wait, and
                   fire at the moment both impact times coincide.
  ==============================
*/
#include "cg_rocket.h"

#include "cg_cvar.h"
#include "cg_input.h"
#include "cg_local.h"
#include "cg_utils.h"
#include "g_local.h"

#include <stdlib.h>

#define MAX_RL_TIME          15000
#define MISSILE_PRESTEP_TIME 50  // g_missile.c: trTime = level.time - MISSILE_PRESTEP_TIME
#define TIMING_SLACK         2   // ms; cg.time doesn't step evenly, allow acting slightly early
#define JUMPSHOOT_RETRY_TIME 150 // ms on the ground after a failed jump before trying again

#define MAX_TRACKED_ROCKETS 10

static vmCvar_t rocketsync_lead;
static vmCvar_t rocketsync_jump_lead;
static vmCvar_t rocketstack_offset;
static vmCvar_t rocket_debug;

static cvarTable_t rocket_cvars[] = {
  { &rocketsync_lead, "mdd_rocketsync_lead", "8", CVAR_ARCHIVE_ND },
  { &rocketsync_jump_lead, "mdd_rocketsync_jump_lead", "24", CVAR_ARCHIVE_ND },
  { &rocketstack_offset, "mdd_rocketstack_offset", "0", CVAR_ARCHIVE_ND },
  { &rocket_debug, "mdd_rocket_debug", "0", CVAR_ARCHIVE_ND },
};

static qboolean held_jumpshoot;
static qboolean held_rocketsync;
static qboolean held_rocketstack;

//
// own rockets in flight
//
typedef struct
{
  int32_t  id; // entity number, -1 if free
  int32_t  trTime;
  int32_t  impact_time; // server time the rocket reaches the world
  vec3_t   impact;
  qboolean seen;

  qboolean sync_jumped;
  qboolean sync_shot;
  qboolean stacked; // a rocket has been fired to land together with this one
} tracked_rocket_t;

static tracked_rocket_t rockets[MAX_TRACKED_ROCKETS];

static void reset_rockets(void)
{
  for (int32_t i = 0; i < MAX_TRACKED_ROCKETS; ++i) rockets[i].id = -1;
}

static tracked_rocket_t* find_rocket(int32_t id)
{
  for (int32_t i = 0; i < MAX_TRACKED_ROCKETS; ++i)
  {
    if (rockets[i].id == id) return &rockets[i];
  }
  return NULL;
}

// Where and when a TR_LINEAR missile hits the world. Returns -1 if it never does.
static int32_t missile_impact(trajectory_t const* pos, vec3_t impact)
{
  vec3_t  dest;
  trace_t tr;
  BG_EvaluateTrajectory(pos, pos->trTime + MAX_RL_TIME, dest);
  trap_CM_BoxTrace(&tr, pos->trBase, dest, NULL, NULL, 0, CONTENTS_SOLID);
  VectorCopy(tr.endpos, impact);
  if (tr.fraction == 1.f) return -1;
  float const speed = VectorLength(pos->trDelta);
  if (speed < 1.f) return -1;
  return pos->trTime + (int32_t)(1000.f * Distance(pos->trBase, tr.endpos) / speed);
}

static void update_rockets(snapshot_t const* snap, playerState_t const* ps)
{
  for (int32_t i = 0; i < MAX_TRACKED_ROCKETS; ++i) rockets[i].seen = qfalse;

  for (int32_t i = 0; i < snap->numEntities; ++i)
  {
    entityState_t const* const ent = &snap->entities[i];
    if (ent->eType != ET_MISSILE || ent->weapon != WP_ROCKET_LAUNCHER || ent->clientNum != ps->clientNum) continue;
    if (ent->pos.trType != TR_LINEAR) continue;

    tracked_rocket_t* r = find_rocket(ent->number);
    if (r && r->trTime != ent->pos.trTime) r = NULL; // entity slot reused by a newer rocket
    if (!r)
    {
      for (int32_t j = 0; j < MAX_TRACKED_ROCKETS; ++j)
      {
        if (rockets[j].id != -1 && rockets[j].id != ent->number) continue;
        r = &rockets[j];
        memset(r, 0, sizeof(*r));
        r->id          = ent->number;
        r->trTime      = ent->pos.trTime;
        r->impact_time = missile_impact(&ent->pos, r->impact);
        if (rocket_debug.integer)
          trap_Print(vaf(
            "^3[rocket] tracking ent %i, impact at %i (in %i ms), cg.time %i\n",
            ent->number,
            r->impact_time,
            r->impact_time - cg.time,
            cg.time));
        break;
      }
      if (!r) continue; // no free slot
    }
    r->seen = qtrue;
  }

  for (int32_t i = 0; i < MAX_TRACKED_ROCKETS; ++i)
  {
    if (rockets[i].id != -1 && !rockets[i].seen) rockets[i].id = -1; // exploded
  }
}

//
// +mdd_jumpshoot
//
typedef enum
{
  JS_ARMED,
  JS_SHOT,
  JS_AIRBORNE
} jumpshoot_state_t;

static jumpshoot_state_t js_state;
static int32_t           js_shot_time;
static int32_t           js_prev_jumppad; // ps->jumppad_ent last frame

static void jumpshoot(playerState_t const* ps)
{
  qboolean const on_ground = ps->groundEntityNum != ENTITYNUM_NONE;

  // Jump pads: BG_TouchJumpPad sets ps->jumppad_ent and replaces the velocity in the frame the pad is touched
  // (prediction clears it the frame after). The pad is applied in G_TouchTriggers, after that command's
  // weapon code, so the next usercmd is the earliest possible shot - fire it then, no jump needed.
  qboolean const hit_pad = ps->jumppad_ent != 0 && js_prev_jumppad == 0;
  js_prev_jumppad        = ps->jumppad_ent;

  if (!held_jumpshoot)
  {
    js_state = JS_ARMED;
    return;
  }

  if (hit_pad && ps->velocity[2] > 0)
  {
    if (ps->weaponTime <= 0)
    {
      tap_key(K_ATTACK);
      js_state     = JS_SHOT;
      js_shot_time = cg.time;
      if (rocket_debug.integer)
        trap_Print(
          vaf("^3[jumpshoot] jump pad %i: shoot at %i (vz %.0f)\n", ps->jumppad_ent, cg.time, ps->velocity[2]));
    }
    else if (rocket_debug.integer)
    {
      trap_Print(vaf(
        "^3[jumpshoot] jump pad %i at %i but weapon not ready (%i ms)\n", ps->jumppad_ent, cg.time, ps->weaponTime));
    }
    return;
  }

  switch (js_state)
  {
  case JS_AIRBORNE:
    if (!on_ground) break;
    js_state = JS_ARMED; // landed, go again right away
    // fall through
  case JS_ARMED:
    if (!on_ground || ps->weaponTime > 0) break;
    // jump and fire in the same usercmd, exactly like `bind x "+moveup;+attack"`: PmoveSingle runs
    // PM_WalkMove (the jump) before PM_Weapon (the shot), so the projectile still leaves at floor height
    tap_key(K_MOVEUP);
    tap_key(K_ATTACK);
    js_state     = JS_SHOT;
    js_shot_time = cg.time;
    if (rocket_debug.integer) trap_Print(vaf("^3[jumpshoot] jump + shoot at %i\n", cg.time));
    break;
  case JS_SHOT:
    if (!on_ground)
      js_state = JS_AIRBORNE;
    else if (cg.time - js_shot_time > JUMPSHOOT_RETRY_TIME)
      js_state = JS_ARMED; // jump didn't take (e.g. jump key held), try again
    break;
  }
}

//
// +mdd_rocketsync
//
static void rocketsync(playerState_t const* ps)
{
  if (!held_rocketsync) return;

  int32_t const now = cg.time;
  int32_t const dt  = input_dt();

  // next rocket to explode
  tracked_rocket_t* r = NULL;
  for (int32_t i = 0; i < MAX_TRACKED_ROCKETS; ++i)
  {
    tracked_rocket_t* c = &rockets[i];
    if (c->id == -1 || c->impact_time < 0 || c->impact_time <= now) continue;
    if (!r || c->impact_time < r->impact_time) r = c;
  }
  if (!r) return;

  // same model as the grenade tools: the server runs a usercmd ~ping after its serverTime; a late shot still
  // fires but a late jump is lost, so the jump gets extra lead
  int32_t const deadline      = r->impact_time - rocketsync_lead.integer - ps->ping;
  int32_t const jump_deadline = deadline - rocketsync_jump_lead.integer;

  if (!r->sync_jumped)
  {
    if (jump_deadline - now <= 2 * dt + TIMING_SLACK)
    {
      tap_key(K_MOVEUP);
      r->sync_jumped = qtrue;
      if (rocket_debug.integer)
        trap_Print(vaf(
          "^3[rocketsync] jump: cmd time %i, impact %i (in %i ms), ping %i, ground %s\n",
          now,
          r->impact_time,
          r->impact_time - now,
          ps->ping,
          ps->groundEntityNum != ENTITYNUM_NONE ? "yes" : "NO"));
    }
    return; // shoot next frame at the earliest
  }

  if (!r->sync_shot && deadline - now <= dt + TIMING_SLACK)
  {
    tap_key(K_ATTACK);
    r->sync_shot = qtrue;
    if (rocket_debug.integer)
      trap_Print(vaf(
        "^3[rocketsync] shoot: cmd time %i, impact %i (in %i ms), ping %i, weaponTime %i\n",
        now,
        r->impact_time,
        r->impact_time - now,
        ps->ping,
        ps->weaponTime));
  }
}

//
// +mdd_rocketstack
//
static void rocketstack(playerState_t const* ps)
{
  if (!held_rocketstack || ps->weapon != WP_ROCKET_LAUNCHER) return;

  int32_t const now = cg.time;
  int32_t const dt  = input_dt();

  // where would a rocket fired in this usercmd hit, and when (server fires it ~ping after cmd time)
  gentity_t ent;
  BG_PlayerStateToEntityState(ps, &ent.s, qtrue);
  VectorCopy(ent.s.pos.trBase, ent.r.currentOrigin);
  gentity_t m;
  FireWeapon(ps, &m, &ent);

  vec3_t        impact;
  int32_t const flight = missile_impact(&m.s.pos, impact) - m.s.pos.trTime; // ms from trTime to impact
  if (flight < 0) return;
  int32_t const impact_new = now + ps->ping - MISSILE_PRESTEP_TIME + flight;

  for (int32_t i = 0; i < MAX_TRACKED_ROCKETS; ++i)
  {
    tracked_rocket_t* r = &rockets[i];
    if (r->id == -1 || r->impact_time < 0 || r->stacked) continue;

    // positive: the existing rocket lands later than ours would -> wait. Fire on the nearest frame to zero.
    int32_t const diff = r->impact_time - impact_new - rocketstack_offset.integer;
    if (abs(diff) > dt / 2 + TIMING_SLACK) continue;

    tap_key(K_ATTACK);
    r->stacked = qtrue;
    if (rocket_debug.integer)
      trap_Print(vaf(
        "^3[rocketstack] fire: cmd time %i, existing impact %i, ours %i (diff %i), ping %i, weaponTime %i\n",
        now,
        r->impact_time,
        impact_new,
        diff,
        ps->ping,
        ps->weaponTime));
    break; // one shot per frame
  }
}

//
// entry points
//
void init_rocket(void)
{
  init_cvars(rocket_cvars, ARRAY_LEN(rocket_cvars));

  register_button("mdd_jumpshoot", &held_jumpshoot);
  register_button("mdd_rocketsync", &held_rocketsync);
  register_button("mdd_rocketstack", &held_rocketstack);

  js_state = JS_ARMED;
  reset_rockets();
}

void run_rocket(void)
{
  update_cvars(rocket_cvars, ARRAY_LEN(rocket_cvars));

  if (cg.demoPlayback)
  {
    reset_rockets();
    return;
  }

  static snapshot_t snap;
  int32_t           snapNum, serverTime;
  trap_GetCurrentSnapshotNumber(&snapNum, &serverTime);
  if (!trap_GetSnapshot(snapNum, &snap)) return;

  playerState_t const* const ps = getPs();
  if (ps->pm_type != PM_NORMAL || (ps->pm_flags & PMF_FOLLOW) || ps->clientNum != cg.clientNum)
  {
    reset_rockets();
    js_state = JS_ARMED;
    return;
  }

  update_rockets(&snap, ps);

  jumpshoot(ps);
  rocketsync(ps);
  rocketstack(ps);
}
