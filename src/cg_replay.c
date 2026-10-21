/*
  ==============================
  Input replay: record the usercmds (keys, buttons, weapon, view angles) and play them back through the
  engine's own buttons (see cg_input.h).

  Start synchronisation
    mdd_replay_record <name>      does `set saveposname <name>; savepos` (defrag), stores the resulting
                                  "placeplayer ..." string in the replay and records from there.
    mdd_replay_start <name>       does `set <name> "placeplayer ..."; vstr <name>` and starts feeding input
                                  as soon as the teleport shows up in a snapshot.
    mdd_replay_telerecord <name>  starts recording with the first usercmd after the next teleporter.
    mdd_replay_telestart <name>   starts feeding input with the first usercmd after the next teleporter.

  Both teleport-synced variants use the same reference point: the first usercmd whose serverTime is newer
  than ps.commandTime of the snapshot that carried the EF_TELEPORT_BIT flip. For recording those commands
  are still in the client's command buffer and get included; for playback they are already sent, so the
  replay skips the matching span to stay time-aligned (on a remote server the first ~ping ms after the
  teleport are therefore your own input - use a local server for exact results).

  View angles
    CL_AdjustAngles turns by 0.001 * cls.frametime * cl_yawspeed * CL_KeyState, and CL_KeyState is
    msec / frame_msec where msec comes from the key down/up timestamps we pass. With a 1ms pulse the
    frametimes cancel and the turn is exactly 0.001 * cl_yawspeed, so cl_yawspeed = delta * 1000 reproduces
    the recorded view angle to the short. Recorded angles are view angles (cmd.angles + delta_angles) so a
    different delta_angles after the teleport doesn't matter.
  ==============================
*/
#include "cg_replay.h"

#include "cg_cvar.h"
#include "cg_input.h"
#include "cg_local.h"
#include "cg_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_DIR   "replays"
#define REPLAY_EXT   ".mdd"
#define REPLAY_MAGIC "mddreplay 1"

#define MAX_REPLAY_CMDS (125 * 60 * 10) // 10 minutes at 125 fps
#define MAX_REPLAY_NAME 64

#define SAVEPOS_TIMEOUT         1000 // ms to wait for defrag's savepos to fill the cvar
#define TELEPORT_EXIT_TOLERANCE 48.f // units; a teleport landing further from the recorded exit is another teleporter
#define FRAME_MSEC              8    // assumed usercmd length for partial key presses

typedef struct
{
  int32_t  time;    // cmd serverTime relative to the first recorded cmd
  int8_t   f, r, u; // forwardmove, rightmove, upmove
  uint16_t buttons;
  uint8_t  weapon;
  uint16_t view[3]; // view angle shorts: cmd.angles + ps.delta_angles
} rcmd_t;

typedef struct
{
  char    name[MAX_REPLAY_NAME];
  char    placeplayer[MAX_STRING_CHARS]; // "placeplayer ..." (record) or "teleporter x y z" (telerecord)
  int32_t num;
  rcmd_t  cmds[MAX_REPLAY_CMDS];
} replay_t;

static replay_t rec;
static replay_t play;

