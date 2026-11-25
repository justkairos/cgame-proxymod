/*
  ==============================
  clipboard                    everything in the console
  clipboard <number>           the last <number> lines of the console
  clipboard <cvar>             the value of the cvar
  clipboard <file>.cfg         the output of "exec <file>.cfg"
  clipboard <command> [...]    the output of the command

  The console text comes from cg_capture.c (whole lines, wrapped rows put back together), the clipboard itself
  from sys_files.c.
  ==============================
*/
#include "cg_clipboard.h"

#include "cg_capture.h"
#include "cg_local.h"
#include "sys_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COPY (2 * 1024 * 1024) // characters handed to the clipboard: whole lines from the end

typedef enum
{
  CLIP_ALL,
  CLIP_LAST,
  CLIP_COMMAND,
} clip_mode_t;

static clip_mode_t request_mode;
static int32_t     request_lines;

static void copy_text(char const* text, int32_t lines, char const* what)
{
  if (!sys_set_clipboard(text))
  {
    trap_Print("^1clipboard: could not put the text on the clipboard (only available on Windows)\n");
    return;
  }
  char line[MAX_STRING_CHARS + 160];
  snprintf(
    line,
    sizeof(line),
    "^3clipboard: copied %s (%i line%s, %i characters)\n",
    what,
    lines,
    lines == 1 ? "" : "s",
    (int32_t)strlen(text));
  trap_Print(line);
}

static void on_captured(char const* error, char** lines, size_t num)
{
  if (error)
  {
    char line[256];
    snprintf(line, sizeof(line), "clipboard: %s, nothing copied\n", error);
    trap_Print(line);
    return;
  }
  size_t first = 0;
  if (request_mode == CLIP_LAST && num > (size_t)request_lines) first = num - (size_t)request_lines;

  // as many whole lines as fit, from the end
  size_t total = 0;
  size_t from  = num;
  while (from > first)
  {
    size_t const n = strlen(lines[from - 1]) + 1;
    if (total + n > MAX_COPY) break;
    total += n;
    --from;
  }
  if (from == num)
  {
    trap_Print("clipboard: the last line alone is too long, nothing copied\n");
    return;
  }
  char* const text = malloc(total + 1);
  if (!text)
  {
    trap_Print("^1clipboard: out of memory\n");
    return;
  }
  char* out = text;
  for (size_t i = from; i < num; ++i)
  {
    size_t const n = strlen(lines[i]);
    memcpy(out, lines[i], n);
    out += n;
    if (i + 1 < num) *out++ = '\n';
  }
  *out = 0;
  copy_text(
    text,
    (int32_t)(num - from),
    request_mode == CLIP_COMMAND ? "the command's output"
    : request_mode == CLIP_LAST  ? "the end of the console"
                                 : "the console");
  if (from > first)
  {
    char line[96];
    snprintf(line, sizeof(line), "^3clipboard: the first %i lines did not fit\n", (int32_t)(from - first));
    trap_Print(line);
  }
  free(text);
}

static qboolean is_number(char const* s)
{
  if (!*s) return qfalse;
  for (; *s; ++s)
  {
    if (*s < '0' || *s > '9') return qfalse;
  }
  return qtrue;
}

void cmd_clipboard(void)
{
  int32_t const argc = trap_Argc();
  char          arg[MAX_STRING_CHARS];
  trap_Argv(1, arg, sizeof(arg));

  if (argc == 2 && (!Q_stricmp(arg, "help") || !Q_stricmp(arg, "?")))
  {
    trap_Print("usage: clipboard                 copy everything in the console\n");
    trap_Print("       clipboard <number>        copy the last <number> lines\n");
    trap_Print("       clipboard <cvar>          copy the value of the cvar\n");
    trap_Print("       clipboard <file>.cfg      exec the file and copy what it prints\n");
    trap_Print("       clipboard <command> ...   run the command and copy what it prints\n");
    return;
  }

  if (argc == 1 || (argc == 2 && is_number(arg)))
  {
    request_mode  = argc == 1 ? CLIP_ALL : CLIP_LAST;
    request_lines = argc == 1 ? 0 : atoi(arg);
    if (request_mode == CLIP_LAST && request_lines < 1)
    {
      trap_Print("clipboard: 0 lines, nothing to copy\n");
      return;
    }
    capture_console("clipboard", on_captured);
    return;
  }

  // a cvar: its value. (A cvar whose value is empty can't be told from a name that is no cvar; it goes the
  // command way, and the engine's "name is ..." line is what gets copied.)
  if (argc == 2)
  {
    char value[MAX_STRING_CHARS];
    trap_Cvar_VariableStringBuffer(arg, value, sizeof(value));
    if (value[0])
    {
      char what[MAX_STRING_CHARS + 32];
      snprintf(what, sizeof(what), "the value of %s", arg);
      copy_text(value, 1, what);
      return;
    }
  }

  // a command: the rest of the line, as typed
  char   command[MAX_STRING_CHARS];
  size_t len           = 0;
  command[0]           = 0;
  size_t const arg_len = strlen(arg);
  if (argc == 2 && arg_len > 4 && !Q_stricmp(arg + arg_len - 4, ".cfg"))
    len = (size_t)snprintf(command, sizeof(command), "exec \"%s\"", arg);
  else
  {
    for (int32_t i = 1; i < argc; ++i)
    {
      trap_Argv(i, arg, sizeof(arg));
      qboolean const quote = !arg[0] || strpbrk(arg, " ;/") != NULL; // /: the engine reads // and /* as comments
      int const n = snprintf(command + len, sizeof(command) - len, quote ? "%s\"%s\"" : "%s%s", i > 1 ? " " : "", arg);
      if (n < 0 || (size_t)n >= sizeof(command) - len)
      {
        len = sizeof(command);
        break;
      }
      len += (size_t)n;
    }
  }
  if (len >= sizeof(command) - 2 || strchr(command, '\n') || strchr(command, '\r'))
  {
    trap_Print("^1clipboard: command too long\n");
    return;
  }
  if (!Q_stricmpn(command, "clipboard", 9) || !Q_stricmpn(command, "grep", 4))
  {
    trap_Print("clipboard: can't copy the output of clipboard or grep (they read the console themselves)\n");
    return;
  }
  request_mode = CLIP_COMMAND;
  capture_command(command, on_captured);
}
