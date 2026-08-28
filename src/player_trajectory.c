/*
  Draws the player's predicted future trajectory as a series of ghost hitboxes. Three models,
  combinable as mdd_player_trajectory_strafe_modes bits, each simulated as its own independent
  pass from the live player state:
   - TRAJECTORY_PASSIVE: "if I released every movement key right now and just kept jumping 
     whenever possible (no input except jump key), where would I end up".
   - TRAJECTORY_STRAFEBOT: "where would a strafebot end up if it kept holding whatever
     movement key(s) I'm holding right now, and kept aiming at the green edge of cgaz hud". 
     -- The angle "phi" in this code refers to the green edge of the cgaz hud accel zone,
       the name phi as it was proposed in injx's "strafing theory" articles.
     -- In similar contexts angle "psi" is the yellow edge of accel zone, perpendicular 
       to the velocity vector.
     -- No input defaults to passive trajectory.
     -- Any backward key activation is interpreted as braking, defaults to passive trajectory.
     -- Forward-only key activation in vq3 defaults to passive trajectory.
     -- Forward-only key activation in cpm shows the aircontrol trajectory - if you hold the
       forward key and keep the mouse at the current yaw angle, what the trajectory will be.
     -- Full-beat strafe modes are the same in vq3 / cpm.
     -- Half-beat strafe modes are different in vq3 / cpm (strafe turn), and both are supported.
     -- Basically impossible for humans at any meaningful trajectory lengths, but it shows the
       ideal trajectory for a perfect strafebot. Optimistic/motivating.
   - TRAJECTORY_ACCEL: "if I keep aiming this same RELATIVE spot in the accel zone" - measures
     where the player's CURRENT aim sits between the green and yellow edges of the cgaz hud.
     -- Almost the same as TRAJECTORY_STRAFEBOT, but it shows the trajectory for the player's 
       current aim percentage within the cgaz hud, instead of the green edge of the cgaz hud.
     -- Cpm HB cgaz hud is too narrow for a meaningful aiming percentage, so this strafe mode
       is the same as in TRAJECTORY_STRAFEBOT - implies aiming at the green cgaz hud edge.
     -- Less optimistic, more realistic than TRAJECTORY_STRAFEBOT.
  In all cases, the player is assumed to make perfect 1-frame jumps whenever possible, and so
  the upmove strafe-reducing factor is almost ignored. It is upmove-optimistic, especially when
  running up stairs.

  Cpm notes for the strafebot model. Double jumps, ramp jumps, and cpm stair jumps are 
  supported. PM_CheckJump models CPM double jump/ramp jump: velocity[2] stacks instead of
  overwriting when already positive, plus a flat +100ups if STAT_JUMPTIME (a 400ms window, see
  PM_DropTimers) is still running from a previous PROMODE jump.

  PM_SlideMove/PM_StepSlideMove/PM_ClipVelocity are ported from Quake 3's bg_slidemove.c
  
  Entity collision tracing is also supported, or can be switched off with CVar 
  mdd_player_trajectory_trace_entities. Traces every SOLID_BMODEL entity in the client's own 
  snapshot (doors, plats, trains, buttons), gathered once per recompute and merged into every 
  trace via CG_TrajectoryTrace (ported from stock cg_predict.c). PVS-bounded only, no 
  distance cull - see s_trajEntities. Other players are never traced (SOLID_BBOX, not 
  SOLID_BMODEL) - deliberate.

  Triggers collision tracing is also suported, or can be switched off with CVar 
  mdd_player_trajectory_trace_triggers. Jump pads and DeFRaG's velocity-pad variant (both 
  ET_PUSH_TRIGGER) apply their real push mid-simulation, ported from stock BG_TouchJumpPad 
  and DF_Reverse_Stuff's BG_TouchVelocityJumpPad. Teleporters (ET_TELEPORT_TRIGGER) highlight 
  the touched frame and stop simulation - the destination is resolved server-side only 
  (G_PickTarget), never networked to the client, so continuing past one would mean guessing.
  
  No water physics - simulation stops early on water.
  Moving map objects are not supported and can break the predicted trajectory.
  Other players are ignored.
  Predicting more frames or more strafe modes can negatively impact FPS proportionally.
*/
#include "player_trajectory.h"

#include "bbox.h"
#include "bg_pmove.h"
#include "cg_cvar.h"
#include "cg_local.h"
#include "cg_utils.h"
#include "help.h"

#include <string.h>

#define TRAJECTORY_MAX_FRAMES 2000 // sanity cap regardless of what's typed into the frames CVar
#define TRAJECTORY_MAX_ENTITIES 32 // sanity cap on SOLID_BMODEL movers gathered per recompute

// mdd_player_trajectory_strafe_modes bits: which physics model(s) to draw, combinable, no
// "master enable" bit needed (0 = nothing selected = off, matching mdd_cgaz_trueness's own
// bitmask convention).
#define TRAJECTORY_PASSIVE   1
#define TRAJECTORY_ACCEL     2 // matches the player's live accel-zone aim percentage
#define TRAJECTORY_STRAFEBOT 4

// mdd_player_trajectory_display_type bits: how each drawn sample is rendered, combinable.
#define TRAJECTORY_TYPE_BOXES 1
#define TRAJECTORY_TYPE_BOTTOMS 2
// Forces jump/teleport-enter frames to draw as a full bbox, overriding BOXES/BOTTOMS - otherwise
// they're just another flat square in bottoms-only mode, easy to miss.
#define TRAJECTORY_TYPE_JUMP_BOXES 4
#define TRAJECTORY_TYPE_LINE 8
// Samples at a world-pinned phase instead of always step/2*step/3*step... - see the phase-shift
// comment above CG_DrawPlayerTrajectory's dispatch section. Only affects BOXES/BOTTOMS; LINE
// always draws every simulated frame regardless of step or this bit.
#define TRAJECTORY_TYPE_STATIC 16

static vmCvar_t trajectory_strafe_modes;
static vmCvar_t trajectory_display_type;
static vmCvar_t trajectory_frames;
static vmCvar_t trajectory_step;
static vmCvar_t trajectory_rgba_passive;
static vmCvar_t trajectory_rgba_jump;
static vmCvar_t trajectory_rgba_strafebot;
static vmCvar_t trajectory_rgba_accel;
static vmCvar_t trajectory_trace_entities;
static vmCvar_t trajectory_trace_triggers;
static vmCvar_t trajectory_rgba_trigger;
static vmCvar_t trajectory_shader_line;
static vmCvar_t trajectory_shader_box;
static vmCvar_t trajectory_debug;

static cvarTable_t trajectory_cvars[] = {
  { &trajectory_strafe_modes, "mdd_player_trajectory_strafe_modes", "0b010", CVAR_ARCHIVE_ND },
  { &trajectory_display_type, "mdd_player_trajectory_display_type", "0b11110", CVAR_ARCHIVE_ND },
  { &trajectory_frames, "mdd_player_trajectory_frames", "270", CVAR_ARCHIVE_ND },
  { &trajectory_step, "mdd_player_trajectory_step", "10", CVAR_ARCHIVE_ND },
  { &trajectory_rgba_passive, "mdd_player_trajectory_rgba_passive", ".5 .5 .5 .5", CVAR_ARCHIVE_ND },
  { &trajectory_rgba_jump, "mdd_player_trajectory_rgba_jump", ".8 0 0 .8", CVAR_ARCHIVE_ND },
  { &trajectory_rgba_strafebot, "mdd_player_trajectory_rgba_strafebot", "0 .5 0 .5", CVAR_ARCHIVE_ND },
  { &trajectory_debug, "mdd_player_trajectory_debug", "0", 0 },
  { &trajectory_rgba_accel, "mdd_player_trajectory_rgba_accel", ".4 .4 0 .4", CVAR_ARCHIVE_ND },
  { &trajectory_trace_entities, "mdd_player_trajectory_trace_entities", "1", CVAR_ARCHIVE_ND },
  { &trajectory_trace_triggers, "mdd_player_trajectory_trace_triggers", "1", CVAR_ARCHIVE_ND },
  { &trajectory_rgba_trigger, "mdd_player_trajectory_rgba_trigger", "0 .5 1 .5", CVAR_ARCHIVE_ND },
  { &trajectory_shader_line, "mdd_player_trajectory_shader_line", "railCore", CVAR_ARCHIVE_ND },
  { &trajectory_shader_box, "mdd_player_trajectory_shader_box", "bbox_nocull", CVAR_ARCHIVE_ND },
};

// Listed alphabetically by cvar name, not by cvarTable index - each entry's trajectory_cvars+N
// pointer still targets the right cvar regardless of listing order.
static help_t trajectory_help[] = {
  {
    trajectory_cvars + 1,
    BINARY_LITERAL,
    {
      "mdd_player_trajectory_display_type 0bXXXXX",
      "                                     |||||",
      "                                     ||||+- full bounding boxes",
      "                                     |||+-- bottom squares only",
      "                                     ||+--- jump/teleport frames always bboxes",
      "                                     |+---- connecting line",
      "                                     +----- static (world-pinned) bboxes/bottoms instead of flowing",
    },
  },
  {
    trajectory_cvars + 2,
    COUNT,
    {
      "mdd_player_trajectory_frames X",
      "  how many simulated frames ahead to predict (capped at 2000)",
    },
  },
  {
    trajectory_cvars + 8,
    RGBA,
    {
      "mdd_player_trajectory_rgba_accel X X X X",
    },
  },
  {
    trajectory_cvars + 5,
    RGBA,
    {
      "mdd_player_trajectory_rgba_jump X X X X",
    },
  },
  {
    trajectory_cvars + 4,
    RGBA,
    {
      "mdd_player_trajectory_rgba_passive X X X X",
    },
  },
  {
    trajectory_cvars + 6,
    RGBA,
    {
      "mdd_player_trajectory_rgba_strafebot X X X X",
    },
  },
  {
    trajectory_cvars + 11,
    RGBA,
    {
      "mdd_player_trajectory_rgba_trigger X X X X",
    },
  },
  {
    trajectory_cvars + 3,
    COUNT,
    {
      "mdd_player_trajectory_step X",
      "  draw a sample every X simulated frames - jump frames are always drawn regardless of",
      "  this, and mdd_player_trajectory_display_type's static bit changes WHICH indices count",
      "  as \"on step\", not this spacing itself",
    },
  },
  {
    trajectory_cvars + 0,
    BINARY_LITERAL,
    {
      "mdd_player_trajectory_strafe_modes 0bXXX",
      "                                     |||",
      "                                     ||+- passive (assumes every key released)",
      "                                     |+-- accel-percent (holds your current phi-to-psi %)",
      "                                     +--- strafebot (perfect phi-following, curves toward your aim)",
    },
  },
  {
    trajectory_cvars + 9,
    COUNT,
    {
      "mdd_player_trajectory_trace_entities 0 or 1",
      "  also trace against nearby door/plat/train/button-type entities, not just world geometry -",
      "  costs more on entity-dense maps (see docs/mdd_player_trajectory.md), disable per-map if needed",
    },
  },
  {
    trajectory_cvars + 10,
    COUNT,
    {
      "mdd_player_trajectory_trace_triggers 0 or 1",
      "  react to jump pads/velocity pads (apply their velocity) and teleporters (highlight the",
      "  touched frame in mdd_player_trajectory_rgba_trigger, stop predicting past it - the",
      "  destination isn't visible to this client, see docs/mdd_player_trajectory.md)",
    },
  },
};

typedef struct
{
  qhandle_t lineShader;
  qhandle_t boxShader;
} player_trajectory_t;

static player_trajectory_t s;

// Tracks the LIVE player's own PMF_JUMP_HELD across render frames (not the simulation's) - see
// the live jump-frame highlight in CG_DrawPlayerTrajectory for why these exist.
// s_liveJumpCommandTime holds the commandTime of the latest detected jump frame: the highlight
// draws for as long as that is still the CURRENT physics frame, not just on the one render
// frame the rising edge happened to be observed in.
static qboolean s_wasLiveJumpHeld;
static int      s_liveJumpCommandTime = -1;

// TL;DR: cache calculated trajectories for correct display in low-timescale demo playback.
//
// Per-model cache of the last simulated trajectory's drawable geometry, keyed by the seed
// playerState's own commandTime (see CG_RunTrajectoryModel). getPs() during demo playback (and
// live spectating) can return a state whose origin/velocity is smoothly render-interpolated
// rather than tied to a discrete command, so re-seeding and re-running the FULL simulation every
// render frame - fine at normal timescale, where render and command cadence are close - visibly
// jitters the drawn path at low timescale (many render frames per real command): the whole
// downstream physics chain is precise enough to amplify a tiny interpolation difference in the 
// seed into a visible forward/back shake. Fix: only re-run the simulation when commandTime 
// actually advances; otherwise redraw the cached geometry from the last real recompute, 
// unchanged, exactly matching the still-current command - ties the trajectory's actual update 
// cadence to the server/command frame rather than the render frame, without touching normal 
// (non-demo, normal-timescale) behavior, where a new command is available on nearly every 
// render frame anyway.
typedef struct
{
  vec3_t   origin;
  qboolean ducked;
  qboolean isJumpFrame;
  qboolean isTeleportFrame;
} trajectory_cached_box_t;

typedef struct
{
  qboolean                valid;
  int                     commandTime;
  trajectory_cached_box_t boxes[TRAJECTORY_MAX_FRAMES];
  int                     numBoxes;
  vec3_t                  linePoints[TRAJECTORY_MAX_FRAMES];
  int                     numLinePoints;
} trajectory_cache_t;

// Indexed by [0]=passive, [1]=strafebot, [2]=accel-percent - each model is simulated/drawn
// independently and can have different commandTime staleness.
static trajectory_cache_t s_trajCache[3];

// SOLID_BMODEL entities (doors/plats/trains/buttons) in the current snapshot, gathered once per
// render frame and shared by every active model. No distance cull - relies on PVS plus
// mdd_player_trajectory_trace_entities on/off. SOLID_BMODEL only, so other players (always
// SOLID_BBOX) are excluded automatically.
typedef struct
{
  int    number;
  int    modelindex;
  vec3_t origin;
  vec3_t angles;
} trajectory_entity_t;
static trajectory_entity_t s_trajEntities[TRAJECTORY_MAX_ENTITIES];
static int                 s_trajNumEntities;

// Capped at MAX_ENTITIES_IN_SNAPSHOT, not an arbitrary smaller number - PVS and
// mdd_player_trajectory_trace_triggers are the real controls, matching s_trajEntities' own cap.
#define TRAJECTORY_MAX_TRIGGERS MAX_ENTITIES_IN_SNAPSHOT

// ET_PUSH_TRIGGER (jump pads and DeFRaG's velocity-pad variant, same eType, distinguished by
// spawnflags) and ET_TELEPORT_TRIGGER, gathered like s_trajEntities. origin2/angles2/weapon are
// the push payload (velocity, per-axis alt speeds, spawnflags) per bg_misc.c's BG_TouchJumpPad
// and DF_Reverse_Stuff's BG_TouchVelocityJumpPad - unused for ET_TELEPORT_TRIGGER.
typedef struct
{
  int    number;
  int    eType;
  int    modelindex;
  vec3_t origin;
  vec3_t angles;
  vec3_t origin2;
  vec3_t angles2;
  int    weapon;
} trajectory_trigger_t;
static trajectory_trigger_t s_trajTriggers[TRAJECTORY_MAX_TRIGGERS];
static int                  s_trajNumTriggers;

// Real trace-call counts for the current timed window, printed in traj-timing - ties a cost spike
// to an actual call-count spike instead of inferring one from entity count alone (the two don't
// always correlate).
static int s_trajWorldTraceCount;
static int s_trajEntityTraceCount;
static int s_trajAllSolidCount; // times PM_TrajectoryCorrectAllSolid's jitter recovery loop fired

