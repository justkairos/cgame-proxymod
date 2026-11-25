/*
  ==============================
  clipboard                    everything in the console
  clipboard <number>           the last <number> lines of the console
  clipboard <cvar>             the value of the cvar
  clipboard <file>.cfg         the output of "exec <file>.cfg"
  clipboard <command> [...]    the output of the command

  A cgame cannot read the console. The engine can write it to a file though (condump), and a file the cgame can
  read. So:
    1. for a command: queue a marker line (echo) and the command
    2. queue a few empty steps (see HOPS)
    3. queue "condump", then the last step, which reads the dump, keeps the wanted lines and hands them to the
       system clipboard (sys_files.c)

  The dump is the console as the engine stores it: rows of the console's width, without colours. A line that was
  too long for the console is therefore copied the way the console shows it, in several rows.
  ==============================
*/
#include "cg_clipboard.h"

#include "cg_local.h"
#include "sys_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_NAME "mdd_clipboard" // the engine appends .txt
#define DUMP_FILE DUMP_NAME ".txt"
#define MAX_DUMP  (16 * 1024 * 1024)
#define MAX_COPY  (2 * 1024 * 1024) // characters handed to the clipboard: whole lines from the end

/*
  A command can itself queue commands (search queues cvarlist and cmdlist, mapinfo queues which, an alias
  queues its text), and whatever is queued lands behind everything already in the engine's command buffer -
  including our dump. Each hop re-queues the next step at the very end, which lets one more generation of
  queued commands run first.
*/
#define HOPS 4

typedef enum
{
  CLIP_ALL,
  CLIP_LAST,
  CLIP_COMMAND,
} clip_mode_t;

static int32_t request_id; // the request the queued steps belong to; a newer request cancels an older one
static int32_t request_mode;
static int32_t request_lines;

static void say(char const* fmt, int32_t a, int32_t b)
{
  char line[256];
  snprintf(line, sizeof(line), fmt, a, b);
  trap_Print(line);
}

static void queue_step(int32_t step)
{
  char line[128];
  if (step < HOPS)
    snprintf(line, sizeof(line), CLIPBOARD_GRAB_COMMAND " %i %i\n", request_id, step);
  else // the dump, then the step that reads it
    snprintf(line, sizeof(line), "condump " DUMP_NAME "\n" CLIPBOARD_GRAB_COMMAND " %i %i\n", request_id, step);
  trap_SendConsoleCommand(line);
}

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
    ++request_id;
    queue_step(HOPS); // nothing to wait for: dump right away
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
  if (
    !Q_stricmpn(command, "clipboard", 9) ||
    !Q_stricmpn(command, CLIPBOARD_GRAB_COMMAND, strlen(CLIPBOARD_GRAB_COMMAND)))
  {
    trap_Print("clipboard: can't copy the output of clipboard itself\n");
    return;
  }

  request_mode = CLIP_COMMAND;
  ++request_id;
  // The marker line the output starts below. It is queued, not printed here: commands that are already
  // waiting in the buffer still print before ours runs.
  char line[MAX_STRING_CHARS + 96];
  snprintf(line, sizeof(line), "echo \"^3--- clipboard #%i ---\"\n%s\n", request_id, command);
  trap_SendConsoleCommand(line);
  queue_step(0);
}

// one row of the dump is the engine's own message about the dump
static qboolean is_dump_message(char const* row)
{
  return !strncmp(row, "Dumped console text to ", 23);
}

