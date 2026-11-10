/*
  ==============================
  +mdd_rocketjump_overbounce

  One rocket jump per key press, aimed where the player is looking, chosen so that the landing is an
  overbounce. At a fixed view angle the only free parameter is how many usercmds lie between the jump and
  the shot; each value gives one knockback and therefore one landing, and only about one landing in twenty
  ends within the 0.25 unit ground trace. So the pitch may additionally be nudged by up to
  mdd_rocketjump_overbounce_nudge degrees right before the shot (restored afterwards), which makes an
  overbounce reachable nearly always. Of all candidates that overbounce, the highest one is taken - the
  closest to the best rocket jump possible at that view angle.

  Every candidate is simulated the way the server will run it (VQ3, 8 ms usercmds):
    jump usercmd    PM_CheckJump (270 ups) + PM_AirMove, gravity half step, velocity snapped every pmove
    fire usercmd    muzzle from the truncated origin + viewheight + 14 forward, truncated; 900 ups, 50 ms prestep
    server frames   the missile advances one server frame at a time; the impact is SnapVectorTowards'ed
    explosion       G_RadiusDamage against the truncated origin's box grown by 1; (int)points;
                    velocity += normalize(exact origin - blast + (0 0 24)) * 5 * points
    flight          free fall against the real map until a move ends within the ground trace untouched
                    (overbounce) or hits the floor (normal landing)

  What can't be known from the client is how many of our usercmds the server has run when the missile
  explodes. Offline that is fixed; mdd_rocketjump_overbounce_explode_offset shifts it, and the debug cvar
  prints predicted against actual state so it can be calibrated.
  ==============================
*/
#include "cg_rjob.h"

#include "bg_local.h"
#include "bg_pmove.h"
#include "bg_public.h"
#include "cg_cvar.h"
#include "cg_input.h"
#include "cg_local.h"
#include "cg_utils.h"
#include "g_local.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RJ_PRESTEP_MSEC    50   // MISSILE_PRESTEP_TIME: a missile is stamped level.time - 50 when fired
#define RJ_CMD_MSEC        8    // usercmd / pmove length everything is simulated with
#define RJ_MAX_DELAY       12   // hard cap for mdd_rocketjump_overbounce_delay_max
#define RJ_MAX_PRE         64   // usercmds simulated before the explosion (delay + missile flight)
#define RJ_MAX_POST        1500 // usercmds after the explosion kept to compare the whole flight with the server
#define RJ_MAX_FLIGHT      2500 // usercmds of flight simulated before giving up (20 s)
#define RJ_MAX_MISSILE     60   // server frames a missile is followed
#define RJ_NUDGE_STEP      9    // pitch search step in usercmd angle units (65536 per turn): ~0.05 degrees
#define RJ_OB_MIN_SPEED    100  // downward speed still held on the ground that counts as an overbounce
#define RJ_PITCH_LIMIT     (16000.f * 360.f / 65536.f) // PM_UpdateViewAngles clamps pitch to 16000 angle units = 87.89 deg
#define RJ_SPLASH_DAMAGE   100.f
#define RJ_SPLASH_RADIUS   120.f
#define RJ_KNOCKBACK_SCALE 5.f // g_knockback 1000 / mass 200
#define RJ_FLIGHT_CACHE    256

static vmCvar_t rj_nudge;
static vmCvar_t rj_delay_max;
static vmCvar_t rj_explode_offset;
static vmCvar_t rj_debug;

static cvarTable_t rj_cvars[] = {
  { &rj_nudge, "mdd_rocketjump_overbounce_nudge", "2", CVAR_ARCHIVE_ND },
  { &rj_delay_max, "mdd_rocketjump_overbounce_delay_max", "6", CVAR_ARCHIVE_ND },
  { &rj_explode_offset, "mdd_rocketjump_overbounce_explode_offset", "0", CVAR_ARCHIVE_ND },
  { &rj_debug, "mdd_rocketjump_overbounce_debug", "0", CVAR_ARCHIVE_ND },
};

static qboolean held;

//
// output: everything goes to the console when the debug cvar is on, and - with more detail - to rjob.log in
// the mod folder, so a failed use can be diagnosed afterwards without relying on what fit on screen
//
static fileHandle_t log_file;
static qboolean     log_open;

static void log_write(char const* text)
{
  if (!log_open) return;
  char   clean[1024];
  size_t n = 0;
  for (char const* c = text; *c && n < sizeof(clean) - 1; ++c)
  {
    if (c[0] == '^' && c[1] >= '0' && c[1] <= '9')
    {
      ++c; // colour code
      continue;
    }
    clean[n++] = *c;
  }
  trap_FS_Write(clean, (int32_t)n, log_file);
}

static void log_begin(void)
{
  if (log_open || !rj_debug.integer) return;
  log_open = trap_FS_FOpenFile("rjob.log", &log_file, FS_APPEND) >= 0 && log_file;
}

static void log_end(void)
{
  if (!log_open) return;
  trap_FS_FCloseFile(log_file);
  log_open = qfalse;
}

static void append(char* dst, size_t size, char const* src)
{
  size_t n = strlen(dst);
  while (*src && n + 1 < size) dst[n++] = *src++;
  dst[n] = 0;
}

// value of `key` in an info string ("\\key\\value\\key\\value")
static char const* info_value(char const* info, char const* key)
{
  static char  value[128];
  size_t const klen = strlen(key);
  value[0]          = 0;
  for (char const* s = info; (s = strchr(s, '\\')) != NULL;)
  {
    ++s;
    char const* const eq = strchr(s, '\\');
    if (!eq) break;
    char const* const end = strchr(eq + 1, '\\');
    size_t const      n   = end ? (size_t)(end - eq - 1) : strlen(eq + 1);
    if ((size_t)(eq - s) == klen && !strncmp(s, key, klen))
    {
      size_t const m = n < sizeof(value) - 1 ? n : sizeof(value) - 1;
      memcpy(value, eq + 1, m);
      value[m] = 0;
      break;
    }
    if (!end) break;
    s = end;
  }
  return value;
}

// console (debug) + log
static void out(char const* text)
{
  if (rj_debug.integer) trap_Print(text);
  log_write(text);
}

// always on the console: things the player has to know even without the debug cvar
static void tell(char const* text)
{
  trap_Print(text);
  log_write(text);
}

static vec3_t const player_mins = { -15, -15, -24 };

typedef struct
{
  vec3_t origin;
  vec3_t velocity;
} state_t;

typedef enum
{
  LAND_UNKNOWN, // hit a wall / ceiling, or never came down
  LAND_NORMAL,
  LAND_OVERBOUNCE,
} landing_t;

