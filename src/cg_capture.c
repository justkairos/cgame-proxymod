/*
  ==============================
  Reading the console.

  A cgame cannot read the console, but the engine can write it to a file (condump) and a file the cgame can read:
    1. for a command: queue a marker line (echo) and the command
    2. queue a few empty steps (see HOPS)
    3. queue "condump", then the last step, which reads the dump and keeps the wanted rows

  The dump is the console as the engine stores it: rows of the console's width, colours gone, and a line that
  was too long is spread over several rows. To put such lines back together the console's width is needed, which
  the engine does not tell either. So the marker line is made to measure it: one unbroken run of characters,
  one longer than the width is expected to be. It fills its row exactly, the dump shows how many characters that
  was, and the one that did not fit shows that the row really was full.

  With the width W, row i continues in row i+1 (oDFe's CL_ConsolePrint)
    - if row i is exactly W characters: the text ran into the edge
    - if the first word of row i+1 would not have fit behind row i: the word was moved down
  The second test cannot tell a moved word from a line that simply ended that close to the edge and was followed
  by a line starting with a long word; those two are joined as well.

  oDFe can put a time in front of every row (con_timestamp). It counts for the width and is cut off afterwards.
  ==============================
*/
#include "cg_capture.h"

#include "cg_local.h"
#include "sys_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_NAME "mdd_console" // the engine appends .txt
#define DUMP_FILE DUMP_NAME ".txt"
#define MAX_DUMP  (16 * 1024 * 1024)

/*
  A command can itself queue commands (search queues cvarlist and cmdlist, mapinfo queues which, an alias
  queues its text), and whatever is queued lands behind everything already in the engine's command buffer -
  including our dump. Each hop re-queues the next step at the very end, which lets one more generation of
  queued commands run first.
*/
#define HOPS 4

/*
  The dump itself goes through defrag's "silent" command, which runs a command without its console output:
  condump would otherwise announce every dump with a line of its own. "silent" is a cgame command too, so the
  condump it runs is queued at the end of the buffer like everything else - behind our next step. Hence a few
  more hops between the dump and reading it. If no dump turns up (a mod without "silent"), the dump is done
  once more the plain way, and from then on always.
*/
#define DUMP_HOPS 3

#define MARKER_TEXT   "---capture#"
#define MARKER_MIN    40
#define MARKER_MAX    1000
#define TIMESTAMP_LEN 9 // "hh:mm:ss "

static int32_t        request_id; // the request the queued steps belong to
static capture_done_t request_done;
static qboolean       request_is_command;
static char           request_self[32];
static int32_t        marker_len; // length of the marker run queued for this request, 0 = none

// The console width that was measured, and what it was measured under.
static int32_t known_width;
static char    known_key[64];
static int32_t next_marker_len; // if the last marker was too short to reach the edge: try this

static void width_key(char* key, size_t size)
{
  char scale[16], stamp[8];
  trap_Cvar_VariableStringBuffer("con_scale", scale, sizeof(scale));
  trap_Cvar_VariableStringBuffer("con_timestamp", stamp, sizeof(stamp));
  snprintf(key, size, "%i/%s/%s", cgs.glconfig.vidWidth, scale, stamp);
}

// One character more than the console is probably wide: vidWidth / (8 * con_scale) - 2 in oDFe and Quake3e
// unless the display is scaled; a wrong guess only costs a longer marker or one more try.
static int32_t guess_marker_len(void)
{
  char scale[16];
  trap_Cvar_VariableStringBuffer("con_scale", scale, sizeof(scale));
  float s = (float)atof(scale);
  if (s < .5f || s > 8.f) s = 1.f;
  int32_t len = (int32_t)(cgs.glconfig.vidWidth / (8.f * s)) - 2 + 1;
  if (next_marker_len > len) len = next_marker_len;
  if (len < MARKER_MIN) len = MARKER_MIN;
  if (len > MARKER_MAX) len = MARKER_MAX;
  return len;
}

static void queue_marker(void)
{
  char         line[MARKER_MAX + 64];
  int          n         = snprintf(line, sizeof(line), "echo " MARKER_TEXT "%i", request_id);
  size_t const run_start = strlen("echo ");
  marker_len             = guess_marker_len();
  while ((int32_t)((size_t)n - run_start) < marker_len) line[n++] = '-';
  line[n++] = '\n';
  line[n]   = 0;
  trap_SendConsoleCommand(line);
}

static qboolean plain_dump; // dump without "silent": it got nothing once (then it is not tried again)

