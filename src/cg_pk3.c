/*
  ==============================
  pk3search / pk3files / pk3folders <pattern> [<pattern> ...]

  Lists the files and folders whose name matches, with the pk3 each one is in, over every pk3 and every loose
  file of the mod folder and the base game folder.

  The engine gives a cgame no way to list files, so this reads the disk itself:
    - the mod folder is the folder this DLL was loaded from; the base game folder (fs_basegame, "baseq3") is its
      sibling
    - a pk3 is a zip file: its list of names is the "central directory" at the end of the file, which is read
      without unpacking anything
  Nothing is cached: every search reads the archives' directories again, so it always shows what is on disk.

  Pattern: * any run of characters, ? one character, [a-z] one of a set, case-insensitive. Without a wildcard it
  is a "contains" search. It is matched against the name (the last part of the path), or against the whole path
  if the pattern contains a /.
  ==============================
*/
#include "cg_pk3.h"

#include "cg_local.h"
#include "cg_search.h"
#include "sys_files.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OS_PATH_LEN 1024
#define MAX_ENTRY_PATH  512
#define MAX_WALK_DEPTH  24
#define MAX_CD_SIZE     (256 * 1024 * 1024) // a central directory larger than this is not a sane pk3
#define MAX_SHOWN       1000                // lines printed per list
#define MAX_PATTERNS    8
#define PATTERN_LEN     128

/*
  ------------------------------ zip central directory ------------------------------
*/
static uint32_t u16(unsigned char const* p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t u32(unsigned char const* p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Reads the central directory of a zip file. Returns a malloc'ed block of *size bytes, or NULL.
static unsigned char* read_central_directory(char const* os_path, size_t* size)
{
  FILE* const f = fopen(os_path, "rb");
  if (!f) return NULL;
  unsigned char* cd = NULL;
  do
  {
    if (sys_seek(f, 0, SEEK_END)) break;
    long long const file_size = sys_tell(f);
    if (file_size < 22) break;

    // the end record is the last thing in the file, followed only by a comment of at most 65535 bytes
    enum
    {
      TAIL_MAX = 22 + 65535
    };
    static unsigned char tail[TAIL_MAX];
    long long const      tail_len = file_size < TAIL_MAX ? file_size : TAIL_MAX;
    if (sys_seek(f, file_size - tail_len, SEEK_SET)) break;
    if (fread(tail, 1, (size_t)tail_len, f) != (size_t)tail_len) break;
    long long end = -1;
    for (long long i = tail_len - 22; i >= 0; --i)
    {
      if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6)
      {
        end = i;
        break;
      }
    }
    if (end < 0) break;
    uint32_t const cd_size   = u32(tail + end + 12);
    uint32_t const cd_offset = u32(tail + end + 16);
    if (cd_offset == 0xffffffffu || cd_size == 0xffffffffu) break; // zip64: the engine can't load it either
    if (!cd_size || cd_size > MAX_CD_SIZE || (long long)cd_offset + cd_size > file_size) break;

    cd = malloc(cd_size);
    if (!cd) break;
    if (sys_seek(f, cd_offset, SEEK_SET) || fread(cd, 1, cd_size, f) != cd_size)
    {
      free(cd);
      cd = NULL;
      break;
    }
    *size = cd_size;
  } while (0);
  fclose(f);
  return cd;
}

/*
  ------------------------------ the walk ------------------------------
*/
typedef struct
{
  pk3_entry_cb cb;
  void*        ctx;
  qboolean     stop;
  int32_t      pk3s;

  // folders already reported for the current pk3 (hashes of the lower-cased path)
  uint64_t* seen;
  size_t    seen_cap, seen_num;
} walk_t;

static uint64_t hash_path(char const* s, size_t n)
{
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i)
  {
    char const c = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] - 'A' + 'a') : s[i];
    h            = (h ^ (unsigned char)c) * 1099511628211ull;
  }
  return h ? h : 1;
}

// qtrue if the folder was not seen before in this pk3
static qboolean seen_add(walk_t* w, uint64_t h)
{
  if ((w->seen_num + 1) * 2 > w->seen_cap)
  {
    size_t const    cap  = w->seen_cap ? w->seen_cap * 2 : 1024;
    uint64_t* const grow = calloc(cap, sizeof(*grow));
    if (!grow) return qfalse; // out of memory: report nothing rather than everything twice
    for (size_t i = 0; i < w->seen_cap; ++i)
    {
      if (!w->seen[i]) continue;
      size_t j = (size_t)(w->seen[i] & (cap - 1));
      while (grow[j]) j = (j + 1) & (cap - 1);
      grow[j] = w->seen[i];
    }
    free(w->seen);
    w->seen     = grow;
    w->seen_cap = cap;
  }
  size_t j = (size_t)(h & (w->seen_cap - 1));
  while (w->seen[j])
  {
    if (w->seen[j] == h) return qfalse;
    j = (j + 1) & (w->seen_cap - 1);
  }
  w->seen[j] = h;
  ++w->seen_num;
  return qtrue;
}