typedef struct
{
  qboolean  valid;
  int32_t   delay;       // usercmds between the jump usercmd and the fire usercmd
  int32_t   nudge;       // pitch change in usercmd angle units
  int32_t   explode_cmd; // usercmd index (jump = 0) after which the blast is applied
  int32_t   knockback;
  vec3_t    blast;
  landing_t landing;
  int32_t   land_cmd; // usercmd index of the landing
  float     apex;     // above the starting height
  float     land_speed;
} candidate_t;

// everything the search needs about "now"
typedef struct
{
  playerState_t ps;
  usercmd_t     cmd;       // movement keys assumed held throughout
  int32_t       jump_hold; // usercmds +moveup stays down (changes PM_CmdScale while it is)
  vec3_t        maxs;
  float         start_z;
  int32_t       server_msec;
  qboolean      synchronous;     // g_synchronousClients 1: usercmds are run inside the server frame
  state_t       pre[RJ_MAX_PRE]; // state after usercmd i, no explosion
  int32_t       num_pre;
} world_t;

static world_t w;

//
// movement
//

static void air_accelerate(state_t* s, int32_t cmd_index)
{
  usercmd_t cmd = w.cmd;
  cmd.upmove    = cmd_index < w.jump_hold ? 127 : 0;
  if (!cmd.forwardmove && !cmd.rightmove) return;

  vec3_t forward, right, wishvel, wishdir;
  AngleVectors(w.ps.viewangles, forward, right, NULL);
  forward[2] = 0;
  right[2]   = 0;
  VectorNormalize(forward);
  VectorNormalize(right);
  for (int32_t i = 0; i < 2; ++i) wishvel[i] = forward[i] * cmd.forwardmove + right[i] * cmd.rightmove;
  wishvel[2] = 0;
  VectorCopy(wishvel, wishdir);
  float const wishspeed = VectorNormalize(wishdir) * PM_CmdScale(&w.ps, &cmd);

  // PM_Accelerate(wishdir, wishspeed, pm_airaccelerate)
  float const addspeed = wishspeed - DotProduct(s->velocity, wishdir);
  if (addspeed <= 0) return;
  float accelspeed = pm_airaccelerate * pm_frametime * wishspeed;
  if (accelspeed > addspeed) accelspeed = addspeed;
  VectorMA(s->velocity, accelspeed, wishdir, s->velocity);
}

typedef enum
{
  MOVE_AIR,     // still flying
  MOVE_GROUND,  // the move ended inside the ground trace without touching anything
  MOVE_LANDED,  // the move hit walkable ground
  MOVE_BLOCKED, // the move hit something else
} move_t;

// One airborne usercmd: PM_AirMove + PM_SlideMove with gravity, the ground trace, the velocity snap.
static move_t air_move(state_t* s, int32_t cmd_index)
{
  air_accelerate(s, cmd_index);

  float const end_vz = s->velocity[2] - (float)w.ps.gravity * pm_frametime;
  vec3_t      move, end;
  VectorCopy(s->velocity, move);
  move[2] = (s->velocity[2] + end_vz) * .5f;
  VectorMA(s->origin, pm_frametime, move, end);

  trace_t tr;
  trap_CM_BoxTrace(&tr, s->origin, end, player_mins, w.maxs, 0, MASK_PLAYERSOLID);
  if (tr.fraction < 1.f)
  {
    VectorCopy(tr.endpos, s->origin);
    s->velocity[2] = end_vz;
    return tr.plane.normal[2] >= MIN_WALK_NORMAL ? MOVE_LANDED : MOVE_BLOCKED;
  }
  VectorCopy(end, s->origin);
  s->velocity[2] = end_vz;

  // PM_GroundTrace
  vec3_t point;
  VectorCopy(s->origin, point);
  point[2] -= .25f;
  trap_CM_BoxTrace(&tr, s->origin, point, player_mins, w.maxs, 0, MASK_PLAYERSOLID);
  if (
    tr.fraction < 1.f && tr.plane.normal[2] >= MIN_WALK_NORMAL &&
    !(s->velocity[2] > 0 && DotProduct(s->velocity, tr.plane.normal) > 10))
  {
    return MOVE_GROUND;
  }

  trap_SnapVector(s->velocity);
  return MOVE_AIR;
}

// States after the jump usercmd (index 0) and every usercmd after it, as if no rocket were fired.
static void simulate_pre(void)
{
  state_t s;
  VectorCopy(w.ps.origin, s.origin);
  VectorCopy(w.ps.velocity, s.velocity);
  s.velocity[2] = JUMP_VELOCITY; // PM_CheckJump
  w.num_pre     = 0;
  for (int32_t i = 0; i < RJ_MAX_PRE; ++i)
  {
    if (air_move(&s, i) != MOVE_AIR) break; // ceiling, wall or already back down: nothing usable beyond
    w.pre[w.num_pre++] = s;
  }
}

//
// rocket
//

// Follow a rocket fired from state `s` with `pitch`. Returns the server frame it explodes in (1 = the first
// frame after the fire usercmd was run) and the blast origin, or 0.
// The muzzle point is computed from the truncated origin (vanilla: CalcMuzzlePoint uses s.pos.trBase), which
// the server's blast points confirm; the exact-origin variant is only evaluated for the debug output.
static qboolean muzzle_from_exact_origin;

static int32_t rocket_impact(state_t const* s, float pitch, vec3_t blast)
{
  playerState_t fps = w.ps;
  VectorCopy(s->origin, fps.origin);
  VectorCopy(s->velocity, fps.velocity);
  fps.viewangles[PITCH] = pitch;

  gentity_t ent;
  memset(&ent, 0, sizeof(ent));
  BG_PlayerStateToEntityState(&fps, &ent.s, !muzzle_from_exact_origin);
  VectorCopy(ent.s.pos.trBase, ent.r.currentOrigin);
  gentity_t m;
  memset(&m, 0, sizeof(m));
  FireWeapon(&fps, &m, &ent);
  if (m.s.pos.trType != TR_LINEAR) return 0;

  vec3_t prev;
  VectorCopy(m.s.pos.trBase, prev);
  for (int32_t k = 1; k <= RJ_MAX_MISSILE; ++k)
  {
    // BG_EvaluateTrajectory at level.time: the missile was stamped level.time - 50 when it was fired.
    // Normally the usercmd is run between two server frames, so the first step comes a frame later; with
    // g_synchronousClients 1 the usercmd is run inside G_RunFrame, before the entity loop reaches the new
    // missile, which is therefore stepped in that same frame with only the prestep behind it.
    float const dt = (float)(RJ_PRESTEP_MSEC + (w.synchronous ? k - 1 : k) * w.server_msec) * .001f;
    vec3_t      pos;
    VectorMA(m.s.pos.trBase, dt, m.s.pos.trDelta, pos);
    trace_t tr;
    trap_CM_BoxTrace(&tr, prev, pos, NULL, NULL, 0, MASK_SHOT);
    if (tr.fraction < 1.f)
    {
      // SnapVectorTowards(trace->endpos, ent->s.pos.trBase)
      for (int32_t i = 0; i < 3; ++i)
        blast[i] = m.s.pos.trBase[i] <= tr.endpos[i] ? (float)(int32_t)tr.endpos[i]
                                                     : (float)((int32_t)tr.endpos[i] + 1);
      return k;
    }
    VectorCopy(pos, prev);
  }
  return 0;
}