// TRAJECTORY_TYPE_STATIC sampling-phase shift. Physics is still recomputed fresh every tick -
// only WHICH simulated indices get a box drawn changes. Flowing (default) draws at step,
// 2*step, 3*step... relative to wherever the player currently is. Static draws at
// step-shift, 2*step-shift..., where shift advances by one every physics tick (8ms), wrapping
// at `step` - keeps each box pinned to roughly the same WORLD-SPACE point as the player closes
// the distance to it, instead of always sitting a fixed number of frames ahead. Shared by every
// active model so they stay in visual lockstep. Updated once per render frame in
// CG_DrawPlayerTrajectory, consumed in CG_RunTrajectoryModel's box-sampling condition.
// CG_DrawPlayerTrajectory only passes this down while the live player is airborne - grounded, it
// passes 0 (flowing) instead, since there's no upcoming jump/landing to stay pinned to.
static int s_trajPhaseShift;
static int s_trajPhaseLastCommandTime;

// Live velocity as of the end of the tick BEFORE the one CG_HumanAccelWish is classifying.
// pm_ps->viewangles reflects a phi decision made from the PRIOR tick's velocity (real
// PM_UpdateViewAngles applies cmd.angles before PM_Accelerate advances velocity further), so
// comparing against pm_ps->velocity directly compares against the wrong tick. Captured once per
// new commandTime; if render rate falls behind the 8ms tick rate, this goes more than one tick
// stale, but that only under-corrects, never worse than the un-cached comparison.
static vec3_t s_trajPrevLiveVelocity;
static int    s_trajPrevLiveVelocityCommandTime = -1;

// Debug instrumentation for mdd_player_trajectory_debug - captured inside PM_SlideMove (the
// only place that runs collision traces) each time it traces, so the outer per-frame loop can
// print a single comprehensive line per simulated frame including what the LAST trace this
// frame actually saw.
static int      s_debugBumpCount;
static float    s_debugLastFraction;
static qboolean s_debugLastAllSolid;

// mdd_player_trajectory_debug values - mutually exclusive verbosity levels, not a bitmask. See
// docs/mdd_player_trajectory.md for the full table of what each prints and when to use it.
#define TRAJECTORY_DEBUG_OFF      0
#define TRAJECTORY_DEBUG_FULL     1
#define TRAJECTORY_DEBUG_NEXTJUMP 2
// Dumps every intermediate value in CG_StrafebotWish's phi/quantization chain, for one full run,
// triggered once per real jump - not throttled like the other levels, so run with
// mdd_player_trajectory_frames lowered (e.g. 90) to bound total output.
#define TRAJECTORY_DEBUG_CHAIN    3
// Dumps the first TRAJECTORY_DEBUG_LIVE_FRAMES simulated frames' full chain every render frame,
// regardless of jumps - catches the SEED ITSELF jumping discontinuously between render frames,
// which mode 3 (one seeded chain, evolving internally) can't show.
#define TRAJECTORY_DEBUG_LIVE     4
#define TRAJECTORY_DEBUG_LIVE_FRAMES 10
// Dumps every SOLID_BMODEL entity/trigger CG_GatherTrajectoryEntities/Triggers actually gathered,
// once per real gather - a way to see what's in play on a map without inferring it from trace
// counts alone.
#define TRAJECTORY_DEBUG_ENTITIES 5

// Populated by CG_StrafebotWish only when it reaches the strafebot/quantization branch (not the 
// early-return passive-equivalent paths for FK/BK/no-WASD or below-cruising-speed RUN phase).
typedef struct
{
  qboolean valid;
  float    velAngleDeg; // atan2(vel_y, vel_x), degrees
  float    vel2d;
  float    gain;
  float    cosD;
  float    deltaDeg;    // RAD2DEG(acos(cosD)), pre-quantization
  float    perAngle;    // deltaDeg after its own AngleNormalize180-equivalent truncation
  float    viewYawBefore; // pm_ps->viewangles[YAW] (this ghost's view-yaw accumulator) pre-update
  float    yawShiftDeg;
  float    phaseOffsetDeg;
  // sumA/sumB: the two operands rawDelta is a difference of (viewYawBefore+yawShiftDeg and
  // velAngleDeg+phaseOffsetDeg). Diagnostic only, no code path reads them but printing - unlike
  // viewYawBefore/velAngleDeg, they're only loosely bounded (up to roughly +-270), so either can
  // land in ANGLE2SHORT's large-representation regime before rawDelta is computed. Lets a
  // fullReport-vs-chain-dump comparison spot every frame where |sumA| or |sumB| exceeds 180.
  float    sumA;
  float    sumB;
  float    rawDelta;
  float    angleDelta;
  float    viewYawAfter;  // pm_ps->viewangles[YAW] after the accumulator update
} trajectory_strafebotDebug_t;
static trajectory_strafebotDebug_t s_strafebotDebug;

// draw_player_trajectory() runs once per RENDERED frame, not per simulated step - unthrottled,
// an uncapped com_maxfps would mean 100,000+ trap_Print calls/sec. Throttled to a full dump
// (still every one of the `frames` simulated steps) at most every TRAJECTORY_DEBUG_INTERVAL_MS.
#define TRAJECTORY_DEBUG_INTERVAL_MS 250
static int s_debugLastDumpTime;

void init_player_trajectory(void)
{
  init_cvars(trajectory_cvars, ARRAY_LEN(trajectory_cvars));
  init_help(trajectory_help, ARRAY_LEN(trajectory_help));

  // Just a sane default - re-registered every render frame from mdd_player_trajectory_shader_line/
  // _box in CG_DrawPlayerTrajectory, same idiom as cg_rl.c's mdd_rl_target_shader.
  s.lineShader = trap_R_RegisterShader("railCore");
  s.boxShader  = trap_R_RegisterShader("bbox_nocull");
}

void update_player_trajectory(void)
{
  update_cvars(trajectory_cvars, ARRAY_LEN(trajectory_cvars));

  // Must re-read via cvar_getInteger(), not plain trap_Cvar_Update: stock atoi() reads "0b1111"
  // as 0, blind to the BINARY_LITERAL syntax the help text promises - same fix every other
  // bitmask cvar in this codebase (mdd_cgaz, mdd_snap, mdd_ammo, mdd_compass) already applies.
  trajectory_strafe_modes.integer = cvar_getInteger("mdd_player_trajectory_strafe_modes");
  trajectory_display_type.integer = cvar_getInteger("mdd_player_trajectory_display_type");
}

static void PM_ClipVelocity(vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
  float backoff;
  float change;
  int   i;

  backoff = DotProduct(in, normal);
  if (backoff < 0)
  {
    backoff *= overbounce;
  }
  else
  {
    backoff /= overbounce;
  }

  for (i = 0; i < 3; ++i)
  {
    change = normal[i] * backoff;
    out[i] = in[i] - change;
  }
}

// Identical to PM_ClipVelocity above except it only clips X/Y (i<2, not i<3) - leaves Z
// completely untouched. Verified against DF_Reverse_Stuff's disassembled bg_slidemove.c, used by
// real CPM's PM_StepSlideMove specifically when catching a step during an active double jump
// (stats[STAT_DJING]), so the jump's vertical momentum survives the catch instead of being
// zeroed like an ordinary flat landing - see PM_StepSlideMove below for the call site.
static void PM_FlatClipVelocity(vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
  float backoff;
  float change;
  int   i;

  backoff = DotProduct(in, normal);
  if (backoff < 0)
  {
    backoff *= overbounce;
  }
  else
  {
    backoff /= overbounce;
  }

  for (i = 0; i < 2; ++i)
  {
    change = normal[i] * backoff;
    out[i] = in[i] - change;
  }
}

// Uses the engine's own Sys_SnapVector (via CG_SNAPVECTOR), not q_shared.h's SnapVector macro -
// required for accuracy. SnapVector truncates toward zero instead of rounding to nearest, and
// since every frame re-derives velocity from an already-snapped value, that error compounds and
// drifts the predicted jump length.

// Mirrors stock PM_DropTimers() - decays PMF_TIME_LAND's pm_time and STAT_JUMPTIME/STAT_DJING
// every frame, since this file's multi-frame simulation must advance them itself (unlike
// cg_cgaz.c/cg_snap.c, which just read the live, already-ticked stats fresh each render frame).
static void PM_DropTimers(playerState_t* pm_ps)
{
  int32_t const msec = 8; // this simulation always advances by a fixed pm_frametime (.008s) tick

  if (pm_ps->pm_time)
  {
    if (msec >= pm_ps->pm_time)
    {
      pm_ps->pm_flags &= ~PMF_ALL_TIMES;
      pm_ps->pm_time = 0;
    }
    else
    {
      pm_ps->pm_time -= msec;
    }
  }

  if (pm_ps->pm_flags & PMF_PROMODE)
  {
    if (pm_ps->stats[STAT_JUMPTIME] > 0)
    {
      pm_ps->stats[STAT_JUMPTIME] -= msec;
    }
    else if (pm_ps->stats[STAT_DJING])
    {
      pm_ps->stats[STAT_DJING] = 0;
    }
  }
}

// Real ground/water/flight/spectator friction - mirrors cg_cgaz.c's own PM_Friction, which
// (unlike its neighboring PM_Accelerate) is genuine physics, not a HUD stand-in.
static void PM_Friction(playerState_t* pm_ps, pmove_t const* pm, pml_t const* pml)
{
  float speed;
  float drop;
  float newspeed;
  int   i;

  speed = pml->walking ? VectorLength2(pm_ps->velocity) : VectorLength(pm_ps->velocity);
  if (speed < 1)
  {
    pm_ps->velocity[0] = 0;
    pm_ps->velocity[1] = 0;
    return;
  }

  drop = 0;
  if (
    pm->waterlevel <= 1 && pml->walking && !(pml->groundTrace.surfaceFlags & SURF_SLICK) &&
    !(pm_ps->pm_flags & PMF_TIME_KNOCKBACK))
  {
    float const control = speed < pm_stopspeed ? pm_stopspeed : speed;
    drop += control * pm_friction * pm_frametime;
  }

  if (pm->waterlevel)
  {
    drop += speed * pm_waterfriction * pm->waterlevel * pm_frametime;
  }

  if (pm_ps->powerups[PW_FLIGHT])
  {
    drop += speed * pm_flightfriction * pm_frametime;
  }

  if (pm_ps->pm_type == PM_SPECTATOR)
  {
    drop += speed * pm_spectatorfriction * pm_frametime;
  }

  newspeed = speed - drop;
  if (newspeed < 0)
  {
    newspeed = 0;
  }
  newspeed /= speed;

  for (i = 0; i < 3; ++i) pm_ps->velocity[i] *= newspeed;
}

// Real acceleration, verbatim from stock Q3 1.32 bg_pmove.c PM_Accelerate's live "q2 style"
// branch (the reference source's own "#else proper way (avoids strafe jump maxspeed bug), but
// feels bad" alternative is disabled there too - the q2-style overshoot IS strafe/circle-jump
// physics, not an accident to correct). Unlike cg_cgaz.c/cg_snap.c's same-named HUD stand-ins,
// this one really moves velocity - the strafebot trajectory model below is the first code in
// this repo to need that.
static void PM_Accelerate(playerState_t* pm_ps, vec3_t wishdir, float wishspeed, float accel)
{
  int   i;
  float addspeed, accelspeed, currentspeed;

  currentspeed = DotProduct(pm_ps->velocity, wishdir);
  addspeed     = wishspeed - currentspeed;
  if (addspeed <= 0)
  {
    return;
  }
  accelspeed = accel * pm_frametime * wishspeed;
  if (accelspeed > addspeed)
  {
    accelspeed = addspeed;
  }

  for (i = 0; i < 3; ++i)
  {
    pm_ps->velocity[i] += accelspeed * wishdir[i];
  }
}

// One frame of "perfect strafebot" wish computation (the strafebot trajectory
// model - "the achievable goal"). Returns the flat wish direction + the wishspeed the player's
// ACTUAL currently-held movement keys produce this frame.
//
// Which side to curve is the PLAYER'S CHOICE, encoded in which key they hold (rightmove's sign
// for FB/HB), never inferred from where they're looking - the real strafebot only ever steers
// VIEW to hit angle phi for whatever pattern is ALREADY held, it never picks the pattern from 
// the view. Reads the actual live cmd via trap_GetUserCmd rather than assuming one.
//
// Pattern handling:
//   - FB (both keys) / HB (side key only): side = sign(rightmove). Both target phi with the
//     SAME formula and side rule - FB and HB only differ in wishspeed's exact CmdScale
//     dilution (both normalize to ps->speed when NOT jumping; a jump frame's upmove dilutes
//     them by slightly different amounts, matching real Q3, see strafes.md's upmove table),
//     never in the geometry, so no separate branch is needed for them here.
//   - FK (forward key alone, no side key): no left/right side signal exists in the input alone,
//     a real strafebot needs a configured direction to know
//     which way to CURVE, and this repo has no such cvar yet, so no phi/curving is attempted.
//     But "forward" itself is completely unambiguous without a side key, so FK still gets plain,
//     ordinary CmdScale-scaled forward wishspeed/wishdir - a player just holding forward keeps
//     accelerating normally, it only can't curve toward an undetermined side.
//   - No WASD held: falls back to passive (wishspeed 0), per explicit spec ("in the lack of WASD
//     input the strafebot trajectory should be the same as the passive one") - naturally zero
//     anyway since rawWishdir has zero length with no keys held.
//   - BK (backward key alone) and any backward diagonal (BFB/S+D): forced to the passive
//     fallback (wishspeed 0) rather than plain backward acceleration or the unverified backward-
//     phi formula - a player braking mid-air still wants to see where they'll land.
//   - Backward patterns (fm<0): not specifically play-tested yet - the vector math below derives
//     wishdir directly from the real forward/right basis and the actual held signs, with no
//     separate "backward" case, so it should be correct by construction, but flag if it isn't.
//
// phi is the classic optimal d_opt: the smallest angle off velocity that still receives the
// full accel*wishspeed*frametime gain (dot(v,wishdir) == wishspeed - accelAmount), the max
// speed gain per frame. Below cruising speed (raw 2D speed < ps->speed-1) there is no 
// established phi target yet: RUN phase, wish aimed exactly where the held pattern's raw 
// forward/right blend points, unmodified.
// Fetches the LIVE player's currently-held WASD/jump/crouch input: real trap_GetUserCmd when a
// real usercmd_t exists, otherwise (demo playback, spectating another player) decoded from the
// bit-packed stats[13] keypresses the server snapshots - same convention as cg_cgaz.c/cg_snap.c's
// own HUD tools. Shared by CG_StrafebotWish and CG_RunTrajectoryModel.
static void CG_GetLiveUsercmd(playerState_t const* pm_ps, usercmd_t* liveCmd)
{
  memset(liveCmd, 0, sizeof(*liveCmd));
  if (!cg.demoPlayback && !(pm_ps->pm_flags & PMF_FOLLOW))
  {
    trap_GetUserCmd(trap_GetCurrentCmdNumber(), liveCmd);
  }
  else
  {
    int8_t const scale = (int8_t)(pm_ps->stats[13] & PSF_USERINPUT_WALK ? 64 : 127);
    liveCmd->forwardmove = (signed char)(scale * ((pm_ps->stats[13] & PSF_USERINPUT_FORWARD) / PSF_USERINPUT_FORWARD -
                                                   (pm_ps->stats[13] & PSF_USERINPUT_BACKWARD) / PSF_USERINPUT_BACKWARD));
    liveCmd->rightmove   = (signed char)(scale * ((pm_ps->stats[13] & PSF_USERINPUT_RIGHT) / PSF_USERINPUT_RIGHT -
                                                   (pm_ps->stats[13] & PSF_USERINPUT_LEFT) / PSF_USERINPUT_LEFT));
    liveCmd->upmove      = (signed char)(scale * ((pm_ps->stats[13] & PSF_USERINPUT_JUMP) / PSF_USERINPUT_JUMP -
                                                   (pm_ps->stats[13] & PSF_USERINPUT_CROUCH) / PSF_USERINPUT_CROUCH));
  }
}