// Reports the folders a path lies in ("a" and "a/b" for "a/b/c.txt"), each once per pk3; with_last: the path
// itself is a folder too.
static void report_folders(walk_t* w, char const* source, char* path, qboolean with_last)
{
  size_t const len = strlen(path);
  for (size_t i = 1; i <= len && !w->stop; ++i)
  {
    if (path[i] != '/' && !(i == len && with_last)) continue;
    if (!seen_add(w, hash_path(path, i))) continue;
    char const c = path[i];
    path[i]      = 0;
    if (!w->cb(source, path, qtrue, w->ctx)) w->stop = qtrue;
    path[i] = c;
  }
}

static void walk_pk3(walk_t* w, char const* os_path, char const* source)
{
  size_t               size = 0;
  unsigned char* const cd   = read_central_directory(os_path, &size);
  if (!cd) return;
  ++w->pk3s;
  if (w->seen_cap) memset(w->seen, 0, w->seen_cap * sizeof(*w->seen));
  w->seen_num = 0;

  for (size_t at = 0; at + 46 <= size && !w->stop;)
  {
    unsigned char const* const e = cd + at;
    if (e[0] != 'P' || e[1] != 'K' || e[2] != 1 || e[3] != 2) break;
    size_t const name_len = u16(e + 28);
    size_t const next     = at + 46 + name_len + u16(e + 30) + u16(e + 32);
    if (next > size) break;
    at = next;
    if (!name_len || name_len >= MAX_ENTRY_PATH) continue;

    char path[MAX_ENTRY_PATH];
    memcpy(path, e + 46, name_len);
    path[name_len] = 0;
    if (strlen(path) != name_len) continue; // a NUL inside the name
    for (char* c = path; *c; ++c)
    {
      if (*c == '\\') *c = '/';
    }
    char const* p = path;
    while (*p == '/') ++p; // a zip made by a careless tool
    if (p != path) memmove(path, p, strlen(p) + 1);
    size_t n = strlen(path);
    if (!n) continue;

    qboolean const is_folder = path[n - 1] == '/';
    if (is_folder) path[--n] = 0;
    if (!n) continue;
    report_folders(w, source, path, is_folder);
    if (!is_folder && !w->stop && !w->cb(source, path, qfalse, w->ctx)) w->stop = qtrue;
  }
  free(cd);
}

// one folder's entries, collected so that they can be sorted: the order the system lists them in is arbitrary
typedef struct
{
  char** names; // each name is preceded by one byte: 'd' folder, 'f' file
  size_t num, cap;
} listing_t;

static void collect(char const* name, int is_dir, void* ctx)
{
  listing_t* const l = ctx;
  if (l->num == l->cap)
  {
    size_t const cap  = l->cap ? l->cap * 2 : 256;
    char** const grow = realloc(l->names, cap * sizeof(*grow));
    if (!grow) return;
    l->names = grow;
    l->cap   = cap;
  }
  size_t const n     = strlen(name);
  char* const  entry = malloc(n + 2);
  if (!entry) return;
  entry[0] = is_dir ? 'd' : 'f';
  memcpy(entry + 1, name, n + 1);
  l->names[l->num++] = entry;
}

static int by_name(void const* a, void const* b)
{
  return Q_stricmp(*(char* const*)a + 1, *(char* const*)b + 1);
}

static void free_listing(listing_t* l)
{
  for (size_t i = 0; i < l->num; ++i) free(l->names[i]);
  free(l->names);
}

static qboolean has_ext(char const* name, char const* ext)
{
  size_t const n = strlen(name), e = strlen(ext);
  return n > e && !Q_stricmp(name + n - e, ext);
}

// Loose files below os_dir; rel is the path inside the game folder so far ("" at the top).
static void walk_loose(walk_t* w, char const* os_dir, char const* source, char const* rel, int32_t depth)
{
  listing_t l = { 0 };
  if (!sys_list_dir(os_dir, collect, &l)) return;
  qsort(l.names, l.num, sizeof(*l.names), by_name);
  for (size_t i = 0; i < l.num && !w->stop; ++i)
  {
    char const* const name = l.names[i] + 1;
    char              path[MAX_ENTRY_PATH];
    if (snprintf(path, sizeof(path), "%s%s%s", rel, rel[0] ? "/" : "", name) >= (int)sizeof(path)) continue;
    qboolean const is_folder = l.names[i][0] == 'd';
    if (!w->cb(source, path, is_folder, w->ctx))
    {
      w->stop = qtrue;
      break;
    }
    if (!is_folder || depth >= MAX_WALK_DEPTH) continue;
    char sub[MAX_OS_PATH_LEN];
    if (snprintf(sub, sizeof(sub), "%s/%s", os_dir, name) >= (int)sizeof(sub)) continue;
    walk_loose(w, sub, source, path, depth + 1);
  }
  free_listing(&l);
}

