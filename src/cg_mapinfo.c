/*
  ==============================
  mapinfo [<map>]

  Reads maps/<map>.bsp (the current map without an argument) and prints what is in it. Everything comes from
  the BSP file itself:
    lump 0   entities    the text the game spawns everything from: one { "key" "value" ... } block per entity
    lump 1   shaders     name, surface flags and content flags of every shader the map uses
    lump 7   models      model 0 is the world, each further one a brush entity (door, trigger, ...)
    lump 8   brushes
    lump 9   brush sides each names its shader: how slick / lava / ... surfaces are counted
    lump 13  surfaces    the drawn faces
  plus two things from outside it: the pk3 archives that contain the file (cg_pk3.c) and your own best times
  from defrag's system/records.
  ==============================
*/
#include "cg_mapinfo.h"

#include "cg_local.h"
#include "cg_pk3.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BSP_IDENT       (('P' << 24) + ('S' << 16) + ('B' << 8) + 'I')
#define BSP_VERSION     46
#define BSP_LUMPS       17
#define BSP_HEADER_SIZE (8 + BSP_LUMPS * 8)
#define MAX_BSP_SIZE    (1024 * 1024 * 1024)

enum
{
  LUMP_ENTITIES   = 0,
  LUMP_SHADERS    = 1,
  LUMP_MODELS     = 7,
  LUMP_BRUSHES    = 8,
  LUMP_BRUSHSIDES = 9,
  LUMP_SURFACES   = 13,
};

#define SHADER_SIZE    72 // char name[64]; int surfaceFlags; int contentFlags
#define MODEL_SIZE     40
#define BRUSH_SIZE     12
#define BRUSHSIDE_SIZE 8 // int planeNum; int shaderNum
#define SURFACE_SIZE   104

#define MAX_CLASSES 512
#define MAX_GIVEN   256
#define NAME_LEN    64

typedef struct
{
  char    name[NAME_LEN];
  int32_t count;
  int32_t with_health; // doors and buttons that are shot instead of touched
  int32_t given;       // items handed out by a target_give instead of lying in the map
} class_t;

typedef struct
{
  class_t classes[MAX_CLASSES];
  int32_t num_classes;
  int32_t entities, triggers, unnamed;
  char    message[256], author[128], music[128];
  char    given[MAX_GIVEN][NAME_LEN]; // targets of the map's target_give entities
  int32_t num_given;
} entities_t;