static void copy_str(char* dst, size_t size, char const* src)
{
  size_t n = strlen(src);
  if (n >= size) n = size - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

static vmCvar_t replay_debug;

static cvarTable_t replay_cvars[] = {
  { &replay_debug, "mdd_replay_debug", "0", CVAR_ARCHIVE_ND },
};

//
// per-frame snapshot bookkeeping shared by recording and playback
//
static int32_t  snap_eflags;
static qboolean teleported;     // EF_TELEPORT_BIT flipped in this frame's snapshot
static int32_t  snap_cmd_time;  // ps.commandTime of this frame's snapshot
static int32_t  latest_cmd_num; // trap_GetCurrentCmdNumber()

// serverTime of the first usercmd newer than the snapshot's commandTime (the first post-teleport cmd),
// or cg.time if none is in flight
static int32_t first_inflight_cmd_time(void)
{
  int32_t   t = cg.time;
  usercmd_t cmd;
  for (int32_t n = latest_cmd_num; n > latest_cmd_num - 64; --n)
  {
    if (!trap_GetUserCmd(n, &cmd) || cmd.serverTime <= snap_cmd_time) break;
    t = cmd.serverTime;
  }
  return t;
}

//
// recording
//
typedef enum
{
  REC_IDLE,
  REC_WAIT_SAVEPOS,
  REC_WAIT_TELEPORT,
  REC_RECORDING
} rec_state_t;

static rec_state_t rec_state;
static int32_t     rec_wait_start;
static int32_t     rec_last_cmd_num;
static int32_t     rec_time0;

static void record_cmd(usercmd_t const* cmd, playerState_t const* ps)
{
  if (rec.num >= MAX_REPLAY_CMDS) return;
  rcmd_t* r  = &rec.cmds[rec.num++];
  r->time    = cmd->serverTime - rec_time0;
  r->f       = (int8_t)cmd->forwardmove;
  r->r       = (int8_t)cmd->rightmove;
  r->u       = (int8_t)cmd->upmove;
  r->buttons = (uint16_t)cmd->buttons;
  r->weapon  = (uint8_t)cmd->weapon;
  for (int32_t i = 0; i < 3; ++i) r->view[i] = (uint16_t)((cmd->angles[i] + ps->delta_angles[i]) & 65535);
}

// record every usercmd newer than rec_last_cmd_num (or, when from_time is set, newer than that serverTime)
static void record_new_cmds(playerState_t const* ps, int32_t from_time)
{
  usercmd_t cmd;
  int32_t   first = rec_last_cmd_num + 1;
  if (first < latest_cmd_num - 63) first = latest_cmd_num - 63;
  for (int32_t n = first; n <= latest_cmd_num; ++n)
  {
    if (!trap_GetUserCmd(n, &cmd)) continue;
    if (cmd.serverTime < from_time) continue;
    if (rec.num == 0) rec_time0 = cmd.serverTime;
    record_cmd(&cmd, ps);
  }
  rec_last_cmd_num = latest_cmd_num;
}

static qboolean write_replay(replay_t const* rp)
{
  fileHandle_t f;
  char         path[MAX_QPATH + MAX_REPLAY_NAME];
  snprintf(path, sizeof(path), REPLAY_DIR "/%s" REPLAY_EXT, rp->name);
  if (trap_FS_FOpenFile(path, &f, FS_WRITE) < 0)
  {
    trap_Print(vaf("^1[replay] can't write %s\n", path));
    return qfalse;
  }
  char    line[MAX_STRING_CHARS];
  int32_t len = snprintf(line, sizeof(line), REPLAY_MAGIC "\n%s\n%i\n", rp->placeplayer, rp->num);
  trap_FS_Write(line, len, f);
  for (int32_t i = 0; i < rp->num; ++i)
  {
    rcmd_t const* c = &rp->cmds[i];
    len             = snprintf(
      line,
      sizeof(line),
      "%i %i %i %i %u %u %u %u %u\n",
      c->time,
      c->f,
      c->r,
      c->u,
      c->buttons,
      c->weapon,
      c->view[0],
      c->view[1],
      c->view[2]);
    trap_FS_Write(line, len, f);
  }
  trap_FS_FCloseFile(f);
  trap_Print(
    vaf("^2[replay] wrote %s: %i cmds, %.2f s\n", path, rp->num, rp->num ? rp->cmds[rp->num - 1].time * .001f : 0.f));
  return qtrue;
}

static qboolean read_replay(char const* name, replay_t* rp)
{
  fileHandle_t f;
  char         path[MAX_QPATH + MAX_REPLAY_NAME];
  snprintf(path, sizeof(path), REPLAY_DIR "/%s" REPLAY_EXT, name);
  int32_t const len = trap_FS_FOpenFile(path, &f, FS_READ);
  if (len < 0)
  {
    trap_Print(vaf("^1[replay] can't open %s\n", path));
    return qfalse;
  }
  char* const buf = malloc((size_t)len + 1);
  trap_FS_Read(buf, len, f);
  trap_FS_FCloseFile(f);
  buf[len] = 0;

  qboolean ok = qfalse;
  char*    p  = buf;
  char*    nl = strchr(p, '\n');
  if (nl && !strncmp(p, REPLAY_MAGIC, strlen(REPLAY_MAGIC)))
  {
    copy_str(rp->name, sizeof(rp->name), name);

    p  = nl + 1;
    nl = strchr(p, '\n');
    if (nl)
    {
      *nl = 0;
      copy_str(rp->placeplayer, sizeof(rp->placeplayer), p);
      p = nl + 1;

      int32_t num = (int32_t)strtol(p, &p, 10);
      if (num > MAX_REPLAY_CMDS) num = MAX_REPLAY_CMDS;
      rp->num = 0;
      for (int32_t i = 0; i < num; ++i)
      {
        rcmd_t* c  = &rp->cmds[i];
        c->time    = (int32_t)strtol(p, &p, 10);
        c->f       = (int8_t)strtol(p, &p, 10);
        c->r       = (int8_t)strtol(p, &p, 10);
        c->u       = (int8_t)strtol(p, &p, 10);
        c->buttons = (uint16_t)strtoul(p, &p, 10);
        c->weapon  = (uint8_t)strtoul(p, &p, 10);
        c->view[0] = (uint16_t)strtoul(p, &p, 10);
        c->view[1] = (uint16_t)strtoul(p, &p, 10);
        c->view[2] = (uint16_t)strtoul(p, &p, 10);
        rp->num    = i + 1;
      }
      ok = rp->num > 0;
    }
  }
  free(buf);
  if (!ok) trap_Print(vaf("^1[replay] %s is not a valid replay\n", path));
  return ok;
}

static void rec_begin(char const* name, qboolean teleporter)
{
  if (rec_state != REC_IDLE)
  {
    trap_Print("^3[replay] already recording, use mdd_replay_stoprecord first\n");
    return;
  }
  memset(rec.name, 0, sizeof(rec.name));
  memset(rec.placeplayer, 0, sizeof(rec.placeplayer));
  copy_str(rec.name, sizeof(rec.name), name);
  rec.num          = 0;
  rec_last_cmd_num = latest_cmd_num;
  rec_wait_start   = cg.time;

  if (teleporter)
  {
    rec_state = REC_WAIT_TELEPORT;
    trap_Print(vaf("^2[replay] recording '%s' after the next teleporter\n", rec.name));
  }
  else
  {
    // defrag: `set saveposname X; savepos` puts "placeplayer x y z pitch yaw roll vx vy vz" into cvar X
    trap_Cvar_Set(rec.name, "");
    trap_SendConsoleCommand(vaf("set saveposname %s\nsavepos\n", rec.name));
    rec_state = REC_WAIT_SAVEPOS;
  }
}

static void rec_stop(void)
{
  if (rec_state == REC_IDLE)
  {
    trap_Print("^3[replay] not recording\n");
    return;
  }
  qboolean const had_data = rec_state == REC_RECORDING && rec.num > 0;
  rec_state               = REC_IDLE;
  if (had_data)
    write_replay(&rec);
  else
    trap_Print("^3[replay] nothing recorded\n");
}

static void rec_frame(playerState_t const* ps)
{
  switch (rec_state)
  {
  case REC_IDLE:
    break;

  case REC_WAIT_SAVEPOS:
  {
    char value[MAX_STRING_CHARS];
    trap_Cvar_VariableStringBuffer(rec.name, value, sizeof(value));
    if (!strncmp(value, "placeplayer", 11))
    {
      copy_str(rec.placeplayer, sizeof(rec.placeplayer), value);
      rec_last_cmd_num = latest_cmd_num; // start with the next usercmd
      rec_state        = REC_RECORDING;
      trap_Print(vaf("^2[replay] recording '%s' from: %s\n", rec.name, rec.placeplayer));
    }
    else if (cg.time - rec_wait_start > SAVEPOS_TIMEOUT)
    {
      rec_state = REC_IDLE;
      trap_Print("^1[replay] savepos didn't produce a position (cheats enabled? defrag loaded?)\n");
    }
    break;
  }

  case REC_WAIT_TELEPORT:
    if (teleported)
    {
      int32_t const t0 = first_inflight_cmd_time();
      rec_last_cmd_num = latest_cmd_num - 64; // pull the in-flight cmds out of the buffer too
      record_new_cmds(ps, t0);
      // the snapshot that carried the flip has ps.origin on the teleporter destination (the in-flight cmds
      // haven't been run by the server yet): that identifies the exit for mdd_replay_telestart
      vec3_t const* const o = &getSnap()->ps.origin;
      snprintf(rec.placeplayer, sizeof(rec.placeplayer), "teleporter %.3f %.3f %.3f", (*o)[0], (*o)[1], (*o)[2]);
      rec_state = REC_RECORDING;
      trap_Print(vaf(
        "^2[replay] teleported, recording '%s' (%i cmds were in flight, exit %.0f %.0f %.0f)\n",
        rec.name,
        rec.num,
        (*o)[0],
        (*o)[1],
        (*o)[2]));
    }
    break;

  case REC_RECORDING:
    record_new_cmds(ps, INT32_MIN);
    if (rec.num >= MAX_REPLAY_CMDS)
    {
      trap_Print("^3[replay] buffer full, stopping\n");
      rec_stop();
    }
    break;
  }
}

//
// playback
//
typedef enum
{
  PLAY_IDLE,
  PLAY_WAIT_TELEPORT,
  PLAY_PLAYING
} play_state_t;

static play_state_t play_state;
static int32_t      play_wait_start;
static vec3_t       play_start_pos;     // from the placeplayer string
static qboolean     play_use_start_pos; // also accept "arrived at the start position" as the teleport
static vec3_t       play_exit_pos;      // from the "teleporter x y z" header
static qboolean     play_check_exit;    // telestart: only accept a teleport that lands on play_exit_pos
static int32_t      play_time0;         // serverTime that recorded cmd 0 maps to
static int32_t      play_index;         // last applied recorded cmd
static int32_t      play_weapon;        // -1: no override
static qboolean     play_cvars_saved;
static char         saved_yawspeed[MAX_CVAR_VALUE_STRING];
static char         saved_pitchspeed[MAX_CVAR_VALUE_STRING];
static char         saved_anglespeedkey[MAX_CVAR_VALUE_STRING];

static void play_release(void)
{
  release_all_keys();
  play_weapon = -1;
  if (play_cvars_saved)
  {
    trap_Cvar_Set("cl_yawspeed", saved_yawspeed);
    trap_Cvar_Set("cl_pitchspeed", saved_pitchspeed);
    trap_Cvar_Set("cl_anglespeedkey", saved_anglespeedkey);
    play_cvars_saved = qfalse;
  }
}

static void play_begin(char const* name, qboolean teleporter)
{
  if (play_state != PLAY_IDLE)
  {
    trap_Print("^3[replay] already playing, use mdd_replay_stop first\n");
    return;
  }
  if (!read_replay(name, &play)) return;

  play_use_start_pos = qfalse;
  play_check_exit    = qfalse;
  if (teleporter)
  {
    play_check_exit =
      sscanf(play.placeplayer, "teleporter %f %f %f", &play_exit_pos[0], &play_exit_pos[1], &play_exit_pos[2]) == 3;
    if (play_check_exit)
      trap_Print(vaf(
        "^2[replay] playing '%s' (%i cmds) after the teleporter exiting at %.0f %.0f %.0f\n",
        play.name,
        play.num,
        play_exit_pos[0],
        play_exit_pos[1],
        play_exit_pos[2]));
    else
      trap_Print(vaf("^2[replay] playing '%s' after the next teleporter (%i cmds)\n", play.name, play.num));
  }
  else
  {
    if (strncmp(play.placeplayer, "placeplayer", 11))
    {
      trap_Print("^1[replay] this replay has no start position (recorded with telerecord), use mdd_replay_telestart\n");
      return;
    }
    trap_SendConsoleCommand(vaf("set %s \"%s\"\nvstr %s\n", play.name, play.placeplayer, play.name));
    trap_Print(vaf("^2[replay] playing '%s' (%i cmds) from: %s\n", play.name, play.num, play.placeplayer));
    // in case placeplayer doesn't flip EF_TELEPORT_BIT: arriving at the saved spot counts too, unless we are
    // already standing there (then only the flag can tell us the teleport happened)
    play_use_start_pos =
      sscanf(play.placeplayer, "placeplayer %f %f %f", &play_start_pos[0], &play_start_pos[1], &play_start_pos[2]) ==
        3 &&
      Distance(getSnap()->ps.origin, play_start_pos) > 8;
  }
  play_wait_start = cg.time;
  play_state      = PLAY_WAIT_TELEPORT;
}

static void play_stop(char const* why)
{
  if (play_state == PLAY_IDLE) return;
  play_state = PLAY_IDLE;
  play_release();
  trap_Print(vaf("^2[replay] playback %s\n", why));
}

static void play_start_now(void)
{
  play_time0  = first_inflight_cmd_time();
  play_index  = -1;
  play_weapon = -1;
  play_state  = PLAY_PLAYING;

  trap_Cvar_VariableStringBuffer("cl_yawspeed", saved_yawspeed, sizeof(saved_yawspeed));
  trap_Cvar_VariableStringBuffer("cl_pitchspeed", saved_pitchspeed, sizeof(saved_pitchspeed));
  trap_Cvar_VariableStringBuffer("cl_anglespeedkey", saved_anglespeedkey, sizeof(saved_anglespeedkey));
  play_cvars_saved = qtrue;
  trap_Cvar_Set("cl_anglespeedkey", "1");

  if (replay_debug.integer)
    trap_Print(vaf(
      "^3[replay] start: cmd 0 = serverTime %i, next cmd %i (skipping %i ms)\n",
      play_time0,
      cg.time,
      cg.time - play_time0));
}

// hold / pulse a +/- key pair for one recorded axis value
static void apply_axis(int8_t value, int8_t prev, int32_t full, vkey_t pos, vkey_t neg)
{
  int32_t const mag = abs(value);
  if (mag == 0)
  {
    set_key(pos, qfalse);
    set_key(neg, qfalse);
    return;
  }
  vkey_t const k = value > 0 ? pos : neg;
  set_key(value > 0 ? neg : pos, qfalse);
  if (mag < full && prev == 0)
  {
    // the key was pressed part-way through this frame: CL_KeyState(msec / frame_msec)
    int32_t msec = (mag * FRAME_MSEC + full / 2) / full;
    if (msec < 1) msec = 1;
    if (msec > FRAME_MSEC - 1) msec = FRAME_MSEC - 1;
    set_key(k, qfalse);
    pulse_key(k, msec);
  }
  else
  {
    set_key(k, qtrue);
  }
}

static void apply_angles(rcmd_t const* c, playerState_t const* ps)
{
  usercmd_t cur;
  if (!trap_GetUserCmd(latest_cmd_num, &cur)) return;

  for (int32_t i = 0; i < 2; ++i) // pitch, yaw
  {
    int32_t const target = (c->view[i] - ps->delta_angles[i]) & 65535;
    int32_t       d      = (target - cur.angles[i]) & 65535;
    if (d >= 32768) d -= 65536;
    if (d == 0) continue;
    // CL_AdjustAngles: angle += 0.001 * frametime * speed * (msec / frame_msec); with msec = 1 the frame
    // times cancel: angle += 0.001 * speed
    float const degrees = (float)d * (360.f / 65536.f);
    if (i == YAW)
    {
      trap_Cvar_Set("cl_yawspeed", vaf("%.6f", (d > 0 ? degrees : -degrees) * 1000.f));
      pulse_key(d > 0 ? K_LEFT : K_RIGHT, 1); // +left increases yaw
    }
    else
    {
      trap_Cvar_Set("cl_pitchspeed", vaf("%.6f", (d > 0 ? degrees : -degrees) * 1000.f));
      pulse_key(d > 0 ? K_LOOKDOWN : K_LOOKUP, 1); // +lookdown increases pitch
    }
  }
}

static void play_frame(playerState_t const* ps)
{
  switch (play_state)
  {
  case PLAY_IDLE:
    break;

  case PLAY_WAIT_TELEPORT:
    if (teleported && play_check_exit)
    {
      // only the teleporter we recorded from: the flip snapshot's ps.origin sits on the destination
      float const d = Distance(getSnap()->ps.origin, play_exit_pos);
      if (d > TELEPORT_EXIT_TOLERANCE)
      {
        trap_Print(vaf("^3[replay] teleported, but %.0f units from the recorded exit - still waiting\n", d));
        play_wait_start = cg.time;
        break;
      }
    }
    if (teleported || (play_use_start_pos && Distance(getSnap()->ps.origin, play_start_pos) < 8))
    {
      play_start_now();
    }
    else if (play_use_start_pos && cg.time - play_wait_start > 10000)
    {
      play_stop("aborted: the start position teleport didn't happen within 10 s (cheats enabled?)");
    }
    break;

  case PLAY_PLAYING:
  {
    // the usercmd our input lands in has serverTime == cg.time; pick the recorded cmd for that time
    int32_t const elapsed = cg.time - play_time0;
    int32_t       idx     = play_index;
    while (idx + 1 < play.num && play.cmds[idx + 1].time <= elapsed) ++idx;
    if (idx < 0) break; // not yet at the first recorded cmd
    if (elapsed > play.cmds[play.num - 1].time)
    {
      play_stop("finished");
      break;
    }

    rcmd_t const* const c    = &play.cmds[idx];
    rcmd_t const* const prev = idx > 0 ? &play.cmds[idx - 1] : NULL;
    play_index               = idx;

    qboolean const walking = (c->buttons & BUTTON_WALKING) != 0;
    int32_t const  full    = walking ? 64 : 127;
    set_key(K_SPEED, walking == (cvar_getInteger("cl_run") != 0)); // CL_KeyMove: walking = !(in_speed ^ cl_run)
    apply_axis(c->f, prev ? prev->f : 0, full, K_FORWARD, K_BACK);
    apply_axis(c->r, prev ? prev->r : 0, full, K_MOVERIGHT, K_MOVELEFT);
    apply_axis(c->u, prev ? prev->u : 0, full, K_MOVEUP, K_MOVEDOWN);
    set_key(K_ATTACK, (c->buttons & BUTTON_ATTACK) != 0);

    apply_angles(c, ps);

    if (play_weapon != c->weapon)
    {
      play_weapon = c->weapon;                                // exact: CG_SETUSERCMDVALUE override
      trap_SendConsoleCommand(vaf("weapon %i\n", c->weapon)); // keep the defrag cgame's selection in sync
    }
    break;
  }
  }
}

//
// entry points
//
int32_t replay_usercmd_weapon(int32_t weapon)
{
  return play_state == PLAY_PLAYING && play_weapon >= 0 ? play_weapon : weapon;
}

qboolean replay_console_command(char const* cmd)
{
  char arg[MAX_REPLAY_NAME];
  if (Q_stricmp(cmd, "mdd_replay_stoprecord") == 0)
  {
    rec_stop();
    return qtrue;
  }
  if (Q_stricmp(cmd, "mdd_replay_stop") == 0)
  {
    if (play_state == PLAY_IDLE)
      trap_Print("^3[replay] not playing\n");
    else
      play_stop("stopped");
    return qtrue;
  }

  qboolean const is_record     = Q_stricmp(cmd, "mdd_replay_record") == 0;
  qboolean const is_telerecord = Q_stricmp(cmd, "mdd_replay_telerecord") == 0;
  qboolean const is_start      = Q_stricmp(cmd, "mdd_replay_start") == 0;
  qboolean const is_telestart  = Q_stricmp(cmd, "mdd_replay_telestart") == 0;
  if (!is_record && !is_telerecord && !is_start && !is_telestart) return qfalse;

  if (trap_Argc() != 2)
  {
    trap_Print(vaf("usage: %s <name>\n", cmd));
    return qtrue;
  }
  trap_Argv(1, arg, sizeof(arg));
  for (char* p = arg; *p; ++p)
  {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
    {
      trap_Print("^1[replay] name may only contain letters, digits, _ and -\n");
      return qtrue;
    }
  }

  if (is_record) rec_begin(arg, qfalse);
  if (is_telerecord) rec_begin(arg, qtrue);
  if (is_start) play_begin(arg, qfalse);
  if (is_telestart) play_begin(arg, qtrue);
  return qtrue;
}

void init_replay(void)
{
  init_cvars(replay_cvars, ARRAY_LEN(replay_cvars));
  rec_state        = REC_IDLE;
  play_state       = PLAY_IDLE;
  play_weapon      = -1;
  play_cvars_saved = qfalse;
  snap_eflags      = 0;
  teleported       = qfalse;

  static char const* const commands[] = {
    "mdd_replay_record", "mdd_replay_telerecord", "mdd_replay_stoprecord",
    "mdd_replay_start",  "mdd_replay_telestart",  "mdd_replay_stop",
  };
  for (size_t i = 0; i < ARRAY_LEN(commands); ++i) trap_AddCommand(commands[i]);
}

void del_replay(void)
{
  if (rec_state == REC_RECORDING && rec.num > 0) write_replay(&rec);
  rec_state = REC_IDLE;
  if (play_state != PLAY_IDLE)
  {
    play_state = PLAY_IDLE;
    play_release();
  }
}

void run_replay(void)
{
  update_cvars(replay_cvars, ARRAY_LEN(replay_cvars));

  snapshot_t const* const    snap = getSnap();
  playerState_t const* const ps   = getPs();

  latest_cmd_num = trap_GetCurrentCmdNumber();
  snap_cmd_time  = snap->ps.commandTime;
  teleported     = ((snap->ps.eFlags ^ snap_eflags) & EF_TELEPORT_BIT) != 0;
  snap_eflags    = snap->ps.eFlags;

  if (cg.demoPlayback || ps->pm_type != PM_NORMAL || (ps->pm_flags & PMF_FOLLOW))
  {
    if (play_state != PLAY_IDLE) play_stop("stopped: not controlling a player");
    return;
  }

  rec_frame(ps);
  play_frame(ps);
}