// CPM "airstop", verified against DF_Reverse_Stuff's disassembled PM_AirMove, q3df-doc's cpm.tex,
// and cpm1_dev_docs' cpm_pm_airstopaccelerate cvar: under PMF_PROMODE, air-only, whenever wishdir
// points more than 90 degrees off current velocity (DotProduct(velocity,wishdir)<0, "beyond
// psi"), the engine swaps in cpm_airstopaccelerate (2.5x pm_airaccelerate) instead of the normal
// accel constant. Overridden (not combined) by the pure-HB sidestrafe case
// (cpm_airstrafeaccelerate=70) when both apply, matching real PM_AirMove's own if-chain order.
// Also computes phi (the accel-zone's near/green edge), quantized onto the 16-bit grid 
// (ANGLE2SHORT, then SHORT2ANGLE) - pure/stateless so the accel-percent model can compute phi_n 
// every simulated frame from its own evolving gain/vel2d. outCosD/outDeltaDeg are optional 
// (pass NULL to skip) - CG_StrafebotWish's caller wants the pre-quantization intermediates for 
// its own chain-dump columns.
static float CG_QuantizedPhi(float gain, float vel2d, float* outCosD, float* outDeltaDeg)
{
  float cosD = gain / vel2d;
  float deltaDeg;
  float perAngle;

  if (cosD > 1.f) cosD = 1.f;
  if (cosD < -1.f) cosD = -1.f;
  deltaDeg = (float)(acos((double)cosD) * (180.0 / M_PI));

  perAngle = SHORT2ANGLE(ANGLE2SHORT(deltaDeg));
  if (perAngle > 180.f) perAngle -= 360.f;

  if (outCosD) *outCosD = cosD;
  if (outDeltaDeg) *outDeltaDeg = deltaDeg;
  return perAngle;
}

static float CG_CpmAirstopAccel(
  vec3_t const velocity,
  vec3_t const wishdir,
  qboolean     isAirMove,
  qboolean     promode,
  qboolean     cpmSideOnly,
  float        accelCoef)
{
  if (isAirMove && promode && !cpmSideOnly && DotProduct(velocity, wishdir) < 0.f)
  {
    return cpm_airstopaccelerate;
  }
  return accelCoef;
}

// CPM air control - direct port of PM_AirControl (DF_Reverse_Stuff's disassembled bg_pmove.c);
// A SEPARATE step from acceleration, called unconditionally after it whenever the caller's 
// eligibility check (isAirMove && PMF_PROMODE && pure forward) passes.
// Only ever ROTATES velocity, never changes its magnitude - the final
// *=speed reapplies the exact original speed regardless of what the dot>0 branch did.
static void CG_CpmAirControl(vec3_t velocity, vec3_t const wishdir, float wishspeed)
{
  float zspeed, speed, dot, k;

  if (wishspeed == 0.f) return;

  zspeed      = velocity[2];
  velocity[2] = 0.f;
  speed       = VectorNormalize(velocity);
  dot         = DotProduct(velocity, wishdir);
  if (dot > 0.f)
  {
    k           = cpm_pm_aircontrol * dot * dot * pm_frametime;
    velocity[0] = velocity[0] * speed + wishdir[0] * k;
    velocity[1] = velocity[1] * speed + wishdir[1] * k;
    VectorNormalize(velocity);
  }

  velocity[0] *= speed;
  velocity[1] *= speed;
  velocity[2] = zspeed;
}

static void CG_StrafebotWish(
  playerState_t*   pm_ps,
  pmove_t const*   pm,
  qboolean         isAirMove,
  float*           accelCoef,
  vec3_t           wishdir,
  float*           wishspeed,
  qboolean*        airControl)
{
  usercmd_t liveCmd;
  usercmd_t cmd;
  vec3_t    forward, right;
  vec3_t    rawWishdir;
  float     vel2d, gain, minSpeed;
  qboolean  cpmSideOnly;
  int       i;

  s_strafebotDebug.valid = qfalse;
  // True only for the pure-forward (FK) case below, under CPM, in the air - the only case real
  // PM_AirControl applies to. Defaulted false here so every other exit path doesn't need its own
  // explicit reset.
  *airControl = qfalse;

  CG_GetLiveUsercmd(pm_ps, &liveCmd);

  if (liveCmd.forwardmove < 0)
  {
    // Any backward pattern (+back-only, or backward fullbeat) is deliberately forced to the
    // passive-equivalent fallback rather than running the real backward-phi formula:
    // a player braking mid-air wants to see where they'll land, not a back strafe curve
    // behind them. wishspeed 0 makes the caller's PM_Accelerate a no-op, same as passive.
    *wishspeed = 0.f;
    VectorClear(wishdir);
    return;
  }

  AngleVectors(pm_ps->viewangles, forward, right, NULL);
  forward[2] = 0.f;
  right[2]   = 0.f;
  VectorNormalize(forward);
  VectorNormalize(right);
  for (i = 0; i < 2; ++i) rawWishdir[i] = forward[i] * liveCmd.forwardmove + right[i] * liveCmd.rightmove;
  rawWishdir[2] = 0.f;

  memset(&cmd, 0, sizeof(cmd));
  cmd.forwardmove = liveCmd.forwardmove;
  cmd.rightmove   = liveCmd.rightmove;
  cmd.upmove      = pm->cmd.upmove;
  *wishspeed = VectorNormalize(rawWishdir) * PM_CmdScale(pm_ps, &cmd);

  // CPM air sidestrafe (pure half beat, forwardmove==0 && rightmove!=0): the engine rescales wishspeed
  // and switches to a different accel constant BEFORE accelerating. Ground CPM strafing has no
  // such case - flat cpm_accelerate regardless of pattern, so this is air-only. Applied
  // unconditionally, not just inside the phi branch below - the real engine's cap/accel switch
  // happens regardless of cruising speed. *accelCoef is an out-param so the caller's subsequent
  // PM_Accelerate call uses the overridden coefficient.
  cpmSideOnly = (qboolean)(isAirMove && liveCmd.forwardmove == 0 && (pm_ps->pm_flags & PMF_PROMODE));
  if (cpmSideOnly)
  {
    if (*wishspeed > cpm_airwishspeed) *wishspeed = cpm_airwishspeed;
    *accelCoef = cpm_airstrafeaccelerate;
  }

  vel2d = sqrtf(pm_ps->velocity[0] * pm_ps->velocity[0] + pm_ps->velocity[1] * pm_ps->velocity[1]);

  // Forward key alone, no side - must NOT fall into the same early-return as backward patterns
  // above - that would zero wishspeed and stop accelerating, even though the player is still
  // holding forward. FK skips the PHI/curving branch below (no side to curve toward - side =
  // sign(rightmove) is meaningless for rightmove==0), but gets the same plain, unmodified
  // wishdir/wishspeed as the RUN-phase fallback (rawWishdir, already CmdScale-scaled above) -
  // correct by construction since "forward" is unambiguous without a side key. CPM's own
  // PM_AirControl rotation for pure FK is applied via *airControl below.
  if (liveCmd.rightmove == 0)
  {
    VectorCopy(rawWishdir, wishdir);
    *accelCoef = CG_CpmAirstopAccel(
      pm_ps->velocity, wishdir, isAirMove, (qboolean)((pm_ps->pm_flags & PMF_PROMODE) != 0), cpmSideOnly, *accelCoef);
    // forwardmove>0 required: forwardmove==0 here is "no input at all", not FK.
    *airControl = (qboolean)(isAirMove && (pm_ps->pm_flags & PMF_PROMODE) &&
                              liveCmd.forwardmove > 0 && *wishspeed > 0.f);
    return;
  }

  // CPM side-only (half beat) needs different constants than the regular full beat or VQ3 case.
  // CPM side-only speed gain is 30 * 70 / 125 = 16.8.
  // (1) MIN SPEED: CPM side-only gates at 16.8, not ps->speed-1 (~319) - sidestrafing is useful
  // at low speed, unlike FB / VQ3 HB which need cruising speed for phi to mean anything.
  // (2) GAIN FORMULA: CPM side-only uses the fixed 16.8 directly as the acos numerator, 
  // not wishspeed-accelAmount from the regular formula.
  if (cpmSideOnly)
  {
    gain     = cpm_airwishspeed * cpm_airstrafeaccelerate * pm_frametime; // == 16.8
    minSpeed = gain;
  }
  else
  {
    gain     = *wishspeed - *accelCoef * pm_frametime * *wishspeed;
    minSpeed = (float)pm_ps->speed - 1.f;
  }

  if (vel2d < minSpeed)
  {
    VectorCopy(rawWishdir, wishdir);
    *accelCoef = CG_CpmAirstopAccel(
      pm_ps->velocity, wishdir, isAirMove, (qboolean)((pm_ps->pm_flags & PMF_PROMODE) != 0), cpmSideOnly, *accelCoef);
    return;
  }

  // Side comes from the held key (rightmove's sign), not from the player's aim.
  //
  // Sign: Q3 yaw increases counterclockwise, so AngleVectors' right(yaw) is forward(yaw - pi/2) -
  // "right" is a yaw DECREASE. Holding right (rightmove>0) must SUBTRACT delta from velYaw to
  // land wishdir on velocity's right side.
  //
  // Quantization: the real player's aim travels through the usercmd pipeline as a 16-bit SHORT
  // angle (ANGLE2SHORT/SHORT2ANGLE, see DF_Reverse_Stuff bg_pmove.c), landing on the
  // 360/65536-degree grid, truncated toward zero, every frame - must be replicated or the ghost's
  // aim is more precise (hence inaccurate, ironically) than any player or strafebot ever is. 
  //
  // Accumulator: must be a running accumulator - subtract AngleNormalize180(target-current) from
  // pm_ps->viewangles[YAW] every frame - NOT a fresh absolute target recomputed from scratch 
  // each frame. Truncating a running DIFFERENCE and truncating a fresh SUM are only equivalent 
  // in some octants, not all - do not re-derive this as a stateless computation. 
  // pm_ps->viewangles[YAW] IS this ghost's accumulator, seeded once from
  // the live player's real view yaw when the simulation starts.
  {
    float velAngleDeg = (float)(atan2((double)pm_ps->velocity[1], (double)pm_ps->velocity[0]) * (180.0 / M_PI));
    float cosD, deltaDeg;
    float perAngle    = CG_QuantizedPhi(gain, vel2d, &cosD, &deltaDeg);
    float side;
    float pattern;
    float yawShiftDeg;
    float phaseOffsetDeg;
    float viewYawBefore;
    float sumA;
    float sumB;
    float rawDelta;
    float angleDelta;
    float qYawRad;

    // side +1 for rightmove>0, pattern 45 (FB) when a forward key is also held,
    // 90 (HB) otherwise - the two supported patterns (FK/BK/no-WASD already
    // returned above; backward patterns are never simulated at all, see the caller).
    side           = (liveCmd.rightmove > 0) ? 1.f : -1.f;
    pattern        = (liveCmd.forwardmove > 0) ? 45.f : 90.f;
    yawShiftDeg    = -side * pattern;
    phaseOffsetDeg = -side * perAngle;

    viewYawBefore = pm_ps->viewangles[YAW];
    sumA          = viewYawBefore + yawShiftDeg;
    sumB          = velAngleDeg + phaseOffsetDeg;
    rawDelta      = sumA - sumB;
    angleDelta    = SHORT2ANGLE(ANGLE2SHORT(rawDelta));
    if (angleDelta > 180.f) angleDelta -= 360.f;

    pm_ps->viewangles[YAW] = viewYawBefore - angleDelta;

    // ANGLE2SHORT's (int32_t) cast truncates TOWARD ZERO, not floor - not sign-symmetric across a
    // +360 shift: the same physical angle as 359.72 vs -0.28 can quantize to a different grid
    // cell purely from which side of zero it sits on. This ghost's accumulator is deliberately
    // unbounded (never reset), so once it drifts past 180 it stays in the large-positive
    // representation, while the real engine's view_yaw is reconstructed from a bounded 16-bit
    // network value every frame and is always the small representation. Rebounding here keeps
    // this accumulator in the same small-representation regime the real engine's view_yaw uses.
    //
    // A second, separate divergence traces to the REAL ENGINE's side, not ghost bookkeeping:
    // cl.viewangles was a plain float accumulator with no periodic requantization, so after 2048
    // degrees of net rotation (float32's 24-bit mantissa boundary for the AGS grid) it could pick
    // up rounding residue less than 16-bit-grid precision (ANGLE2SHORT/SHORT2ANGLE). 
    // Should be fixed in the client side for good (CL_FinishMove should re-bind
    // cl.viewangles to the same grid cmd->angles was just quantized to, every frame).
    pm_ps->viewangles[YAW] = SHORT2ANGLE(ANGLE2SHORT(pm_ps->viewangles[YAW]));
    if (pm_ps->viewangles[YAW] > 180.f) pm_ps->viewangles[YAW] -= 360.f;

    // wishYaw = velAngleDeg + phaseOffsetDeg = new_view_yaw + yawShiftDeg (NOT minus - derived
    // from new_view_yaw = view_yaw - angleDelta ~= view_yaw - [(view_yaw+yawShiftDeg) -
    // (velAngleDeg+phaseOffsetDeg)] = velAngleDeg+phaseOffsetDeg - yawShiftDeg).
    //
    // Must use cos()/sin() here, not cosf()/sinf(): real wishdir is built by stock AngleVectors
    // (q_math.c), which computes sy/cy via sin()/cos() on a float argument - C's implicit
    // promotion runs the trig in DOUBLE and truncates the double result to float on assignment.
    // float32 trig is not proven bit-identical to double-then-truncate for the same input -
    // normally harmless, but at some angles the last bit differs enough to flip trap_SnapVector's
    // rounding to a different integer.
    qYawRad = (pm_ps->viewangles[YAW] + yawShiftDeg) * ((float)M_PI / 180.f);
    wishdir[0] = (float)cos((double)qYawRad);
    wishdir[1] = (float)sin((double)qYawRad);
    wishdir[2] = 0.f;

    *accelCoef = CG_CpmAirstopAccel(
      pm_ps->velocity, wishdir, isAirMove, (qboolean)((pm_ps->pm_flags & PMF_PROMODE) != 0), cpmSideOnly, *accelCoef);

    s_strafebotDebug.valid         = qtrue;
    s_strafebotDebug.velAngleDeg   = velAngleDeg;
    s_strafebotDebug.vel2d         = vel2d;
    s_strafebotDebug.gain          = gain;
    s_strafebotDebug.cosD          = (float)cosD;
    s_strafebotDebug.deltaDeg      = (float)deltaDeg;
    s_strafebotDebug.perAngle      = perAngle;
    s_strafebotDebug.viewYawBefore = viewYawBefore;
    s_strafebotDebug.yawShiftDeg   = yawShiftDeg;
    s_strafebotDebug.phaseOffsetDeg= phaseOffsetDeg;
    s_strafebotDebug.sumA          = sumA;
    s_strafebotDebug.sumB          = sumB;
    s_strafebotDebug.rawDelta      = rawDelta;
    s_strafebotDebug.angleDelta    = angleDelta;
    s_strafebotDebug.viewYawAfter  = pm_ps->viewangles[YAW];
  }
}

// Which if-then regime this simulated chain locked into. Computed LAZILY, once, on
// CG_HumanAccelWish's first call for a chain (see `locked` below), not upfront in
// CG_RunTrajectoryModel - classification needs a correctly ground/slick/air-resolved accelCoef,
// which only exists once the caller's own surface branching has run for frame 1. Once locked,
// reused unchanged for the rest of the chain, never re-derived mid-simulation. t/side/pattern are
// only meaningful for ACCEL_REGIME_PERCENTAGE.
typedef enum
{ // phi is accel zone's green edge, psi is its yellow edge
  ACCEL_REGIME_PASSIVE,    // outside [phi,psi] at simulation start, backward held, or no WASD
  ACCEL_REGIME_DELEGATE,   // CPM HB (zone too narrow to aim), or CPM FK (real PM_AirControl case)
                           // - literally call CG_StrafebotWish, not a re-derivation of it
  ACCEL_REGIME_PERCENTAGE  // VQ3 FB/HB/FK, or CPM FB - hold t constant between phi_n and psi(=90)
} trajectory_accel_regime_t;