static int32_t i32(unsigned char const* p)
{
  return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static void out(char const* fmt, ...)
{
  char    line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  trap_Print(line);
}

static void copy_str(char* dst, size_t size, char const* src)
{
  size_t n = strlen(src);
  if (n >= size) n = size - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

/*
  ------------------------------ entities ------------------------------
*/

// Next token of the entity text: { } or a "quoted string" (returned without the quotes, terminated in place).
// Returns NULL at the end.
static char* next_token(char** at, char const* end)
{
  char* p = *at;
  while (p < end && (unsigned char)*p <= ' ') ++p;
  if (p >= end || !*p) return NULL;
  if (*p == '{' || *p == '}')
  {
    static char brace[2][2] = { "{", "}" };
    *at                     = p + 1;
    return brace[*p == '}'];
  }
  if (*p == '"')
  {
    char* const start = ++p;
    while (p < end && *p && *p != '"') ++p;
    *at = p < end ? p + 1 : p;
    if (p < end) *p = 0;
    return start;
  }
  // a bare word: not valid in a compiled map, skip it
  char* const start = p;
  while (p < end && (unsigned char)*p > ' ' && *p != '"' && *p != '{' && *p != '}') ++p;
  *at = p < end && (unsigned char)*p <= ' ' ? p + 1 : p;
  if (p < end && (unsigned char)*p <= ' ') *p = 0;
  return start;
}

static class_t* class_of(entities_t* e, char const* name)
{
  for (int32_t i = 0; i < e->num_classes; ++i)
  {
    if (!Q_stricmp(e->classes[i].name, name)) return &e->classes[i];
  }
  if (e->num_classes >= MAX_CLASSES) return NULL;
  class_t* const c = &e->classes[e->num_classes++];
  memset(c, 0, sizeof(*c));
  copy_str(c->name, sizeof(c->name), name);
  return c;
}

static qboolean is_given(entities_t const* e, char const* targetname)
{
  if (!targetname[0]) return qfalse;
  for (int32_t i = 0; i < e->num_given; ++i)
  {
    if (!Q_stricmp(e->given[i], targetname)) return qtrue;
  }
  return qfalse;
}

/*
  pass 0 collects what the target_give entities point at, pass 1 counts.
  The text is tokenized in place, so each pass gets its own copy.
*/
static void parse_entities(entities_t* e, char* text, size_t len, int32_t pass)
{
  char const* const end = text + len;
  char*             at  = text;
  for (char* tok; (tok = next_token(&at, end));)
  {
    if (tok[0] != '{' || tok[1]) continue;
    char const *classname = "", *health = "", *target = "", *targetname = "", *message = "", *author = "", *music = "";
    for (;;)
    {
      char* const key = next_token(&at, end);
      if (!key || ((key[0] == '}' || key[0] == '{') && !key[1])) break;
      char* const value = next_token(&at, end);
      if (!value) break;
      if (!Q_stricmp(key, "classname"))
        classname = value;
      else if (!Q_stricmp(key, "health"))
        health = value;
      else if (!Q_stricmp(key, "target"))
        target = value;
      else if (!Q_stricmp(key, "targetname"))
        targetname = value;
      else if (!Q_stricmp(key, "message"))
        message = value;
      else if (!Q_stricmp(key, "author"))
        author = value;
      else if (!Q_stricmp(key, "music"))
        music = value;
    }

    if (pass == 0)
    {
      if (!Q_stricmp(classname, "target_give") && target[0] && e->num_given < MAX_GIVEN)
        copy_str(e->given[e->num_given++], NAME_LEN, target);
      continue;
    }

    ++e->entities;
    if (!classname[0])
    {
      ++e->unnamed;
      continue;
    }
    if (!Q_stricmpn(classname, "trigger_", 8)) ++e->triggers;
    if (!Q_stricmp(classname, "worldspawn"))
    {
      copy_str(e->message, sizeof(e->message), message);
      copy_str(e->author, sizeof(e->author), author);
      copy_str(e->music, sizeof(e->music), music);
    }
    class_t* const c = class_of(e, classname);
    if (!c) continue;
    ++c->count;
    if (atoi(health) > 0) ++c->with_health;
    if (is_given(e, targetname)) ++c->given;
  }
}

static int32_t count_of(entities_t const* e, char const* name)
{
  for (int32_t i = 0; i < e->num_classes; ++i)
  {
    if (!Q_stricmp(e->classes[i].name, name)) return e->classes[i].count;
  }
  return 0;
}

static int by_count(void const* a, void const* b)
{
  class_t const* const x = a;
  class_t const* const y = b;
  if (x->count != y->count) return y->count - x->count;
  return Q_stricmp(x->name, y->name);
}

/*
  ------------------------------ what the classes mean ------------------------------
*/
typedef struct
{
  char const* classname;
  char const* label;
} label_t;

static label_t const weapons[] = {
  { "weapon_gauntlet", "gauntlet" },
  { "weapon_machinegun", "machinegun" },
  { "weapon_shotgun", "shotgun" },
  { "weapon_grenadelauncher", "grenade launcher" },
  { "weapon_rocketlauncher", "rocket launcher" },
  { "weapon_lightning", "lightning gun" },
  { "weapon_railgun", "railgun" },
  { "weapon_plasmagun", "plasma gun" },
  { "weapon_bfg", "BFG" },
  { "weapon_grapplinghook", "grappling hook" },
  { "weapon_nailgun", "nailgun" },
  { "weapon_prox_launcher", "prox launcher" },
  { "weapon_chaingun", "chaingun" },
};

static label_t const functions[] = {
  // defrag timing and scoring
  { "target_startTimer", "start timer" },
  { "target_stopTimer", "stop timer" },
  { "target_checkpoint", "checkpoint" },
  { "target_score", "score (target_score)" },
  { "target_fragsFilter", "frags filter (needs frags)" },
  { "target_init", "player reset (target_init)" },
  // movement
  { "trigger_push", "jump pad (trigger_push)" },
  { "target_push", "push target (target_push)" },
  { "target_speed", "target_speed" },
  { "trigger_teleport", "teleporter (trigger_teleport)" },
  { "target_teleporter", "teleporter (target_teleporter)" },
  { "misc_teleporter_dest", "teleporter destination" },
  { "misc_portal_surface", "portal surface" },
  // movers
  { "func_door", "door" },
  { "func_button", "button" },
  { "func_plat", "platform (func_plat)" },
  { "func_train", "train" },
  { "func_rotating", "rotating brush" },
  { "func_bobbing", "bobbing brush" },
  { "func_pendulum", "pendulum" },
  { "func_static", "static brush (func_static)" },
  { "func_timer", "timer (func_timer)" },
  // triggers and logic
  { "trigger_multiple", "trigger_multiple" },
  { "trigger_hurt", "hurt trigger" },
  { "trigger_always", "trigger_always" },
  { "target_give", "item giver (target_give)" },
  { "target_remove_powerups", "powerup remover" },
  { "target_kill", "kill target" },
  { "target_relay", "relay" },
  { "target_delay", "delay" },
  { "target_print", "print" },
  { "target_smallprint", "small print" },
  { "target_speaker", "speaker" },
  { "target_position", "target_position" },
  { "target_location", "location" },
  // hazards
  { "shooter_rocket", "rocket shooter" },
  { "shooter_plasma", "plasma shooter" },
  { "shooter_grenade", "grenade shooter" },
};

static char const* label_in(label_t const* table, size_t count, char const* classname)
{
  for (size_t i = 0; i < count; ++i)
  {
    if (!Q_stricmp(table[i].classname, classname)) return table[i].label;
  }
  return NULL;
}

/*
  ------------------------------ outside the BSP ------------------------------
*/
#define MAX_SOURCES 16

typedef struct
{
  char    path[MAX_QPATH + 16];
  char    sources[MAX_SOURCES][256];
  int32_t num, more;
} where_t;

static qboolean on_entry(char const* source, char const* path, qboolean is_folder, void* ctx)
{
  where_t* const w = ctx;
  if (is_folder || Q_stricmp(path, w->path)) return qtrue;
  if (w->num < MAX_SOURCES)
    copy_str(w->sources[w->num++], sizeof(w->sources[0]), source);
  else
    ++w->more;
  return qtrue;
}

// defrag keeps your best time per map, mode and physics in system/records/<map>_<mode>_<physics>.rec:
// 136 bytes, 34 little-endian ints. [1..32] are the times in ms of the checkpoints and then of the finish,
// [33] is the number of checkpoints. (Worked out from files whose times are known, not from documentation.)
static void print_records(char const* map)
{
  static char const* const modes[]   = { "df", "mdf", "fs", "mfs", "fc", "mfc" };
  static char const* const physics[] = { "vq3", "cpm" };
  qboolean                 any       = qfalse;
  for (size_t m = 0; m < ARRAY_LEN(modes); ++m)
  {
    for (size_t p = 0; p < ARRAY_LEN(physics); ++p)
    {
      char path[MAX_QPATH * 2];
      snprintf(path, sizeof(path), "system/records/%s_%s_%s.rec", map, modes[m], physics[p]);
      fileHandle_t  f;
      int32_t const len = trap_FS_FOpenFile(path, &f, FS_READ);
      if (len < 0) continue;
      unsigned char rec[136];
      if (len != (int32_t)sizeof(rec))
      {
        trap_FS_FCloseFile(f);
        continue;
      }
      trap_FS_Read(rec, sizeof(rec), f);
      trap_FS_FCloseFile(f);
      int32_t const checkpoints = i32(rec + 132);
      if (checkpoints < 0 || checkpoints > 31) continue;
      int32_t const ms = i32(rec + 4 + 4 * checkpoints);
      if (ms <= 0) continue;
      if (!any) out("^3--- your best times (defrag system/records) ---\n");
      any         = qtrue;
      char cp[48] = "";
      if (checkpoints) snprintf(cp, sizeof(cp), "  (%i checkpoint%s)", checkpoints, checkpoints == 1 ? "" : "s");
      out("  %-4s %-4s %i:%02i.%03i%s\n", modes[m], physics[p], ms / 60000, ms / 1000 % 60, ms % 1000, cp);
    }
  }
  if (!any) out("^3--- your best times ---\n  none recorded in defrag's system/records\n");
}

/*
  ------------------------------ the command ------------------------------
*/

// value of `key` in an info string ("\key\value\key\value")
static void info_value(char const* info, char const* key, char* value, size_t size)
{
  value[0] = 0;
  while (*info == '\\')
  {
    char const* const k     = info + 1;
    char const* const k_end = strchr(k, '\\');
    if (!k_end) return;
    char const* const v     = k_end + 1;
    char const*       v_end = strchr(v, '\\');
    if (!v_end) v_end = v + strlen(v);
    if ((size_t)(k_end - k) == strlen(key) && !Q_stricmpn(k, key, (int)strlen(key)))
    {
      size_t n = (size_t)(v_end - v);
      if (n >= size) n = size - 1;
      memcpy(value, v, n);
      value[n] = 0;
      return;
    }
    info = v_end;
  }
}

static void print_list(entities_t const* e, char const* title, qboolean (*wanted)(class_t const*), qboolean labels)
{
  out("^3--- %s ---\n", title);
  qboolean any = qfalse;
  for (int32_t i = 0; i < e->num_classes; ++i)
  {
    class_t const* const c = &e->classes[i];
    if (!wanted(c)) continue;
    any               = qtrue;
    char const* label = NULL;
    if (labels) label = label_in(weapons, ARRAY_LEN(weapons), c->name);
    if (labels && !label) label = label_in(functions, ARRAY_LEN(functions), c->name);
    char note[96] = "";
    if (c->given)
      snprintf(note, sizeof(note), "  (%i handed out by a target_give)", c->given);
    else if (c->with_health && (!Q_stricmp(c->name, "func_door") || !Q_stricmp(c->name, "func_button")))
      snprintf(note, sizeof(note), "  (%i shootable: has health)", c->with_health);
    out("  %4i  %s%s\n", c->count, label ? label : c->name, note);
  }
  if (!any) out("  none\n");
}

static qboolean is_weapon(class_t const* c)
{
  return !Q_stricmpn(c->name, "weapon_", 7);
}

static qboolean is_item(class_t const* c)
{
  return !Q_stricmpn(c->name, "ammo_", 5) || !Q_stricmpn(c->name, "item_", 5) || !Q_stricmpn(c->name, "holdable_", 9) ||
         !Q_stricmpn(c->name, "team_CTF_", 9);
}

static qboolean is_function(class_t const* c)
{
  return label_in(functions, ARRAY_LEN(functions), c->name) != NULL;
}

static qboolean is_any(class_t const* c)
{
  (void)c;
  return qtrue;
}

void cmd_mapinfo(void)
{
  char map[MAX_QPATH];
  if (trap_Argc() >= 2)
  {
    trap_Argv(1, map, sizeof(map));
    // "maps/foo.bsp" and "foo.bsp" are accepted too
    char* name = map;
    if (!Q_stricmpn(name, "maps/", 5) || !Q_stricmpn(name, "maps\\", 5)) name += 5;
    size_t n = strlen(name);
    if (n > 4 && !Q_stricmp(name + n - 4, ".bsp")) name[n - 4] = 0;
    memmove(map, name, strlen(name) + 1);
  }
  else
  {
    info_value(CG_ConfigString(0 /* CS_SERVERINFO */), "mapname", map, sizeof(map));
  }
  for (char const* c = map; *c; ++c)
  {
    if (*c == '/' || *c == '\\' || *c == ':' || (unsigned char)*c < ' ' || (*c == '.' && c[1] == '.'))
    {
      out("^1mapinfo: \"%s\" is not a map name\n", map);
      return;
    }
  }
  if (!map[0])
  {
    out("usage: mapinfo [<map>]   (without a name: the map being played)\n");
    return;
  }

  char path[MAX_QPATH + 16];
  snprintf(path, sizeof(path), "maps/%s.bsp", map);
  fileHandle_t  f;
  int32_t const len = trap_FS_FOpenFile(path, &f, FS_READ);
  if (len < 0)
  {
    out("^1mapinfo: %s not found\n", path);
    return;
  }
  if (len < BSP_HEADER_SIZE || len > MAX_BSP_SIZE)
  {
    trap_FS_FCloseFile(f);
    out("^1mapinfo: %s is not a map (%i bytes)\n", path, len);
    return;
  }
  unsigned char* const bsp = malloc((size_t)len + 1);
  if (!bsp)
  {
    trap_FS_FCloseFile(f);
    out("^1mapinfo: out of memory reading %s (%i bytes)\n", path, len);
    return;
  }
  trap_FS_Read(bsp, len, f);
  trap_FS_FCloseFile(f);
  bsp[len] = 0;

  if (i32(bsp) != BSP_IDENT || i32(bsp + 4) != BSP_VERSION)
  {
    out("^1mapinfo: %s is not a Quake 3 map (ident %.4s, version %i)\n", path, (char const*)bsp, i32(bsp + 4));
    free(bsp);
    return;
  }
  // every lump must lie inside the file
  int32_t offset[BSP_LUMPS], length[BSP_LUMPS];
  for (int32_t i = 0; i < BSP_LUMPS; ++i)
  {
    offset[i] = i32(bsp + 8 + i * 8);
    length[i] = i32(bsp + 12 + i * 8);
    if (offset[i] < 0 || length[i] < 0 || offset[i] > len || length[i] > len - offset[i])
    {
      out("^1mapinfo: %s is damaged (lump %i points outside the file)\n", path, i);
      free(bsp);
      return;
    }
  }

  // ---- entities
  static entities_t e; // large
  memset(&e, 0, sizeof(e));
  size_t const ent_len = (size_t)length[LUMP_ENTITIES];
  char* const  text    = malloc(ent_len + 1);
  if (!text)
  {
    out("^1mapinfo: out of memory\n");
    free(bsp);
    return;
  }
  for (int32_t pass = 0; pass < 2; ++pass)
  {
    memcpy(text, bsp + offset[LUMP_ENTITIES], ent_len);
    text[ent_len] = 0;
    parse_entities(&e, text, ent_len, pass);
  }
  free(text);
  qsort(e.classes, (size_t)e.num_classes, sizeof(e.classes[0]), by_count);

  // ---- shaders, brushes, sides
  int32_t const        num_shaders = length[LUMP_SHADERS] / SHADER_SIZE;
  unsigned char const* shaders     = bsp + offset[LUMP_SHADERS];
  int32_t const        num_brushes = length[LUMP_BRUSHES] / BRUSH_SIZE;
  int32_t const        num_sides   = length[LUMP_BRUSHSIDES] / BRUSHSIDE_SIZE;
  int32_t const        num_models  = length[LUMP_MODELS] / MODEL_SIZE;
  int32_t const        num_faces   = length[LUMP_SURFACES] / SURFACE_SIZE;

  struct
  {
    char const* label;
    int32_t     flag;
    qboolean    content; // a brush content (counted per brush) or a surface flag (counted per brush side)
    int32_t     count, shaders;
  } kinds[] = {
    { "slick", SURF_SLICK, qfalse, 0, 0 },    { "no fall damage", SURF_NODAMAGE, qfalse, 0, 0 },
    { "ladder", SURF_LADDER, qfalse, 0, 0 },  { "lava", CONTENTS_LAVA, qtrue, 0, 0 },
    { "slime", CONTENTS_SLIME, qtrue, 0, 0 }, { "water", CONTENTS_WATER, qtrue, 0, 0 },
    { "fog", CONTENTS_FOG, qtrue, 0, 0 },
  };
  for (size_t k = 0; k < ARRAY_LEN(kinds); ++k)
  {
    for (int32_t i = 0; i < num_shaders; ++i)
    {
      if (i32(shaders + i * SHADER_SIZE + (kinds[k].content ? 68 : 64)) & kinds[k].flag) ++kinds[k].shaders;
    }
    if (kinds[k].content)
    {
      for (int32_t i = 0; i < num_brushes; ++i)
      {
        int32_t const sh = i32(bsp + offset[LUMP_BRUSHES] + i * BRUSH_SIZE + 8);
        if (sh >= 0 && sh < num_shaders && (i32(shaders + sh * SHADER_SIZE + 68) & kinds[k].flag)) ++kinds[k].count;
      }
    }
    else
    {
      for (int32_t i = 0; i < num_sides; ++i)
      {
        int32_t const sh = i32(bsp + offset[LUMP_BRUSHSIDES] + i * BRUSHSIDE_SIZE + 4);
        if (sh >= 0 && sh < num_shaders && (i32(shaders + sh * SHADER_SIZE + 64) & kinds[k].flag)) ++kinds[k].count;
      }
    }
  }
  free(bsp);

  // ---- which pk3
  static where_t where;
  memset(&where, 0, sizeof(where));
  copy_str(where.path, sizeof(where.path), path);
  int32_t const pk3s = pk3_scan(on_entry, &where);

  // ---- output
  out("^3=== %s ===\n", map);
  if (e.message[0]) out("  title    %s\n", e.message);
  if (e.author[0]) out("  author   %s\n", e.author);
  out("  bsp      %s, %.2f MB\n", path, len / (1024. * 1024.));
  if (pk3s < 0)
    out("  pk3      (can't look: the folder this DLL was loaded from is unknown)\n");
  else if (!where.num)
    out("  pk3      not found in any pk3 or folder of the mod and base game folders\n");
  for (int32_t i = 0; i < where.num; ++i)
  {
    size_t const   n     = strlen(where.sources[i]);
    qboolean const loose = n && where.sources[i][n - 1] == '/';
    out("  %s  %s%s\n", i ? "also in " : "pk3     ", where.sources[i], loose ? "  (loose file, not in a pk3)" : "");
  }
  if (where.more) out("  and %i more\n", where.more);
  if (e.music[0]) out("  music    %s\n", e.music);

  out("^3--- size ---\n");
  out(
    "  %i entities, of which %i triggers and %i brush entities\n",
    e.entities,
    e.triggers,
    num_models > 0 ? num_models - 1 : 0);
  out("  %i brushes, %i drawn surfaces, %i shaders\n", num_brushes, num_faces, num_shaders);

  // defrag run or not, frags or not
  int32_t const starts = count_of(&e, "target_startTimer"), stops = count_of(&e, "target_stopTimer");
  int32_t const cps          = count_of(&e, "target_checkpoint");
  int32_t const frag_filters = count_of(&e, "target_fragsFilter"), scores = count_of(&e, "target_score");
  out("^3--- type ---\n");
  if (starts || stops)
    out("  timed run: %i start, %i stop, %i checkpoint trigger%s\n", starts, stops, cps, cps == 1 ? "" : "s");
  else
    out("  no defrag timer in the map (no target_startTimer / target_stopTimer)\n");
  if (frag_filters || scores)
    out("  uses frags / score: %i target_fragsFilter, %i target_score\n", frag_filters, scores);
  out(
    "  %i spawn point%s\n",
    count_of(&e, "info_player_deathmatch") + count_of(&e, "info_player_start"),
    count_of(&e, "info_player_deathmatch") + count_of(&e, "info_player_start") == 1 ? "" : "s");

  print_list(&e, "weapons", is_weapon, qtrue);
  print_list(&e, "ammo, items, powerups", is_item, qfalse);
  print_list(&e, "functions", is_function, qtrue);

  out("^3--- surfaces ---\n");
  qboolean any = qfalse;
  for (size_t k = 0; k < ARRAY_LEN(kinds); ++k)
  {
    if (!kinds[k].count && !kinds[k].shaders) continue;
    any = qtrue;
    out(
      "  %-15s %i %s (%i shader%s)\n",
      kinds[k].label,
      kinds[k].count,
      kinds[k].content ? (kinds[k].count == 1 ? "brush" : "brushes")
                       : (kinds[k].count == 1 ? "brush side " : "brush sides"),
      kinds[k].shaders,
      kinds[k].shaders == 1 ? "" : "s");
  }
  if (!any) out("  nothing special (no slick, lava, slime, water, fog)\n");

  print_list(&e, "all entities by class", is_any, qfalse);
  if (e.unnamed) out("  %4i  (no classname)\n", e.unnamed);
  if (e.num_classes >= MAX_CLASSES) out("  (more than %i different classes: the rest is not listed)\n", MAX_CLASSES);

  print_records(map);

  // the engine knows which copy it actually loads
  char which[sizeof(path) + 64];
  snprintf(which, sizeof(which), "echo \"^3--- the engine loads it from ---\"\nwhich \"%s\"\n", path);
  trap_SendConsoleCommand(which);
}