// Steps 0..HOPS-1 wait for the command's output, step HOPS dumps, the steps after it wait for the dump, and
// step HOPS + DUMP_HOPS reads it.
static void queue_step(int32_t step)
{
  char line[160];
  if (step == HOPS)
  {
    // Empty the dump file first. If no dump is written (no "silent" in this mod), an old dump that could not
    // be removed must not be mistaken for the new one.
    fileHandle_t f;
    if (trap_FS_FOpenFile(DUMP_FILE, &f, FS_WRITE) >= 0) trap_FS_FCloseFile(f);
    snprintf(
      line,
      sizeof(line),
      "%scondump " DUMP_NAME "\n" CAPTURE_STEP_COMMAND " %i %i\n",
      plain_dump ? "" : "silent ",
      request_id,
      step + 1);
  }
  else
    snprintf(line, sizeof(line), CAPTURE_STEP_COMMAND " %i %i\n", request_id, step);
  trap_SendConsoleCommand(line);
}

void capture_console(char const* self, capture_done_t done)
{
  ++request_id;
  request_done       = done;
  request_is_command = qfalse;
  snprintf(request_self, sizeof(request_self), "%s", self);
  marker_len = 0;
  // the width is only measured when it is not known yet: the marker is a line in the console after all
  char key[sizeof(known_key)];
  width_key(key, sizeof(key));
  if (!known_width || strcmp(key, known_key)) queue_marker();
  queue_step(HOPS); // nothing to wait for
}

void capture_command(char const* command, capture_done_t done)
{
  ++request_id;
  request_done       = done;
  request_is_command = qtrue;
  request_self[0]    = 0;
  // The marker the output starts below. It is queued, not printed here: commands that are already waiting in
  // the buffer still print before ours runs.
  queue_marker();
  char line[MAX_STRING_CHARS + 8];
  snprintf(line, sizeof(line), "%s\n", command);
  trap_SendConsoleCommand(line);
  queue_step(0);
}

static qboolean has_timestamp(char const* row)
{
  for (int32_t i = 0; i < 8; ++i)
  {
    if (i == 2 || i == 5 ? row[i] != ':' : row[i] < '0' || row[i] > '9') return qfalse;
  }
  return row[8] == ' ' || !row[8];
}

// the row without the time in front
static char const* content(char const* row, qboolean timestamps)
{
  if (!timestamps || !has_timestamp(row)) return row;
  return row[8] ? row + TIMESTAMP_LEN : row + 8;
}

static size_t word_len(char const* s)
{
  size_t n = 0;
  while ((unsigned char)s[n] > ' ') ++n;
  return n;
}

static qboolean all_dashes(char const* s)
{
  if (!*s) return qfalse;
  for (; *s; ++s)
  {
    if (*s != '-') return qfalse;
  }
  return qtrue;
}

static void finish(char const* error, char** lines, size_t num)
{
  capture_done_t const done = request_done;
  request_done              = NULL;
  if (done) done(error, lines, num);
}

