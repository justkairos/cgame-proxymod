#include "bg_pmove.h"

#include "cg_local.h"

#include <stdlib.h>

/*
============
PM_CmdScale

Returns the scale factor to apply to cmd movements
This allows the clients to use axial -127 to 127 values for all directions
without getting a sqrt(2) distortion in speed.
============
*/
float PM_CmdScale(playerState_t const* pm_ps, usercmd_t const* cmd)
{
  int32_t max = abs(cmd->forwardmove);
  if (abs(cmd->rightmove) > max)
  {
    max = abs(cmd->rightmove);
  }
  if (abs(cmd->upmove) > max)
  {
    max = abs(cmd->upmove);
  }
  if (!max)
  {
    return 0;
  }

  float const total =
    sqrtf((float)(cmd->forwardmove * cmd->forwardmove + cmd->rightmove * cmd->rightmove + cmd->upmove * cmd->upmove));
  return (float)pm_ps->speed * max / (127.f * total);
}

/* PM_CmdScale without upmove */
float PM_AltCmdScale(playerState_t const* pm_ps, usercmd_t const* cmd)
{
  int32_t max = abs(cmd->forwardmove);
  if (abs(cmd->rightmove) > max)
  {
    max = abs(cmd->rightmove);
  }
  if (!max)
  {
    return 0;
  }

  float const total = sqrtf((float)(cmd->forwardmove * cmd->forwardmove + cmd->rightmove * cmd->rightmove));
  return (float)pm_ps->speed * max / (127.f * total);
}

/*
=============
PM_CheckJump
=============
*/
qboolean PM_CheckJump(pmove_t* pm, playerState_t* pm_ps, pml_t* pml)
{
  if (pm_ps->pm_flags & PMF_RESPAWNED)
  {
    return qfalse; // don't allow jump until all buttons are up
  }

  if (pm->cmd.upmove < 10)
  {
    // not holding jump
    return qfalse;
  }

  // must wait for jump to be released
  if (pm_ps->pm_flags & PMF_JUMP_HELD)
  {
    // clear upmove so cmdscale doesn't lower running speed
    pm->cmd.upmove = 0;
    return qfalse;
  }

  pml->groundPlane = qfalse; // jumping away
  pml->walking     = qfalse;
  pm_ps->pm_flags |= PMF_JUMP_HELD;

  pm_ps->groundEntityNum = ENTITYNUM_NONE;

  // CPM (PMF_PROMODE) double jump / ramp jump - verified against DF_Reverse_Stuff's disassembled
  // real bg_pmove.c, not the generic cpm1_dev_docs tutorial (which only describes a flat +100ups
  // bonus and misses the stacking behavior entirely). Two independent effects, both PROMODE-only:
  //  (a) if velocity[2] is still positive (rising - either mid-jump-arc, e.g. jumping again
  //      before the apex, or off a ramp's upward slope) the new jump STACKS via += instead of
  //      overwriting, so a "ramp jump" keeps the existing upward speed rather than resetting it;
  //  (b) STAT_JUMPTIME is a 400ms window (set below, ticked down elsewhere - see
  //      player_trajectory.c's PM_DropTimers) - jumping again while it's still running (the
  //      actual "double jump") adds a further flat +100ups on top of whichever branch (a) took,
  //      e.g. JUMP_VELOCITY(270)+100=370 for a flat double jump, or more when stacked on a ramp.
  if (pm_ps->velocity[2] <= 0.f || !(pm_ps->pm_flags & PMF_PROMODE))
  {
    pm_ps->velocity[2] = JUMP_VELOCITY;
  }
  else
  {
    pm_ps->velocity[2] += JUMP_VELOCITY;
  }

  if (pm_ps->pm_flags & PMF_PROMODE)
  {
    if (pm_ps->stats[STAT_JUMPTIME] > 0)
    {
      pm_ps->velocity[2] += 100.f;
      pm_ps->stats[STAT_DJING] = 1;
    }
    pm_ps->stats[STAT_JUMPTIME] = 400;
  }

  return qtrue;
}