// One game folder: its pk3 archives, then its loose files.
static void walk_game_dir(walk_t* w, char const* os_dir, char const* game)
{
  listing_t l = { 0 };
  if (!sys_list_dir(os_dir, collect, &l)) return;
  qsort(l.names, l.num, sizeof(*l.names), by_name);
  // the engine searches the archives of a folder from the last name to the first: zzz.pk3 wins over aaa.pk3
  for (size_t i = l.num; i-- > 0 && !w->stop;)
  {
    char const* const name = l.names[i] + 1;
    if (l.names[i][0] != 'f' || !has_ext(name, ".pk3")) continue;
    char os_path[MAX_OS_PATH_LEN], source[MAX_ENTRY_PATH];
    if (snprintf(os_path, sizeof(os_path), "%s/%s", os_dir, name) >= (int)sizeof(os_path)) continue;
    if (snprintf(source, sizeof(source), "%s/%s", game, name) >= (int)sizeof(source)) continue;
    walk_pk3(w, os_path, source);
  }
  free_listing(&l);

  char source[MAX_ENTRY_PATH];
  snprintf(source, sizeof(source), "%s/", game);
  if (!w->stop) walk_loose(w, os_dir, source, "", 0);
}

int32_t pk3_scan(pk3_entry_cb cb, void* ctx)
{
  char mod_dir[MAX_OS_PATH_LEN];
  if (!sys_module_dir(mod_dir, sizeof(mod_dir))) return -1;
  for (char* c = mod_dir; *c; ++c)
  {
    if (*c == '\\') *c = '/';
  }
  char* const slash = strrchr(mod_dir, '/');
  if (!slash || !slash[1]) return -1;
  char const* const mod_name = slash + 1;

  walk_t w = { 0 };
  w.cb     = cb;
  w.ctx    = ctx;
  walk_game_dir(&w, mod_dir, mod_name);

  // the base game folder next to it, unless the mod folder is the base game
  char base[64];
  trap_Cvar_VariableStringBuffer("fs_basegame", base, sizeof(base));
  if (!base[0]) snprintf(base, sizeof(base), "baseq3");
  qboolean ok = qtrue;
  for (char const* c = base; *c; ++c)
  {
    if (*c == '/' || *c == '\\' || *c == ':' || (*c == '.' && c[1] == '.')) ok = qfalse;
  }
  if (ok && Q_stricmp(base, mod_name) && !w.stop)
  {
    char base_dir[MAX_OS_PATH_LEN];
    if (snprintf(base_dir, sizeof(base_dir), "%.*s/%s", (int)(slash - mod_dir), mod_dir, base) < (int)sizeof(base_dir))
      walk_game_dir(&w, base_dir, base);
  }
  free(w.seen);
  return w.pk3s;
}

/*
  ------------------------------ the commands ------------------------------
*/
typedef struct
{
  char     glob[PATTERN_LEN + 4];
  qboolean whole_path;
} pk3_pattern_t;

typedef struct
{
  pk3_pattern_t patterns[MAX_PATTERNS];
  int32_t       num_patterns;
  qboolean      files, folders;

  int32_t files_found, folders_found;
  char    last_source[MAX_ENTRY_PATH];
  int32_t sources; // distinct pk3s / folders with a matching file

  // folder lines are printed after all files
  char*   folder_lines[MAX_SHOWN];
  int32_t num_folder_lines;
} pk3_search_t;

static qboolean matches(pk3_search_t const* s, char const* path)
{
  char const* const slash = strrchr(path, '/');
  char const* const name  = slash ? slash + 1 : path;
  for (int32_t i = 0; i < s->num_patterns; ++i)
  {
    if (glob_match(s->patterns[i].glob, s->patterns[i].whole_path ? path : name)) return qtrue;
  }
  return qfalse;
}

