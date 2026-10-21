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
#define MAX_REPLAY_NAME 128

#define SAVEPOS_TIMEOUT         1000 // ms to wait for defrag's savepos to fill the cvar
#define TELEPORT_EXIT_TOLERANCE 48.f // units; a teleport landing further from the recorded exit is another teleporter
#define FRAME_MSEC              8    // assumed usercmd length for partial key presses
#define TELEPORT_LATCH          500  // ms a seen teleport flip stays valid for a waiting recorder/player

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
  int32_t t0; // absolute serverTime of cmd 0 (for the pmove_fixed grid phase)
  rcmd_t  cmds[MAX_REPLAY_CMDS];
} replay_t;

static replay_t rec;
static replay_t play;

// Replay names end up in a file name (replays/<name>.mdd), in a cvar name (savepos) and in console commands,
// so: no path separators, nothing the console or cvar parser chokes on, no spaces, printable ASCII only.
// Everything else goes, so defrag's autorecord demo names (brackets, dots, parentheses) work as-is.
static qboolean valid_replay_name(char const* name)
{
  if (!name[0] || name[0] == '.' || name[0] == '-' || name[0] == '+')
  {
    trap_Print("^1[replay] invalid name\n");
    return qfalse;
  }
  for (char const* p = name; *p; ++p)
  {
    if (*p <= ' ' || *p > '~' || strchr("/\\:*?\"<>|;", *p))
    {
      trap_Print("^1[replay] name can't contain spaces, control characters or any of / \\ : * ? \" < > | ;\n");
      return qfalse;
    }
  }
  return qtrue;
}

