/*
  ==============================
  grep "<command>" r'<regex>' [<file>]

  Runs the command, searches every line it printed with the regular expression, and prints each match on a line
  of its own - the matched text, not the whole line (use .*word.* for whole lines). With a file name the matches
  are written to that file in the mod folder instead.

  The output is read back from the console (cg_capture.c), the matching is regex_lite.c.

  About the syntax: the engine has split the command line into words long before a cgame sees it. It knows
  double quotes, so the command arrives whole; it does not know single quotes, so the regex arrives in pieces
  wherever it had a space and is put together again here with single spaces. What the engine does to a line
  cannot be undone from here: an unquoted ; ends the command, // and slash-star start a comment, and a double
  quote starts or ends a word. A regex that needs one of those is written as "r'...'" (inside double quotes) or
  with \x3b (;) \x2f (/) \x22 (").
  ==============================
*/
#include "cg_grep.h"

#include "cg_capture.h"
#include "cg_local.h"
#include "regex_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PRINTED 2000 // matches printed to the console; a file gets all of them

static re_t regex;
static char regex_text[MAX_STRING_CHARS];
static char out_file[MAX_QPATH];

static void say(char const* fmt, char const* a, char const* b)
{
  char line[MAX_STRING_CHARS * 2 + 256];
  snprintf(line, sizeof(line), fmt, a, b);
  trap_Print(line);
}

static void usage(void)
{
  trap_Print("usage: grep \"<command>\" r'<regex>' [<file>]\n");
  trap_Print("  runs the command and prints each thing the regex finds in its output, one per line\n");
  trap_Print("  with a file name (.txt or .log) the matches are saved to it instead of printed\n");
  trap_Print("  the command goes in double quotes, the regex in r'...'; write \\' for a ' inside it\n");
  trap_Print("  regex: . [a-z] [^x] \\d \\w \\s \\b ^ $ ( ) | * + ? {n,m}, lazy *? +?, (?i) first = ignore case\n");
  trap_Print("  e.g. ^3grep \"cvarlist cg_*\" r'cg_\\w*time\\w*'^7\n");
  trap_Print("       ^3grep \"mapinfo\" r'.*weapon.*'^7          whole lines that mention weapon\n");
  trap_Print("       ^3grep \"cmdlist\" r'^\\+mdd_\\w+' buttons.txt^7\n");
}

// Appends to a growing buffer; returns qfalse when out of memory.
typedef struct
{
  char*  data;
  size_t len, cap;
} buffer_t;

static qboolean append(buffer_t* b, char const* s, size_t n)
{
  if (b->len + n + 1 > b->cap)
  {
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->len + n + 1) cap *= 2;
    char* const grow = realloc(b->data, cap);
    if (!grow) return qfalse;
    b->data = grow;
    b->cap  = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
  return qtrue;
}

static void on_captured(char const* error, char** lines, size_t num)
{
  if (error)
  {
    say("grep: %s\n", error, "");
    return;
  }
  buffer_t out      = { 0 };
  int32_t  matches  = 0;
  int32_t  in_lines = 0;
  int32_t  gave_up  = 0;
  qboolean ok       = qtrue;
  for (size_t i = 0; i < num && ok; ++i)
  {
    char const* const line = lines[i];
    size_t const      len  = strlen(line);
    qboolean          any  = qfalse;
    size_t            from = 0, start, end;
    while (from <= len && ok)
    {
      int const r = re_search(&regex, line, len, from, &start, &end);
      if (r < 0) ++gave_up;
      if (r <= 0) break;
      if (end == start)
      {
        from = start + 1; // an empty match is nothing to print; look on behind it
        continue;
      }
      ++matches;
      any  = qtrue;
      ok   = append(&out, line + start, end - start) && append(&out, "\n", 1);
      from = end;
    }
    if (any) ++in_lines;
  }
  if (!ok)
  {
    free(out.data);
    trap_Print("^1grep: out of memory\n");
    return;
  }

  char counts[160];
  snprintf(
    counts, sizeof(counts), "%i match%s in %i of %i lines", matches, matches == 1 ? "" : "es", in_lines, (int32_t)num);
  if (out_file[0])
  {
    fileHandle_t f;
    if (trap_FS_FOpenFile(out_file, &f, FS_WRITE) < 0)
    {
      say("^1grep: can't write %s\n", out_file, "");
      free(out.data);
      return;
    }
    if (out.len) trap_FS_Write(out.data, (int32_t)out.len, f);
    trap_FS_FCloseFile(f);
    say("^3grep: %s, written to %s\n", counts, out_file);
  }
  else
  {
    // one console print per match: a single print is cut at the engine's print buffer
    int32_t printed = 0;
    for (char* p = out.data; p && *p && printed < MAX_PRINTED; ++printed)
    {
      char* const nl   = strchr(p, '\n');
      char const  keep = nl[1];
      nl[1]            = 0;
      // a match can be longer than one print allows
      for (size_t left = strlen(p); left;)
      {
        char         chunk[1001];
        size_t const n = left < sizeof(chunk) - 1 ? left : sizeof(chunk) - 1;
        memcpy(chunk, p, n);
        chunk[n] = 0;
        trap_Print(chunk);
        p += n;
        left -= n;
      }
      nl[1] = keep;
    }
    if (matches > MAX_PRINTED)
    {
      char more[96];
      snprintf(more, sizeof(more), "^3(first %i shown: give a file name to get all)\n", MAX_PRINTED);
      trap_Print(more);
    }
    say("^3grep: %s\n", counts, "");
  }
  if (gave_up) trap_Print("^3grep: some lines were skipped: the regex backtracks too much on them\n");
  free(out.data);
}