void cmd_capture_step(void)
{
  if (trap_Argc() != 3) return; // internal: sent by queue_step
  char a[16], b[16];
  trap_Argv(1, a, sizeof(a));
  trap_Argv(2, b, sizeof(b));
  if (atoi(a) != request_id || !request_done) return; // a newer request took over
  int32_t const step = atoi(b);
  if (step < HOPS + DUMP_HOPS)
  {
    queue_step(step + 1);
    return;
  }

  fileHandle_t  f;
  int32_t const len = trap_FS_FOpenFile(DUMP_FILE, &f, FS_READ);
  if (len <= 0)
  {
    if (len == 0) trap_FS_FCloseFile(f);
    if (!plain_dump)
    {
      plain_dump = qtrue; // no "silent" here: dump again without it
      queue_step(HOPS);
      return;
    }
    finish("the engine did not write the console dump (" DUMP_FILE ")", NULL, 0);
    return;
  }
  if (len > MAX_DUMP)
  {
    trap_FS_FCloseFile(f);
    finish("the console dump is too large", NULL, 0);
    return;
  }
  char* const buf = malloc((size_t)len + 1);
  if (!buf)
  {
    trap_FS_FCloseFile(f);
    finish("out of memory", NULL, 0);
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

  // rows, in place. The engine has already cut the spaces at the end of each.
  size_t max_rows = 1;
  for (int32_t i = 0; i < len; ++i) max_rows += buf[i] == '\n';
  char** const rows  = malloc(max_rows * sizeof(*rows));
  char** const lines = malloc(max_rows * sizeof(*lines));
  char* const  text  = malloc((size_t)len + max_rows + 1); // joined lines: never longer than the rows
  if (!rows || !lines || !text)
  {
    free(rows);
    free(lines);
    free(text);
    free(buf);
    finish("out of memory", NULL, 0);
    return;
  }
  size_t num = 0;
  for (char* row = buf; row;)
  {
    char* const end = strchr(row, '\n');
    if (end) *end = 0;
    size_t const n = strlen(row);
    if (n && row[n - 1] == '\r') row[n - 1] = 0;
    for (char* c = row; *c; ++c)
    {
      if ((unsigned char)*c < ' ') *c = ' '; // the console font's symbols below space have no text form
    }
    rows[num++] = row;
    row         = end ? end + 1 : NULL;
  }

  char stamp[8];
  trap_Cvar_VariableStringBuffer("con_timestamp", stamp, sizeof(stamp));
  qboolean const timestamps = atoi(stamp) != 0;

  // the end: the dump's own message and whatever follows it
  size_t last = num;
  for (size_t i = num; i-- > 0;)
  {
    if (!strncmp(content(rows[i], timestamps), "Dumped console text to ", 23))
    {
      last = i;
      break;
    }
  }

  // our marker: where a command's output starts, and how wide the console is
  size_t   first        = 0;
  qboolean found_marker = qfalse;
  int32_t  width        = 0;
  if (marker_len)
  {
    char marker[48];
    snprintf(marker, sizeof(marker), MARKER_TEXT "%i-", request_id);
    for (size_t i = last; i-- > 0;)
    {
      char const* const at = strstr(rows[i], marker);
      if (!at) continue;
      found_marker = qtrue;
      // how much of the run is in this row; the rest was pushed to the next rows
      int32_t const in_row = (int32_t)strlen(at);
      int32_t       spill  = marker_len - in_row;
      size_t        after  = i + 1;
      if (spill > 0) width = (int32_t)strlen(rows[i]);
      while (spill > 0 && after < last && all_dashes(content(rows[after], timestamps)))
      {
        spill -= (int32_t)strlen(content(rows[after], timestamps));
        ++after;
      }
      if (request_is_command)
        first = after;
      else
        last = i; // reading the whole console: the marker is ours, and so is everything below it
      break;
    }
    char key[sizeof(known_key)];
    width_key(key, sizeof(key));
    if (found_marker && width)
    {
      known_width     = width;
      next_marker_len = 0;
      snprintf(known_key, sizeof(known_key), "%s", key);
    }
    else if (found_marker)
    {
      // the whole run fit into one row: the console is wider than guessed. No width this time; reach further
      // the next time.
      next_marker_len = marker_len * 2;
      if (!strcmp(key, known_key)) width = known_width;
    }
  }
  else
  {
    width = known_width; // capture_console only goes without a marker when this is current
  }
  if (request_is_command && !found_marker)
  {
    free(rows);
    free(lines);
    free(text);
    free(buf);
    finish("the command printed more than the console holds", NULL, 0);
    return;
  }
  while (last > first && !content(rows[last - 1], timestamps)[0]) --last; // empty rows at the end
  if (!request_is_command && last > 0 && request_self[0])
  {
    // the line that was typed to get here: "]\clipboard" / "]/clipboard 10"
    char const* const typed = content(rows[last - 1], timestamps);
    if (typed[0] == ']' && strstr(typed, request_self)) --last;
  }
  while (last > first && !content(rows[last - 1], timestamps)[0]) --last; // empty rows at the end
  while (first < last && !content(rows[first], timestamps)[0]) ++first;   // and the unused top of the console

  // rows to lines
  size_t num_lines = 0;
  char*  out       = text;
  for (size_t i = first; i < last; ++i)
  {
    lines[num_lines++] = out;
    for (;; ++i)
    {
      char const* const c = content(rows[i], timestamps);
      size_t const      n = strlen(c);
      memcpy(out, c, n);
      out += n;
      if (!width || i + 1 >= last) break;
      size_t const      row_len = strlen(rows[i]);
      char const* const next    = content(rows[i + 1], timestamps);
      size_t const      prefix  = (size_t)(next - rows[i + 1]);
      size_t const      word    = word_len(next);
      if (!n || !next[0]) break;
      if (row_len == (size_t)width) continue; // ran into the edge: the next row carries straight on
      if (next[0] != ' ' && row_len + 1 + word > (size_t)width && prefix + word < (size_t)width)
      {
        *out++ = ' '; // the word was moved down; the space before it was at the end of this row
        continue;
      }
      break;
    }
    *out++ = 0;
  }

  if (!num_lines)
    finish(request_is_command ? "the command printed nothing" : "the console is empty", NULL, 0);
  else
    finish(NULL, lines, num_lines);
  free(rows);
  free(lines);
  free(text);
  free(buf);
}