void cmd_clipboard_grab(void)
{
  if (trap_Argc() != 3) return; // internal: sent by queue_step
  char a[16], b[16];
  trap_Argv(1, a, sizeof(a));
  trap_Argv(2, b, sizeof(b));
  if (atoi(a) != request_id) return; // a newer request took over
  int32_t const step = atoi(b);
  if (step < HOPS)
  {
    queue_step(step + 1);
    return;
  }

  fileHandle_t  f;
  int32_t const len = trap_FS_FOpenFile(DUMP_FILE, &f, FS_READ);
  if (len < 0)
  {
    trap_Print("^1clipboard: the engine did not write the console dump (" DUMP_FILE ")\n");
    return;
  }
  if (len == 0 || len > MAX_DUMP)
  {
    trap_FS_FCloseFile(f);
    trap_Print("clipboard: the console is empty\n");
    return;
  }
  char* const buf = malloc((size_t)len + 1);
  if (!buf)
  {
    trap_FS_FCloseFile(f);
    trap_Print("^1clipboard: out of memory\n");
    return;
  }
  trap_FS_Read(buf, len, f);
  trap_FS_FCloseFile(f);
  buf[len] = 0;

  // the dump stays behind in the mod folder otherwise; where the engine wrote it is the folder we run from
  // on a normal install, and if it is somewhere else the next dump simply overwrites it
  char path[1024];
  if (sys_module_dir(path, sizeof(path) - sizeof(DUMP_FILE) - 1))
  {
    strcat(path, "/" DUMP_FILE);
    remove(path);
  }

  // rows, in place
  size_t max_rows = 1;
  for (int32_t i = 0; i < len; ++i) max_rows += buf[i] == '\n';
  char** const rows = malloc(max_rows * sizeof(*rows));
  if (!rows)
  {
    free(buf);
    trap_Print("^1clipboard: out of memory\n");
    return;
  }
  size_t num = 0;
  for (char* row = buf; row;)
  {
    char* const end = strchr(row, '\n');
    if (end) *end = 0;
    size_t n = strlen(row);
    while (n && (row[n - 1] == '\r' || row[n - 1] == ' ')) row[--n] = 0;
    for (char* c = row; *c; ++c)
    {
      if ((unsigned char)*c < ' ') *c = ' '; // the console font's symbols below space have no text form
    }
    rows[num++] = row;
    row         = end ? end + 1 : NULL;
  }

  // the end: drop the dump's own message and whatever follows it
  size_t last = num;
  for (size_t i = num; i-- > 0;)
  {
    if (is_dump_message(rows[i]))
    {
      last = i;
      break;
    }
  }
  size_t first = 0;
  if (request_mode == CLIP_COMMAND)
  {
    // everything below our marker line
    char marker[64];
    snprintf(marker, sizeof(marker), "--- clipboard #%i ---", request_id);
    qboolean found = qfalse;
    for (size_t i = last; i-- > 0;)
    {
      if (!strcmp(rows[i], marker))
      {
        first = i + 1;
        found = qtrue;
        break;
      }
    }
    if (!found)
    {
      free(rows);
      free(buf);
      trap_Print("clipboard: the command printed more than the console holds, nothing copied\n");
      return;
    }
  }
  else
  {
    // the line that was typed to get here: "]\clipboard" / "]/clipboard 10"
    if (last > 0 && rows[last - 1][0] == ']' && strstr(rows[last - 1], "clipboard")) --last;
  }
  while (last > first && !rows[last - 1][0]) --last; // empty rows at the end
  while (first < last && !rows[first][0]) ++first;   // and the unused top of the console
  if (request_mode == CLIP_LAST && last - first > (size_t)request_lines) first = last - (size_t)request_lines;

  // as many whole lines as fit, from the end
  size_t total = 0;
  size_t from  = last;
  while (from > first)
  {
    size_t const n = strlen(rows[from - 1]) + 1;
    if (total + n > MAX_COPY) break;
    total += n;
    --from;
  }
  if (from == last)
  {
    free(rows);
    free(buf);
    trap_Print(
      request_mode == CLIP_COMMAND ? "clipboard: the command printed nothing\n" : "clipboard: the console is empty\n");
    return;
  }
  char* const text = malloc(total + 1);
  if (!text)
  {
    free(rows);
    free(buf);
    trap_Print("^1clipboard: out of memory\n");
    return;
  }
  char* out = text;
  for (size_t i = from; i < last; ++i)
  {
    size_t const n = strlen(rows[i]);
    memcpy(out, rows[i], n);
    out += n;
    if (i + 1 < last) *out++ = '\n';
  }
  *out = 0;
  copy_text(
    text,
    (int32_t)(last - from),
    request_mode == CLIP_COMMAND ? "the command's output"
    : request_mode == CLIP_LAST  ? "the end of the console"
                                 : "the console");
  if (from > first) say("^3clipboard: the first %i lines did not fit\n", (int32_t)(from - first), 0);
  free(text);
  free(rows);
  free(buf);
}