// The file name to write, or "" if it is not acceptable.
static qboolean check_file(char const* arg, char* file, size_t size)
{
  char   name[MAX_QPATH];
  size_t n = strlen(arg);
  if (n >= 2 && arg[0] == '(' && arg[n - 1] == ')') // "(file.txt)" as the usage line writes it
  {
    ++arg;
    n -= 2;
  }
  if (!n || n >= sizeof(name) - 5) return qfalse;
  memcpy(name, arg, n);
  name[n] = 0;
  for (char* c = name; *c; ++c)
  {
    if (*c == '\\') *c = '/';
    qboolean const alnum = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
    if (!alnum && !strchr("_-./", *c)) return qfalse;
  }
  if (name[0] == '/' || strstr(name, "..") || strstr(name, "//")) return qfalse;
  char const* const slash = strrchr(name, '/');
  char const* const dot   = strrchr(slash ? slash : name, '.');
  if (!dot)
    strcat(name, ".txt");
  else if (Q_stricmp(dot, ".txt") && Q_stricmp(dot, ".log"))
    return qfalse;
  snprintf(file, size, "%s", name);
  return qtrue;
}

void cmd_grep(void)
{
  int32_t const argc = trap_Argc();
  if (argc < 3)
  {
    usage();
    return;
  }
  char command[MAX_STRING_CHARS];
  trap_Argv(1, command, sizeof(command));

  // everything behind the command, put together again
  char   rest[MAX_STRING_CHARS * 2];
  size_t len = 0;
  rest[0]    = 0;
  for (int32_t i = 2; i < argc; ++i)
  {
    char arg[MAX_STRING_CHARS];
    trap_Argv(i, arg, sizeof(arg));
    int const n = snprintf(rest + len, sizeof(rest) - len, "%s%s", i > 2 ? " " : "", arg);
    if (n < 0 || (size_t)n >= sizeof(rest) - len)
    {
      trap_Print("^1grep: line too long\n");
      return;
    }
    len += (size_t)n;
  }
  if (rest[0] != 'r' || rest[1] != '\'')
  {
    trap_Print("^1grep: the regex must be written as r'...'\n");
    usage();
    return;
  }

  // r'...': up to the first ' that has no backslash in front of it
  char const* p      = rest + 2;
  size_t      n      = 0;
  qboolean    closed = qfalse;
  while (*p && n < sizeof(regex_text) - 2)
  {
    if (p[0] == '\\' && p[1] == '\'')
    {
      regex_text[n++] = '\'';
      p += 2;
    }
    else if (p[0] == '\\' && p[1])
    {
      regex_text[n++] = *p++; // any other escape belongs to the regex: keep both characters
      regex_text[n++] = *p++;
    }
    else if (*p == '\'')
    {
      closed = qtrue;
      ++p;
      break;
    }
    else
    {
      regex_text[n++] = *p++;
    }
  }
  regex_text[n] = 0;
  if (!closed)
  {
    trap_Print("^1grep: the regex has no closing ' (write \\' for a ' inside it)\n");
    return;
  }
  if (!n)
  {
    trap_Print("^1grep: the regex is empty\n");
    return;
  }

  // what is left is the file name
  while (*p == ' ') ++p;
  out_file[0] = 0;
  if (*p)
  {
    if (strchr(p, ' '))
    {
      say("^1grep: unexpected text after the regex: %s\n", p, "");
      return;
    }
    if (!check_file(p, out_file, sizeof(out_file)))
    {
      say("^1grep: can't use %s as a file name (letters, digits, _ - . /, ending in .txt or .log)\n", p, "");
      return;
    }
  }

  if (!re_compile(&regex, regex_text))
  {
    say("^1grep: bad regex %s: %s\n", regex_text, regex.error);
    return;
  }
  for (char const* c = command; *c; ++c)
  {
    if (*c == '\n' || *c == '\r')
    {
      trap_Print("^1grep: the command must be one line\n");
      return;
    }
  }
  char const* first = command;
  while (*first == ' ' || *first == '\\' || *first == '/') ++first;
  if (!*first)
  {
    trap_Print("^1grep: the command is empty\n");
    return;
  }
  if (!Q_stricmpn(first, "grep", 4) || !Q_stricmpn(first, "clipboard", 9))
  {
    trap_Print("grep: can't search the output of grep or clipboard (they read the console themselves)\n");
    return;
  }
  capture_command(first, on_captured);
}