// G_RadiusDamage + G_Damage's knockback on the player in state `s`. Returns the knockback points.
static int32_t apply_blast(state_t* s, vec3_t const blast)
{
  // Measured against the server (33 of 33 blasts in one log): the amount comes from the distance to the box
  // linked at the TRUNCATED origin - vanilla's "snapped origin for linking" - while the direction is taken
  // from the EXACT origin. Using the exact origin for both got the amount wrong whenever the truncation
  // moved the box across a whole-point boundary.
  vec3_t linked, v, dir;
  VectorCopy(s->origin, linked);
  SnapVector(linked);

  for (int32_t i = 0; i < 3; ++i)
  {
    float const absmin = linked[i] + player_mins[i] - 1;
    float const absmax = linked[i] + w.maxs[i] + 1;
    if (blast[i] < absmin)
      v[i] = absmin - blast[i];
    else if (blast[i] > absmax)
      v[i] = blast[i] - absmax;
    else
      v[i] = 0;
  }
  float const dist = VectorLength(v);
  if (dist >= RJ_SPLASH_RADIUS) return 0;

  int32_t const knockback = (int32_t)(RJ_SPLASH_DAMAGE * (1.f - dist / RJ_SPLASH_RADIUS));
  if (knockback <= 0) return 0;

  VectorSubtract(s->origin, blast, dir);
  dir[2] += 24;
  VectorNormalize(dir);
  VectorMA(s->velocity, RJ_KNOCKBACK_SCALE * (float)knockback, dir, s->velocity);
  return knockback;
}

//
// flight after the blast, cached: many candidates share the same explosion
//

typedef struct
{
  int32_t   explode_cmd;
  vec3_t    blast;
  int32_t   knockback;
  landing_t landing;
  int32_t   land_cmd;
  float     apex;
  float     land_speed;
} flight_t;

static flight_t flights[RJ_FLIGHT_CACHE];
static int32_t  num_flights;

// post[] (optional) receives the states after each usercmd following the explosion
static void fly(state_t s, int32_t explode_cmd, flight_t* f, state_t* post, int32_t* num_post)
{
  f->landing    = LAND_UNKNOWN;
  f->land_cmd   = 0;
  f->apex       = s.origin[2] - w.start_z;
  f->land_speed = 0;
  for (int32_t i = 1; i <= RJ_MAX_FLIGHT; ++i)
  {
    float const  vz = s.velocity[2];
    move_t const r  = air_move(&s, explode_cmd + i);
    if (post && num_post && *num_post < RJ_MAX_POST) post[(*num_post)++] = s;
    if (s.origin[2] - w.start_z > f->apex) f->apex = s.origin[2] - w.start_z;
    if (r == MOVE_AIR) continue;
    f->land_cmd   = explode_cmd + i;
    f->land_speed = -s.velocity[2];
    if (r == MOVE_GROUND && s.velocity[2] < -RJ_OB_MIN_SPEED)
      f->landing = LAND_OVERBOUNCE;
    else if (r == MOVE_LANDED && vz < 0)
      f->landing = LAND_NORMAL;
    return;
  }
}

static candidate_t evaluate(int32_t delay, int32_t nudge, float pitch)
{
  candidate_t c;
  memset(&c, 0, sizeof(c));
  c.delay = delay;
  c.nudge = nudge;
  if (delay >= w.num_pre) return c;

  int32_t const k = rocket_impact(&w.pre[delay], pitch, c.blast);
  if (!k) return c;

  // usercmds the server runs between the fire usercmd and the frame the missile explodes in
  int32_t extra = k * w.server_msec / RJ_CMD_MSEC - 1 + rj_explode_offset.integer;
  if (extra < 0) extra = 0;
  c.explode_cmd = delay + extra;
  if (c.explode_cmd >= w.num_pre) return c;

  for (int32_t i = 0; i < num_flights; ++i)
  {
    flight_t const* f = &flights[i];
    if (f->explode_cmd != c.explode_cmd || !VectorCompare(f->blast, c.blast)) continue;
    c.knockback  = f->knockback;
    c.landing    = f->landing;
    c.land_cmd   = f->land_cmd;
    c.apex       = f->apex;
    c.land_speed = f->land_speed;
    c.valid      = f->knockback > 0;
    return c;
  }

  flight_t f;
  memset(&f, 0, sizeof(f));
  f.explode_cmd = c.explode_cmd;
  VectorCopy(c.blast, f.blast);
  state_t s   = w.pre[c.explode_cmd];
  f.knockback = apply_blast(&s, c.blast);
  if (f.knockback > 0) fly(s, c.explode_cmd, &f, NULL, NULL);
  if (num_flights < RJ_FLIGHT_CACHE) flights[num_flights++] = f;

  c.knockback  = f.knockback;
  c.landing    = f.landing;
  c.land_cmd   = f.land_cmd;
  c.apex       = f.apex;
  c.land_speed = f.land_speed;
  c.valid      = f.knockback > 0;
  return c;
}

static int32_t delay_max(void)
{
  int32_t d = rj_delay_max.integer;
  if (d < 1) d = 1;
  if (d > RJ_MAX_DELAY) d = RJ_MAX_DELAY;
  return d;
}

// Snapshot the present and prepare the shared simulation.
static void capture(playerState_t const* ps, int32_t jump_hold)
{
  memset(&w, 0, sizeof(w));
  w.ps = *ps;
  trap_GetUserCmd(trap_GetCurrentCmdNumber(), &w.cmd);
  w.jump_hold = jump_hold;
  VectorSet(w.maxs, 15, 15, (ps->pm_flags & PMF_DUCKED) ? 16 : 32);
  w.start_z = ps->origin[2];

  // server frame length: known offline, assumed equal to the usercmd length online
  w.server_msec = RJ_CMD_MSEC;
  if (cvar_getInteger("sv_running"))
  {
    int32_t const fps = cvar_getInteger("sv_fps");
    if (fps > 0) w.server_msec = 1000 / fps;
  }
  w.synchronous = cvar_getInteger("g_synchronousClients") != 0;
  num_flights   = 0;
  simulate_pre();
}