static qboolean on_entry(char const* source, char const* path, qboolean is_folder, void* ctx)
{
  pk3_search_t* const s = ctx;
  if (is_folder ? !s->folders : !s->files) return qtrue;
  if (!matches(s, path)) return qtrue;

  char line[MAX_ENTRY_PATH * 2 + 32];
  snprintf(line, sizeof(line), "^5%s^7  %s%s\n", source, path, is_folder ? "/" : "");
  if (!is_folder)
  {
    if (strcmp(s->last_source, source))
    {
      snprintf(s->last_source, sizeof(s->last_source), "%s", source);
      ++s->sources;
    }
    if (s->files_found++ < MAX_SHOWN) trap_Print(line);
  }
  else if (s->folders_found++ < MAX_SHOWN)
  {
    size_t const n    = strlen(line) + 1;
    char* const  copy = malloc(n);
    if (copy)
    {
      memcpy(copy, line, n);
      s->folder_lines[s->num_folder_lines++] = copy;
    }
  }
  return qtrue;
}

static void print_count(int32_t found, char const* what, char const* extra)
{
  char line[256];
  if (found > MAX_SHOWN)
    snprintf(
      line,
      sizeof(line),
      "^3%i %s%s%s^7  (first %i shown: narrow the pattern)\n",
      found,
      what,
      found == 1 ? "" : "s",
      extra,
      MAX_SHOWN);
  else
    snprintf(line, sizeof(line), "^3%i %s%s%s\n", found, what, found == 1 ? "" : "s", extra);
  trap_Print(line);
}

void cmd_pk3search(void)
{
  char cmd[64];
  trap_Argv(0, cmd, sizeof(cmd));
  static pk3_search_t s; // too large for the stack
  memset(&s, 0, sizeof(s));
  s.files   = Q_stricmp(cmd, "pk3folders") != 0;
  s.folders = Q_stricmp(cmd, "pk3files") != 0;

  int32_t const argc = trap_Argc();
  char          shown[MAX_PATTERNS * (PATTERN_LEN + 8)];
  shown[0] = 0;
  for (int32_t a = 1; a < argc && s.num_patterns < MAX_PATTERNS; ++a)
  {
    char arg[MAX_STRING_CHARS];
    trap_Argv(a, arg, sizeof(arg));
    size_t   n    = 0;
    qboolean wild = qfalse;
    char     clean[PATTERN_LEN];
    for (char const* c = arg; *c && n < sizeof(clean) - 1; ++c)
    {
      if ((unsigned char)*c < ' ') continue;
      char const ch = *c == '\\' ? '/' : *c;
      if (ch == '*' || ch == '?' || ch == '[') wild = qtrue;
      clean[n++] = ch;
    }
    clean[n] = 0;
    if (!n) continue;
    pk3_pattern_t* const p = &s.patterns[s.num_patterns++];
    p->whole_path          = strchr(clean, '/') != NULL;
    if (wild)
      snprintf(p->glob, sizeof(p->glob), "%s", clean);
    else
      snprintf(p->glob, sizeof(p->glob), "*%s*", clean);
    size_t const used = strlen(shown);
    snprintf(shown + used, sizeof(shown) - used, "%s%s", used ? "  " : "", p->glob);
  }
  if (!s.num_patterns)
  {
    trap_Print("usage: pk3search <pattern> [<pattern> ...]   files and folders\n");
    trap_Print("       pk3files  <pattern> ...                files only\n");
    trap_Print("       pk3folders <pattern> ...               folders only\n");
    trap_Print("  searches every pk3 and every loose file of the mod folder and the base game folder\n");
    trap_Print("  * = any characters, ? = one character, [a-z] = one of a set; no wildcard = contains\n");
    trap_Print("  the name is matched, or the whole path if the pattern has a / in it\n");
    trap_Print("  e.g. ^3pk3search rocket^7, ^3pk3files *.arena^7, ^3pk3folders sky*^7, ^3pk3files maps/run*.bsp\n");
    return;
  }

  char head[sizeof(shown) + 64];
  if (s.files)
  {
    snprintf(head, sizeof(head), "^3--- files matching %s ---\n", shown);
    trap_Print(head);
  }
  int32_t const pk3s = pk3_scan(on_entry, &s);
  if (pk3s < 0)
  {
    trap_Print("^1can't find the game folders (the folder this DLL was loaded from)\n");
    return;
  }
  if (s.files)
  {
    char extra[64];
    snprintf(extra, sizeof(extra), " in %i place%s", s.sources, s.sources == 1 ? "" : "s");
    print_count(s.files_found, "file", s.files_found ? extra : "");
  }
  if (s.folders)
  {
    snprintf(head, sizeof(head), "^3--- folders matching %s ---\n", shown);
    trap_Print(head);
    for (int32_t i = 0; i < s.num_folder_lines; ++i)
    {
      trap_Print(s.folder_lines[i]);
      free(s.folder_lines[i]);
    }
    print_count(s.folders_found, "folder", "");
  }
  char tail[96];
  snprintf(tail, sizeof(tail), "^3%i pk3 archives read\n", pk3s);
  trap_Print(tail);
}