static void copy_str(char* dst, size_t size, char const* src)
{
  size_t n = strlen(src);
  if (n >= size) n = size - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

static vmCvar_t replay_debug;
// mdd_replay_convert survives the cgame reload that demo playback causes through these: pending = "convert"
// and the demo name
static vmCvar_t replay_pending;
static vmCvar_t replay_pending_name;
static vmCvar_t replay_pending_timescale; // the user's timescale, put back after the conversion
static vmCvar_t replay_convert_timescale; // timescale used while the demo plays for conversion

static cvarTable_t replay_cvars[] = {
  { &replay_debug, "mdd_replay_debug", "0", CVAR_ARCHIVE_ND },
  { &replay_pending, "mdd_replay_pending", "", CVAR_TEMP },
  { &replay_pending_name, "mdd_replay_pending_name", "", CVAR_TEMP },
  { &replay_pending_timescale, "mdd_replay_pending_timescale", "", CVAR_TEMP },
  { &replay_convert_timescale, "mdd_replay_convert_timescale", "10", CVAR_ARCHIVE_ND },
};

//
// per-frame snapshot bookkeeping shared by recording and playback
//
static int32_t  snap_eflags;
static qboolean snap_seen;  // at least one snapshot has been looked at (the first one can't "flip")
static qboolean teleported; // EF_TELEPORT_BIT flipped recently (latched until consumed or TELEPORT_LATCH ms)
static int32_t  teleported_at;
static int32_t  teleported_cmd_time; // ps.commandTime of the snapshot that carried the flip
static int32_t  snap_cmd_time;       // ps.commandTime of this frame's snapshot
static int32_t  latest_cmd_num;      // trap_GetCurrentCmdNumber()

static void consume_teleport(void)
{
  teleported = qfalse;
}

// serverTime of the first usercmd newer than the snapshot's commandTime (the first post-teleport cmd),
// or cg.time if none is in flight
static int32_t first_inflight_cmd_time(void)
{
  // if a latched flip is being consumed a frame or two late, measure against the snapshot that carried it
  int32_t const ref = teleported ? teleported_cmd_time : snap_cmd_time;
  int32_t       t   = cg.time;
  usercmd_t     cmd;
  for (int32_t n = latest_cmd_num; n > latest_cmd_num - 64; --n)
  {
    if (!trap_GetUserCmd(n, &cmd) || cmd.serverTime <= ref) break;
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
static char        saved_saveposname[MAX_CVAR_VALUE_STRING]; // the user's saveposname, restored after savepos
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
    if (rec.num == 0) rec_time0 = rec.t0 = cmd.serverTime;
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
  int32_t len = snprintf(line, sizeof(line), REPLAY_MAGIC "\n%s\n%i %i\n", rp->placeplayer, rp->num, rp->t0);
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
      rp->t0      = (int32_t)strtol(p, &p, 10); // 0 in files written before the grid phase was stored
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
  consume_teleport(); // don't trigger on a flip that happened before the command

  if (teleporter)
  {
    rec_state = REC_WAIT_TELEPORT;
    trap_Print(vaf("^2[replay] recording '%s' after the next teleporter\n", rec.name));
  }
  else
  {
    // defrag: `set saveposname X; savepos` puts "placeplayer x y z pitch yaw roll vx vy vz" into cvar X
    trap_Cvar_VariableStringBuffer("saveposname", saved_saveposname, sizeof(saved_saveposname));
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
      trap_Cvar_Set("saveposname", saved_saveposname);
      copy_str(rec.placeplayer, sizeof(rec.placeplayer), value);
      rec_last_cmd_num = latest_cmd_num; // start with the next usercmd
      rec_state        = REC_RECORDING;
      trap_Print(vaf("^2[replay] recording '%s' from: %s\n", rec.name, rec.placeplayer));
    }
    else if (cg.time - rec_wait_start > SAVEPOS_TIMEOUT)
    {
      trap_Cvar_Set("saveposname", saved_saveposname);
      rec_state = REC_IDLE;
      trap_Print("^1[replay] savepos didn't produce a position (defrag loaded?)\n");
    }
    break;
  }

  case REC_WAIT_TELEPORT:
    if (teleported)
    {
      int32_t const t0 = first_inflight_cmd_time();
      consume_teleport();
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

// value of a key in the systeminfo configstring (CS_SYSTEMINFO == 1), "" if absent
static char const* systeminfo_value(char const* key)
{
  static char  value[MAX_CVAR_VALUE_STRING];
  char const*  s    = CG_ConfigString(1);
  size_t const klen = strlen(key);
  value[0]          = 0;
  while ((s = strchr(s, '\\')) != NULL)
  {
    ++s;
    char const* const eq = strchr(s, '\\');
    if (!eq) break;
    if ((size_t)(eq - s) == klen && !Q_stricmpn(s, key, (int)klen))
    {
      char const* const end = strchr(eq + 1, '\\');
      size_t            n   = end ? (size_t)(end - eq - 1) : strlen(eq + 1);
      if (n >= sizeof(value)) n = sizeof(value) - 1;
      memcpy(value, eq + 1, n);
      value[n] = 0;
      return value;
    }
    s = eq;
  }
  return value;
}

// With pmove_fixed the server rounds every usercmd's serverTime UP to the pmove_msec grid, drops a cmd
// that rounds onto an already-run slot and runs the next cmd for the whole gap if a slot was skipped. So
// what matters is "input per grid slot", not "input per cmd". Both the recording and the playback are
// expressed as slot offsets from their own first cmd and matched on those.
static qboolean play_fixed;
static int32_t  play_msec;

static int32_t ceil_to_grid(int32_t t)
{
  return ((t + play_msec - 1) / play_msec) * play_msec;
}

typedef enum
{
  PLAY_IDLE,
  PLAY_WAIT_TELEPORT,
  PLAY_PLAYING
} play_state_t;

#define MAX_CHAIN 16
static char    play_chain[MAX_CHAIN][MAX_REPLAY_NAME]; // replays to play back to back
static int32_t play_chain_num;
static int32_t play_chain_pos;

static play_state_t play_state;
static int32_t      play_wait_start;
static vec3_t       play_start_pos;     // from the placeplayer string
static qboolean     play_use_start_pos; // also accept "arrived at the start position" as the teleport
static vec3_t       play_exit_pos;      // from the "teleporter x y z" header
static qboolean     play_check_exit;    // telestart: only accept a teleport that lands on play_exit_pos
static qboolean     play_prehold;       // hold cmd 0's keys while waiting so the in-flight cmds carry them
static int32_t      play_time0;         // serverTime that recorded cmd 0 maps to
static int32_t      play_index;         // last applied recorded cmd
static int32_t      play_weapon;        // -1: no override
static qboolean     play_cvars_saved;
static char         saved_yawspeed[MAX_CVAR_VALUE_STRING];
static char         saved_pitchspeed[MAX_CVAR_VALUE_STRING];
static char         saved_anglespeedkey[MAX_CVAR_VALUE_STRING];

static void play_stop(char const* why);
static void play_start_now(qboolean sync_to_teleport);

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

// first replay of a chain: decides how playback is synchronised
static void play_begin(qboolean teleporter)
{
  if (play_state != PLAY_IDLE)
  {
    trap_Print("^3[replay] already playing, use mdd_replay_stop first\n");
    return;
  }
  play_chain_pos = 0;
  if (!read_replay(play_chain[0], &play)) return;
  consume_teleport(); // don't trigger on a flip that happened before the command
  if (play_chain_num > 1) trap_Print(vaf("^2[replay] chain of %i replays\n", play_chain_num));

  play_use_start_pos = qfalse;
  play_check_exit    = qfalse;
  play_prehold       = qfalse;
  if (!strncmp(play.placeplayer, "spawn", 5))
  {
    // converted from a demo: starts at the spawn, so kill and sync on the respawn. The spawn origin (when the
    // file has it) doubles as a fallback in case the respawn doesn't flip EF_TELEPORT_BIT on this server.
    trap_SendConsoleCommand("kill\n");
    play_use_start_pos =
      sscanf(play.placeplayer, "spawn %f %f %f", &play_start_pos[0], &play_start_pos[1], &play_start_pos[2]) == 3 &&
      Distance(getSnap()->ps.origin, play_start_pos) > 8;
    trap_Print(vaf("^2[replay] playing '%s' (%i cmds) from the respawn\n", play.name, play.num));
    play_prehold = qtrue;
  }
  else if (teleporter)
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
  else if (!strncmp(play.placeplayer, "placeplayer", 11))
  {
    if (!atoi(systeminfo_value("sv_cheats")))
    {
      // placeplayer is a cheat: just feed the input from wherever we are, right now
      trap_Print(vaf("^2[replay] cheats are off, playing '%s' (%i cmds) from here\n", play.name, play.num));
      play_start_now(qfalse);
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
    play_prehold = qtrue;
  }
  else
  {
    // telerecord replay started with mdd_replay_start: no position to go to, play from here
    trap_Print(vaf("^2[replay] playing '%s' (%i cmds) from here\n", play.name, play.num));
    play_start_now(qfalse);
    return;
  }
  play_wait_start = cg.time;
  play_state      = PLAY_WAIT_TELEPORT;
}

// the current replay ran out: continue straight into the next one of the chain, its cmd 0 landing in the
// usercmd we are about to build. No placeplayer / teleport waiting for chained replays.
static qboolean play_advance(void)
{
  if (play_chain_pos + 1 >= play_chain_num) return qfalse;
  ++play_chain_pos;
  if (!read_replay(play_chain[play_chain_pos], &play))
  {
    play_stop("aborted: next replay of the chain is missing");
    return qfalse;
  }
  play_time0 = cg.time;
  play_index = -1;
  trap_Print(vaf("^2[replay] -> '%s' (%i cmds, %i/%i)\n", play.name, play.num, play_chain_pos + 1, play_chain_num));
  return qtrue;
}

static void play_stop(char const* why)
{
  if (play_state == PLAY_IDLE) return;
  play_state = PLAY_IDLE;
  play_release();
  trap_Print(vaf("^2[replay] playback %s\n", why));
}

static void play_start_now(qboolean sync_to_teleport)
{
  play_time0  = sync_to_teleport ? first_inflight_cmd_time() : cg.time;
  play_index  = -1;
  play_weapon = -1;
  play_state  = PLAY_PLAYING;

  play_fixed = atoi(systeminfo_value("pmove_fixed")) != 0 || cvar_getInteger("pmove_fixed") != 0;
  play_msec  = atoi(systeminfo_value("pmove_msec"));
  if (play_msec < 8) play_msec = 8;
  if (play_msec > 33) play_msec = 33;
  if (play_fixed && !play.t0)
  {
    trap_Print("^3[replay] old replay without grid phase, falling back to time-based matching\n");
    play_fixed = qfalse;
  }

  trap_Cvar_VariableStringBuffer("cl_yawspeed", saved_yawspeed, sizeof(saved_yawspeed));
  trap_Cvar_VariableStringBuffer("cl_pitchspeed", saved_pitchspeed, sizeof(saved_pitchspeed));
  trap_Cvar_VariableStringBuffer("cl_anglespeedkey", saved_anglespeedkey, sizeof(saved_anglespeedkey));
  play_cvars_saved = qtrue;
  trap_Cvar_Set("cl_anglespeedkey", "1");

  trap_Print(vaf(
    "^2[replay] start: cmd 0 = serverTime %i, next cmd %i (skipping %i ms), %s\n",
    play_time0,
    cg.time,
    cg.time - play_time0,
    play_fixed ? vaf("pmove_fixed grid %i ms", play_msec) : "time-based matching (pmove_fixed is off)"));
  if (atoi(systeminfo_value("g_synchronousClients")))
    trap_Print(
      "^1[replay] g_synchronousClients is 1: the server samples one of your commands per server frame by "
      "wall-clock timing, so playback can't be exact. Use pmove_fixed 1 / g_synchronousClients 0, also "
      "offline.\n");
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

static void apply_movement(rcmd_t const* c, rcmd_t const* prev)
{
  qboolean const walking = (c->buttons & BUTTON_WALKING) != 0;
  int32_t const  full    = walking ? 64 : 127;
  set_key(K_SPEED, walking == (cvar_getInteger("cl_run") != 0)); // CL_KeyMove: walking = !(in_speed ^ cl_run)
  apply_axis(c->f, prev ? prev->f : 0, full, K_FORWARD, K_BACK);
  apply_axis(c->r, prev ? prev->r : 0, full, K_MOVERIGHT, K_MOVELEFT);
  apply_axis(c->u, prev ? prev->u : 0, full, K_MOVEUP, K_MOVEDOWN);
  set_key(K_ATTACK, (c->buttons & BUTTON_ATTACK) != 0);
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
    // The usercmds already in flight when the teleport shows up are skipped by the replay (they were sent
    // before we could know). For a start we triggered ourselves (kill / placeplayer) hold cmd 0's keys
    // meanwhile, so those cmds carry the right input instead of none - a strafe run starts on cmd 0.
    if (play_prehold && play.num > 0) apply_movement(&play.cmds[0], &play.cmds[0]);
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
      if (replay_debug.integer)
        trap_Print(vaf("^3[replay] sync: %s\n", teleported ? "teleport flag" : "arrived at the start position"));
      play_start_now(qtrue); // uses the latched flip's commandTime
      consume_teleport();
    }
    else if (play_use_start_pos && !strncmp(play.placeplayer, "placeplayer", 11) && cg.time - play_wait_start > 10000)
    {
      play_stop("aborted: the start position teleport didn't happen within 10 s (cheats enabled?)");
    }
    else if (replay_debug.integer && (cg.time - play_wait_start) % 2000 < input_dt())
    {
      trap_Print(vaf(
        "^3[replay] waiting for the teleport... (pm_type %i, eFlags 0x%x, origin %.0f %.0f %.0f)\n",
        ps->pm_type,
        getSnap()->ps.eFlags,
        getSnap()->ps.origin[0],
        getSnap()->ps.origin[1],
        getSnap()->ps.origin[2]));
    }
    break;

  case PLAY_PLAYING:
  {
    // the usercmd our input lands in has serverTime == cg.time; pick the recorded cmd for it
    int32_t idx = play_index;
    for (;;)
    {
      qboolean      finished = qfalse;
      int32_t const elapsed  = cg.time - play_time0;
      if (play_fixed)
      {
        // slot this cmd will round to, relative to the slot of our first cmd; take the first recorded cmd
        // whose own slot offset is >= that (the one the server actually ran for this slot)
        int32_t const slot = ceil_to_grid(cg.time) - ceil_to_grid(play_time0);
        int32_t const base = ceil_to_grid(play.t0);
        if (idx < 0) idx = 0;
        while (idx + 1 < play.num && ceil_to_grid(play.t0 + play.cmds[idx].time) - base < slot) ++idx;
        finished = ceil_to_grid(play.t0 + play.cmds[idx].time) - base < slot;
      }
      else
      {
        while (idx + 1 < play.num && play.cmds[idx + 1].time <= elapsed) ++idx;
        if (idx < 0) return; // not yet at the first recorded cmd
        finished = elapsed > play.cmds[play.num - 1].time;
      }
      if (!finished) break;
      if (!play_advance())
      {
        if (play_state != PLAY_IDLE) play_stop("finished");
        return;
      }
      idx = play_index; // -1: restart the search in the new replay
    }

    rcmd_t const* const c    = &play.cmds[idx];
    rcmd_t const* const prev = idx > 0 ? &play.cmds[idx - 1] : NULL;
    play_index               = idx;

    apply_movement(c, prev);

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
// demo conversion (mdd_replay_convert)
//
// A .dm_68 holds no usercmds, but defrag puts every command's key state into ps.stats[13] and each snapshot
// carries the resulting view angles, weapon and commandTime. So the demo is simply played back by the engine
// and the snapshots are turned into replay cmds here; with snaps 125 that is one entry per usercmd.
//
typedef enum
{
  CONV_OFF,
  CONV_RUNNING
} conv_state_t;

static conv_state_t conv_state;
static int32_t      conv_last_snap;
static int32_t      conv_last_cmd_time;
static int32_t      conv_first_server_time;
static int32_t      conv_eflags;
static qboolean     conv_spawn_seen;
static int32_t      conv_min_interval;
static int32_t      conv_max_interval;
static qboolean     conv_lost;    // gaps between consecutive snapshots
static int32_t      conv_missing; // how many snapshot slots those gaps add up to

#define CONV_SPAWN_WINDOW 5000 // ms of demo in which a respawn resets the start of the replay

static void convert_snapshot(snapshot_t const* snap)
{
  playerState_t const* const ps = &snap->ps;

  if (conv_first_server_time < 0)
  {
    conv_first_server_time = snap->serverTime;
    conv_eflags            = ps->eFlags;
  }
  else if (
    ((ps->eFlags ^ conv_eflags) & EF_TELEPORT_BIT) && snap->serverTime - conv_first_server_time < CONV_SPAWN_WINDOW)
  {
    // the (re)spawn at the start of the run: everything before it is pre-spawn, restart from here
    rec.num            = 0;
    conv_last_cmd_time = ps->commandTime; // this snapshot is the spawn state itself; cmds after it count
    conv_spawn_seen    = qtrue;
    conv_eflags        = ps->eFlags;
    snprintf(
      rec.placeplayer, sizeof(rec.placeplayer), "spawn %.3f %.3f %.3f", ps->origin[0], ps->origin[1], ps->origin[2]);
    if (replay_debug.integer) trap_Print(vaf("^3[replay] demo: spawn at serverTime %i\n", snap->serverTime));
    return;
  }
  conv_eflags = ps->eFlags;

  if (ps->commandTime <= conv_last_cmd_time) return; // no new usercmd in this snapshot
  if (rec.num)
  {
    int32_t const interval = ps->commandTime - conv_last_cmd_time;
    if (interval < conv_min_interval) conv_min_interval = interval;
    if (interval > conv_max_interval) conv_max_interval = interval;
    if (conv_min_interval > 0 && interval > conv_min_interval)
    {
      conv_missing += interval / conv_min_interval - 1; // snapshots not in the demo (or overwritten by a hitch)
      conv_lost = qtrue;
    }
  }
  conv_last_cmd_time = ps->commandTime;
  if (rec.num >= MAX_REPLAY_CMDS) return;

  int32_t const  keys    = ps->stats[13];
  qboolean const walking = (keys & PSF_USERINPUT_WALK) != 0;
  int8_t const   full    = walking ? 64 : 127;

  if (rec.num == 0) rec_time0 = rec.t0 = ps->commandTime;
  rcmd_t* r  = &rec.cmds[rec.num++];
  r->time    = ps->commandTime - rec_time0;
  r->f       = keys & PSF_USERINPUT_FORWARD ? full : keys & PSF_USERINPUT_BACKWARD ? -full : 0;
  r->r       = keys & PSF_USERINPUT_RIGHT ? full : keys & PSF_USERINPUT_LEFT ? -full : 0;
  r->u       = keys & PSF_USERINPUT_JUMP ? full : keys & PSF_USERINPUT_CROUCH ? -full : 0;
  r->buttons = (uint16_t)((keys & PSF_USERINPUT_ATTACK ? BUTTON_ATTACK : 0) | (walking ? BUTTON_WALKING : 0));
  r->weapon  = (uint8_t)ps->weapon;
  // ps.viewangles are SHORT2ANGLE() results, exactly representable, so this round-trips to the same short
  for (int32_t i = 0; i < 3; ++i) r->view[i] = (uint16_t)ANGLE2SHORT(ps->viewangles[i]);
}

static void conv_frame(void)
{
  int32_t           cur, serverTime;
  static snapshot_t snap;
  trap_GetCurrentSnapshotNumber(&cur, &serverTime);
  int32_t first = conv_last_snap + 1;
  if (conv_last_snap >= 0 && cur - conv_last_snap > 31 && rec.num > 0)
  {
    // more messages arrived this frame than the client keeps (PACKET_BACKUP 32): a hitch or a timescale too
    // high for this frame rate. Halve the speed so it doesn't repeat (gaps are counted via the times below).
    float ts = cvar_getValue("timescale");
    if (ts > 1.5f)
    {
      ts = ts > 2 ? ts * .5f : 1.f;
      trap_Cvar_Set("timescale", vaf("%g", ts));
      trap_Print(vaf("^3[replay] frame overran the snapshot buffer, timescale lowered to %g\n", ts));
    }
  }
  if (first < cur - 31) first = cur - 31; // PACKET_BACKUP
  for (int32_t n = first; n <= cur; ++n)
  {
    if (trap_GetSnapshot(n, &snap)) convert_snapshot(&snap);
  }
  conv_last_snap = cur;
}

static void conv_begin(char const* name)
{
  memset(rec.name, 0, sizeof(rec.name));
  copy_str(rec.name, sizeof(rec.name), name);
  copy_str(rec.placeplayer, sizeof(rec.placeplayer), "spawn");
  rec.num                = 0;
  conv_last_snap         = -1;
  conv_last_cmd_time     = INT32_MIN;
  conv_first_server_time = -1;
  conv_spawn_seen        = qfalse;
  conv_min_interval      = INT32_MAX;
  conv_max_interval      = 0;
  conv_lost              = qfalse;
  conv_missing           = 0;
  conv_state             = CONV_RUNNING;

  // the demo command disconnected us, which makes sv_cheats 1, so timescale can be raised: snapshots are
  // pulled by number, so a faster demo converts just the same (up to ~30, PACKET_BACKUP is 32 per frame)
  if (replay_convert_timescale.value > 0)
  {
    char ts[MAX_CVAR_VALUE_STRING];
    trap_Cvar_VariableStringBuffer("timescale", ts, sizeof(ts));
    trap_Cvar_Set("mdd_replay_pending_timescale", ts[0] ? ts : "1");
    trap_Cvar_Set("timescale", vaf("%g", replay_convert_timescale.value > 30 ? 30.f : replay_convert_timescale.value));
  }
  trap_Print(vaf("^2[replay] converting demo '%s'...\n", name));
}

// demo over (cgame shutting down): write the replay, put the timescale back, done
static void conv_finish(void)
{
  conv_state = CONV_OFF;
  trap_Cvar_Set("mdd_replay_pending", "");

  char ts[MAX_CVAR_VALUE_STRING];
  trap_Cvar_VariableStringBuffer("mdd_replay_pending_timescale", ts, sizeof(ts));
  if (ts[0])
  {
    trap_Cvar_Set("timescale", ts);
    trap_Cvar_Set("mdd_replay_pending_timescale", "");
  }

  if (rec.num == 0)
  {
    trap_Print("^1[replay] demo conversion produced nothing\n");
    return;
  }
  if (!conv_spawn_seen) trap_Print("^3[replay] no spawn seen at the start of the demo, start alignment is a guess\n");
  if (conv_max_interval > FRAME_MSEC)
    trap_Print(vaf(
      "^3[replay] demo snapshot interval is %i-%i ms, not every usercmd is in it (record demos with snaps 125 for exact conversions)\n",
      conv_min_interval,
      conv_max_interval));
  if (conv_lost)
    trap_Print(
      "^1[replay] snapshots were lost during this conversion, convert again with a lower mdd_replay_convert_timescale\n");
  if (write_replay(&rec))
    trap_Print(vaf(
      "^2[replay] conversion done. On any server/map: mdd_replay_start %s (kills you and plays from the respawn)\n",
      rec.name));
}

static void convert(char const* demo)
{
  char name[MAX_REPLAY_NAME];
  copy_str(name, sizeof(name), demo);
  size_t n = strlen(name);
  if (n > 6 && !Q_stricmp(name + n - 6, ".dm_68")) name[n - 6] = 0;

  if (!valid_replay_name(name)) return;
  trap_Cvar_Set("mdd_replay_pending", "convert");
  trap_Cvar_Set("mdd_replay_pending_name", name);
  trap_Print(vaf("^2[replay] playing demo '%s' to convert it to replays/%s" REPLAY_EXT "\n", name, name));
  trap_SendConsoleCommand(vaf("demo %s\n", name));
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

  if (Q_stricmp(cmd, "mdd_replay_convert") == 0)
  {
    if (trap_Argc() != 2)
    {
      trap_Print("usage: mdd_replay_convert <demo> (a .dm_68 in demos/, extension optional)\n");
      return qtrue;
    }
    trap_Argv(1, arg, sizeof(arg));
    convert(arg);
    return qtrue;
  }

  qboolean const is_record     = Q_stricmp(cmd, "mdd_replay_record") == 0;
  qboolean const is_telerecord = Q_stricmp(cmd, "mdd_replay_telerecord") == 0;
  qboolean const is_start      = Q_stricmp(cmd, "mdd_replay_start") == 0;
  qboolean const is_telestart  = Q_stricmp(cmd, "mdd_replay_telestart") == 0;
  if (!is_record && !is_telerecord && !is_start && !is_telestart) return qfalse;

  qboolean const chain = is_start || is_telestart;
  int32_t const  argc  = trap_Argc();
  if (argc < 2 || (!chain && argc != 2) || (chain && argc - 1 > MAX_CHAIN))
  {
    trap_Print(vaf(
      chain ? "usage: %s <name> [<name> ...]  (up to %i, played back to back)\n" : "usage: %s <name>\n",
      cmd,
      MAX_CHAIN));
    return qtrue;
  }
  for (int32_t i = 1; i < argc; ++i)
  {
    trap_Argv(i, arg, sizeof(arg));
    if (!valid_replay_name(arg)) return qtrue;
    if (chain)
    {
      // make sure every file of the chain exists before starting anything
      fileHandle_t f;
      char         path[MAX_QPATH + MAX_REPLAY_NAME];
      snprintf(path, sizeof(path), REPLAY_DIR "/%s" REPLAY_EXT, arg);
      if (trap_FS_FOpenFile(path, &f, FS_READ) < 0)
      {
        trap_Print(vaf("^1[replay] can't open %s\n", path));
        return qtrue;
      }
      trap_FS_FCloseFile(f);
      copy_str(play_chain[i - 1], sizeof(play_chain[i - 1]), arg);
    }
  }
  if (chain) play_chain_num = argc - 1;

  if (is_record) rec_begin(arg, qfalse);
  if (is_telerecord) rec_begin(arg, qtrue);
  if (is_start) play_begin(qfalse);
  if (is_telestart) play_begin(qtrue);
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
  snap_seen        = qfalse;
  teleported       = qfalse;
  conv_state       = CONV_OFF;

  static char const* const commands[] = {
    "mdd_replay_record",    "mdd_replay_telerecord", "mdd_replay_stoprecord", "mdd_replay_start",
    "mdd_replay_telestart", "mdd_replay_stop",       "mdd_replay_convert",
  };
  for (size_t i = 0; i < ARRAY_LEN(commands); ++i) trap_AddCommand(commands[i]);
}

void del_replay(void)
{
  if (conv_state == CONV_RUNNING) conv_finish();
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
  // The predicted player state can lag the latest snapshot by one, so a flip is latched instead of being a
  // one-frame pulse: whoever is waiting for it consumes it (see consume_teleport), or it expires.
  if (snap_seen && ((snap->ps.eFlags ^ snap_eflags) & EF_TELEPORT_BIT))
  {
    teleported          = qtrue;
    teleported_at       = cg.time;
    teleported_cmd_time = snap->ps.commandTime;
    if (replay_debug.integer) trap_Print(vaf("^3[replay] teleport flag flipped at %i\n", cg.time));
  }
  else if (teleported && cg.time - teleported_at > TELEPORT_LATCH)
  {
    teleported = qfalse;
  }
  snap_eflags = snap->ps.eFlags;
  if (snap->serverTime > 0) snap_seen = qtrue;

  if (cg.demoPlayback)
  {
    if (conv_state == CONV_OFF && !Q_stricmp(replay_pending.string, "convert") && replay_pending_name.string[0])
      conv_begin(replay_pending_name.string);
    if (conv_state == CONV_RUNNING) conv_frame();
    if (play_state != PLAY_IDLE) play_stop("stopped: demo playback");
    return;
  }

  if (ps->pm_flags & PMF_FOLLOW)
  {
    if (play_state != PLAY_IDLE) play_stop("stopped: not controlling a player");
    return;
  }

  if (ps->pm_type != PM_NORMAL)
  {
    // dead (e.g. after our own `kill`): an armed playback keeps waiting for the respawn, a running one stops
    if (play_state == PLAY_PLAYING) play_stop("stopped: player died");
    if (play_state == PLAY_WAIT_TELEPORT && play_prehold && play.num > 0) apply_movement(&play.cmds[0], &play.cmds[0]);
    return;
  }

  rec_frame(ps);
  play_frame(ps);
}
