/*
  ==============================
  search / mdd_search <pattern> [<pattern> ...]

  Looks for the pattern in
    1. the names of all cvars       (the engine's cvarlist)
    2. the names of all commands    (the engine's cmdlist)
    3. the description of engine cvars, the text "\m_side" prints under the value
    4. the help files in system/help: the proxymod's own and DeFRaG's

  A cgame can neither enumerate cvars and commands nor read a cvar's description nor list a directory. 1 and 2
  are therefore done by the engine's own list commands, which take a filter and see everything that is
  registered (engine, defrag, proxymod, +buttons). 3 uses a copy of oDFe's descriptions compiled into the DLL
  (engine_cvar_help.inc), 4 opens the help files by name from a compiled-in list (help_names.inc).

  Pattern: * any run of characters, ? one character, [a-z] one of a set, case-insensitive. Without a wildcard it
  is a "contains" search. In text (3, 4) a pattern always matches anywhere in a line.
  ==============================
*/
#include "cg_search.h"

#include "cg_local.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATTERN_LEN   64
#define MAX_PATTERNS  8
#define MAX_HELP_FILE (256 * 1024)
#define SNIPPET_LEN   200

typedef struct
{
  char const* name;
  char const* text;
} cvar_help_t;

static cvar_help_t const engine_cvar_help[] = {
#include "engine_cvar_help.inc"
};

static char const* const help_names[] = {
#include "help_names.inc"
};