/*
=============
PM_CorrectAllSolid
=============
*/
static qboolean PM_CorrectAllSolid(pmove_t* pm, playerState_t* pm_ps, pml_t* pml, trace_t* trace)
{
  vec3_t point;

  // if (pm->debugLevel)
  // {
  //   Com_Printf("%i:allsolid\n", c_pmove);
  // }

  // jitter around
  for (int8_t i = -1; i <= 1; ++i)
  {
    for (int8_t j = -1; j <= 1; ++j)
    {
      for (int8_t k = -1; k <= 1; ++k)
      {
        VectorCopy(pm_ps->origin, point);
        point[0] += (float)i;
        point[1] += (float)j;
        point[2] += (float)k;
        trap_CM_BoxTrace(trace, point, point, pm->mins, pm->maxs, 0, pm->tracemask);
        if (!trace->allsolid)
        {
          point[0] = pm_ps->origin[0];
          point[1] = pm_ps->origin[1];
          point[2] = pm_ps->origin[2] - .25f;

          trap_CM_BoxTrace(trace, pm_ps->origin, point, pm->mins, pm->maxs, 0, pm->tracemask);
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

/*
=============
PM_GroundTraceMissed

The ground trace didn't hit a surface, so we are in freefall
=============
*/
static void PM_GroundTraceMissed(playerState_t* pm_ps, pml_t* pml)
{
  pm_ps->groundEntityNum = ENTITYNUM_NONE;
  pml->groundPlane       = qfalse;
  pml->walking           = qfalse;
}

/*
=============
PM_GroundTrace
=============
*/
void PM_GroundTrace(pmove_t* pm, playerState_t* pm_ps, pml_t* pml)
{
  vec3_t  point;
  trace_t trace;

  point[0] = pm_ps->origin[0];
  point[1] = pm_ps->origin[1];
  point[2] = pm_ps->origin[2] - .25f;

  trap_CM_BoxTrace(&trace, pm_ps->origin, point, pm->mins, pm->maxs, 0, pm->tracemask);
  pml->groundTrace = trace;

  // do something corrective if the trace starts in a solid...
  if (trace.allsolid)
  {
    if (!PM_CorrectAllSolid(pm, pm_ps, pml, &trace)) return;
  }

  // if the trace didn't hit anything, we are in free fall
  if (trace.fraction == 1.f)
  {
    PM_GroundTraceMissed(pm_ps, pml);
    return;
  }

  // check if getting thrown off the ground
  if (pm_ps->velocity[2] > 0 && DotProduct(pm_ps->velocity, trace.plane.normal) > 10)
  {
    // if ( pm->debugLevel ) {
    //   Com_Printf("%i:kickoff\n", c_pmove);
    // }
    pm_ps->groundEntityNum = ENTITYNUM_NONE;
    pml->groundPlane       = qfalse;
    pml->walking           = qfalse;
    return;
  }

  // slopes that are too steep will not be considered onground
  if (trace.plane.normal[2] < MIN_WALK_NORMAL)
  {
    // if ( pm->debugLevel ) {
    //   Com_Printf("%i:steep\n", c_pmove);
    // }
    // FIXME: if they can't slide down the slope, let them
    // walk (sharp crevices)
    pm_ps->groundEntityNum = ENTITYNUM_NONE;
    pml->groundPlane       = qtrue;
    pml->walking           = qfalse;
    return;
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
    // just hit the ground
    // if ( pm->debugLevel ) {
    //   Com_Printf("%i:Land\n", c_pmove);
    // }

    // PM_CrashLand();

    // don't do landing time if we were just going down a slope
    if (pml->previous_velocity[2] < -200)
    {
      // don't allow another jump for a little while
      pm_ps->pm_flags |= PMF_TIME_LAND;
      pm_ps->pm_time = 250;
    }
  }

  pm_ps->groundEntityNum = trace.entityNum;

  // don't reset the z velocity for slopes
  // pm.ps->velocity[2] = 0;

  // PM_AddTouchEnt(trace.entityNum);
}

/*
=============
PM_SetWaterLevel  FIXME: avoid this twice?  certainly if not moving
=============
*/
void PM_SetWaterLevel(pmove_t* pm, playerState_t* pm_ps)
{
  vec3_t point;
  int    cont;
  int    sample1;
  int    sample2;

  // get waterlevel, accounting for ducking
  pm->waterlevel = 0;
  pm->watertype  = 0;

  point[0] = pm_ps->origin[0];
  point[1] = pm_ps->origin[1];
  point[2] = pm_ps->origin[2] + MINS_Z + 1;

  cont = trap_CM_PointContents(point, 0);

  if (cont & MASK_WATER)
  {
    sample2 = pm_ps->viewheight - MINS_Z;
    sample1 = sample2 / 2;

    pm->watertype  = cont;
    pm->waterlevel = 1;
    point[2]       = pm_ps->origin[2] + MINS_Z + sample1;
    cont           = trap_CM_PointContents(point, 0);
    if (cont & MASK_WATER)
    {
      pm->waterlevel = 2;
      point[2]       = pm_ps->origin[2] + MINS_Z + sample2;
      cont           = trap_CM_PointContents(point, 0);
      if (cont & MASK_WATER)
      {
        pm->waterlevel = 3;
      }
    }
  }
}

/*
==============
PM_CheckDuck

Sets mins, maxs, and pm_ps.viewheight
==============
*/
void PM_CheckDuck(pmove_t* pm, playerState_t* pm_ps)
{
  trace_t trace;

  if (pm_ps->powerups[PW_INVULNERABILITY])
  {
    if (pm_ps->pm_flags & PMF_INVULEXPAND)
    {
      // invulnerability sphere has a 42 units radius
      VectorSet(pm->mins, -42, -42, -42);
      VectorSet(pm->maxs, 42, 42, 42);
    }
    else
    {
      VectorSet(pm->mins, -15, -15, MINS_Z);
      VectorSet(pm->maxs, 15, 15, 16);
    }
    pm_ps->pm_flags |= PMF_DUCKED;
    pm_ps->viewheight = CROUCH_VIEWHEIGHT;
    return;
  }
  pm_ps->pm_flags &= ~PMF_INVULEXPAND;

  pm->mins[0] = -15;
  pm->mins[1] = -15;

  pm->maxs[0] = 15;
  pm->maxs[1] = 15;

  pm->mins[2] = MINS_Z;

  if (pm_ps->pm_type == PM_DEAD)
  {
    pm->maxs[2]       = -8;
    pm_ps->viewheight = DEAD_VIEWHEIGHT;
    return;
  }

  if (pm->cmd.upmove < 0)
  { // duck
    pm_ps->pm_flags |= PMF_DUCKED;
  }
  else
  { // stand up if possible
    if (pm_ps->pm_flags & PMF_DUCKED)
    {
      // try to stand up
      pm->maxs[2] = 32;
      trap_CM_BoxTrace(&trace, pm_ps->origin, pm_ps->origin, pm->mins, pm->maxs, 0, pm->tracemask);
      if (!trace.allsolid) pm_ps->pm_flags &= ~PMF_DUCKED;
    }
  }

  if (pm_ps->pm_flags & PMF_DUCKED)
  {
    pm->maxs[2]       = 16;
    pm_ps->viewheight = CROUCH_VIEWHEIGHT;
  }
  else
  {
    pm->maxs[2]       = 32;
    pm_ps->viewheight = DEFAULT_VIEWHEIGHT;
  }
}