typedef struct
{
  qboolean                   locked;
  trajectory_accel_regime_t  regime;
  float                      t;       // 0=phi, 1=psi - PERCENTAGE only
  float                      side;    // +1/-1 - PERCENTAGE only
  float                      pattern; // 0=FK, 45=FB, 90=HB (view shift) - PERCENTAGE only
} trajectory_accel_plan_t;

// Accel-percent human-aim model.
// CPM HB and CPM FK need no new logic at all, because real PM_AirControl's own
// movementDir gate and PM_Accelerate's own addspeed<=0 gate ALREADY produce exactly the described
// behavior automatically; delegating straight to CG_StrafebotWish is not an approximation of
// that, it IS that. Only VQ3 FB/HB/FK and CPM FB get genuinely new steering logic here.
static void CG_HumanAccelWish(
  playerState_t*           pm_ps,
  pmove_t const*           pm,
  qboolean                 isAirMove,
  float*                   accelCoef,
  vec3_t                   wishdir,
  float*                   wishspeed,
  qboolean*                airControl,
  trajectory_accel_plan_t* plan)
{
  usercmd_t liveCmd;
  usercmd_t cmd;
  vec3_t    forward, right;
  vec3_t    rawWishdir;
  float     vel2d, gain;
  int       i;

  CG_GetLiveUsercmd(pm_ps, &liveCmd);

  // Classify and lock the plan on this chain's first call only - see trajectory_accel_plan_t's
  // own comment for why this happens here (lazily) instead of upfront in CG_RunTrajectoryModel.
  if (!plan->locked)
  {
    qboolean promode = (qboolean)((pm_ps->pm_flags & PMF_PROMODE) != 0);
    plan->locked = qtrue;
    plan->regime = ACCEL_REGIME_PASSIVE; // default; only overridden below where it actually applies

    // phi/phaseMag must use the velocity current when THIS tick's viewangles were decided (one
    // tick back), not pm_ps->velocity itself - see s_trajPrevLiveVelocity. Falls back to
    // pm_ps->velocity if no prior tick is cached yet.
    qboolean const havePrevTick = (qboolean)(s_trajPrevLiveVelocityCommandTime >= 0 &&
                                              s_trajPrevLiveVelocityCommandTime < pm_ps->commandTime);
    float const*   velForPlan   = havePrevTick ? s_trajPrevLiveVelocity : pm_ps->velocity;

    if (liveCmd.forwardmove < 0)
    {
      // Any backward input - passive trajectory, matches CG_StrafebotWish's own backward fallback.
    }
    else if (promode && ((liveCmd.forwardmove == 0 && liveCmd.rightmove != 0) ||
                          (liveCmd.forwardmove > 0 && liveCmd.rightmove == 0)))
    {
      // CPM HB (zone too narrow to aim within) or CPM FK (real PM_AirControl case) -
      // literally the same handling as mdd_player_trajectory_strafe_modes 4, not a re-derivation
      // of it - see this function's own top comment.
      plan->regime = ACCEL_REGIME_DELEGATE;
    }
    else if (liveCmd.forwardmove != 0 || liveCmd.rightmove != 0)
    {
      // VQ3 FB/HB/FK, or CPM FB - candidate for the percentage model. Everything here reads the
      // STILL-PRISTINE live pm_ps/velocity (this runs before any acceleration has been applied
      // for this frame), and *accelCoef is whatever the caller already correctly resolved for
      // the CURRENT ground/slick/air surface - exactly what phi's own gain formula needs.
      vec3_t planForward, planRight, planRawWishdir;
      usercmd_t planCmd;
      float  planWishspeed, planVel2d, planGain, phiNow;
      float  velAngleDeg, sumA, offsetSigned, phaseMag;
      int    ii;

      AngleVectors(pm_ps->viewangles, planForward, planRight, NULL);
      planForward[2] = 0.f;
      planRight[2]   = 0.f;
      VectorNormalize(planForward);
      VectorNormalize(planRight);
      for (ii = 0; ii < 2; ++ii)
        planRawWishdir[ii] = planForward[ii] * liveCmd.forwardmove + planRight[ii] * liveCmd.rightmove;
      planRawWishdir[2] = 0.f;

      // Must be liveCmd.upmove, not pm->cmd.upmove - the latter is the SIMULATED ghost's own
      // frame-1 autohop decision, unrelated to what the real player is doing this tick, and
      // dilutes planWishspeed via PM_CmdScale whenever the ghost happens to plan a jump.
      memset(&planCmd, 0, sizeof(planCmd));
      planCmd.forwardmove = liveCmd.forwardmove;
      planCmd.rightmove   = liveCmd.rightmove;
      planCmd.upmove      = liveCmd.upmove;
      planWishspeed = VectorNormalize(planRawWishdir) * PM_CmdScale(pm_ps, &planCmd);

      planVel2d = sqrtf(velForPlan[0] * velForPlan[0] + velForPlan[1] * velForPlan[1]);
      planGain  = planWishspeed - *accelCoef * pm_frametime * planWishspeed;
      phiNow    = CG_QuantizedPhi(planGain, planVel2d, NULL, NULL);

      velAngleDeg = (float)(atan2((double)velForPlan[1], (double)velForPlan[0]) * (180.0 / M_PI));

      if (liveCmd.rightmove != 0)
      {
        // FB/HB: side comes from the held key, matching CG_StrafebotWish's own convention -
        // phaseMag is the player's CURRENT position along that side's phi-to-psi axis.
        plan->pattern = (liveCmd.forwardmove > 0) ? 45.f : 90.f;
        plan->side    = (liveCmd.rightmove > 0) ? 1.f : -1.f;
        sumA          = pm_ps->viewangles[YAW] + (-plan->side * plan->pattern);
        offsetSigned  = sumA - velAngleDeg;
        if (offsetSigned > 180.f) offsetSigned -= 360.f;
        if (offsetSigned < -180.f) offsetSigned += 360.f;
        phaseMag = -plan->side * offsetSigned;
      }
      else
      {
        // VQ3 FK: no key to determine side - pattern shift is 0 (view yaw IS the wish angle
        // directly), so side comes from whichever side the CURRENT aim actually sits on.
        plan->pattern = 0.f;
        sumA          = pm_ps->viewangles[YAW];
        offsetSigned  = sumA - velAngleDeg;
        if (offsetSigned > 180.f) offsetSigned -= 360.f;
        if (offsetSigned < -180.f) offsetSigned += 360.f;
        plan->side = (offsetSigned >= 0.f) ? -1.f : 1.f;
        phaseMag   = -plan->side * offsetSigned; // == fabsf(offsetSigned) by construction
      }

      // Needs a tiny (~0.0055deg) tolerance below phiNow, not a strict >=: even with
      // velForPlan/liveCmd.upmove fixed above, a genuinely exact phi aim can still land phaseMag a
      // hair under phiNow from grid rounding alone (see CG_QuantizedPhi's quantization comment).
      if (phaseMag >= phiNow - (360.f / 65536.f) && phaseMag <= 90.f)
      {
        plan->regime = ACCEL_REGIME_PERCENTAGE;
        plan->t      = (phiNow < 90.f) ? (phaseMag - phiNow) / (90.f - phiNow) : 0.f;
        if (plan->t < 0.f) plan->t = 0.f;
        if (plan->t > 1.f) plan->t = 1.f;
      }
      // else: outside [phi,psi] (inner cone) or beyond psi/wrong side entirely - stays PASSIVE.

      // Prints the real numbers rather than guessing at a tolerance value blind. Gated on any
      // nonzero mdd_player_trajectory_debug; doesn't affect the classification itself.
      if (trajectory_debug.integer != TRAJECTORY_DEBUG_OFF)
      {
        trap_Print(vaf(
          "traj-accel-plan t=%d phiNow=%.6f phaseMag=%.6f diff=%.6f offsetSigned=%.6f side=%.1f "
          "pattern=%.1f regime=%d t_locked=%.4f\n",
          cg.time, phiNow, phaseMag, phaseMag - phiNow, offsetSigned, plan->side, plan->pattern,
          plan->regime, plan->t));
      }
    }
    // else: forwardmove==0 && rightmove==0, no WASD at all - stays PASSIVE.

    // Advance the previous-tick velocity cache exactly once per genuinely new commandTime,
    // regardless of which branch above ran - see s_trajPrevLiveVelocity's own comment.
    if (s_trajPrevLiveVelocityCommandTime != pm_ps->commandTime)
    {
      VectorCopy(pm_ps->velocity, s_trajPrevLiveVelocity);
      s_trajPrevLiveVelocityCommandTime = pm_ps->commandTime;
    }
  }

  if (plan->regime == ACCEL_REGIME_PASSIVE)
  {
    *wishspeed  = 0.f;
    *airControl = qfalse;
    VectorClear(wishdir);
    return;
  }

  if (plan->regime == ACCEL_REGIME_DELEGATE)
  {
    CG_StrafebotWish(pm_ps, pm, isAirMove, accelCoef, wishdir, wishspeed, airControl);
    return;
  }

  // ACCEL_REGIME_PERCENTAGE from here on. wishspeed/rawWishdir mirror CG_StrafebotWish's own
  // setup exactly (same AngleVectors+PM_CmdScale derivation from the LIVE held keys, liveCmd
  // already fetched above) - safe to duplicate, this part carries none of the steering formula's
  // hard-won fix history. Never reaches the cpmSideOnly (CPM pure-HB) special case at all - that
  // pattern is always ACCEL_REGIME_DELEGATE, never PERCENTAGE.
  AngleVectors(pm_ps->viewangles, forward, right, NULL);
  forward[2] = 0.f;
  right[2]   = 0.f;
  VectorNormalize(forward);
  VectorNormalize(right);
  for (i = 0; i < 2; ++i) rawWishdir[i] = forward[i] * liveCmd.forwardmove + right[i] * liveCmd.rightmove;
  rawWishdir[2] = 0.f;

  memset(&cmd, 0, sizeof(cmd));
  cmd.forwardmove = liveCmd.forwardmove;
  cmd.rightmove   = liveCmd.rightmove;
  cmd.upmove      = pm->cmd.upmove;
  *wishspeed = VectorNormalize(rawWishdir) * PM_CmdScale(pm_ps, &cmd);

  vel2d = sqrtf(pm_ps->velocity[0] * pm_ps->velocity[0] + pm_ps->velocity[1] * pm_ps->velocity[1]);
  gain  = *wishspeed - *accelCoef * pm_frametime * *wishspeed;

  // Mirrors CG_StrafebotWish's own steering tail (same accumulator/quantization mechanics - see
  // its own comments for why each step is shaped the way it is), with exactly one change: the
  // target angle is phi_n blended toward psi(=90) by the LOCKED t, instead of phi_n alone.
  {
    float velAngleDeg = (float)(atan2((double)pm_ps->velocity[1], (double)pm_ps->velocity[0]) * (180.0 / M_PI));
    float cosD, deltaDeg;
    float phiNow        = CG_QuantizedPhi(gain, vel2d, &cosD, &deltaDeg);
    float targetAngle   = phiNow + plan->t * (90.f - phiNow);
    float yawShiftDeg    = -plan->side * plan->pattern;
    float phaseOffsetDeg = -plan->side * targetAngle;
    float viewYawBefore, sumA, sumB, rawDelta, angleDelta, qYawRad;

    viewYawBefore = pm_ps->viewangles[YAW];
    sumA          = viewYawBefore + yawShiftDeg;
    sumB          = velAngleDeg + phaseOffsetDeg;
    rawDelta      = sumA - sumB;
    angleDelta    = SHORT2ANGLE(ANGLE2SHORT(rawDelta));
    if (angleDelta > 180.f) angleDelta -= 360.f;

    pm_ps->viewangles[YAW] = viewYawBefore - angleDelta;
    pm_ps->viewangles[YAW] = SHORT2ANGLE(ANGLE2SHORT(pm_ps->viewangles[YAW]));
    if (pm_ps->viewangles[YAW] > 180.f) pm_ps->viewangles[YAW] -= 360.f;

    qYawRad    = (pm_ps->viewangles[YAW] + yawShiftDeg) * ((float)M_PI / 180.f);
    wishdir[0] = (float)cos((double)qYawRad);
    wishdir[1] = (float)sin((double)qYawRad);
    wishdir[2] = 0.f;

    // Only the sidestrafe case (cpm movementDir 2/6, never reached here, that's always 
    // ACCEL_REGIME_DELEGATE) is excluded and OVERRIDES airstop when both would apply. 
    // So this DOES need the same call strafebot's own tail makes. *airControl stays 
    // false for a real reason though: PM_AirControl's own guard excludes every 
    // movementDir except pure FK/BK (0/4), and FB is neither.
    *accelCoef = CG_CpmAirstopAccel(
      pm_ps->velocity, wishdir, isAirMove, (qboolean)((pm_ps->pm_flags & PMF_PROMODE) != 0), qfalse, *accelCoef);
    *airControl = qfalse;

    s_strafebotDebug.valid         = qtrue;
    s_strafebotDebug.velAngleDeg   = velAngleDeg;
    s_strafebotDebug.vel2d         = vel2d;
    s_strafebotDebug.gain          = gain;
    s_strafebotDebug.cosD          = cosD;
    s_strafebotDebug.deltaDeg      = deltaDeg;
    s_strafebotDebug.perAngle      = targetAngle; // blended target, NOT pure phi, for this model
    s_strafebotDebug.viewYawBefore = viewYawBefore;
    s_strafebotDebug.yawShiftDeg   = yawShiftDeg;
    s_strafebotDebug.phaseOffsetDeg= phaseOffsetDeg;
    s_strafebotDebug.sumA          = sumA;
    s_strafebotDebug.sumB          = sumB;
    s_strafebotDebug.rawDelta      = rawDelta;
    s_strafebotDebug.angleDelta    = angleDelta;
    s_strafebotDebug.viewYawAfter  = pm_ps->viewangles[YAW];
  }
}

// Populates s_trajEntities from the current snapshot - see docs/mdd_player_trajectory.md. No
// distance filtering: no cheap way to get a SOLID_BMODEL entity's real position (would need a new
// engine syscall), so this relies on snapshot PVS plus mdd_player_trajectory_trace_entities.
static void CG_GatherTrajectoryEntities(qboolean shouldDebugEntities)
{
  snapshot_t const* snap = getSnap();
  int               i;

  s_trajNumEntities = 0;
  if (!snap || !trajectory_trace_entities.integer) return;

  for (i = 0; i < snap->numEntities && s_trajNumEntities < TRAJECTORY_MAX_ENTITIES; ++i)
  {
    entityState_t const* ent = &snap->entities[i];

    if (ent->solid != SOLID_BMODEL) continue;

    // Triggers are ALSO SOLID_BMODEL - excluded here so they aren't traced as blocking movers
    // (which would stop the ghost before it reaches CG_TrajectoryTouchesTrigger's own overlap
    // test). Handled entirely by the dedicated trigger path instead.
    if (ent->eType == ET_PUSH_TRIGGER || ent->eType == ET_TELEPORT_TRIGGER) continue;

    if (shouldDebugEntities)
    {
      trap_Print(vaf("traj-entities t=%d number=%d modelindex=%d\n", cg.time, ent->number, ent->modelindex));
    }

    {
      trajectory_entity_t* out = &s_trajEntities[s_trajNumEntities++];
      out->number     = ent->number;
      out->modelindex = ent->modelindex;
      VectorCopy(ent->pos.trBase, out->origin);
      VectorCopy(ent->angles, out->angles);
    }
  }
}