static char lower(char c)
{
  return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// Length of the pattern element at p (a character, ? or a [set]) and whether it matches c.
static size_t element(char const* p, char c, qboolean* matches)
{
  if (*p == '?')
  {
    *matches = qtrue;
    return 1;
  }
  char const* const close = *p == '[' && p[1] ? strchr(p + 2, ']') : NULL; // a set has at least one member
  if (!close)
  {
    *matches = lower(*p) == lower(c);
    return 1;
  }
  *matches = qfalse;
  for (char const* s = p + 1; s < close; ++s)
  {
    if (s + 2 < close && s[1] == '-')
    {
      if (lower(c) >= lower(s[0]) && lower(c) <= lower(s[2])) *matches = qtrue;
      s += 2;
    }
    else if (lower(*s) == lower(c))
    {
      *matches = qtrue;
    }
  }
  return (size_t)(close - p) + 1;
}

// Does the whole of `text` match `pattern`? Linear backtracking: only the last * is ever retried.
static qboolean glob(char const* pattern, char const* text)
{
  char const* star_p = NULL; // pattern position after the last *
  char const* star_t = NULL; // text position that * is currently assumed to end at
  while (*text)
  {
    if (*pattern == '*')
    {
      star_p = ++pattern;
      star_t = text;
      continue;
    }
    if (*pattern)
    {
      qboolean     matches;
      size_t const n = element(pattern, *text, &matches);
      if (matches)
      {
        pattern += n;
        ++text;
        continue;
      }
    }
    if (!star_p) return qfalse;
    pattern = star_p;
    text    = ++star_t;
  }
  while (*pattern == '*') ++pattern;
  return !*pattern;
}

/*
  The pattern as it is searched for:
    names: the pattern itself, open at the end like the engine's filter ("cg_*time" also finds cg_drawTimer)
    text:  anywhere in the line
  Both get a * at the end; the text pattern also one at the start.
*/
typedef struct
{
  char shown[PATTERN_LEN];    // what cvarlist and cmdlist get
  char name[PATTERN_LEN + 2]; // shown + "*"
  char text[PATTERN_LEN + 4]; // "*" + shown + "*"
  char at[PATTERN_LEN + 2];   // name without leading *: matches where the hit starts in a line
} pattern_t;

// qfalse if nothing searchable is left. The pattern goes back into the console inside quotes, so only
// characters that a name, a filter or a plain word can contain are kept.
static qboolean make_pattern(char const* arg, pattern_t* out)
{
  size_t   n    = 0;
  qboolean wild = qfalse;
  for (char const* c = arg; *c && n < PATTERN_LEN - 3; ++c)
  {
    qboolean const alnum = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
    if (!alnum && !strchr("_+-.*?[] ", *c)) continue;
    if (*c == '*' || *c == '?' || *c == '[') wild = qtrue;
    out->shown[n++] = *c;
  }
  while (n && out->shown[n - 1] == ' ') --n;
  out->shown[n] = 0;
  size_t lead   = 0;
  while (out->shown[lead] == ' ') ++lead;
  if (lead) memmove(out->shown, out->shown + lead, n - lead + 1);
  if (!out->shown[0]) return qfalse;
  if (!wild)
  {
    // no wildcard: contains
    char plain[PATTERN_LEN];
    snprintf(plain, sizeof(plain), "%s", out->shown);
    snprintf(out->shown, sizeof(out->shown), "*%.*s*", PATTERN_LEN - 5, plain);
  }
  snprintf(out->name, sizeof(out->name), "%s*", out->shown);
  snprintf(out->text, sizeof(out->text), "*%s*", out->shown);
  snprintf(out->at, sizeof(out->at), "%s", out->name + strspn(out->name, "*"));
  return qtrue;
}

void cmd_search(void)
{
  int32_t const argc = trap_Argc();
  if (argc < 2)
  {
    trap_Print("usage: search <pattern> [<pattern> ...]   (also mdd_search)\n");
    trap_Print("  searches cvar and command names, engine cvar descriptions and the help files\n");
    trap_Print("  * = any characters, ? = one character, [a-z] = one of a set; no wildcard = contains\n");
    trap_Print("  e.g. ^3search mouse^7, ^3search cg_*time^7, ^3search cg_crosshair*^7, ^3search +mdd*^7\n");
    return;
  }

  for (int32_t a = 1; a < argc && a <= MAX_PATTERNS; ++a)
  {
    char arg[MAX_STRING_CHARS];
    trap_Argv(a, arg, sizeof(arg));
    pattern_t p;
    if (!make_pattern(arg, &p)) continue;

    char line[PATTERN_LEN * 5 + 256];
    snprintf(
      line,
      sizeof(line),
      "echo \"^3--- cvars matching %s ---\"\ncvarlist \"%s\"\necho \"^3--- commands matching %s ---\"\ncmdlist "
      "\"%s\"\n" SEARCH_TEXT_COMMAND " \"%s\"\n",
      p.shown,
      p.shown,
      p.shown,
      p.shown,
      p.shown);
    trap_SendConsoleCommand(line);
  }
}

// one line of console output: a name, an optional value, and text cut to a sensible length on one line
static void print_hit(char const* color, char const* name, char const* value, char const* text, size_t text_len)
{
  char   flat[SNIPPET_LEN * 4 + 8];
  size_t n = 0;
  // descriptions may span lines and the console has no tabs: one line, single spaces
  for (size_t i = 0; i < text_len && n < sizeof(flat) - 4; ++i)
  {
    char const c = (unsigned char)text[i] < ' ' ? ' ' : text[i];
    if (c == ' ' && (!n || flat[n - 1] == ' ')) continue;
    flat[n++] = c;
  }
  flat[n] = 0;

  char line[sizeof(flat) + 400];
  if (value)
    snprintf(line, sizeof(line), "%s%s^7 is \"%s^7\"  %s\n", color, name, value, flat);
  else
    snprintf(line, sizeof(line), "%s%s^7  %s\n", color, name, flat);
  trap_Print(line);
}

// First line of a help file that matches, not counting the title box at the top. Returns the length of the
// piece to show (the part of the line around the match), or 0.
static size_t find_line(char* buf, pattern_t const* pattern, char const** found)
{
  int32_t  line_no = 0;
  qboolean in_box  = qfalse;
  for (char* line = buf; line;)
  {
    char* const end = strchr(line, '\n');
    if (end) *end = 0;
    size_t len = strlen(line);
    if (len && line[len - 1] == '\r') line[--len] = 0;
    ++line_no;

    // "=====" / title / "=====" at the very top repeats the name, which the name searches already cover
    qboolean const rule = len >= 3 && strspn(line, "=") == len;
    qboolean       skip = qfalse;
    if (line_no == 1 && rule)
      in_box = skip = qtrue;
    else if (in_box)
    {
      skip = qtrue;
      if (rule) in_box = qfalse;
    }

    if (!skip && len && glob(pattern->text, line))
    {
      while (*line == ' ') // indented example text
      {
        ++line;
        --len;
      }
      // a long paragraph: show the part where the match begins
      size_t start = 0;
      if (len > SNIPPET_LEN)
      {
        size_t at = 0;
        while (at < len && !glob(pattern->at, line + at)) ++at;
        if (at < len && at > SNIPPET_LEN / 4) start = at - SNIPPET_LEN / 4;
        if (start > len - SNIPPET_LEN) start = len - SNIPPET_LEN;
        while (start && line[start - 1] != ' ') --start; // begin at a word
      }
      *found = line + start;
      return len - start > SNIPPET_LEN ? SNIPPET_LEN : len - start;
    }
    line = end ? end + 1 : NULL;
  }
  return 0;
}

void cmd_search_text(void)
{
  if (trap_Argc() != 2) return; // internal: always sent by cmd_search with one quoted pattern
  char arg[MAX_STRING_CHARS];
  trap_Argv(1, arg, sizeof(arg));
  pattern_t p;
  if (!make_pattern(arg, &p)) return;

  char line[PATTERN_LEN + 128];
  snprintf(line, sizeof(line), "^3--- engine cvar descriptions matching %s ---\n", p.shown);
  trap_Print(line);
  int32_t descriptions = 0;
  for (size_t i = 0; i < ARRAY_LEN(engine_cvar_help); ++i)
  {
    cvar_help_t const* const h = &engine_cvar_help[i];
    if (!glob(p.name, h->name) && !glob(p.text, h->text)) continue;
    char value[256];
    trap_Cvar_VariableStringBuffer(h->name, value, sizeof(value));
    print_hit("^3", h->name, value, h->text, strlen(h->text));
    ++descriptions;
  }

  snprintf(line, sizeof(line), "^3--- help files matching %s ---\n", p.shown);
  trap_Print(line);
  int32_t files = 0;
  for (size_t i = 0; i < ARRAY_LEN(help_names); ++i)
  {
    char path[MAX_QPATH + 32];
    snprintf(path, sizeof(path), "system/help/%s.txt", help_names[i]);
    fileHandle_t  f;
    int32_t const len = trap_FS_FOpenFile(path, &f, FS_READ);
    if (len < 0) continue; // not installed
    if (len == 0 || len > MAX_HELP_FILE)
    {
      trap_FS_FCloseFile(f);
      continue;
    }
    char* const buf = malloc((size_t)len + 1);
    if (!buf)
    {
      trap_FS_FCloseFile(f);
      continue;
    }
    trap_FS_Read(buf, len, f);
    trap_FS_FCloseFile(f);
    buf[len] = 0;

    char const*  found     = NULL;
    size_t const found_len = find_line(buf, &p, &found);
    if (found_len)
    {
      print_hit("^5", help_names[i], NULL, found, found_len);
      ++files;
    }
    free(buf);
  }

  char counts[160];
  snprintf(
    counts,
    sizeof(counts),
    "^3%i description%s, %i help file%s^7  (^5? <name>^7 shows a help file)\n",
    descriptions,
    descriptions == 1 ? "" : "s",
    files,
    files == 1 ? "" : "s");
  trap_Print(counts);
}
