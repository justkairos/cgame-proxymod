/*
  ==============================
  alias / unalias (also mdd_alias / mdd_unalias): user-defined commands as in Quake 1 and 2. Quake 3 dropped them.

    alias ss screenshotjpeg         "ss" runs "screenshotjpeg"
    alias rec "g_synchronousClients 1; record"
    alias +grapple +button5         "+grapple" runs "+button5", "-grapple" runs "-button5"
    alias +zoomfov cg_fov 60 120    "+zoomfov" sets cg_fov to 60, "-zoomfov" sets it to 120
    alias +zoomfov cg_fov 60        "-zoomfov" puts back whatever cg_fov was when "+zoomfov" was pressed
    alias                           list
    unalias ss

  How a name gets here: the engine runs its own commands first, then cvars, then asks the cgame. defrag is asked
  before the proxymod. So an alias can never replace an engine command, a cvar or a defrag command - which is the
  Quake 1 order too (commands, then aliases).

  A key bound to "+name" makes the engine send "+name <key> <time>" on press (and on every key repeat) and
  "-name <key> <time>" on release (Key_ParseBinding). A "+" alias to commands is plain text substitution: every
  ";"-separated part that starts with "+" is sent with the same <key> <time>, and on release the same parts are
  sent with "-", exactly what the engine does for a key bound to that text. If an alias "-name" exists it is run
  on release instead. A "+" alias to a variable keeps the set of keys holding it, like an engine button.

  Aliases are kept in mdd_aliases.txt in the mod folder, because this DLL is reloaded on every map change.
  ==============================
*/
#include "cg_alias.h"

#include "cg_local.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALIAS_FILE      "mdd_aliases.txt"
#define ALIAS_MAGIC     "mdd aliases 1"
#define MAX_ALIASES     256
#define ALIAS_NAME_LEN  32   // including the terminator
#define ALIAS_TEXT_LEN  512  // including the terminator
#define ALIAS_VALUE_LEN 256  // including the terminator
#define ALIAS_KEYS      4    // keys that can hold one "+" variable alias at the same time
#define KEY_CONSOLE     (-1) // "+name" typed without a key number

// Runaway protection: "alias a a" would otherwise spin inside the engine's command buffer forever.
#define LOOP_WINDOW_MS 250
#define LOOP_MAX       1000 // alias executions per window; a loop gets there in well under a millisecond

typedef struct
{
  char     name[ALIAS_NAME_LEN];
  qboolean is_var;
  char     text[ALIAS_TEXT_LEN]; // !is_var: the commands
  char     var[ALIAS_NAME_LEN];  // is_var: the variable
  char     on[ALIAS_VALUE_LEN];  // is_var: value on press
  char     off[ALIAS_VALUE_LEN]; // is_var && has_off: value on release
  qboolean has_off;

  // state of a "+" variable alias, not saved
  int32_t  keys[ALIAS_KEYS];
  int32_t  num_keys;
  qboolean active;                 // "on" has been applied and not yet undone
  char     saved[ALIAS_VALUE_LEN]; // !has_off: value of the variable when it was pressed
} alias_t;

static alias_t aliases[MAX_ALIASES];
static int32_t num_aliases;

static int32_t loop_window;
static int32_t loop_count;

static char const* const engine_cmds[] = {
#include "engine_cmds.inc"
};

static char const* const own_cmds[] = { "alias", "unalias", "mdd_alias", "mdd_unalias" };