// Populates s_trajTriggers - see that struct's own comment for field sourcing. Same cadence as
// CG_GatherTrajectoryEntities, kept separate since triggers and movers need different fields and
// feed entirely different code paths (touch-detection vs. collision tracing).
static void CG_GatherTrajectoryTriggers(qboolean shouldDebugEntities)
{
  snapshot_t const* snap = getSnap();
  int               i;

  s_trajNumTriggers = 0;
  if (!snap || !trajectory_trace_triggers.integer) return;

  for (i = 0; i < snap->numEntities && s_trajNumTriggers < TRAJECTORY_MAX_TRIGGERS; ++i)
  {
    entityState_t const* ent = &snap->entities[i];

    if (ent->eType != ET_PUSH_TRIGGER && ent->eType != ET_TELEPORT_TRIGGER) continue;
    if (ent->solid != SOLID_BMODEL) continue; // matches stock CG_TouchTriggerPrediction's own check

    if (shouldDebugEntities)
    {
      trap_Print(vaf("traj-triggers t=%d number=%d eType=%d modelindex=%d origin2=(%.1f %.1f %.1f) "
        "weapon=%d angles2=(%.1f %.1f %.1f)\n",
        cg.time, ent->number, ent->eType, ent->modelindex,
        ent->origin2[0], ent->origin2[1], ent->origin2[2],
        ent->weapon, ent->angles2[0], ent->angles2[1], ent->angles2[2]));
    }

    {
      trajectory_trigger_t* out = &s_trajTriggers[s_trajNumTriggers++];
      out->number     = ent->number;
      out->eType      = ent->eType;
      out->modelindex = ent->modelindex;
      VectorCopy(ent->pos.trBase, out->origin);
      VectorCopy(ent->angles, out->angles);
      VectorCopy(ent->origin2, out->origin2);
      VectorCopy(ent->angles2, out->angles2);
      out->weapon     = ent->weapon;
    }
  }
}

// Ported from stock cg_predict.c's CG_ClipMoveToEntities, simplified to SOLID_BMODEL-only (see
// s_trajEntities comment) - stock's other branch (encoded-bbox solids, used for other players) is
// deliberately out of scope, not an oversight.
static void CG_TrajectoryClipMoveToEntities(
  vec3_t const start, vec3_t const end, vec3_t const mins, vec3_t const maxs,
  int brushmask, trace_t* tr)
{
  int i;

  for (i = 0; i < s_trajNumEntities; ++i)
  {
    trajectory_entity_t const* ent    = &s_trajEntities[i];
    clipHandle_t                cmodel = trap_CM_InlineModel(ent->modelindex);
    trace_t                     trace;

    trap_CM_TransformedBoxTrace(&trace, start, end, mins, maxs, cmodel, brushmask, ent->origin, ent->angles);
    s_trajEntityTraceCount++;

    if (trace.allsolid || trace.fraction < tr->fraction)
    {
      trace.entityNum = ent->number;
      *tr              = trace;
    }
    else if (trace.startsolid)
    {
      tr->startsolid = qtrue;
    }
    if (tr->allsolid) return;
  }
}

// Drop-in replacement for every trap_CM_BoxTrace(&trace, start, end, mins, maxs, 0, mask) call
// site in this file's local PM_SlideMove/PM_StepSlideMove/PM_GroundTrace forks - traces the world
// first exactly like before, then merges in whichever gathered entity (see s_trajEntities) is
// closer, mirroring stock cg_predict.c's CG_Trace.
static void CG_TrajectoryTrace(
  trace_t* tr, vec3_t const start, vec3_t const end, vec3_t const mins, vec3_t const maxs, int brushmask)
{
  trap_CM_BoxTrace(tr, start, end, mins, maxs, 0, brushmask);
  s_trajWorldTraceCount++;
  tr->entityNum = (tr->fraction != 1.f) ? ENTITYNUM_WORLD : ENTITYNUM_NONE;

  CG_TrajectoryClipMoveToEntities(start, end, mins, maxs, brushmask, tr);
}

// Forked from bg_pmove.c's PM_CorrectAllSolid/PM_GroundTrace, same reason PM_SlideMove/
// PM_StepSlideMove/PM_ClipVelocity already are local forks (see file header): the
// real PM_GroundTrace is shared with cg_snap.c/cg_cgaz.c's unrelated HUD math, and its own
// PM_CorrectAllSolid helper is file-static there - not callable from here even if it weren't
// shared. Routed through CG_TrajectoryTrace instead of a raw trap_CM_BoxTrace so a player
// standing on/in a mover (e.g. a nearly-closed door) is recognized the same as world geometry -
// otherwise identical to the real functions.
static qboolean PM_TrajectoryCorrectAllSolid(pmove_t const* pm, playerState_t* pm_ps, pml_t* pml, trace_t* trace)
{
  vec3_t  point;
  int8_t  i, j, k;

  s_trajAllSolidCount++;

  for (i = -1; i <= 1; ++i)
  {
    for (j = -1; j <= 1; ++j)
    {
      for (k = -1; k <= 1; ++k)
      {
        VectorCopy(pm_ps->origin, point);
        point[0] += (float)i;
        point[1] += (float)j;
        point[2] += (float)k;
        CG_TrajectoryTrace(trace, point, point, pm->mins, pm->maxs, pm->tracemask);
        if (!trace->allsolid)
        {
          point[0] = pm_ps->origin[0];
          point[1] = pm_ps->origin[1];
          point[2] = pm_ps->origin[2] - .25f;

          CG_TrajectoryTrace(trace, pm_ps->origin, point, pm->mins, pm->maxs, pm->tracemask);
          pml->groundTrace = *trace;
          return qtrue;
        }
      }
    }
  }

  pm_ps->groundEntityNum = ENTITYNUM_NONE;
  pml->groundPlane       = qfalse;
  pml->walking           = qfalse;

  return qfalse;
}

// Returns qfalse only when PM_TrajectoryCorrectAllSolid couldn't find a way out (genuinely
// wedged, not just touching solid) - every other outcome resolves and returns qtrue. The caller
// stops simulating further frames once stuck, instead of repeating the same failed 27-probe
// recovery every frame. Without it, wedging in moving entities observed on map nrjt - when the 
// player was entering through the door, the simulated trajectory got wedged and "killed" FPS.
static qboolean PM_TrajectoryGroundTrace(pmove_t const* pm, playerState_t* pm_ps, pml_t* pml)
{
  vec3_t  point;
  trace_t trace;

  point[0] = pm_ps->origin[0];
  point[1] = pm_ps->origin[1];
  point[2] = pm_ps->origin[2] - .25f;

  CG_TrajectoryTrace(&trace, pm_ps->origin, point, pm->mins, pm->maxs, pm->tracemask);
  pml->groundTrace = trace;

  // do something corrective if the trace starts in a solid...
  if (trace.allsolid)
  {
    if (!PM_TrajectoryCorrectAllSolid(pm, pm_ps, pml, &trace)) return qfalse;
  }

  // if the trace didn't hit anything, we are in free fall
  if (trace.fraction == 1.f)
  {
    pm_ps->groundEntityNum = ENTITYNUM_NONE;
    pml->groundPlane       = qfalse;
    pml->walking           = qfalse;
    return qtrue;
  }

  // check if getting thrown off the ground
  if (pm_ps->velocity[2] > 0 && DotProduct(pm_ps->velocity, trace.plane.normal) > 10)
  {
    pm_ps->groundEntityNum = ENTITYNUM_NONE;
    pml->groundPlane       = qfalse;
    pml->walking           = qfalse;
    return qtrue;
  }

  // slopes that are too steep will not be considered onground
  if (trace.plane.normal[2] < MIN_WALK_NORMAL)
  {
    pm_ps->groundEntityNum = ENTITYNUM_NONE;
    pml->groundPlane       = qtrue;
    pml->walking           = qfalse;
    return qtrue;
  }

  pml->groundPlane = qtrue;
  pml->walking     = qtrue;

  // hitting solid ground will end a waterjump
  if (pm_ps->pm_flags & PMF_TIME_WATERJUMP)
  {
    pm_ps->pm_flags &= ~(PMF_TIME_WATERJUMP | PMF_TIME_LAND);
    pm_ps->pm_time = 0;
  }

  if (pm_ps->groundEntityNum == ENTITYNUM_NONE)
  {
    // don't do landing time if we were just going down a slope
    if (pml->previous_velocity[2] < -200)
    {
      pm_ps->pm_flags |= PMF_TIME_LAND;
      pm_ps->pm_time = 250;
    }
  }

  pm_ps->groundEntityNum = trace.entityNum;
  return qtrue;
}

// Same "am I touching this volume" test stock's own CG_TouchTriggerPrediction (cg_predict.c) uses
// - a zero-length trace (start==end) against the trigger's own inline model, checking startsolid.
// Must be the PLAIN, untransformed trap_CM_BoxTrace, matching that function exactly - not the
// transformed variant movers use (trig->angles is always (0,0,0) here anyway, per stock
// InitTrigger's own G_SetMovedir zeroing, but matching the reference is what actually matters).
static qboolean CG_TrajectoryTouchesTrigger(
  vec3_t const origin, vec3_t const mins, vec3_t const maxs, trajectory_trigger_t const* trig)
{
  clipHandle_t cmodel = trap_CM_InlineModel(trig->modelindex);
  trace_t      trace;

  // !cmodel means trap_CM_InlineModel couldn't resolve modelindex - a config problem, not a
  // per-frame event, so warn once rather than throttling like the other debug prints.
  static qboolean s_warnedNoModel = qfalse;
  if (!cmodel)
  {
    if (trajectory_debug.integer && !s_warnedNoModel)
    {
      trap_Print(vaf("traj-trigger-nomodel number=%d modelindex=%d\n", trig->number, trig->modelindex));
      s_warnedNoModel = qtrue;
    }
    return qfalse;
  }

  trap_CM_BoxTrace(&trace, origin, origin, mins, maxs, cmodel, MASK_ALL);
  return trace.startsolid;
}

// Ported from DF_Reverse_Stuff's BG_TouchVelocityJumpPad, a superset of stock BG_TouchJumpPad:
// spawnflags==0 is a plain jump pad (unconditional VectorCopy of the push velocity); nonzero
// spawnflags select DeFRaG's per-axis "velocity pad" behavior - bits 1/4 scale along current
// velocity's own XY/Z direction instead of a fixed one; bits 2/8 add to current velocity instead
// of overwriting, gated to first touch only via jumppad_ent (matching real re-touch semantics);
// bit 16/0x20 picks whichever sign better matches current heading; bit 0x40 zeroes instead of
// reversing when the result would point backward. No sound/event port (meaningless for a ghost).
// frameIndex substitutes for ps->pmove_framecount, which this simulation never maintains.
static void CG_TrajectoryTouchPushTrigger(playerState_t* pm_ps, trajectory_trigger_t const* trig, int frameIndex)
{
  int    const spawnflags = trig->weapon;
  vec3_t       velocityXY, velocityZ, plus, minus;

  if (!spawnflags)
  {
    VectorCopy(trig->origin2, pm_ps->velocity);
    pm_ps->jumppad_ent   = trig->number;
    pm_ps->jumppad_frame = frameIndex;
    return;
  }

  if (spawnflags & 1)
  {
    VectorCopy(pm_ps->velocity, velocityXY);
    velocityXY[2] = 0.f;
    VectorNormalize(velocityXY);
    VectorScale(velocityXY, trig->angles2[0], velocityXY);
  }
  else
  {
    velocityXY[0] = trig->origin2[0];
    velocityXY[1] = trig->origin2[1];
    velocityXY[2] = 0.f;
    if (spawnflags & 16)
    {
      VectorCopy(pm_ps->velocity, plus);
      VectorCopy(pm_ps->velocity, minus);
      plus[0]  += velocityXY[0];
      plus[1]  += velocityXY[1];
      minus[0] -= velocityXY[0];
      minus[1] -= velocityXY[1];
      if (VectorLength(plus) < VectorLength(minus)) VectorNegate(velocityXY, velocityXY);
    }
  }

  if (spawnflags & 4)
  {
    VectorClear(velocityZ);
    velocityZ[2] = pm_ps->velocity[2];
    VectorNormalize(velocityZ);
    VectorScale(velocityZ, trig->angles2[2], velocityZ);
  }
  else
  {
    VectorClear(velocityZ);
    velocityZ[2] = trig->origin2[2];
    if (spawnflags & 0x20)
    {
      VectorCopy(pm_ps->velocity, plus);
      VectorCopy(pm_ps->velocity, minus);
      plus[2]  += velocityZ[2];
      minus[2] -= velocityZ[2];
      if (VectorLength(plus) < VectorLength(minus)) VectorNegate(velocityZ, velocityZ);
    }
  }

  if (spawnflags & 2)
  {
    if (pm_ps->jumppad_ent != trig->number)
    {
      float theta;
      plus[0] = pm_ps->velocity[0] + velocityXY[0];
      plus[1] = pm_ps->velocity[1] + velocityXY[1];
      plus[2] = 0.f;
      theta = DotProduct(plus, pm_ps->velocity) / (VectorLength(plus) * VectorLength(pm_ps->velocity));
      if ((spawnflags & 0x40) && theta < 0.f)
      {
        pm_ps->velocity[0] = 0.f;
        pm_ps->velocity[1] = 0.f;
      }
      else
      {
        pm_ps->velocity[0] = plus[0];
        pm_ps->velocity[1] = plus[1];
      }
    }
  }
  else
  {
    pm_ps->velocity[0] = velocityXY[0];
    pm_ps->velocity[1] = velocityXY[1];
  }

  if (spawnflags & 8)
  {
    if (pm_ps->jumppad_ent != trig->number)
    {
      float theta;
      plus[0] = 0.f;
      plus[1] = 0.f;
      plus[2] = pm_ps->velocity[2] + velocityZ[2];
      theta = DotProduct(plus, pm_ps->velocity) / (VectorLength(plus) * VectorLength(pm_ps->velocity));
      if ((spawnflags & 0x40) && theta < 0.f)
      {
        pm_ps->velocity[2] = 0.f;
      }
      else
      {
        pm_ps->velocity[2] = plus[2];
      }
    }
  }
  else
  {
    pm_ps->velocity[2] = velocityZ[2];
  }

  pm_ps->jumppad_ent   = trig->number;
  pm_ps->jumppad_frame = frameIndex;
}

// Checked once per simulated frame (mirroring real Pmove's own single end-of-frame trigger-touch
// pass), after that frame's move/groundtrace already resolved - see the call site. Applies push
// triggers immediately (velocity carries into the NEXT frame's own accel/friction/slide, same slot
// real Pmove applies them in) and returns qfalse on a teleport touch so the caller can stop
// simulating further frames: the destination isn't visible to this client at all (see
// s_trajTriggers comment) - v1 scope, matching the accepted plan, is to highlight the touched
// frame and stop there rather than guess at a landing spot.
static qboolean CG_TrajectoryCheckTriggers(
  playerState_t* pm_ps, vec3_t const mins, vec3_t const maxs, int frameIndex, qboolean* touchedTeleport)
{
  int i;

  *touchedTeleport = qfalse;

  for (i = 0; i < s_trajNumTriggers; ++i)
  {
    trajectory_trigger_t const* trig = &s_trajTriggers[i];

    if (!CG_TrajectoryTouchesTrigger(pm_ps->origin, mins, maxs, trig)) continue;

    if (trig->eType == ET_PUSH_TRIGGER)
    {
      if (trajectory_debug.integer)
      {
        vec3_t const before = { pm_ps->velocity[0], pm_ps->velocity[1], pm_ps->velocity[2] };
        CG_TrajectoryTouchPushTrigger(pm_ps, trig, frameIndex);
        trap_Print(vaf("traj-trigger-touch t=%d i=%d number=%d weapon=%d vBefore=(%.1f %.1f %.1f) vAfter=(%.1f %.1f %.1f)\n",
          cg.time, frameIndex, trig->number, trig->weapon,
          before[0], before[1], before[2],
          pm_ps->velocity[0], pm_ps->velocity[1], pm_ps->velocity[2]));
      }
      else
      {
        CG_TrajectoryTouchPushTrigger(pm_ps, trig, frameIndex);
      }
      // First touched (lowest snapshot index) wins, not last - confirmed 2026-08-11 against a
      // real q3dm17 two-adjacent-jump-pads spot. Why the real server resolves it in this order is
      // unknown, but this matches real jump-pad behavior in practice.
      break;
    }
    else // ET_TELEPORT_TRIGGER
    {
      *touchedTeleport = qtrue;
      if (trajectory_debug.integer)
      {
        trap_Print(vaf("traj-trigger-touch t=%d i=%d number=%d (teleport, stopping)\n", cg.time, frameIndex, trig->number));
      }
      return qfalse;
    }
  }

  return qtrue;
}