// Best candidate over the given pitch offsets: the highest overbounce, else (ob == NULL result) nothing.
// *best_plain receives the highest jump at offset 0 regardless of the landing.
static candidate_t search(int32_t max_nudge, candidate_t* best_plain)
{
  candidate_t best;
  memset(&best, 0, sizeof(best));
  if (best_plain) memset(best_plain, 0, sizeof(*best_plain));

  float const unit = 360.f / 65536.f;
  // smallest offsets first, so that among equally high overbounces the least view change wins
  for (int32_t step = 0; step * RJ_NUDGE_STEP <= max_nudge; ++step)
  {
    for (int32_t sign = 1; sign >= -1; sign -= 2)
    {
      if (step == 0 && sign < 0) continue;
      int32_t const nudge = sign * step * RJ_NUDGE_STEP;
      float const   pitch = w.ps.viewangles[PITCH] + (float)nudge * unit;
      // the exact view is always a candidate; a nudge can't push the view past the engine's pitch clamp
      if (nudge != 0 && fabsf(pitch) > RJ_PITCH_LIMIT) continue;
      for (int32_t d = 1; d <= delay_max(); ++d)
      {
        candidate_t const c = evaluate(d, nudge, pitch);
        if (!c.valid) continue;
        if (best_plain && nudge == 0 && (!best_plain->valid || c.apex > best_plain->apex)) *best_plain = c;
        if (c.landing == LAND_OVERBOUNCE && (!best.valid || c.apex > best.apex + .01f)) best = c;
      }
    }
  }
  return best;
}

//
// execution
//

typedef enum
{
  RJ_IDLE,
  RJ_TURNING, // the pitch nudge has been sent, waiting for it to show in the usercmds
  RJ_ARMED,   // plan fixed, waiting for a frame whose usercmd opens a new pmove slot to jump in
  RJ_JUMPED,  // jump sent, waiting for the slot the shot belongs in
  RJ_ACTIVE,  // jump and shot are out
} rj_state_t;

static rj_state_t state;
static qboolean   was_held;
static int32_t    state_time;
static int32_t    attempts;
static qboolean   retry;

static int32_t  nudge_applied; // usercmd angle units currently added to the user's pitch
static char     saved_pitchspeed[MAX_CVAR_VALUE_STRING];
static char     saved_anglespeedkey[MAX_CVAR_VALUE_STRING];
static qboolean look_saved;

// what was predicted for the jump in progress, for the report
static candidate_t plan;
static state_t     plan_states[RJ_MAX_PRE + RJ_MAX_POST];
static int32_t     plan_num_states;
static state_t     plan_pre[RJ_MAX_PRE]; // the same jump without any blast, to spot when the blast really came
static int32_t     plan_num_pre;
static vec3_t      plan_blast_alt; // blast point if the muzzle were computed from the exact origin
static float       plan_pitch;     // view pitch the plan was evaluated at
static int32_t     plan_jump_hold;
static int32_t     plan_jump_time;  // slot (see slot_of) of the usercmd the jump happened in, 0 until seen
static int32_t     plan_fire_time;  // same for the shot
static int32_t     plan_go_cmd;     // usercmd number current when the jump was sent
static int32_t     plan_jump_frame; // frame counter when the jump was sent
static int32_t     frame_counter;
static qboolean    reported_fire, reported_blast, reported_landing, restore_done;

// diagnosis of the run in progress
static float     wanted_pitch; // view pitch the search asked for (after the nudge)
static usercmd_t track_ref;    // the jump usercmd: view and movement keys everything else is compared with
static qboolean  track_have_ref;
static int32_t   track_cmd;       // last usercmd number logged
static int32_t   track_snap_time; // commandTime of the last snapshot logged
static int32_t   diverged_at;     // first usercmd index at which the server left the predicted path, -1 = never
static qboolean  timing_off, blast_mismatch, view_changed, keys_changed;

// With pmove_fixed the server rounds every usercmd's time up to the pmove_msec grid and runs one pmove per
// grid slot: a usercmd falling into a slot that has already run is dropped whole, one that skips a slot runs
// two pmoves. Usercmd times step 7-9 ms, so "n usercmds later" is not "n pmoves later"; everything here is
// therefore counted in slots.
static int32_t slot_of(int32_t time)
{
  if (!cvar_getInteger("pmove_fixed")) return time;
  return (time + RJ_CMD_MSEC - 1) / RJ_CMD_MSEC * RJ_CMD_MSEC;
}

static int32_t latest_cmd_time(void)
{
  usercmd_t c;
  return trap_GetUserCmd(trap_GetCurrentCmdNumber(), &c) ? c.serverTime : 0;
}

static void turn_pitch(int32_t units)
{
  if (!units) return;
  if (!look_saved)
  {
    trap_Cvar_VariableStringBuffer("cl_pitchspeed", saved_pitchspeed, sizeof(saved_pitchspeed));
    trap_Cvar_VariableStringBuffer("cl_anglespeedkey", saved_anglespeedkey, sizeof(saved_anglespeedkey));
    look_saved = qtrue;
  }
  // CL_AdjustAngles: angle += 0.001 * frametime * speed * (msec / frame_msec); with a 1 ms key press the
  // frame times cancel and the turn is exactly 0.001 * speed degrees
  float const degrees = (float)abs(units) * (360.f / 65536.f);
  trap_Cvar_Set("cl_anglespeedkey", "1");
  trap_Cvar_Set("cl_pitchspeed", vaf("%.6f", degrees * 1000.f));
  pulse_key(units > 0 ? K_LOOKDOWN : K_LOOKUP, 1); // +lookdown increases pitch
  nudge_applied += units;
}

static void restore_look_cvars(void)
{
  if (!look_saved) return;
  trap_Cvar_Set("cl_pitchspeed", saved_pitchspeed);
  trap_Cvar_Set("cl_anglespeedkey", saved_anglespeedkey);
  look_saved = qfalse;
}

static void describe(char const* what, candidate_t const* c)
{
  out(vaf(
    "^3[rjob] %s: shot %i cmds after the jump, pitch %+.2f deg, blast %i pts after cmd %i, apex %.1f, %s at cmd %i "
    "(%.0f ups)\n",
    what,
    c->delay,
    (float)c->nudge * (360.f / 65536.f),
    c->knockback,
    c->explode_cmd,
    c->apex,
    c->landing == LAND_OVERBOUNCE ? "OVERBOUNCE"
    : c->landing == LAND_NORMAL   ? "normal landing"
                                  : "landing unknown",
    c->land_cmd,
    c->land_speed));
}