static void copy_str(char* dst, size_t size, char const* src)
{
  size_t n = strlen(src);
  if (n >= size) n = size - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

static void print(char const* fmt, char const* a, char const* b, char const* c)
{
  char line[ALIAS_TEXT_LEN + 2 * ALIAS_VALUE_LEN + 128];
  snprintf(line, sizeof(line), fmt, a, b, c);
  trap_Print(line);
}

// one command line to the engine's command buffer
static void send_line(char const* text)
{
  char line[ALIAS_TEXT_LEN + 80];
  snprintf(line, sizeof(line), "%s\n", text);
  trap_SendConsoleCommand(line);
}

static void set_var(char const* var, char const* value)
{
  char line[ALIAS_NAME_LEN + ALIAS_VALUE_LEN + 8];
  snprintf(line, sizeof(line), "%s \"%s\"\n", var, value);
  trap_SendConsoleCommand(line);
}

static alias_t* find(char const* name)
{
  for (int32_t i = 0; i < num_aliases; ++i)
  {
    if (!Q_stricmp(aliases[i].name, name)) return &aliases[i];
  }
  return NULL;
}

// the alias with the other sign: "+x" for "-x" and the other way round
static alias_t* find_counterpart(char const* name)
{
  char other[ALIAS_NAME_LEN];
  copy_str(other, sizeof(other), name);
  other[0] = name[0] == '+' ? '-' : '+';
  return find(other);
}

static qboolean in_list(char const* name, char const* const* list, size_t count)
{
  for (size_t i = 0; i < count; ++i)
  {
    if (!Q_stricmp(name, list[i])) return qtrue;
  }
  return qfalse;
}

// Why `name` can't be an alias, or NULL if it can.
static char const* name_problem(char const* name)
{
  size_t const len = strlen(name);
  if (!len) return "the name is empty";
  if (len >= ALIAS_NAME_LEN) return "the name is too long (31 characters at most)";
  for (char const* c = name; *c; ++c)
  {
    if (*c <= ' ' || *c > '~' || *c == '"' || *c == ';') return "names can't contain spaces, quotes or ;";
  }
  char const* const base = name[0] == '+' || name[0] == '-' ? name + 1 : name;
  if (!*base) return "the name is empty";
  if (!Q_stricmpn(base, "mdd_", 4) || in_list(name, own_cmds, ARRAY_LEN(own_cmds)) || !Q_stricmp(name, "search"))
    return "that is a proxymod name";
  if (in_list(name, engine_cmds, ARRAY_LEN(engine_cmds)))
    return "that is an engine command, and the engine runs its own commands first";
  if (name[0] == '+' || name[0] == '-')
  {
    // "+attack" would also claim "-attack"
    char other[ALIAS_NAME_LEN];
    copy_str(other, sizeof(other), name);
    other[0] = name[0] == '+' ? '-' : '+';
    if (in_list(other, engine_cmds, ARRAY_LEN(engine_cmds)))
      return "that is an engine button, and the engine runs its own commands first";
  }
  char value[8];
  trap_Cvar_VariableStringBuffer(name, value, sizeof(value));
  if (value[0]) return "that is a cvar, and the engine checks cvars first";
  return NULL;
}

// control characters (tabs, newlines) would break the command buffer and the file format
static void clean(char* s, qboolean strip_quotes)
{
  char* out = s;
  for (; *s; ++s)
  {
    if (strip_quotes && *s == '"') continue;
    *out++ = (unsigned char)*s < ' ' ? ' ' : *s;
  }
  *out = 0;
}

static void trim(char* s)
{
  size_t n = strlen(s);
  while (n && s[n - 1] == ' ') s[--n] = 0;
  size_t lead = 0;
  while (s[lead] == ' ') ++lead;
  if (lead) memmove(s, s + lead, n - lead + 1);
}

/*
  ------------------------------ file ------------------------------
  one alias per line, fields separated by tabs:
    C <name> <text>
    V <name> <variable> <on> <0|1 has off> <off>
*/
static void save(void)
{
  fileHandle_t f;
  if (trap_FS_FOpenFile(ALIAS_FILE, &f, FS_WRITE) < 0)
  {
    trap_Print("^1[alias] can't write " ALIAS_FILE ": aliases will be lost on the next map change\n");
    return;
  }
  char          line[ALIAS_NAME_LEN * 2 + ALIAS_TEXT_LEN + ALIAS_VALUE_LEN * 2 + 16];
  int32_t const head = snprintf(line, sizeof(line), ALIAS_MAGIC "\n");
  trap_FS_Write(line, head, f);
  for (int32_t i = 0; i < num_aliases; ++i)
  {
    alias_t const* const a = &aliases[i];
    int32_t const        n = a->is_var ? snprintf(
                                    line,
                                    sizeof(line),
                                    "V\t%s\t%s\t%s\t%i\t%s\n",
                                    a->name,
                                    a->var,
                                    a->on,
                                    a->has_off ? 1 : 0,
                                    a->has_off ? a->off : "")
                                       : snprintf(line, sizeof(line), "C\t%s\t%s\n", a->name, a->text);
    trap_FS_Write(line, n, f);
  }
  trap_FS_FCloseFile(f);
}

static void register_name(char const* name)
{
  trap_AddCommand(name); // tab completion; the engine ignores a name it already has
  if (name[0] == '+')
  {
    char release[ALIAS_NAME_LEN];
    copy_str(release, sizeof(release), name);
    release[0] = '-';
    trap_AddCommand(release);
  }
}

// Adds or replaces. The fields have been validated by the caller, except the name.
static alias_t* define(char const* name, char const** problem)
{
  *problem = name_problem(name);
  if (*problem) return NULL;
  alias_t* a = find(name);
  if (!a)
  {
    if (num_aliases >= MAX_ALIASES)
    {
      *problem = "too many aliases (256 at most)";
      return NULL;
    }
    a = &aliases[num_aliases++];
  }
  memset(a, 0, sizeof(*a));
  copy_str(a->name, sizeof(a->name), name);
  return a;
}

// split `line` at the next tab; returns the rest, or NULL if there was no tab
static char* next_field(char* line)
{
  char* const tab = strchr(line, '\t');
  if (!tab) return NULL;
  *tab = 0;
  return tab + 1;
}

static void load_line(char* line)
{
  char* name = next_field(line);
  if (!name) return;
  char* rest = next_field(name);
  if (!rest) return;

  char const* problem = NULL;
  if (line[0] == 'C' && !line[1])
  {
    clean(rest, qfalse);
    trim(rest);
    if (!rest[0] || strlen(rest) >= ALIAS_TEXT_LEN) return;
    alias_t* const a = define(name, &problem);
    if (a) copy_str(a->text, sizeof(a->text), rest);
  }
  else if (line[0] == 'V' && !line[1])
  {
    char* const on      = next_field(rest);
    char* const has_off = on ? next_field(on) : NULL;
    if (!has_off) return;
    char*       off = next_field(has_off); // may be missing if an editor stripped the trailing tab
    static char empty[1];
    if (!off) off = empty;
    clean(on, qtrue);
    clean(off, qtrue);
    if (name[0] != '+' || !rest[0] || strlen(rest) >= ALIAS_NAME_LEN) return;
    if (strlen(on) >= ALIAS_VALUE_LEN || strlen(off) >= ALIAS_VALUE_LEN) return;
    for (char const* c = rest; *c; ++c)
    {
      if (*c <= ' ' || *c > '~' || *c == '"' || *c == ';') return;
    }
    alias_t* const a = define(name, &problem);
    if (a)
    {
      a->is_var = qtrue;
      copy_str(a->var, sizeof(a->var), rest);
      copy_str(a->on, sizeof(a->on), on);
      a->has_off = has_off[0] == '1';
      if (a->has_off) copy_str(a->off, sizeof(a->off), off);
    }
  }
  else
  {
    return;
  }
  if (problem) print("^3[alias] %s from " ALIAS_FILE " skipped: %s\n", name, problem, "");
}

static void load(void)
{
  fileHandle_t  f;
  int32_t const len = trap_FS_FOpenFile(ALIAS_FILE, &f, FS_READ);
  if (len < 0) return; // none saved yet
  if (len == 0 || len > 1024 * 1024)
  {
    trap_FS_FCloseFile(f);
    return;
  }
  char* const buf = malloc((size_t)len + 1);
  if (!buf)
  {
    trap_FS_FCloseFile(f);
    return;
  }
  trap_FS_Read(buf, len, f);
  trap_FS_FCloseFile(f);
  buf[len] = 0;

  qboolean first = qtrue;
  for (char* line = buf; line;)
  {
    char* const end = strchr(line, '\n');
    if (end) *end = 0;
    size_t const n = strlen(line);
    if (n && line[n - 1] == '\r') line[n - 1] = 0;
    if (first)
    {
      first = qfalse;
      if (strcmp(line, ALIAS_MAGIC)) break; // not our file
    }
    else
    {
      load_line(line);
    }
    line = end ? end + 1 : NULL;
  }
  free(buf);
}

/*
  ------------------------------ running ------------------------------
*/

// qfalse once aliases are being run faster than any key could (an alias that runs itself)
static qboolean loop_guard(char const* name)
{
  int32_t const window = trap_Milliseconds() / LOOP_WINDOW_MS;
  if (window != loop_window)
  {
    loop_window = window;
    loop_count  = 0;
  }
  if (++loop_count <= LOOP_MAX) return qtrue;
  if (loop_count == LOOP_MAX + 1) print("^1[alias] %s runs in a loop, stopped\n", name, "", "");
  return qfalse;
}

/*
  Sends the ";"-separated parts of a "+"/"-" alias.
    press:   a part starting with "+" is sent with `args` (the engine's "<key> <time>"), other parts as they are
    release: only the parts starting with "+" are sent, with "-" and `args`
  explicit (an alias named "-x"): every part is sent, "+" and "-" parts with `args`
*/
static void send_parts(char const* text, char const* args, qboolean release, qboolean explicit_release)
{
  char buf[ALIAS_TEXT_LEN];
  copy_str(buf, sizeof(buf), text);
  for (char* p = buf; p;)
  {
    // a ; inside quotes does not separate
    char*    end    = NULL;
    qboolean quoted = qfalse;
    for (char* c = p; *c; ++c)
    {
      if (*c == '"') quoted = !quoted;
      if (*c == ';' && !quoted)
      {
        end = c;
        break;
      }
    }
    if (end) *end = 0;
    while (*p == ' ') ++p;

    char           line[ALIAS_TEXT_LEN + 64];
    qboolean const button = *p == '+' || (explicit_release && *p == '-');
    if (button && release && !explicit_release)
      snprintf(line, sizeof(line), "-%s%s%s", p + 1, args[0] ? " " : "", args);
    else if (button)
      snprintf(line, sizeof(line), "%s%s%s", p, args[0] ? " " : "", args);
    else if (!release || explicit_release)
      snprintf(line, sizeof(line), "%s", p);
    else
      line[0] = 0;
    if (line[0]) send_line(line);

    p = end ? end + 1 : NULL;
  }
}

// The alias is going away or being replaced: undo whatever it may be holding. For commands that is every "+"
// part released without a key number, which the engine takes as "clear this button" (as typed in the console).
static void var_release(alias_t* a);
static void let_go(alias_t* a)
{
  if (a->is_var)
  {
    if (a->active) var_release(a);
  }
  else if (a->name[0] == '+')
  {
    send_parts(a->text, "", qtrue, qfalse);
  }
}

static void var_release(alias_t* a)
{
  a->num_keys = 0;
  if (a->has_off)
    set_var(a->var, a->off);
  else if (a->active)
    set_var(a->var, a->saved);
  a->active = qfalse;
}

// "+name [<key> <time>]" of a variable alias
static void var_down(alias_t* a, char const* key)
{
  int32_t const k = key[0] ? atoi(key) : KEY_CONSOLE;
  for (int32_t i = 0; i < a->num_keys; ++i)
  {
    if (a->keys[i] == k) return; // key repeat
  }
  if (a->num_keys >= ALIAS_KEYS) return;
  a->keys[a->num_keys++] = k;
  if (a->active) return; // another key already holds it
  if (!a->has_off)
  {
    trap_Cvar_VariableStringBuffer(a->var, a->saved, sizeof(a->saved));
    clean(a->saved, qtrue);
  }
  set_var(a->var, a->on);
  a->active = qtrue;
}

// "-name [<key> <time>]" of a variable alias
static void var_up(alias_t* a, char const* key)
{
  if (key[0])
  {
    int32_t const k = atoi(key);
    int32_t       i = 0;
    while (i < a->num_keys && a->keys[i] != k) ++i;
    if (i == a->num_keys) return; // key up without a down
    a->keys[i] = a->keys[--a->num_keys];
    if (a->num_keys) return; // another key still holds it
  }
  // the last key, or typed in the console: let go whatever holds it
  var_release(a);
}

static void run(alias_t* a, char const* cmd)
{
  if (!loop_guard(cmd)) return;

  char key[16];
  char time[16];
  char args[40];
  trap_Argv(1, key, sizeof(key));
  trap_Argv(2, time, sizeof(time));
  snprintf(args, sizeof(args), "%s%s%s", key, time[0] ? " " : "", time);

  if (cmd[0] == '-')
  {
    alias_t* const own = find(cmd);
    if (own)
    {
      // an alias for the release itself replaces the automatic one; a held variable alias just lets go
      if (a != own && a->is_var)
      {
        a->num_keys = 0;
        a->active   = qfalse;
      }
      send_parts(own->text, args, qtrue, qtrue);
    }
    else if (a->is_var)
      var_up(a, key);
    else
      send_parts(a->text, args, qtrue, qfalse);
  }
  else if (cmd[0] == '+')
  {
    if (a->is_var)
      var_down(a, key);
    else
      send_parts(a->text, args, qfalse, qfalse);
  }
  else
  {
    send_line(a->text);
  }
}

/*
  ------------------------------ commands ------------------------------
*/
static void print_alias(alias_t const* a)
{
  if (!a->is_var)
    print("alias %s \"%s\"\n", a->name, a->text, "");
  else if (a->has_off)
  {
    char values[ALIAS_VALUE_LEN * 2 + 8];
    snprintf(values, sizeof(values), "\"%s\" \"%s\"", a->on, a->off);
    print("alias %s %s %s\n", a->name, a->var, values);
  }
  else
  {
    char values[ALIAS_VALUE_LEN + 4];
    snprintf(values, sizeof(values), "\"%s\"", a->on);
    print("alias %s %s %s   ^5(release: value from before the press)\n", a->name, a->var, values);
  }
}

static void usage(void)
{
  trap_Print("usage: alias <name> <commands>           e.g. alias ss screenshotjpeg\n");
  trap_Print("       alias +<name> <+button>            e.g. alias +grapple +button5\n");
  trap_Print("       alias +<name> <cvar> <on> [<off>]  e.g. alias +zoomfov cg_fov 60 120\n");
  trap_Print("       alias                              list all;  alias <name>  show one;  unalias <name>\n");
}

static void cmd_alias(void)
{
  int32_t const argc = trap_Argc();
  if (argc == 1)
  {
    if (!num_aliases)
    {
      trap_Print("no aliases\n");
      usage();
      return;
    }
    for (int32_t i = 0; i < num_aliases; ++i) print_alias(&aliases[i]);
    char count[16];
    snprintf(count, sizeof(count), "%i", num_aliases);
    print("%s alias%s\n", count, num_aliases == 1 ? "" : "es", "");
    return;
  }

  char name[MAX_STRING_CHARS];
  trap_Argv(1, name, sizeof(name));
  if (argc == 2)
  {
    alias_t const* const a = strlen(name) < ALIAS_NAME_LEN ? find(name) : NULL;
    if (a)
      print_alias(a);
    else
    {
      print("no alias %s\n", name, "", "");
      usage();
    }
    return;
  }

  char first[MAX_STRING_CHARS];
  trap_Argv(2, first, sizeof(first));

  // "+name <variable> <on> [<off>]": one word, then one or two values
  qboolean is_var = name[0] == '+' && (argc == 4 || argc == 5) && first[0] && first[0] != '+' && first[0] != '-' &&
                    strlen(first) < ALIAS_NAME_LEN;
  for (char const* c = first; is_var && *c; ++c)
  {
    if (*c <= ' ' || *c > '~' || *c == '"' || *c == ';') is_var = qfalse;
  }

  char on[MAX_STRING_CHARS]  = "";
  char off[MAX_STRING_CHARS] = "";
  char text[MAX_STRING_CHARS * 2];
  text[0] = 0;
  if (is_var)
  {
    trap_Argv(3, on, sizeof(on));
    if (argc == 5) trap_Argv(4, off, sizeof(off));
    clean(on, qtrue);
    clean(off, qtrue);
    if (strlen(on) >= ALIAS_VALUE_LEN || strlen(off) >= ALIAS_VALUE_LEN)
    {
      trap_Print("^1[alias] value too long (255 characters at most)\n");
      return;
    }
  }
  else
  {
    // the rest of the line, as Quake does
    size_t len = 0;
    for (int32_t i = 2; i < argc; ++i)
    {
      char arg[MAX_STRING_CHARS];
      trap_Argv(i, arg, sizeof(arg));
      int const n = snprintf(text + len, sizeof(text) - len, "%s%s", i > 2 ? " " : "", arg);
      if (n < 0 || (size_t)n >= sizeof(text) - len)
      {
        len = sizeof(text); // too long
        break;
      }
      len += (size_t)n;
    }
    clean(text, qfalse);
    trim(text);
    if (len >= sizeof(text) || strlen(text) >= ALIAS_TEXT_LEN)
    {
      trap_Print("^1[alias] commands too long (511 characters at most)\n");
      return;
    }
    if (!text[0])
    {
      trap_Print("^1[alias] nothing to run\n");
      return;
    }
  }

  // redefining something that may be held: let go first, with the old definition
  alias_t* const old = strlen(name) < ALIAS_NAME_LEN ? find(name) : NULL;
  if (old) let_go(old);

  char const*    problem = NULL;
  alias_t* const a       = define(name, &problem);
  if (!a)
  {
    print("^1[alias] can't create %s: %s\n", name, problem, "");
    return;
  }
  if (is_var)
  {
    a->is_var = qtrue;
    copy_str(a->var, sizeof(a->var), first);
    copy_str(a->on, sizeof(a->on), on);
    a->has_off = argc == 5;
    if (a->has_off) copy_str(a->off, sizeof(a->off), off);
  }
  else
  {
    copy_str(a->text, sizeof(a->text), text);
  }
  register_name(a->name);
  save();
  print_alias(a);
}

static void cmd_unalias(void)
{
  int32_t const argc = trap_Argc();
  if (argc < 2)
  {
    trap_Print("usage: unalias <name> [<name> ...]\n");
    return;
  }
  qboolean changed = qfalse;
  for (int32_t i = 1; i < argc; ++i)
  {
    char name[MAX_STRING_CHARS];
    trap_Argv(i, name, sizeof(name));
    alias_t* const a = strlen(name) < ALIAS_NAME_LEN ? find(name) : NULL;
    if (!a)
    {
      print("no alias %s\n", name, "", "");
      continue;
    }
    let_go(a);
    print("removed alias %s\n", a->name, "", "");
    // keep the order of the list
    int32_t const index = (int32_t)(a - aliases);
    memmove(a, a + 1, (size_t)(num_aliases - index - 1) * sizeof(*a));
    --num_aliases;
    changed = qtrue;
  }
  // The names stay registered for tab completion until the next map load: removing a command by name is not
  // safe on every engine (it can remove, or error out on, an engine command of the same name).
  if (changed) save();
}

qboolean alias_console_command(char const* cmd)
{
  if (!Q_stricmp(cmd, "alias") || !Q_stricmp(cmd, "mdd_alias"))
  {
    cmd_alias();
    return qtrue;
  }
  if (!Q_stricmp(cmd, "unalias") || !Q_stricmp(cmd, "mdd_unalias"))
  {
    cmd_unalias();
    return qtrue;
  }
  if (strlen(cmd) >= ALIAS_NAME_LEN) return qfalse;

  alias_t* a = find(cmd);
  // "-name" without an alias of its own is the release of "+name"
  if (cmd[0] == '-')
  {
    alias_t* const press = find_counterpart(cmd);
    if (press) a = press;
  }
  if (!a) return qfalse;
  run(a, cmd);
  return qtrue;
}

void init_alias(void)
{
  num_aliases = 0;
  loop_window = 0;
  loop_count  = 0;
  load();
  for (size_t i = 0; i < ARRAY_LEN(own_cmds); ++i) trap_AddCommand(own_cmds[i]);
  for (int32_t i = 0; i < num_aliases; ++i) register_name(aliases[i].name);
}

void shutdown_alias(void)
{
  for (int32_t i = 0; i < num_aliases; ++i)
  {
    if (aliases[i].is_var && aliases[i].active) var_release(&aliases[i]);
  }
}