#define MAX_CLIP_PLANES 5

static qboolean PM_SlideMove(playerState_t* pm_ps, pmove_t const* pm, pml_t* pml, qboolean gravity)
{
  int     bumpcount, numbumps;
  vec3_t  dir;
  float   d;
  int     numplanes;
  vec3_t  planes[MAX_CLIP_PLANES];
  vec3_t  primalVelocity;
  vec3_t  clipVelocity;
  int     i, j, k;
  trace_t trace;
  vec3_t  end;
  float   timeLeft;
  float   into;
  vec3_t  endVelocity;
  vec3_t  endClipVelocity;

  numbumps = 4;

  VectorCopy(pm_ps->velocity, primalVelocity);

  if (gravity)
  {
    VectorCopy(pm_ps->velocity, endVelocity);
    endVelocity[2] -= pm_ps->gravity * pm_frametime;
    pm_ps->velocity[2] = (pm_ps->velocity[2] + endVelocity[2]) * 0.5f;
    primalVelocity[2]  = endVelocity[2];

    if (pml->groundPlane)
    {
      PM_ClipVelocity(pm_ps->velocity, pml->groundTrace.plane.normal, pm_ps->velocity, OVERCLIP);
    }
  }
  else
  {
    // never actually consumed (only copied back to pm_ps->velocity under "if (gravity)" below),
    // but PM_ClipVelocity(endVelocity, ...) below still runs unconditionally each bump, so this
    // needs a defined value rather than reading uninitialized memory
    VectorCopy(pm_ps->velocity, endVelocity);
  }

  timeLeft = pm_frametime;

  if (pml->groundPlane)
  {
    numplanes = 1;
    VectorCopy(pml->groundTrace.plane.normal, planes[0]);
  }
  else
  {
    numplanes = 0;
  }

  VectorNormalize2(pm_ps->velocity, planes[numplanes]);
  numplanes++;

  for (bumpcount = 0; bumpcount < numbumps; ++bumpcount)
  {
    VectorMA(pm_ps->origin, timeLeft, pm_ps->velocity, end);

    CG_TrajectoryTrace(&trace, pm_ps->origin, end, pm->mins, pm->maxs, pm->tracemask);

    // mdd_player_trajectory_debug: record every trace this call makes, so whatever the LAST
    // one saw survives regardless of which of this function's several return points is taken.
    s_debugBumpCount    = bumpcount;
    s_debugLastFraction = trace.fraction;
    s_debugLastAllSolid = trace.allsolid;

    if (trace.allsolid)
    {
      // entity is completely trapped in another solid
      pm_ps->velocity[2] = 0;
      return qtrue;
    }

    if (trace.fraction > 0)
    {
      VectorCopy(trace.endpos, pm_ps->origin);
    }

    if (trace.fraction == 1)
    {
      break; // moved the entire distance
    }

    timeLeft -= timeLeft * trace.fraction;

    if (numplanes >= MAX_CLIP_PLANES)
    {
      // this shouldn't really happen
      VectorClear(pm_ps->velocity);
      return qtrue;
    }

    // if this is the same plane we hit before, nudge velocity out along it, which fixes some
    // epsilon issues with non-axial planes
    for (i = 0; i < numplanes; ++i)
    {
      if (DotProduct(trace.plane.normal, planes[i]) > 0.99f)
      {
        VectorAdd(trace.plane.normal, pm_ps->velocity, pm_ps->velocity);
        break;
      }
    }
    if (i < numplanes)
    {
      continue;
    }
    VectorCopy(trace.plane.normal, planes[numplanes]);
    numplanes++;

    // modify velocity so it parallels all of the clip planes

    // find a plane that it enters
    for (i = 0; i < numplanes; ++i)
    {
      into = DotProduct(pm_ps->velocity, planes[i]);
      if (into >= 0.1f)
      {
        continue; // move doesn't interact with the plane
      }

      // slide along the plane
      PM_ClipVelocity(pm_ps->velocity, planes[i], clipVelocity, OVERCLIP);
      PM_ClipVelocity(endVelocity, planes[i], endClipVelocity, OVERCLIP);

      // see if there is a second plane that the new move enters
      for (j = 0; j < numplanes; ++j)
      {
        if (j == i)
        {
          continue;
        }
        if (DotProduct(clipVelocity, planes[j]) >= 0.1f)
        {
          continue; // move doesn't interact with the plane
        }

        // try clipping the move to the plane
        PM_ClipVelocity(clipVelocity, planes[j], clipVelocity, OVERCLIP);
        PM_ClipVelocity(endClipVelocity, planes[j], endClipVelocity, OVERCLIP);

        // see if it goes back into the first clip plane
        if (DotProduct(clipVelocity, planes[i]) >= 0)
        {
          continue;
        }

        // slide the original velocity along the crease
        CrossProduct(planes[i], planes[j], dir);
        VectorNormalize(dir);
        d = DotProduct(dir, pm_ps->velocity);
        VectorScale(dir, d, clipVelocity);

        CrossProduct(planes[i], planes[j], dir);
        VectorNormalize(dir);
        d = DotProduct(dir, endVelocity);
        VectorScale(dir, d, endClipVelocity);

        // see if there is a third plane the new move enters
        for (k = 0; k < numplanes; ++k)
        {
          if (k == i || k == j)
          {
            continue;
          }
          if (DotProduct(clipVelocity, planes[k]) >= 0.1f)
          {
            continue; // move doesn't interact with the plane
          }

          // stop dead at a triple plane interaction
          VectorClear(pm_ps->velocity);
          return qtrue;
        }
      }

      // if we have fixed all interactions, try another move
      VectorCopy(clipVelocity, pm_ps->velocity);
      VectorCopy(endClipVelocity, endVelocity);
      break;
    }
  }

  if (gravity)
  {
    VectorCopy(endVelocity, pm_ps->velocity);
  }

  // don't change velocity if in a timer
  if (pm_ps->pm_time)
  {
    VectorCopy(primalVelocity, pm_ps->velocity);
  }

  return (qboolean)(bumpcount != 0);
}

// Must commit to the step unconditionally once attempted, matching stock Q3 1.32 - do NOT compare
// the stepped vs non-stepped move's horizontal distance and revert the step if the plain slide
// went farther, and do NOT overwrite velocity[2] with the non-stepped slide's Z when keeping the
// step. That's Quake 2's PM_StepSlideMove tail (remnants sit in stock bg_slidemove.c inside a
// disabled "#if 0" block) - it breaks stair prediction: the ghost grinds into risers instead of
// stepping up, or keeps downward vz on step tops instead of getting clipped to ~0 and re-grounded
// for the next jump, the way a real player does.
static void PM_StepSlideMove(playerState_t* pm_ps, pmove_t const* pm, pml_t* pml, qboolean gravity)
{
  vec3_t  startOrigin, startVelocity;
  trace_t trace;
  vec3_t  up, down;
  float   stepSize;

  VectorCopy(pm_ps->origin, startOrigin);
  VectorCopy(pm_ps->velocity, startVelocity);

  if (!PM_SlideMove(pm_ps, pm, pml, gravity))
  {
    return; // we got exactly where we wanted to go first try
  }

  VectorCopy(startOrigin, down);
  down[2] -= STEPSIZE;
  CG_TrajectoryTrace(&trace, startOrigin, down, pm->mins, pm->maxs, pm->tracemask);
  VectorSet(up, 0, 0, 1);
  // Don't step up when you still have up velocity - EXCEPT under an active CPM double-jump
  // window: verified against DF_Reverse_Stuff's disassembled bg_slidemove.c - real CPM
  // relaxes this refusal specifically when PMF_PROMODE, velocity[2]<=370 (roughly the DJ vz
  // range), and stats[STAT_JUMPTIME] is still running, letting a double-jumped player catch
  // steps a plain VQ3-style refusal would reject - part of why CPM staircases need fewer real
  // jumps than the vz formula alone predicts. Also verified in-game and confirmed.
  if (pm_ps->velocity[2] > 0 && (trace.fraction == 1.f || DotProduct(trace.plane.normal, up) < 0.7f))
  {
    if (!(pm_ps->pm_flags & PMF_PROMODE) || pm_ps->velocity[2] > 370.f || pm_ps->stats[STAT_JUMPTIME] <= 0)
    {
      return;
    }
  }

  VectorCopy(startOrigin, up);
  up[2] += STEPSIZE;

  // test the player position if they were a stepheight higher
  CG_TrajectoryTrace(&trace, startOrigin, up, pm->mins, pm->maxs, pm->tracemask);
  if (trace.allsolid)
  {
    return; // can't step up
  }

  // the ACTUAL clearance gained, not a blind full STEPSIZE - if something overhead limited the
  // up trace, both the raised start position and the final push-down must use the smaller value
  // (the earlier Q2-flavored version raised and pushed by the full STEPSIZE unconditionally,
  // which under a low ceiling would have started the re-slide inside the obstruction)
  stepSize = trace.endpos[2] - startOrigin[2];

  // try slidemove from this position
  VectorCopy(trace.endpos, pm_ps->origin);
  VectorCopy(startVelocity, pm_ps->velocity);

  PM_SlideMove(pm_ps, pm, pml, gravity);

  // push down the final amount
  VectorCopy(pm_ps->origin, down);
  down[2] -= stepSize;
  CG_TrajectoryTrace(&trace, pm_ps->origin, down, pm->mins, pm->maxs, pm->tracemask);
  if (!trace.allsolid)
  {
    VectorCopy(trace.endpos, pm_ps->origin);
  }
  if (trace.fraction < 1.f)
  {
    if ((pm_ps->pm_flags & PMF_PROMODE) && pm_ps->stats[STAT_DJING])
    {
      // In CPM the vertical velocity SURVIVES a step-catch made during an active double jump -
      // verified against DF_Reverse_Stuff's disassembled bg_slidemove.c, real CPM only clips X/Y
      // here (PM_FlatClipVelocity), not Z, letting momentum carry the player up several steps on
      // existing vz without needing a fresh jump at each one.
      // Also verified in-game and confirmed.
      PM_FlatClipVelocity(pm_ps->velocity, trace.plane.normal, pm_ps->velocity, OVERCLIP);
    }
    else
    {
      // landing on a flat step top, this is what zeroes the vertical velocity - the "step-up sets
      // vz to 0" behavior known from play is this clip against the step's up-facing normal, not
      // an explicit assignment anywhere
      PM_ClipVelocity(pm_ps->velocity, trace.plane.normal, pm_ps->velocity, OVERCLIP);
    }
  }
}

// Draws a chain of RT_RAIL_CORE beam segments through consecutive points - same primitive
// cg_rl.c already uses for the rocket path line, just chained across many points instead of
// cg_rl.c's single missile-to-impact segment.
static void CG_DrawLine(vec3_t* points, int numPoints, vec4_t rgba)
{
  refEntity_t beam;
  int         i;

  if (!s.lineShader || numPoints < 2) return;

  memset(&beam, 0, sizeof(beam));
  beam.reType       = RT_RAIL_CORE;
  beam.customShader = s.lineShader;
  AxisClear(beam.axis);
  beam.shaderRGBA[0] = (byte)(rgba[0] * 255);
  beam.shaderRGBA[1] = (byte)(rgba[1] * 255);
  beam.shaderRGBA[2] = (byte)(rgba[2] * 255);
  beam.shaderRGBA[3] = (byte)(rgba[3] * 255);

  for (i = 0; i < numPoints - 1; ++i)
  {
    VectorCopy(points[i], beam.oldorigin);
    VectorCopy(points[i + 1], beam.origin);
    trap_R_AddRefEntityToScene(&beam);
  }
}

