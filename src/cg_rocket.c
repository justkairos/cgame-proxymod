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
static vmCvar_t jumpshoot_pad_delay;

static cvarTable_t rocket_cvars[] = {
  { &rocketsync_lead, "mdd_rocketsync_lead", "8", CVAR_ARCHIVE_ND },
  { &rocketsync_jump_lead, "mdd_rocketsync_jump_lead", "24", CVAR_ARCHIVE_ND },
  { &rocketstack_offset, "mdd_rocketstack_offset", "0", CVAR_ARCHIVE_ND },
  { &rocket_debug, "mdd_rocket_debug", "0", CVAR_ARCHIVE_ND },
  { &jumpshoot_pad_delay, "mdd_jumpshoot_pad_delay", "8", CVAR_ARCHIVE_ND },
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
#define PAD_PREDICT_MS    400  // how far ahead to look for a pad
#define PAD_ROCKET_RADIUS 96.f // the rocket must land this close to the launch point to count as a pad shot

// ms until the player's box enters an upward ET_PUSH_TRIGGER (as CG_TouchTriggers tests it), -1 if none in
// PAD_PREDICT_MS. Straight-line motion with gravity, stopped by the world; no air control.
static int32_t predict_pad_contact(playerState_t const* ps, vec3_t contact_org)
{
  snapshot_t const* const snap = getSnap();
  clipHandle_t            pads[32];
  int32_t                 num_pads   = 0;
  int32_t const           num_bmodel = trap_CM_NumInlineModels();
  for (int32_t i = 0; i < snap->numEntities && num_pads < 32; ++i)
  {
    entityState_t const* const e = &snap->entities[i];
    // a jump pad is a trigger bmodel whose origin2 is the push velocity (what BG_TouchJumpPad copies). Matched
    // structurally rather than by eType, in case the mod's entity type enum differs from vanilla's.
    if (e->modelindex <= 0 || e->modelindex >= num_bmodel || e->origin2[2] <= 0) continue;
    if (e->eType == ET_MOVER || e->eType == ET_PLAYER || e->eType == ET_MISSILE) continue;
    clipHandle_t const cm = trap_CM_InlineModel(e->modelindex);
    if (cm) pads[num_pads++] = cm;
  }
  if (!num_pads) return -1;

  vec3_t const mins = { -15, -15, -24 };
  vec3_t const maxs = { 15, 15, ps->pm_flags & PMF_DUCKED ? 16.f : 32.f };
  vec3_t       org, vel;
  VectorCopy(ps->origin, org);
  VectorCopy(ps->velocity, vel);
  qboolean    ground = ps->groundEntityNum != ENTITYNUM_NONE;
  float const dt     = .008f;

  for (int32_t t = 0; t <= PAD_PREDICT_MS; t += 8)
  {
    trace_t tr;
    for (int32_t i = 0; i < num_pads; ++i)
    {
      trap_CM_BoxTrace(&tr, org, org, mins, maxs, pads[i], -1);
      if (tr.startsolid)
      {
        VectorCopy(org, contact_org);
        return t;
      }
    }
    // advance one usercmd
    vec3_t next;
    if (!ground)
    {
      float const vz_end = vel[2] - ps->gravity * dt;
      VectorMA(org, dt, vel, next);
      next[2] = org[2] + (vel[2] + vz_end) * .5f * dt; // PM_SlideMove's half-gravity integration
      vel[2]  = vz_end;
    }
    else
    {
      VectorMA(org, dt, vel, next);
      next[2] = org[2];
    }
    trap_CM_BoxTrace(&tr, org, next, mins, maxs, 0, MASK_PLAYERSOLID);
    VectorCopy(tr.endpos, org);
    if (tr.fraction < 1.f)
    {
      if (tr.plane.normal[2] > .7f)
      {
        ground = qtrue;
        vel[2] = 0;
      }
      else
      {
        return -1; // hit a wall, don't guess further
      }
    }
  }
  return -1;
}

typedef enum
{
  JS_ARMED,
  JS_SHOT,
  JS_AIRBORNE
} jumpshoot_state_t;

static jumpshoot_state_t js_state;
static int32_t           js_shot_time;
static int32_t           js_prev_jumppad; // ps->jumppad_ent last frame
static int32_t           js_pad_fired_at; // cg.time of the last pad shot (don't double fire), 0 if none
static float             js_pad_peak_vz;  // highest upward velocity seen after the last pad launch (debug)
static int32_t           js_launch_time;  // cg.time of the last upward pad launch while measuring, else 0

static void jumpshoot(playerState_t const* ps)
{
  qboolean const on_ground = ps->groundEntityNum != ENTITYNUM_NONE;

  // Jump pads: BG_TouchJumpPad sets ps->jumppad_ent and replaces the velocity in the frame the pad is touched
  // (prediction clears it the frame after). The pad is applied in G_TouchTriggers, after that command's
  // weapon code, so the next usercmd is the earliest possible shot - fire it then, no jump needed.
  qboolean const hit_pad = ps->jumppad_ent != 0 && js_prev_jumppad == 0;
  js_prev_jumppad        = ps->jumppad_ent;
  if (hit_pad && rocket_debug.integer > 1)
  {
    snapshot_t const* const snap = getSnap();
    for (int32_t i = 0; i < snap->numEntities; ++i)
    {
      entityState_t const* const e = &snap->entities[i];
      if (e->number != ps->jumppad_ent) continue;
      trap_Print(vaf(
        "^3[jumpshoot] pad ent %i: eType %i, modelindex %i, origin2 %.0f %.0f %.0f (ET_PUSH_TRIGGER would be %i)\n",
        e->number,
        e->eType,
        e->modelindex,
        e->origin2[0],
        e->origin2[1],
        e->origin2[2],
        ET_PUSH_TRIGGER));
    }
  }

  // peak upward speed after any upward pad launch (manual or automated), for comparing techniques
  if (hit_pad && ps->velocity[2] > 0 && rocket_debug.integer)
  {
    js_launch_time = cg.time;
    js_pad_peak_vz = ps->velocity[2];
  }
  if (js_launch_time)
  {
    if (ps->velocity[2] > js_pad_peak_vz) js_pad_peak_vz = ps->velocity[2];
    if (cg.time - js_launch_time > 400)
    {
      trap_Print(vaf("^2[jumpshoot] peak upward speed after the pad launch: %.0f ups\n", js_pad_peak_vz));
      js_launch_time = 0;
    }
  }

  if (!held_jumpshoot)
  {
    js_state        = JS_ARMED;
    js_pad_fired_at = 0;
    return;
  }

  // Pad-rocket: the pad REPLACES the velocity, so knockback only counts if the rocket explodes after the
  // launch. Predict when the player enters a pad trigger and when a rocket fired now would hit next to it,
  // and fire so the impact lands mdd_jumpshoot_pad_delay ms after the launch. If no such shot is possible
  // (not looking at the pad), fall back to firing in the usercmd right after the launch.
  if (js_pad_fired_at && cg.time - js_pad_fired_at > 400) js_pad_fired_at = 0;
  vec3_t        contact_org;
  int32_t const contact = js_pad_fired_at ? -1 : predict_pad_contact(ps, contact_org);
  if (contact >= 0 && ps->weaponTime <= 0)
  {
    {
      gentity_t ent;
      BG_PlayerStateToEntityState(ps, &ent.s, qtrue);
      VectorCopy(ent.s.pos.trBase, ent.r.currentOrigin);
      gentity_t m;
      FireWeapon(ps, &m, &ent);
      vec3_t        impact;
      int32_t const flight = missile_impact(&m.s.pos, impact) - m.s.pos.trTime; // ms from trTime (= fire - 50)
      if (rocket_debug.integer > 1)
        trap_Print(vaf(
          "^3[jumpshoot] pad in %i ms, rocket flight %i ms, lands %.0f from the launch point\n",
          contact,
          flight,
          Distance(impact, contact_org)));
      // rocket lands near where the launch happens?
      if (flight >= 0 && Distance(impact, contact_org) < PAD_ROCKET_RADIUS)
      {
        // fired in the next usercmd: impact at cg.time - 50 + flight; launch at cg.time + contact
        int32_t const impact_after_launch = flight - MISSILE_PRESTEP_TIME - contact;
        // No early slack: an impact before the launch is wiped entirely, one slightly after only costs a few
        // units of distance - so err late, never early. Except for the touch command itself (contact 0):
        // G_TouchTriggers runs before ClientEvents fires the weapon, so a rocket fired in that command is
        // spawned after the pad applied and explodes in the next server frame - always after the launch.
        // mdd_jumpshoot_pad_delay = where the rocket impact should land relative to the predicted touch, in ms
        // (negative: before it). Both the touch time (box vs the trigger brush, with the real velocity and
        // gravity) and the rocket flight to the surface are predicted, so this adapts to the trigger height
        // and the approach speed by itself. contact 0 fires regardless: that's the last chance.
        if (contact <= 0 || impact_after_launch >= jumpshoot_pad_delay.integer)
        {
          tap_key(K_ATTACK);
          js_pad_fired_at = cg.time;
          js_state        = JS_SHOT;
          js_shot_time    = cg.time;
          if (rocket_debug.integer)
            trap_Print(vaf(
              "^3[jumpshoot] pad shot at %i: launch in %i ms, rocket lands %i ms after it\n",
              cg.time,
              contact,
              impact_after_launch));
        }
      }
    }
  }
  if (hit_pad && ps->velocity[2] > 0 && !js_pad_fired_at)
  {
    // launched without a planned shot: earliest fallback is this usercmd
    if (ps->weaponTime <= 0)
    {
      tap_key(K_ATTACK);
      js_pad_fired_at = cg.time;
      js_state        = JS_SHOT;
      js_shot_time    = cg.time;
      if (rocket_debug.integer)
        trap_Print(vaf("^3[jumpshoot] pad shot at %i (after launch, no prediction)\n", cg.time));
    }
    else if (rocket_debug.integer)
    {
      trap_Print(vaf("^3[jumpshoot] jump pad at %i but weapon not ready (%i ms)\n", cg.time, ps->weaponTime));
    }
    return;
  }
  if (contact >= 0) return; // a pad is coming up: don't do a ground jump-shot in front of it

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