// Fix the plan for candidate `c` (already evaluated against the current view); the keys follow from the
// state machine, slot by slot.
static void set_plan(candidate_t const* c)
{
  plan            = *c;
  plan_pitch      = w.ps.viewangles[PITCH];
  plan_jump_hold  = w.jump_hold;
  plan_num_states = 0;
  plan_num_pre    = w.num_pre;
  memcpy(plan_pre, w.pre, sizeof(plan_pre));
  for (int32_t i = 0; i <= c->explode_cmd && i < w.num_pre; ++i) plan_states[plan_num_states++] = w.pre[i];
  {
    state_t  s = w.pre[c->explode_cmd];
    flight_t f;
    int32_t  n = 0;
    memset(&f, 0, sizeof(f));
    apply_blast(&s, c->blast);
    fly(s, c->explode_cmd, &f, plan_states + plan_num_states, &n);
    plan_num_states += n;
  }
  // where the blast would be under the other muzzle model, for the debug comparison
  muzzle_from_exact_origin = qtrue;
  VectorClear(plan_blast_alt);
  rocket_impact(&w.pre[c->delay], plan_pitch, plan_blast_alt);
  muzzle_from_exact_origin = qfalse;
}

static void go(candidate_t const* c)
{
  set_plan(c);
  plan_jump_time  = 0;
  plan_fire_time  = 0;
  track_have_ref  = qfalse;
  track_cmd       = trap_GetCurrentCmdNumber();
  track_snap_time = 0;
  diverged_at     = -1;
  timing_off = blast_mismatch = view_changed = keys_changed = qfalse;
  reported_fire                                             = qfalse;
  reported_blast                                            = qfalse;
  reported_landing                                          = qfalse;
  restore_done                                              = qfalse;
  state                                                     = RJ_ARMED;
  state_time                                                = cg.time;
}

// Jump hold in usercmds. Always longer than the longest delay, so the release can never fall on the usercmd
// the shot goes out in (the input layer would slide it by one and the simulated hold would be wrong), and
// the shot is always pressed while jump is still down, which lets the input layer stagger the two releases.
static int32_t roll_jump_hold(void)
{
  int32_t const dt = input_dt();
  return delay_max() + (input_stagger() + dt - 1) / dt;
}

static qboolean ready(playerState_t const* ps, qboolean say)
{
  char const* why = NULL;
  if (ps->groundEntityNum == ENTITYNUM_NONE)
    why = "not on the ground";
  else if (ps->weapon != WP_ROCKET_LAUNCHER)
    why = "the rocket launcher isn't the current weapon";
  else if (ps->weaponTime > 0)
    why = "the weapon isn't ready";
  else if (ps->weaponstate != WEAPON_READY && ps->weaponstate != WEAPON_FIRING)
    why = "the weapon is being switched";
  if (why && say) tell(vaf("^3[rjob] nothing done: %s\n", why));
  return why == NULL;
}

static void start(playerState_t const* ps)
{
  int32_t const max_nudge = (int32_t)(rj_nudge.value * (65536.f / 360.f));

  capture(ps, roll_jump_hold());
  log_begin();
  if (cvar_getInteger("com_maxfps") != 125)
    tell(vaf(
      "^3[rjob] ^1com_maxfps is %i^3: with anything but 125 the server sometimes runs two of your usercmds before the "
      "rocket explodes, and the blast then comes one usercmd late\n",
      cvar_getInteger("com_maxfps")));
  log_write(vaf(
    "\n==== use at cg.time %i, map %s ====\n  origin %.3f %.3f %.3f  velocity %.2f %.2f %.2f  view %.4f %.4f  delta_angles %i %i\n"
    "  weapon %i weaponTime %i ground %i gravity %i speed %i pm_flags %i  jump hold %i cmds\n"
    "  cvars: nudge %s delay_max %i explode_offset %i  pmove_fixed %i pmove_msec %i g_synchronousClients %i sv_fps %i "
    "com_maxfps %i\n",
    cg.time,
    info_value(CG_ConfigString(0), "mapname"),
    ps->origin[0],
    ps->origin[1],
    ps->origin[2],
    ps->velocity[0],
    ps->velocity[1],
    ps->velocity[2],
    ps->viewangles[PITCH],
    ps->viewangles[YAW],
    ps->delta_angles[PITCH],
    ps->delta_angles[YAW],
    ps->weapon,
    ps->weaponTime,
    ps->groundEntityNum,
    ps->gravity,
    ps->speed,
    ps->pm_flags,
    w.jump_hold,
    rj_nudge.string,
    delay_max(),
    rj_explode_offset.integer,
    cvar_getInteger("pmove_fixed"),
    cvar_getInteger("pmove_msec"),
    cvar_getInteger("g_synchronousClients"),
    cvar_getInteger("sv_fps"),
    cvar_getInteger("com_maxfps")));
  int32_t const t0 = trap_Milliseconds();
  candidate_t   plain;
  candidate_t   best = search(max_nudge > 0 ? max_nudge : 0, &plain);
  int32_t const t1   = trap_Milliseconds();

  if (rj_debug.integer)
  {
    out(vaf(
      "^3[rjob] searched %i flights in %i ms (server frame %i ms, pmove_fixed %i, g_synchronousClients %i, holding %i %i)\n",
      num_flights,
      t1 - t0,
      w.server_msec,
      cvar_getInteger("pmove_fixed"),
      w.synchronous,
      w.cmd.forwardmove,
      w.cmd.rightmove));
    if (plain.valid) describe("best plain jump at this view", &plain);
  }

  if (log_open)
  {
    for (int32_t d = 1; d <= delay_max(); ++d)
    {
      candidate_t const c = evaluate(d, 0, ps->viewangles[PITCH]);
      log_write(vaf(
        "  exact view, delay %i: valid %i blast %.0f %.0f %.0f pts %i after cmd %i apex %.2f landing %i at cmd %i (%.0f ups)\n",
        d,
        c.valid,
        c.blast[0],
        c.blast[1],
        c.blast[2],
        c.knockback,
        c.explode_cmd,
        c.apex,
        c.landing,
        c.land_cmd,
        c.land_speed));
    }
  }

  if (!best.valid)
  {
    if (!plain.valid)
    {
      tell("^3[rjob] nothing done: no rocket jump possible from here (rocket wouldn't land within the blast radius)\n");
      return;
    }
    tell(vaf(
      "^3[rjob] no overbounce reachable from here (delays 1..%i, pitch +-%.2f deg): defaulting to the best rocket "
      "jump possible\n",
      delay_max(),
      rj_nudge.value));
    best = plain;
  }
  else if (rj_debug.integer)
  {
    describe("chosen", &best);
    if (plain.valid)
      out(vaf("^3[rjob] apex against the best plain jump at the exact view: %+.1f\n", best.apex - plain.apex));
  }

  wanted_pitch = ps->viewangles[PITCH] + (float)best.nudge * (360.f / 65536.f);
  if (best.nudge)
  {
    turn_pitch(best.nudge);
    plan       = best;
    state      = RJ_TURNING;
    state_time = cg.time;
    return;
  }
  go(&best);
}