// Runs one full frames-long simulation pass for a single prediction model and draws its
// samples. Each active model re-simulates independently from the live player state - the cost
// multiplier for combining models is the collision tracing, as accepted in the original plan.
static void CG_RunTrajectoryModel(
  int      model,
  int      frames,
  int      step,
  int      types,
  vec4_t   rgba,
  qboolean shouldDebugDump,
  qboolean shouldDebugNextJump,
  qboolean shouldDebugChain,
  qboolean shouldDebugLive,
  int      phaseShift)
{
  playerState_t  pm_ps;
  pmove_t        pm;
  pml_t          pml;
  qboolean       collectLine;
  vec4_t         rgbaJump;
  vec4_t         rgbaTrigger;
  int            i;
  int            jumpDelay;
  int            seedCommandTime;
  static vec3_t  linePoints[TRAJECTORY_MAX_FRAMES];
  int            numLinePoints;
  qboolean       debugNextJumpFound;
  qboolean const strafebot      = (qboolean)(model == TRAJECTORY_STRAFEBOT);
  qboolean const accelModel     = (qboolean)(model == TRAJECTORY_ACCEL);
  // TRAJECTORY_TYPE_STATIC: sample at a world-pinned phase (phaseShift, shared across models,
  // see s_trajPhaseShift) instead of always step/2*step/3*step... - see that comment for why.
  qboolean const staticMode     = (qboolean)((types & TRAJECTORY_TYPE_STATIC) != 0);
  // See the trajectory_cache_t comment above s_trajCache: skip the whole re-simulation (and the
  // seed jitter it can introduce during demo playback at low timescale) when the seed's own
  // commandTime hasn't advanced since the last real recompute, and just redraw that cached
  // geometry instead. Debug dumps always force a real recompute - they're a manual diagnostic
  // aid, not performance-sensitive, and must keep reflecting a live run.
  qboolean const      forceRecompute = (qboolean)(shouldDebugDump || shouldDebugNextJump ||
                                                    shouldDebugChain || shouldDebugLive);
  int const           cacheIdx       = strafebot ? 1 : (accelModel ? 2 : 0);
  trajectory_cache_t* cache          = &s_trajCache[cacheIdx];
  usercmd_t           liveCmd;
  qboolean            liveCrouching;
  trajectory_accel_plan_t accelPlan; // locked lazily by CG_HumanAccelWish's first call

  // Zero the whole struct, not just .locked - regime/t/side/pattern are never read while
  // ACCEL_REGIME_PASSIVE (its default), but leaving them uninitialized made the diagnostic print
  // show garbage for t_locked on every passive classification, confusing to read.
  memset(&accelPlan, 0, sizeof(accelPlan));

  pm_ps = *getPs();
  seedCommandTime = pm_ps.commandTime;

  // Must reset: the seed above copies the LIVE player's own jumppad_ent/jumppad_frame, which
  // would make this simulation's first touch of that same pad spuriously read as already-touched.
  pm_ps.jumppad_ent   = 0;
  pm_ps.jumppad_frame = 0;

  // Mid-air crouch-hold assumption: a real jump requires cmd.upmove >= 10, mutually exclusive
  // with a crouching cmd.upmove < 0, so if the live player is holding crouch right now (not
  // jumping), assume they keep holding it for the whole predicted chain instead of the usual
  // jump-continuation assumption below - PM_CheckDuck then sets PMF_DUCKED every simulated frame,
  // which is what the box-drawing code already keys its crouched-bbox size off of.
  CG_GetLiveUsercmd(&pm_ps, &liveCmd);
  liveCrouching = (qboolean)(liveCmd.upmove < 0);

  collectLine = (qboolean)((types & TRAJECTORY_TYPE_LINE) != 0);

  memset(&pm, 0, sizeof(pm));
  pm.tracemask = MASK_PLAYERSOLID;

  ParseVec(trajectory_rgba_jump.string, rgbaJump, 4);
  ParseVec(trajectory_rgba_trigger.string, rgbaTrigger, 4);

  if (!forceRecompute && cache->valid && cache->commandTime == pm_ps.commandTime)
  {
    int bi;
    for (bi = 0; bi < cache->numBoxes; ++bi)
    {
      trajectory_cached_box_t const* b = &cache->boxes[bi];
      vec3_t                         origin;
      float*                         sampleColor = b->isTeleportFrame ? rgbaTrigger : (b->isJumpFrame ? rgbaJump : rgba);
      VectorCopy(b->origin, origin);
      if ((b->isJumpFrame || b->isTeleportFrame) && (types & TRAJECTORY_TYPE_JUMP_BOXES))
      {
        CG_DrawBoxAt(origin, b->ducked, sampleColor, qtrue, s.boxShader);
      }
      else
      {
        if (types & TRAJECTORY_TYPE_BOXES) CG_DrawBoxAt(origin, b->ducked, sampleColor, qtrue, s.boxShader);
        if (types & TRAJECTORY_TYPE_BOTTOMS) CG_DrawBoxAt(origin, b->ducked, sampleColor, qfalse, s.boxShader);
      }
    }
    if (types & TRAJECTORY_TYPE_LINE) CG_DrawLine(cache->linePoints, cache->numLinePoints, rgba);
    return;
  }

  // cmd handling per model - passive keeps fm/rm at 0 for the whole run ("as if every key was
  // released", which is what makes acceleration provably a no-op for it, see file header);
  // strafebot conceptually holds an FB full-beat, but only PM_CmdScale ever reads a cmd for it
  // (inside CG_StrafebotWish) - the wish direction is synthesized against phi directly, so this
  // shared cmd stays zeroed for both models. upmove is driven by the jumpDelay state machine
  // below either way.
  memset(&pm.cmd, 0, sizeof(pm.cmd));

  numLinePoints      = 0;
  jumpDelay          = -1; // -1 = ready to jump
  debugNextJumpFound = qfalse;
  cache->numBoxes    = 0;

  if (shouldDebugDump)
  {
    trap_Print(vaf("---- traj debug dump t=%d frames=%d model=%d ----\n", cg.time, frames, model));
  }
  else if (shouldDebugChain)
  {
    trap_Print(vaf("---- traj chain dump t=%d frames=%d model=%d ----\n", cg.time, frames, model));
  }
  else if (shouldDebugLive)
  {
    trap_Print(vaf("---- traj live dump t=%d model=%d ----\n", cg.time, model));
  }

  for (i = 1; i <= frames; ++i)
  {
    qboolean isJumpFrame     = qfalse;
    qboolean isTeleportFrame = qfalse;

    memset(&pml, 0, sizeof(pml));
    VectorCopy(pm_ps.velocity, pml.previous_velocity);
    // pml.forward/right/up are deliberately never computed here - nothing in this file's
    // simulation ever reads them, unlike a real Pmove: passive has no directional input at all,
    // and the strafebot model synthesizes its wish direction directly (CG_StrafebotWish)
    // instead of projecting a cmd through view angles.

    pm.cmd.upmove = liveCrouching ? -127 : 0;

    PM_CheckDuck(&pm, &pm_ps);
    PM_SetWaterLevel(&pm, &pm_ps);

    if (pm.waterlevel > 1) break; // no water physics, stop rather than simulate them wrong

    // Wedged in solid with no escape found (see PM_TrajectoryGroundTrace's own comment) - stop
    // simulating this model rather than repeating the same failed recovery every remaining frame.
    // Whatever was already collected into cache/linePoints up to this point still draws normally.
    if (!PM_TrajectoryGroundTrace(&pm, &pm_ps, &pml)) break;
    PM_DropTimers(&pm_ps);

    // Jump-continuation: assume the player keeps auto-hopping rather than letting friction fade
    // them to a stop on strafe-pad-like ground. Jump once grounded, then force a 1-frame release 
    // before allowing the next one (PM_CheckJump's own PMF_JUMP_HELD gate requires upmove to 
    // drop below 10 for at least one frame between jumps, or it silently refuses to jump again). 
    // Driven by THIS frame's own freshly-traced pml.walking.
    //
    // Do NOT gate this on "ground state as of the previous frame's own GroundTrace" to reproduce
    // 1-frame lag - tried it, made things worse (stairs unchanged, jump highlight was wrong, 
    // new blinking/shaking artifacts in previously-stable spots). Only reintroduce from 
    // confirmed per-frame data, not a plausible-sounding trace. Skipped entirely while 
    // liveCrouching: a held crouch (cmd.upmove < 0) and a jump (cmd.upmove >= 10) can never 
    // coexist in one command, and the crouch-hold assumption above already takes precedence 
    // for this whole predicted chain.
    if (!liveCrouching && jumpDelay == -1 && pml.walking)
    {
      // 10, not 127: the minimum PM_CheckJump accepts. Very optimistic for a human player.
      // But this trajectory simulation does 1-frame jumps anyway, so it is already 
      // very optimistic anyway.
      pm.cmd.upmove = 10;
      jumpDelay     = 0;
    }
    else if (!liveCrouching && jumpDelay >= 0)
    {
      jumpDelay++;
      if (jumpDelay == 1) jumpDelay = -1;
    }

    if (pm.cmd.upmove < 10)
    {
      pm_ps.pm_flags &= ~PMF_JUMP_HELD;
    }

    if (pml.walking && PM_CheckJump(&pm, &pm_ps, &pml))
    {
      // PM_CheckJump already set velocity[2]=JUMP_VELOCITY, groundEntityNum=ENTITYNUM_NONE and
      // cleared groundPlane/walking - the rest of this frame runs through the air branch below.
      isJumpFrame = qtrue;
    }

    if (pml.walking)
    {
      float vel;

      PM_Friction(&pm_ps, &pm, &pml);

      if (strafebot || accelModel)
      {
        // Same slot stock PM_WalkMove accelerates in: after friction, before the slick/
        // knockback gravity step. Coefficient selection mirrors stock (slick/knockback ground
        // accelerates like air) with the CPM values picked by PMF_PROMODE exactly the way
        // cg_cgaz.c/cg_snap.c already do. Computing the wish from the POST-friction velocity is
        // deliberate and simpler than the real client-side bots, which must pre-apply a
        // predicted friction step because they act before the frame's Pmove - here friction has
        // genuinely already run. v1 simplification: wishdir stays flat instead of being
        // projected onto the ground plane (stock projects forward/right along slopes) - exact
        // on flat ground, slightly off on slopes.
        vec3_t   wishdir;
        float    wishspeed;
        float    accelCoef;
        qboolean airControl; // always false here - PM_AirControl is air-only, never read below
        if ((pml.groundTrace.surfaceFlags & SURF_SLICK) || (pm_ps.pm_flags & PMF_TIME_KNOCKBACK))
        {
          accelCoef = (pm_ps.pm_flags & PMF_PROMODE) ? cpm_slickaccelerate : pm_slickaccelerate;
        }
        else
        {
          accelCoef = (pm_ps.pm_flags & PMF_PROMODE) ? cpm_accelerate : pm_accelerate;
        }
        if (accelModel)
        {
          CG_HumanAccelWish(&pm_ps, &pm, qfalse, &accelCoef, wishdir, &wishspeed, &airControl, &accelPlan);
        }
        else
        {
          CG_StrafebotWish(&pm_ps, &pm, qfalse, &accelCoef, wishdir, &wishspeed, &airControl);
        }
        PM_Accelerate(&pm_ps, wishdir, wishspeed, accelCoef);
      }

      if ((pml.groundTrace.surfaceFlags & SURF_SLICK) || (pm_ps.pm_flags & PMF_TIME_KNOCKBACK))
      {
        pm_ps.velocity[2] -= pm_ps.gravity * pm_frametime;
      }

      vel = VectorLength(pm_ps.velocity);
      PM_ClipVelocity(pm_ps.velocity, pml.groundTrace.plane.normal, pm_ps.velocity, OVERCLIP);
      VectorNormalize(pm_ps.velocity);
      VectorScale(pm_ps.velocity, vel, pm_ps.velocity);

      if (pm_ps.velocity[0] != 0.f || pm_ps.velocity[1] != 0.f)
      {
        PM_StepSlideMove(&pm_ps, &pm, &pml, qfalse);
      }
    }
    else
    {
      PM_Friction(&pm_ps, &pm, &pml);

      if (strafebot || accelModel)
      {
        // Same slot stock PM_AirMove accelerates in: after friction, before the steep-ground-
        // plane clip. Base air coefficient is pm_airaccelerate (1) in both physics - an FB
        // full-beat is the diagonal movementDir case in CPM's PM_AirMove, which gets plain
        // pm_airaccelerate with no wishspeed cap. Pure HB (strafebot's other supported pattern,
        // always ACCEL_REGIME_DELEGATE for the accel model) DOES get CPM's special sidestrafe
        // cap+accel override - see the CG_StrafebotWish comment - hence accelCoef is a variable
        // here, not a constant passed straight through: CG_StrafebotWish/CG_HumanAccelWish may
        // rewrite it before this frame's PM_Accelerate call.
        vec3_t   wishdir;
        float    wishspeed;
        float    accelCoef = pm_airaccelerate;
        qboolean airControl;
        if (accelModel)
        {
          CG_HumanAccelWish(&pm_ps, &pm, qtrue, &accelCoef, wishdir, &wishspeed, &airControl, &accelPlan);
        }
        else
        {
          CG_StrafebotWish(&pm_ps, &pm, qtrue, &accelCoef, wishdir, &wishspeed, &airControl);
        }
        PM_Accelerate(&pm_ps, wishdir, wishspeed, accelCoef);
        // CPM's real PM_AirMove calls PM_AirControl unconditionally right after PM_Accelerate,
        // every frame under PMF_PROMODE - a SEPARATE step, not folded into acceleration, and not
        // gated on the accel zone (see strafes.md's "CPM air control (FK/BK)" section). airControl
        // is only ever true for the pure-forward case, matching real PM_AirControl's movementDir gate.
        if (airControl) CG_CpmAirControl(pm_ps.velocity, wishdir, wishspeed);
      }

      if (pml.groundPlane)
      {
        PM_ClipVelocity(pm_ps.velocity, pml.groundTrace.plane.normal, pm_ps.velocity, OVERCLIP);
      }

      PM_StepSlideMove(&pm_ps, &pm, &pml, qtrue);
    }

    // Second ground trace of the frame, mirroring real PmoveSingle exactly (stock bg_pmove.c
    // re-runs PM_GroundTrace + PM_SetWaterLevel after the move branches, BEFORE trap_SnapVector).
    // NOT redundant with the top-of-loop trace: (a) this is the ONLY point where
    // pml.previous_velocity still holds the frame's PRE-move falling velocity, which
    // PM_GroundTrace's hard-landing check needs (previous_velocity[2] < -200 ->
    // PMF_TIME_LAND/pm_time=250) - skip this call and the simulation never sets its own landing
    // timer, leaving PM_SlideMove's pm_time velocity-revert permanently inert, bleeding speed on
    // post-landing clips the real Pmove would have preserved. (b) a step-up landing becomes
    // jumpable ground at the END of the frame it happens in, exactly like the real game, instead
    // of waiting for the next frame's own trace.
    // Unlike the top-of-loop call, this frame's move already happened (PM_StepSlideMove above) -
    // its origin/velocity are still valid new data worth keeping even if wedged here, so the
    // normal debug/box-collection tail below still runs; only further FRAMES get skipped, via
    // stillMoving checked at the bottom of this loop body.
    qboolean const stillMoving = PM_TrajectoryGroundTrace(&pm, &pm_ps, &pml);
    PM_SetWaterLevel(&pm, &pm_ps);

    // Checked once per frame here, same slot real Pmove's own end-of-frame trigger pass runs in,
    // so a push trigger's velocity carries into the next frame's accel/friction/slide. Applies to
    // every model unconditionally - a map feature, not WASD-dependent like acceleration. A
    // teleport touch folds into stillMoving so it shares the allsolid-wedge break/draw path.
    qboolean const keepSimulating =
      CG_TrajectoryCheckTriggers(&pm_ps, pm.mins, pm.maxs, i, &isTeleportFrame) && stillMoving;

    trap_SnapVector(pm_ps.velocity); // see the comment block above PM_DropTimers

    if (shouldDebugDump)
    {
      trap_Print(vaf(
        "traj i=%d walk=%d jump=%d up=%d o=(%.1f %.1f %.1f) v=(%.1f %.1f %.1f) flags=0x%x "
        "time=%d ground=%d bump=%d frac=%.4f allsolid=%d\n",
        i, pml.walking, isJumpFrame, pm.cmd.upmove, pm_ps.origin[0], pm_ps.origin[1], pm_ps.origin[2],
        pm_ps.velocity[0], pm_ps.velocity[1], pm_ps.velocity[2], pm_ps.pm_flags, pm_ps.pm_time,
        pm_ps.groundEntityNum, s_debugBumpCount, s_debugLastFraction, s_debugLastAllSolid));
    }
    else if (shouldDebugNextJump && isJumpFrame && !debugNextJumpFound)
    {
      debugNextJumpFound = qtrue;
      trap_Print(vaf("traj-jump t=%d m=%d i=%d o=(%.2f %.2f %.2f)\n",
        cg.time, model, i, pm_ps.origin[0], pm_ps.origin[1], pm_ps.origin[2]));
    }
    else if (shouldDebugChain)
    {
      // bump/frac/allsolid: whatever PM_SlideMove's LAST trace this frame saw (see the comment
      // above s_debugBumpCount) - shows whether a collision trace saw anything (fraction <1) even
      // when origin/velocity kept sailing on a clean, uninterrupted ballistic arc.
      trap_Print(vaf(
        "chain i=%d walk=%d jump=%d up=%d o=(%.3f %.3f %.3f) v=(%.3f %.3f %.3f) "
        "bump=%d frac=%.4f allsolid=%d ground=%d "
        "SB:%d velAngleDeg=%.6f vel2d=%.6f gain=%.6f cosD=%.6f deltaDeg=%.6f perAngle=%.6f "
        "viewYawBefore=%.6f yawShiftDeg=%.6f phaseOffsetDeg=%.6f sumA=%.6f sumB=%.6f "
        "rawDelta=%.6f angleDelta=%.6f viewYawAfter=%.6f\n",
        i, pml.walking, isJumpFrame, pm.cmd.upmove,
        pm_ps.origin[0], pm_ps.origin[1], pm_ps.origin[2],
        pm_ps.velocity[0], pm_ps.velocity[1], pm_ps.velocity[2],
        s_debugBumpCount, s_debugLastFraction, s_debugLastAllSolid, pm_ps.groundEntityNum,
        s_strafebotDebug.valid, s_strafebotDebug.velAngleDeg, s_strafebotDebug.vel2d,
        s_strafebotDebug.gain, s_strafebotDebug.cosD, s_strafebotDebug.deltaDeg, s_strafebotDebug.perAngle,
        s_strafebotDebug.viewYawBefore, s_strafebotDebug.yawShiftDeg, s_strafebotDebug.phaseOffsetDeg,
        s_strafebotDebug.sumA, s_strafebotDebug.sumB,
        s_strafebotDebug.rawDelta, s_strafebotDebug.angleDelta, s_strafebotDebug.viewYawAfter));
    }
    else if (shouldDebugLive && i <= TRAJECTORY_DEBUG_LIVE_FRAMES)
    {
      trap_Print(vaf(
        "live i=%d walk=%d up=%d o=(%.3f %.3f %.3f) v=(%.3f %.3f %.3f) "
        "SB:%d velAngleDeg=%.6f vel2d=%.6f deltaDeg=%.6f perAngle=%.6f "
        "viewYawBefore=%.6f yawShiftDeg=%.6f phaseOffsetDeg=%.6f sumA=%.6f sumB=%.6f "
        "rawDelta=%.6f angleDelta=%.6f viewYawAfter=%.6f\n",
        i, pml.walking, pm.cmd.upmove,
        pm_ps.origin[0], pm_ps.origin[1], pm_ps.origin[2],
        pm_ps.velocity[0], pm_ps.velocity[1], pm_ps.velocity[2],
        s_strafebotDebug.valid, s_strafebotDebug.velAngleDeg, s_strafebotDebug.vel2d,
        s_strafebotDebug.deltaDeg, s_strafebotDebug.perAngle,
        s_strafebotDebug.viewYawBefore, s_strafebotDebug.yawShiftDeg, s_strafebotDebug.phaseOffsetDeg,
        s_strafebotDebug.sumA, s_strafebotDebug.sumB,
        s_strafebotDebug.rawDelta, s_strafebotDebug.angleDelta, s_strafebotDebug.viewYawAfter));
    }

    if (collectLine)
    {
      // MINS_Z (-24): the bottom of the player bbox, i.e. the actual ground-contact/"feet"
      // point - playerState_t.origin itself sits 24 units above that (stock Q3 player bbox,
      // see bbox.c/bg_local.h - unrelated to how DeFRaG's own coordinate HUD displays origin).
      VectorCopy(pm_ps.origin, linePoints[numLinePoints]);
      linePoints[numLinePoints][2] += MINS_Z;
      numLinePoints++;
    }

    // Jump frames are drawn every frame, in their own color, regardless of the step - they're
    // the single most actionable detail on a strafe pad chain. Takes precedence over the
    // regular step-sampled draw rather than drawing both (would just overlap at the same spot).
    // staticMode shifts which index counts as "on step" by phaseShift (re-wrapped mod step here
    // since step can change live) instead of always step/2*step/3*step... - see s_trajPhaseShift.
    // A teleport-touch frame is also always drawn regardless of step, same as jump frames - it's
    // the last frame this recompute has anything meaningful to show.
    if (isJumpFrame || isTeleportFrame || (staticMode ? (i + phaseShift) % step == 0 : i % step == 0))
    {
      qboolean const ducked      = (qboolean)((pm_ps.pm_flags & PMF_DUCKED) != 0);
      float*         sampleColor = isTeleportFrame ? rgbaTrigger : (isJumpFrame ? rgbaJump : rgba);
      if ((isJumpFrame || isTeleportFrame) && (types & TRAJECTORY_TYPE_JUMP_BOXES))
      {
        CG_DrawBoxAt(pm_ps.origin, ducked, sampleColor, qtrue, s.boxShader);
      }
      else
      {
        if (types & TRAJECTORY_TYPE_BOXES) CG_DrawBoxAt(pm_ps.origin, ducked, sampleColor, qtrue, s.boxShader);
        if (types & TRAJECTORY_TYPE_BOTTOMS) CG_DrawBoxAt(pm_ps.origin, ducked, sampleColor, qfalse, s.boxShader);
      }

      if (cache->numBoxes < TRAJECTORY_MAX_FRAMES)
      {
        trajectory_cached_box_t* b = &cache->boxes[cache->numBoxes++];
        VectorCopy(pm_ps.origin, b->origin);
        b->ducked         = ducked;
        b->isJumpFrame     = isJumpFrame;
        b->isTeleportFrame = isTeleportFrame;
      }
    }

    // See the comment above stillMoving/PM_TrajectoryGroundTrace: this frame's own box/line point
    // was already collected above (still valid), only further frames (wedged OR past a teleport
    // entrance, see keepSimulating) are skipped.
    if (!keepSimulating) break;
  }

  if (collectLine)
  {
    CG_DrawLine(linePoints, numLinePoints, rgba);
  }

  // seedCommandTime was captured before the loop, from the same *getPs() call the whole
  // simulation was seeded from - nothing in this simulation ever writes playerState_t.commandTime
  // (only real engine Pmove/usercmd processing does), so it's stable as a "still the same server
  // command" cache key even though pm_ps itself has since been mutated by the simulation above.
  cache->valid          = qtrue;
  cache->commandTime    = seedCommandTime;
  // Copy only numLinePoints, not the whole fixed-size buffer - avoids a full-size memcpy when
  // frames is far below TRAJECTORY_MAX_FRAMES, the common case.
  memcpy(cache->linePoints, linePoints, (size_t)numLinePoints * sizeof(linePoints[0]));
  cache->numLinePoints  = numLinePoints;
}

