/*
  ==============================
  Written by id software, nightmare and hk of mdd

  This file is part of mdd client proxymod.

  mdd client proxymod is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  mdd client proxymod is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with mdd client proxymod.  If not, see <http://www.gnu.org/licenses/>.
  ==============================
  Note: mdd client proxymod contains large quantities from the quake III arena source code
*/
#include "cg_input.h"
#include "cg_local.h"
#include "cg_replay.h"
#include "cg_vm.h"
#include "help.h"

#include <stdlib.h>
#include <string.h>

static void cmdHelp(void);
static void cmdSearch(void);
#ifndef NDEBUG
static void cmdPointsTo_DebugOnly(void);
#endif

typedef struct
{
  char const* cmd;
  void (*function)(void);
} consoleCommand_t;

static consoleCommand_t commands[] = {
  { "mdd_help", cmdHelp },
  { "mdd_search", cmdSearch },
  { "search", cmdSearch },
#ifndef NDEBUG
  { "mdd_points_to", cmdPointsTo_DebugOnly },
#endif
};

/*
=================
CG_ConsoleCommand

The string has been tokenized and can be retrieved with
Cmd_Argc() / Cmd_Argv()
=================
*/
qboolean CG_ConsoleCommand(void)
{
  char cmd[MAX_STRING_CHARS];
  trap_Argv(0, cmd, sizeof(cmd));

  if (input_button_command(cmd)) return qtrue;
  if (replay_console_command(cmd)) return qtrue;

  for (uint8_t i = 0; i < ARRAY_LEN(commands); ++i)
  {
    if (!Q_stricmp(cmd, commands[i].cmd))
    {
      commands[i].function();
      return qtrue;
    }
  }

  return qfalse;
}

/*
=================
CG_InitConsoleCommands

Let the client system know about all of our commands
so it can perform tab completion
=================
*/
void CG_InitConsoleCommands(void)
{
  for (uint8_t i = 0; i < ARRAY_LEN(commands); ++i)
  {
    trap_AddCommand(commands[i].cmd);
  }
}

// print system/help/<name>.txt (the same files defrag's "? <name>" reads); returns qfalse if there is none
static qboolean print_help_file(char const* name)
{
  if (name[0] == '+' || name[0] == '-') ++name; // +mdd_jumpshoot -> mdd_jumpshoot.txt
  for (char const* c = name; *c; ++c)
  {
    if (*c == '/' || *c == '\\' || *c == '.') return qfalse;
  }

  fileHandle_t  f;
  int32_t const len = trap_FS_FOpenFile(vaf("system/help/%s.txt", name), &f, FS_READ);
  if (len <= 0) return qfalse;

  char* const buf = malloc((size_t)len + 1);
  trap_FS_Read(buf, len, f);
  trap_FS_FCloseFile(f);
  buf[len] = 0;

  // trap_Print has a limited buffer: print line by line
  for (char* line = buf; *line;)
  {
    char* end = strchr(line, '\n');
    if (end) *end = 0;
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r') line[n - 1] = 0;
    trap_Print(vaf("%s\n", line));
    if (!end) break;
    line = end + 1;
  }
  free(buf);
  return qtrue;
}

static void cmdHelp(void)
{
  if (trap_Argc() != 2)
  {
    trap_Print("usage: mdd_help <cvar|command>\n");
    trap_Print(
      "^3mdd_help mdd^7 gives an overview of the automation features; type ^3mdd_^7 + tab to list everything.\n");
    cvars_with_help();
    return;
  }

  char cvar[MAX_STRING_CHARS];
  trap_Argv(1, cvar, sizeof(cvar));

  if (print_help_file(cvar)) return;
  cvar_help(cvar);
}

/*
  mdd_search / search <pattern> [<pattern> ...]: lists every cvar AND every command whose name matches.

  A cgame cannot enumerate the engine's cvars or commands itself, but the engine's own cvarlist and cmdlist take a
  filter (Com_Filter: * any run of characters, ? one character, [a-z] a set, case-insensitive) and they see
  everything that is registered: the engine's, defrag's and ours (+buttons included). So this just runs both.
  A pattern without any wildcard is searched as a substring (*pattern*).
*/
static void cmdSearch(void)
{
  int32_t const argc = trap_Argc();
  if (argc < 2)
  {
    trap_Print("usage: search <pattern> [<pattern> ...]   (also mdd_search)\n");
    trap_Print("  * = any characters, ? = one character, [a-z] = one of a set; no wildcard = contains\n");
    trap_Print("  e.g. ^3search cg_*time^7, ^3search cg_crosshair*^7, ^3search *_*time*^7, ^3search +mdd*^7\n");
    return;
  }

  for (int32_t a = 1; a < argc && a <= 8; ++a)
  {
    char arg[MAX_STRING_CHARS];
    trap_Argv(a, arg, sizeof(arg));

    // the pattern goes back into the console: keep only characters a name or a filter can contain
    char     pattern[64];
    size_t   n    = 0;
    qboolean wild = qfalse;
    for (char const* c = arg; *c && n < sizeof(pattern) - 3; ++c)
    {
      qboolean const alnum = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
      if (!alnum && !strchr("_+-.*?[]", *c)) continue;
      if (*c == '*' || *c == '?' || *c == '[') wild = qtrue;
      pattern[n++] = *c;
    }
    pattern[n] = 0;
    if (!n) continue;
    if (!wild)
    {
      memmove(pattern + 1, pattern, n + 1);
      pattern[0]     = '*';
      pattern[n + 1] = '*';
      pattern[n + 2] = 0;
    }

    trap_SendConsoleCommand(vaf(
      "echo \"^3--- cvars matching %s ---\"\ncvarlist \"%s\"\necho \"^3--- commands matching %s ---\"\ncmdlist \"%s\"\n",
      pattern,
      pattern,
      pattern,
      pattern));
  }
}

#ifndef NDEBUG
static void cmdPointsTo_DebugOnly(void)
{
  if (trap_Argc() != 2)
  {
    trap_Print("usage: mdd_points_to <pointer>\n");
    return;
  }

  char cmd[MAX_STRING_CHARS];
  trap_Argv(1, cmd, sizeof(cmd));

  long const offset = strtol(cmd, NULL, 0);
  trap_Print(vaf("%s -> 0x%lx\n", cmd, *(int32_t*)VM_ArgPtr(offset)));
}
#endif