// After the nudge: re-evaluate at the view the usercmds really carry now, and go.
static void after_turn(playerState_t const* ps)
{
  if (cg.time - state_time < 2 * input_dt()) return; // let the pulse reach a usercmd and the prediction

  if (!ready(ps, qtrue))
  {
    state = RJ_IDLE;
    return;
  }

  capture(ps, roll_jump_hold());
  out(vaf(
    "^3[rjob] view after the nudge: pitch %.4f, wanted %.4f%s\n",
    ps->viewangles[PITCH],
    wanted_pitch,
    fabsf(ps->viewangles[PITCH] - wanted_pitch) < .003f ? "" : "  ^1OFF"));

  // the view is where the nudge put it: only the delay is searched now
  candidate_t plain;
  candidate_t best = search(0, &plain);
  if (best.valid && best.landing == LAND_OVERBOUNCE)
  {
    best.nudge = nudge_applied;
    if (rj_debug.integer) describe("after the nudge", &best);
    best.nudge = 0;
    go(&best);
    plan.nudge = nudge_applied;
    return;
  }

  // the turn didn't land where it was planned (mouse moved, angle rounding): undo it and search again
  if (rj_debug.integer) out("^3[rjob] the nudged view no longer gives an overbounce, searching again\n");
  turn_pitch(-nudge_applied);
  if (++attempts >= 3)
  {
    if (plain.valid)
    {
      tell("^3[rjob] no overbounce after 3 tries: defaulting to the best rocket jump possible\n");
      // plain was evaluated with the nudge still in the view; it is good enough for a plain jump
      go(&plain);
      restore_done = qtrue; // already turned back above
    }
    else
    {
      state = RJ_IDLE;
    }
    return;
  }
  state      = RJ_IDLE;
  retry      = qtrue; // search again next frame, from the restored view
  state_time = cg.time;
}

// Usercmd index (jump = 0) of a server command time.
static int32_t index_of(int32_t command_time)
{
  int32_t const d = slot_of(command_time) - plan_jump_time;
  if (d < -RJ_CMD_MSEC / 2) return -1; // before the jump (integer division would fold this onto 0)
  return (d + RJ_CMD_MSEC / 2) / RJ_CMD_MSEC;
}

// The slot in which the first usercmd after `from` carrying the jump (or the attack button) takes effect; 0 if
// there is none yet. A usercmd that isn't dropped runs every slot after the previous one up to its own, and
// a jump or a shot happens in the first of them.
static int32_t find_cmd_slot(int32_t from, qboolean attack)
{
  int32_t const latest = trap_GetCurrentCmdNumber();
  usercmd_t     prev;
  if (!trap_GetUserCmd(from, &prev)) return 0;
  for (int32_t n = from + 1; n <= latest; ++n)
  {
    usercmd_t c;
    if (!trap_GetUserCmd(n, &c)) return 0;
    qboolean const set     = attack ? (c.buttons & BUTTON_ATTACK) != 0 : c.upmove > 0;
    qboolean const dropped = slot_of(c.serverTime) <= slot_of(prev.serverTime);
    if (set && !dropped) return cvar_getInteger("pmove_fixed") ? slot_of(prev.serverTime) + RJ_CMD_MSEC : c.serverTime;
    if (!dropped) prev = c;
  }
  return 0;
}

// Every frame of a run: log each new usercmd and each new server playerstate next to the prediction, and
// notice the first usercmd at which the two part ways or the player's own input changes.
static void track(void)
{
  if (!rj_debug.integer) return;

  int32_t const latest = trap_GetCurrentCmdNumber();
  for (int32_t n = track_cmd + 1; n <= latest; ++n)
  {
    usercmd_t c;
    if (!trap_GetUserCmd(n, &c)) continue;
    log_write(vaf(
      "  cmd %i time %i slot %i move %i %i %i buttons %i angles %i %i\n",
      n,
      c.serverTime,
      slot_of(c.serverTime),
      c.forwardmove,
      c.rightmove,
      c.upmove,
      c.buttons,
      c.angles[PITCH],
      c.angles[YAW]));
    if (n <= plan_go_cmd) continue;
    if (!track_have_ref)
    {
      track_ref      = c;
      track_have_ref = qtrue;
      continue;
    }
    // the view matters until the shot is out, the movement keys for the whole flight
    qboolean const before_shot = !plan_fire_time || slot_of(c.serverTime) <= plan_fire_time;
    if (
      !view_changed && before_shot &&
      (c.angles[PITCH] != track_ref.angles[PITCH] || c.angles[YAW] != track_ref.angles[YAW]))
    {
      view_changed = qtrue;
      out(vaf(
        "^3[rjob] ^1the view moved between the jump and the shot^3: pitch %+.3f yaw %+.3f deg\n",
        (float)(int16_t)(c.angles[PITCH] - track_ref.angles[PITCH]) * (360.f / 65536.f),
        (float)(int16_t)(c.angles[YAW] - track_ref.angles[YAW]) * (360.f / 65536.f)));
    }
    if (!keys_changed && (c.forwardmove != w.cmd.forwardmove || c.rightmove != w.cmd.rightmove))
    {
      keys_changed = qtrue;
      out(vaf(
        "^3[rjob] ^1movement keys changed during the jump^3: now %i %i, planned with %i %i\n",
        c.forwardmove,
        c.rightmove,
        w.cmd.forwardmove,
        w.cmd.rightmove));
    }
  }
  track_cmd = latest;

  if (!plan_jump_time) return;
  playerState_t const* const sp = &getSnap()->ps;
  if (sp->commandTime == track_snap_time) return;
  track_snap_time   = sp->commandTime;
  int32_t const idx = index_of(sp->commandTime);
  if (idx < 0 || idx >= plan_num_states)
  {
    log_write(vaf(
      "  snap time %i cmd %i: server origin %.3f %.3f %.3f vel %.2f %.2f %.2f ground %i (no prediction)\n",
      sp->commandTime,
      idx,
      sp->origin[0],
      sp->origin[1],
      sp->origin[2],
      sp->velocity[0],
      sp->velocity[1],
      sp->velocity[2],
      sp->groundEntityNum));
    return;
  }
  state_t const* const p    = &plan_states[idx];
  float const          dorg = Distance(p->origin, sp->origin);
  log_write(vaf(
    "  snap time %i cmd %i: server origin %.3f %.3f %.3f vel %.2f %.2f %.2f ground %i | predicted origin %.3f %.3f %.3f "
    "vel %.2f %.2f %.2f | off by %.4f\n",
    sp->commandTime,
    idx,
    sp->origin[0],
    sp->origin[1],
    sp->origin[2],
    sp->velocity[0],
    sp->velocity[1],
    sp->velocity[2],
    sp->groundEntityNum,
    p->origin[0],
    p->origin[1],
    p->origin[2],
    p->velocity[0],
    p->velocity[1],
    p->velocity[2],
    dorg));
  if (diverged_at < 0 && dorg > .02f && idx < plan.land_cmd)
  {
    diverged_at = idx;
    out(vaf(
      "^3[rjob] ^1left the predicted path at cmd %i^3 (%s the blast): predicted origin %.3f %.3f %.3f vel %.1f %.1f %.1f, server "
      "origin %.3f %.3f %.3f vel %.1f %.1f %.1f\n",
      idx,
      idx <= plan.explode_cmd ? "before" : "after",
      p->origin[0],
      p->origin[1],
      p->origin[2],
      p->velocity[0],
      p->velocity[1],
      p->velocity[2],
      sp->origin[0],
      sp->origin[1],
      sp->origin[2],
      sp->velocity[0],
      sp->velocity[1],
      sp->velocity[2]));
  }
}