// Mirrors CG_RunTrajectoryModel's own cache-hit condition, checked per model before gathering
// entities - a cache hit just redraws cached boxes, never calls CG_TrajectoryTrace, so gathering
// is wasted if every active model's cache is already valid. cacheIdx must match
// CG_RunTrajectoryModel's own indexing (0=passive, 1=strafebot, 2=accel).
static qboolean CG_TrajectoryCacheStale(int cacheIdx, qboolean forceRecompute, int commandTime)
{
  trajectory_cache_t const* cache = &s_trajCache[cacheIdx];
  return (qboolean)(forceRecompute || !cache->valid || cache->commandTime != commandTime);
}

static void CG_DrawPlayerTrajectory(void)
{
  playerState_t pm_ps;
  int           frames;
  int           step;
  int           types;
  vec4_t        rgba;
  qboolean      shouldDebugDump;
  qboolean      shouldDebugNextJump;
  qboolean      shouldDebugChain;
  qboolean      shouldDebugLive;
  qboolean      shouldDebugTiming;
  qboolean      shouldDebugEntities;
  int           effectivePhaseShift;

  if (!(trajectory_strafe_modes.integer & (TRAJECTORY_PASSIVE | TRAJECTORY_ACCEL | TRAJECTORY_STRAFEBOT))) return;

  // Re-registered from the cvar strings every draw call, same idiom as cg_rl.c's
  // mdd_rl_target_shader - cheap (engine caches by name), lets players change these live.
  s.lineShader = trap_R_RegisterShader(trajectory_shader_line.string);
  s.boxShader  = trap_R_RegisterShader(trajectory_shader_box.string);

  pm_ps = *getPs();

  // Live jump-frame highlight - MUST run before every early return below, not after: the
  // near-zero-velocity check returns on EVERY landing that brings the player to a dead stop
  // (exactly what a flat no-WASD jump test does), and skipping the tracking on those frames
  // once left the static stuck at "still held", silently eating the next jump's rising edge.
  // This block sees every call; only the drawing is gated on a normal, alive player.
  //
  // Diagnostic (any nonzero mdd_player_trajectory_debug): prints only on PMF_JUMP_HELD
  // transitions as seen from THIS sampling point - a couple of lines per hop, self-throttled
  // by nature. The rising edge fires reliably on every jump.
  if (trajectory_debug.integer &&
      (qboolean)((pm_ps.pm_flags & PMF_JUMP_HELD) != 0) != s_wasLiveJumpHeld)
  {
    trap_Print(vaf("traj-live t=%d flags=0x%x held %d->%d pm_type=%d\n",
      cg.time, pm_ps.pm_flags, s_wasLiveJumpHeld, (pm_ps.pm_flags & PMF_JUMP_HELD) != 0,
      pm_ps.pm_type));
  }

  // TL;DR: This block is required to draw the jump bbox around the player during the jump frame
  // itself (and not only for the future jump frames).
  //
  // DETECTION vs DISPLAY - two different lifetimes (the first build conflated them and drew
  // only on the rising-edge RENDER frame: an ~8ms flash, imperceptible at full speed and -
  // the subtle part, found via the traj-live log. The edge only LATCHES the jump frame's
  // commandTime; the draw below then persists for as long as that is still the current physics
  // frame - stepped/paused: visible the whole time the jump frame is the frame on screen; full
  // speed: visible for exactly the jump frame's own 8ms, taking over from the future-prediction
  // box that vanishes at that instant.
  qboolean const justJumped =
    (qboolean)(!s_wasLiveJumpHeld && (pm_ps.pm_flags & PMF_JUMP_HELD) && pm_ps.pm_type == PM_NORMAL);

  if (justJumped)
  {
    s_liveJumpCommandTime = pm_ps.commandTime;
    if (trajectory_debug.integer)
    {
      trap_Print(vaf("traj-live JUMP latched t=%d ct=%d o=(%.1f %.1f %.1f)\n",
        cg.time, pm_ps.commandTime, pm_ps.origin[0], pm_ps.origin[1], pm_ps.origin[2]));
    }
  }
  s_wasLiveJumpHeld = (qboolean)((pm_ps.pm_flags & PMF_JUMP_HELD) != 0);

  shouldDebugChain = (qboolean)(justJumped && trajectory_debug.integer == TRAJECTORY_DEBUG_CHAIN);

  if (pm_ps.commandTime == s_liveJumpCommandTime && pm_ps.pm_type == PM_NORMAL)
  {
    vec4_t rgbaJumpLive;
    int    typesLive = trajectory_display_type.integer;
    ParseVec(trajectory_rgba_jump.string, rgbaJumpLive, 4);
    // no duck check needed here (unlike the future-frame draws): a jump frame always has
    // cmd.upmove >= 10, which is never true at the same time as a duck-triggering upmove < 0 -
    // a jump frame is unducked by construction.
    if (typesLive & TRAJECTORY_TYPE_JUMP_BOXES)
    {
      CG_DrawBoxAt(pm_ps.origin, qfalse, rgbaJumpLive, qtrue, s.boxShader);
    }
    else
    {
      if (typesLive & TRAJECTORY_TYPE_BOXES) CG_DrawBoxAt(pm_ps.origin, qfalse, rgbaJumpLive, qtrue, s.boxShader);
      if (typesLive & TRAJECTORY_TYPE_BOTTOMS) CG_DrawBoxAt(pm_ps.origin, qfalse, rgbaJumpLive, qfalse, s.boxShader);
    }
  }

  // don't try to predict anything for dead/spectating/intermission states
  if (pm_ps.pm_type != PM_NORMAL) return;

  // No velocity, nothing to predict - checking the current velocity directly.
  if (VectorLength(pm_ps.velocity) < 1.f) return;

  types = trajectory_display_type.integer;

  frames = trajectory_frames.integer;
  if (frames < 1) frames = 1;
  if (frames > TRAJECTORY_MAX_FRAMES) frames = TRAJECTORY_MAX_FRAMES;

  step = trajectory_step.integer;
  if (step < 1) step = 1;

  // One shared throttle tick for however many models run this frame - each model then prints
  // its own dump under the same tick, distinguished by the model= header / m= tag.
  shouldDebugDump     = qfalse;
  shouldDebugNextJump = qfalse;
  shouldDebugLive     = qfalse;
  shouldDebugTiming   = qfalse;
  shouldDebugEntities = qfalse;
  if (trajectory_debug.integer != TRAJECTORY_DEBUG_OFF &&
      cg.time - s_debugLastDumpTime >= TRAJECTORY_DEBUG_INTERVAL_MS)
  {
    s_debugLastDumpTime = cg.time;
    shouldDebugDump     = (qboolean)(trajectory_debug.integer == TRAJECTORY_DEBUG_FULL);
    shouldDebugNextJump = (qboolean)(trajectory_debug.integer == TRAJECTORY_DEBUG_NEXTJUMP);
    shouldDebugLive     = (qboolean)(trajectory_debug.integer == TRAJECTORY_DEBUG_LIVE);
    shouldDebugEntities = (qboolean)(trajectory_debug.integer == TRAJECTORY_DEBUG_ENTITIES);
    // Any nonzero debug mode also gets a timing line at this same cadence, regardless of which
    // mode - measuring entity-tracing cost doesn't need a dedicated mode of its own.
    shouldDebugTiming   = qtrue;
  }

  // Advance the shared STATIC-mode phase shift by however many physics ticks (8ms each) passed
  // since the last render - see the s_trajPhaseShift comment above s_trajCache. Harmless to
  // compute even when no active model is in static mode: CG_RunTrajectoryModel only reads it
  // when TRAJECTORY_TYPE_STATIC is actually set. Non-negative double-modulo guards against a
  // backward commandTime jump (new map load, demo seek) ever producing a negative shift.
  {
    int const deltaTicks = (pm_ps.commandTime - s_trajPhaseLastCommandTime) / 8;
    s_trajPhaseShift = ((s_trajPhaseShift + deltaTicks) % TRAJECTORY_MAX_FRAMES +
                         TRAJECTORY_MAX_FRAMES) % TRAJECTORY_MAX_FRAMES;
    s_trajPhaseLastCommandTime = pm_ps.commandTime;
  }

  // STATIC's world-pinned phase only makes sense while predicting an upcoming jump/landing -
  // falls back to flowing (phase 0) while grounded, since there's nothing for it to stay pinned
  // to, and only uses the real s_trajPhaseShift while airborne.
  effectivePhaseShift = (pm_ps.groundEntityNum == ENTITYNUM_NONE) ? s_trajPhaseShift : 0;

  // Gather SOLID_BMODEL entities ONCE, shared by every model's simulation below (see the
  // s_trajEntities comment). Wrapped in real trap_Milliseconds() timing, printed alongside the
  // other throttled debug output.
  {
    int const      timeBefore      = shouldDebugTiming ? trap_Milliseconds() : 0;
    // Only gather if at least one active model will actually recompute this frame - see
    // CG_TrajectoryCacheStale. forceRecompute mirrors CG_RunTrajectoryModel's own derivation,
    // computed here too since this needs it before calling into that function.
    qboolean const forceRecompute  = (qboolean)(shouldDebugDump || shouldDebugNextJump ||
                                                  shouldDebugChain || shouldDebugLive);
    qboolean const needsRecompute  =
      ((trajectory_strafe_modes.integer & TRAJECTORY_PASSIVE) &&
        CG_TrajectoryCacheStale(0, forceRecompute, pm_ps.commandTime)) ||
      ((trajectory_strafe_modes.integer & TRAJECTORY_ACCEL) &&
        CG_TrajectoryCacheStale(2, forceRecompute, pm_ps.commandTime)) ||
      ((trajectory_strafe_modes.integer & TRAJECTORY_STRAFEBOT) &&
        CG_TrajectoryCacheStale(1, forceRecompute, pm_ps.commandTime));

    s_trajWorldTraceCount  = 0;
    s_trajEntityTraceCount = 0;
    s_trajAllSolidCount    = 0;

    if (needsRecompute) CG_GatherTrajectoryEntities(shouldDebugEntities);
    if (needsRecompute) CG_GatherTrajectoryTriggers(shouldDebugEntities);

    if (trajectory_strafe_modes.integer & TRAJECTORY_PASSIVE)
    {
      ParseVec(trajectory_rgba_passive.string, rgba, 4);
      CG_RunTrajectoryModel(TRAJECTORY_PASSIVE, frames, step, types, rgba, shouldDebugDump, shouldDebugNextJump, qfalse, qfalse, effectivePhaseShift);
    }
    if (trajectory_strafe_modes.integer & TRAJECTORY_ACCEL)
    {
      // Backward patterns and no-WASD both resolve to ACCEL_REGIME_PASSIVE inside
      // CG_HumanAccelWish itself (see that function), so - same reasoning as strafebot just below -
      // this model always runs and lets its own regime classification do the rest, rather than
      // being skipped here based on a coarser check. shouldDebugChain/shouldDebugLive shared with
      // strafebot's own chain/live dump capability (same s_strafebotDebug fields, populated by
      // CG_HumanAccelWish's percentage branch too) - useful for verifying this model the same way
      // every other patch this session was verified, not just eyeballing it in-game.
      ParseVec(trajectory_rgba_accel.string, rgba, 4);
      CG_RunTrajectoryModel(TRAJECTORY_ACCEL, frames, step, types, rgba, shouldDebugDump, shouldDebugNextJump, shouldDebugChain, shouldDebugLive, effectivePhaseShift);
    }
    if (trajectory_strafe_modes.integer & TRAJECTORY_STRAFEBOT)
    {
      // Backward patterns (BK, or a backward diagonal like BFB/S+D) are no longer skipped: a player
      // may be braking mid-air rather than styling backward, and still wants to see where they'll
      // land. CG_StrafebotWish's own fallback (forwardmove<0 forces wishspeed 0) makes this model
      // numerically identical to the passive model for those patterns, so the model always runs
      // here and the fallback does the rest.
      ParseVec(trajectory_rgba_strafebot.string, rgba, 4);
      CG_RunTrajectoryModel(TRAJECTORY_STRAFEBOT, frames, step, types, rgba, shouldDebugDump, shouldDebugNextJump, shouldDebugChain, shouldDebugLive, effectivePhaseShift);
    }

    if (shouldDebugTiming)
    {
      trap_Print(vaf("traj-timing t=%d ms=%d entities=%d worldTraces=%d entityTraces=%d allSolid=%d\n",
        cg.time, trap_Milliseconds() - timeBefore, s_trajNumEntities,
        s_trajWorldTraceCount, s_trajEntityTraceCount, s_trajAllSolidCount));
    }
  }
}

void draw_player_trajectory(void)
{
  if (!trajectory_strafe_modes.integer) return;

  CG_DrawPlayerTrajectory();
}