static void report(void)
{
  if (!plan_jump_time) return;

  if (!plan_fire_time)
  {
    plan_fire_time = find_cmd_slot(plan_go_cmd, qtrue);
    if (plan_fire_time && rj_debug.integer)
    {
      int32_t const d = (plan_fire_time - plan_jump_time) / RJ_CMD_MSEC;
      out(vaf(
        "^3[rjob] shot went out %i pmoves after the jump (planned %i)%s\n",
        d,
        plan.delay,
        d == plan.delay ? "" : "  ^1TIMING OFF"));
    }
    if (plan_fire_time && (plan_fire_time - plan_jump_time) / RJ_CMD_MSEC != plan.delay) timing_off = qtrue;
  }

  // undo the nudge once the shot is out
  if (!restore_done && plan_fire_time && frame_counter - plan_jump_frame > plan.delay + 3)
  {
    turn_pitch(-nudge_applied);
    restore_done = qtrue;
  }

  if (!rj_debug.integer) return;

  // the server's own word: the latest snapshot's playerstate, at its commandTime
  playerState_t const* const sp  = &getSnap()->ps;
  int32_t const              idx = index_of(sp->commandTime);
  if (idx < 0 || idx >= plan_num_states) return;
  state_t const* const p = &plan_states[idx];

  if (
    !reported_fire && idx <= plan.explode_cmd && idx < plan_num_pre &&
    sp->velocity[2] < plan_pre[idx].velocity[2] + 100)
  {
    reported_fire = qtrue;
    out(vaf(
      "^3[rjob] before the blast, cmd %i: predicted origin %.3f %.3f %.3f vz %.1f, server origin %.3f %.3f %.3f vz %.1f%s\n",
      idx,
      p->origin[0],
      p->origin[1],
      p->origin[2],
      p->velocity[2],
      sp->origin[0],
      sp->origin[1],
      sp->origin[2],
      sp->velocity[2],
      Distance(p->origin, sp->origin) < .01f ? "  ^2MATCH" : "  ^1MISMATCH"));
  }

  // the first snapshot in which the velocity is far above the blast-free jump is the usercmd the blast followed
  if (!reported_blast && idx < plan_num_pre && sp->velocity[2] > plan_pre[idx].velocity[2] + 100)
  {
    reported_blast = qtrue;
    if (idx != plan.explode_cmd) timing_off = qtrue;
    vec3_t dv;
    VectorSubtract(sp->velocity, plan_pre[idx].velocity, dv);
    float const mag = VectorLength(dv);
    // undo G_Damage: dv = normalize(origin + (0 0 24) - blast) * 5 * points; the blast height is the one thing
    // taken from the plan
    float const t = dv[2] != 0 ? (sp->origin[2] + 24 - plan.blast[2]) / dv[2] : 0;
    out(vaf(
      "^3[rjob] blast seen after cmd %i (planned %i)%s: server origin %.3f %.3f %.3f, velocity change %.2f %.2f %.2f = "
      "%.1f pts (planned %i)\n",
      idx,
      plan.explode_cmd,
      idx == plan.explode_cmd ? "" : "  ^1TIMING OFF^3",
      sp->origin[0],
      sp->origin[1],
      sp->origin[2],
      dv[0],
      dv[1],
      dv[2],
      mag / RJ_KNOCKBACK_SCALE,
      plan.knockback));
    out(vaf(
      "^3[rjob] blast point: server %.2f %.2f %.0f (worked back from the velocity change), planned %.0f %.0f %.0f, with "
      "the muzzle from the exact origin %.0f %.0f %.0f\n",
      sp->origin[0] - dv[0] * t,
      sp->origin[1] - dv[1] * t,
      plan.blast[2],
      plan.blast[0],
      plan.blast[1],
      plan.blast[2],
      plan_blast_alt[0],
      plan_blast_alt[1],
      plan_blast_alt[2]));
    if (idx == plan.explode_cmd)
    {
      state_t after = plan_pre[idx];
      apply_blast(&after, plan.blast);
      if (Distance(after.velocity, sp->velocity) >= .05f) blast_mismatch = qtrue;
      out(vaf(
        "^3[rjob] velocity after the blast: predicted %.2f %.2f %.2f, server %.2f %.2f %.2f%s\n",
        after.velocity[0],
        after.velocity[1],
        after.velocity[2],
        sp->velocity[0],
        sp->velocity[1],
        sp->velocity[2],
        Distance(after.velocity, sp->velocity) < .05f ? "  ^2MATCH" : "  ^1MISMATCH"));
    }
  }
}

void init_rjob(void)
{
  init_cvars(rj_cvars, ARRAY_LEN(rj_cvars));
  register_button("mdd_rocketjump_overbounce", &held);
  state = RJ_IDLE;
}

void run_rjob(void)
{
  update_cvars(rj_cvars, ARRAY_LEN(rj_cvars));
  ++frame_counter;

  if (cg.demoPlayback) return;
  playerState_t const* const ps = getPs();
  if (ps->clientNum != cg.clientNum || ps->pm_type != PM_NORMAL || (ps->pm_flags & PMF_FOLLOW))
  {
    was_held = held;
    return;
  }

  qboolean const pressed = held && !was_held;
  was_held               = held;

  switch (state)
  {
  case RJ_IDLE:
    if (pressed) attempts = 0;
    if (pressed || (retry && cg.time - state_time >= 2 * input_dt()))
    {
      retry = qfalse;
      if (ready(ps, qtrue)) start(ps);
    }
    else if (!retry && nudge_applied == 0)
    {
      restore_look_cvars();
      log_end(); // a use that ended without a jump (nothing done, aborted)
    }
    break;

  case RJ_TURNING:
    after_turn(ps);
    break;

  case RJ_ARMED:
  {
    // Jump in a frame whose usercmd (it will carry this frame's cg.time) opens exactly the next pmove slot:
    // not one that would be dropped, and not one that runs two pmoves and so swallows the shot's slot.
    int32_t const prev = slot_of(latest_cmd_time());
    if (cvar_getInteger("pmove_fixed") && slot_of(cg.time) != prev + RJ_CMD_MSEC)
    {
      if (cg.time - state_time < 300) break;
      // never lined up (frame rate not 125?): go anyway
    }
    if (!ready(ps, qtrue))
    {
      turn_pitch(-nudge_applied);
      state = RJ_IDLE;
      break;
    }
    // re-evaluate from the state the jump really starts in, so the report compares like with like
    {
      int32_t const   nudge = plan.nudge;
      landing_t const was   = plan.landing;
      capture(ps, plan_jump_hold);
      candidate_t c = evaluate(plan.delay, 0, ps->viewangles[PITCH]);
      if (c.valid)
      {
        c.nudge = nudge;
        set_plan(&c);
        if (rj_debug.integer && was == LAND_OVERBOUNCE && c.landing != LAND_OVERBOUNCE)
          out("^3[rjob] the state changed since the search: this jump is no longer predicted to overbounce\n");
      }
    }
    tap_key_at(K_MOVEUP, 0, (plan_jump_hold - 1) * input_dt() + 1);
    plan_go_cmd     = trap_GetCurrentCmdNumber();
    plan_jump_frame = frame_counter;
    state           = RJ_JUMPED;
    state_time      = cg.time;
    break;
  }

  case RJ_JUMPED:
  {
    if (!plan_jump_time) plan_jump_time = find_cmd_slot(plan_go_cmd, qfalse);
    track();
    if (!plan_jump_time)
    {
      if (cg.time - state_time > 500)
      {
        // the jump never showed up in a usercmd (keys blocked, console open, ...): give up cleanly
        tell("^3[rjob] aborted: the jump was never sent\n");
        turn_pitch(-nudge_applied);
        state = RJ_IDLE;
      }
      break;
    }
    // The shot belongs in slot jump + delay. The usercmd made from this frame's input runs the slots after
    // the latest usercmd's up to its own, and fires in the first of them.
    int32_t const target = plan_jump_time + plan.delay * RJ_CMD_MSEC;
    qboolean      fire;
    if (cvar_getInteger("pmove_fixed"))
    {
      int32_t const first = slot_of(latest_cmd_time()) + RJ_CMD_MSEC;
      fire                = slot_of(cg.time) >= first && first >= target; // not dropped, and the slot is due
    }
    else
    {
      fire = cg.time >= target - RJ_CMD_MSEC / 2;
    }
    if (fire)
    {
      tap_key(K_ATTACK);
      state = RJ_ACTIVE;
    }
    break;
  }

  case RJ_ACTIVE:
    track();
    report();
    if (plan_jump_time)
    {
      // landing: back on the ground after the blast
      // A landing shows as the fall ending: grounded, or the downward speed gone. An overbounce keeps the
      // speed (redirected along the ground or straight back up); a normal landing loses the vertical part.
      playerState_t const* const sp    = &getSnap()->ps;
      int32_t const              idx   = index_of(sp->commandTime);
      float const                speed = VectorLength(sp->velocity);
      static float               fall_speed, fall_vz;
      if (idx <= plan.explode_cmd + 4) fall_speed = fall_vz = 0;
      if (
        !reported_landing && idx > plan.explode_cmd + 4 && fall_vz < -RJ_OB_MIN_SPEED &&
        (sp->groundEntityNum != ENTITYNUM_NONE || sp->velocity[2] > fall_vz + 50))
      {
        reported_landing        = qtrue;
        qboolean const kept     = speed > .9f * fall_speed;
        qboolean const grounded = sp->groundEntityNum != ENTITYNUM_NONE && sp->velocity[2] < -RJ_OB_MIN_SPEED;
        if (rj_debug.integer)
          out(vaf(
            "^3[rjob] landed by cmd %i: fell at %.0f ups (vz %.0f), now %.0f ups (vz %.0f) -> %s (predicted %s at cmd %i)\n",
            idx,
            fall_speed,
            fall_vz,
            speed,
            sp->velocity[2],
            kept || grounded ? "^2OVERBOUNCE^3" : "no overbounce",
            plan.landing == LAND_OVERBOUNCE ? "overbounce" : "a normal landing",
            plan.land_cmd));
        // one line that says what happened and, if it failed, at which stage
        {
          qboolean const ob = kept || grounded;
          char           why[256];
          why[0] = 0;
          if (plan.landing != LAND_OVERBOUNCE) append(why, sizeof(why), " no overbounce was predicted (plain jump);");
          if (timing_off) append(why, sizeof(why), " shot or blast a usercmd off;");
          if (view_changed) append(why, sizeof(why), " view moved before the shot;");
          if (blast_mismatch) append(why, sizeof(why), " blast velocity differed from the prediction;");
          if (keys_changed) append(why, sizeof(why), " movement keys changed;");
          if (diverged_at >= 0) append(why, sizeof(why), vaf(" path left the prediction at cmd %i;", diverged_at));
          if (!why[0] && !ob)
            append(why, sizeof(why), " everything matched until the landing itself (landing model) - see rjob.log;");
          out(vaf("^3[rjob] RESULT: %s%s%s\n", ob ? "^2OVERBOUNCE^3" : "^1no overbounce^3", why[0] ? " -" : "", why));
        }
      }
      else if (sp->groundEntityNum == ENTITYNUM_NONE && sp->velocity[2] < 0)
      {
        fall_speed = speed;
        fall_vz    = sp->velocity[2];
      }
    }
    if ((reported_landing || cg.time - state_time > 20000) && restore_done && nudge_applied == 0)
    {
      restore_look_cvars();
      if (!reported_landing) out("^3[rjob] RESULT: ^1no landing seen within 20 s^3\n");
      log_end();
      state    = RJ_IDLE;
      attempts = 0;
    }
    else if (cg.time - state_time > 20000)
    {
      turn_pitch(-nudge_applied);
      restore_done = qtrue;
    }
    break;
  }
}
