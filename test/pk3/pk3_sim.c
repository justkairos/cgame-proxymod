// Standalone check of cg_pk3.c, cg_mapinfo.c and sys_files.c against the tree test/pk3/make_tree.py builds.
// The binary must be built INTO <root>/defrag, because the code under test finds the mod folder from the
// location of the binary it is part of:
//   python3 test/pk3/make_tree.py /tmp/t
//   gcc -fsanitize=address,undefined -Iinclude -I<build dir> test/pk3/pk3_sim.c src/cg_pk3.c src/cg_mapinfo.c
//       src/cg_search.c src/sys_files.c src/q_shared.c -o /tmp/t/defrag/pk3_sim
//   /tmp/t/defrag/pk3_sim
#include "cg_local.h"
#include "cg_mapinfo.h"
#include "cg_pk3.h"
#include "sys_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char  argv_[16][1024];
static int   argc_;
static char  out[1 << 24];
static char  sent[1 << 16];
static char  dir[2048];
static FILE* fh;
static char  serverinfo[256] = "\\sv_fps\\125\\mapname\\testrun\\g_gametype\\0";

void    trap_Print(char const* s) { strcat(out, s); }
void    trap_SendConsoleCommand(char const* t) { strcat(sent, t); }
int32_t trap_Argc(void) { return argc_; }
void    trap_Argv(int32_t n, char* b, int32_t sz) { snprintf(b, sz, "%s", n < argc_ ? argv_[n] : ""); }
void    trap_Cvar_VariableStringBuffer(char const* n, char* b, int32_t sz) { snprintf(b, sz, "%s", !strcmp(n, "fs_basegame") ? "baseq3" : ""); }
char const* CG_ConfigString(int32_t i) { return i == 0 ? serverinfo : ""; }
int32_t trap_FS_FOpenFile(char const* q, fileHandle_t* f, fsMode_t m)
{
  (void)m;
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", dir, q);
  fh = fopen(path, "rb");
  *f = 0;
  if (!fh) return -1;
  fseek(fh, 0, SEEK_END);
  long n = ftell(fh);
  fseek(fh, 0, SEEK_SET);
  *f = 1;
  return (int32_t)n;
}
void trap_FS_Read(void* b, int32_t len, fileHandle_t f) { (void)f; if (fread(b, 1, len, fh) != (size_t)len) abort(); }
void trap_FS_FCloseFile(fileHandle_t f) { (void)f; if (fh) fclose(fh); fh = NULL; }

static int fails, passes;
#define EXPECT(c) do { if (!(c)) { printf("FAIL line %d: %s\n--- output:\n%.6000s\n", __LINE__, #c, out); fails++; } else passes++; } while (0)
#define HAS(s) (strstr(out, s) != NULL)
static int count(const char* needle) { int n = 0; for (const char* p = out; (p = strstr(p, needle)); p++) n++; return n; }
static void run(void (*fn)(void), int n, const char** args)
{
  out[0] = sent[0] = 0;
  argc_ = n;
  for (int i = 0; i < n; i++) strcpy(argv_[i], args[i]);
  fn();
}
#define RUN(fn, ...) run(fn, sizeof((const char*[]){ __VA_ARGS__ }) / sizeof(char*), (const char*[]){ __VA_ARGS__ })

int main(int c, char** v)
{
  (void)v;
  if (!sys_module_dir(dir, sizeof dir)) { puts("no module dir"); return 2; }
  // --- pk3search
  RUN(cmd_pk3search, "pk3search", "rocket");
  if (c > 1) puts(out);
  EXPECT(HAS("^5defrag/zzz-maps.pk3^7  textures/test/Rocket_Fire.tga\n"));
  EXPECT(HAS("^5defrag/aaa.pk3^7  sound/weapons/rocket/rocklf1a.wav\n") == 0); // the FILE is rocklf1a.wav: name has no "rocket"
  EXPECT(HAS("^5defrag/aaa.pk3^7  sound/weapons/rocket/\n"));                   // ... but the folder does
  EXPECT(HAS("^5baseq3/pak0.pk3^7  models/weapons2/rocketl/rocketl.md3\n") && HAS("^5baseq3/pak0.pk3^7  models/weapons2/rocketl/\n"));
  EXPECT(!HAS("^5defrag/^7") && !HAS("sky_loose"));
  EXPECT(strstr(out, "--- files matching") < strstr(out, "--- folders matching"));
  EXPECT(strstr(out, "rocketl.md3") < strstr(out, "--- folders matching") && strstr(out, "rocket/\n") > strstr(out, "--- folders matching"));
  EXPECT(HAS("2 files in 2 pk3s") && HAS("2 folders\n") && HAS("3 pk3 archives read"));
  RUN(cmd_pk3search, "pk3files", "rocket"); EXPECT(!HAS("folders") && HAS("2 files"));
  RUN(cmd_pk3search, "pk3folders", "rocket"); EXPECT(!HAS("files matching") && HAS("2 folders"));
  RUN(cmd_pk3search, "pk3folders", "sk*"); EXPECT(HAS("^5defrag/aaa.pk3^7  textures/skies/\n") && HAS("1 folder\n") && count("skies/") == 1);
  RUN(cmd_pk3search, "pk3files", "*.bsp"); EXPECT(HAS("^5defrag/zzz-maps.pk3^7  maps/testrun.bsp") && HAS("^5defrag/aaa.pk3^7  maps/testrun.bsp") && !HAS("loose.bsp") && HAS("3 files in 2 pk3s"));
  EXPECT(strstr(out, "zzz-maps.pk3") < strstr(out, "aaa.pk3") && !HAS("baseq3"));
  RUN(cmd_pk3search, "pk3files", "maps/test*"); EXPECT(HAS("maps/testrun.bsp") && !HAS("levelshots"));
  RUN(cmd_pk3search, "pk3files", "MAPS\\TESTRUN.BSP"); EXPECT(count("maps/testrun.bsp") == 2);
  RUN(cmd_pk3search, "pk3files", "test*"); EXPECT(HAS("levelshots/testrun.jpg") && HAS("scripts/testrun.arena"));
  RUN(cmd_pk3search, "pk3files", "*.pk3"); EXPECT(HAS("0 files"));
  // wildcards: whole name, with or without the extension; \\ as the path separator
  RUN(cmd_pk3search, "pk3files", "*rocket_fire"); EXPECT(HAS("Rocket_Fire.tga") && HAS("1 file "));
  RUN(cmd_pk3search, "pk3files", "rocket*"); EXPECT(HAS("Rocket_Fire.tga") && HAS("rocketl.md3") && HAS("2 files"));
  RUN(cmd_pk3search, "pk3files", "?ocket_fire.tga"); EXPECT(HAS("1 file "));
  RUN(cmd_pk3search, "pk3files", "ocket*"); EXPECT(HAS("0 files"));
  RUN(cmd_pk3search, "pk3files", "maps\\*run*"); EXPECT(count("maps/testrun.bsp") == 2 && HAS("2 files"));
  RUN(cmd_pk3search, "pk3files", "*\\test\\*"); EXPECT(HAS("textures/test/slick.tga") && HAS("2 files"));
  RUN(cmd_pk3search, "pk3folders", "*\\weapons*"); EXPECT(HAS("sound/weapons/\n") && HAS("models/weapons2/\n"));
  RUN(cmd_pk3search, "pk3folders", "weapons?"); EXPECT(HAS("models/weapons2/\n") && HAS("1 folder\n"));
  RUN(cmd_pk3search, "pk3search", "slash.txt"); EXPECT(HAS("^5defrag/aaa.pk3^7  leading/slash.txt\n"));
  RUN(cmd_pk3search, "pk3search", "*.md3"); EXPECT(HAS("3001 files") && HAS("first 1000 shown") && count(".md3\n") == 1000);
  RUN(cmd_pk3search, "pk3folders", "*"); EXPECT(HAS("^5defrag/aaa.pk3^7  models/many/\n") && count("models/many/\n") == 1);
  RUN(cmd_pk3search, "pk3search", "sky.shader", "*.arena"); EXPECT(HAS("scripts/sky.shader") && HAS("testrun.arena") && HAS("2 files"));
  RUN(cmd_pk3search, "pk3search", "zzzznothing"); EXPECT(HAS("0 files") && HAS("0 folders"));
  RUN(cmd_pk3search, "pk3search"); EXPECT(HAS("usage: pk3search"));
  RUN(cmd_pk3search, "pk3search", "["); RUN(cmd_pk3search, "pk3search", "\x01\x02"); EXPECT(HAS("usage"));
  // --- mapinfo
  RUN(cmd_mapinfo, "mapinfo");
  if (c > 1) puts(out);
  EXPECT(HAS("=== testrun ===") && HAS("title    Test Run by kairos") && HAS("bsp      maps/testrun.bsp"));
  EXPECT(HAS("pk3       defrag/zzz-maps.pk3\n") && HAS("also in   defrag/aaa.pk3\n") && HAS("also in   defrag/  (loose file, not in a pk3)"));
  EXPECT(HAS("19 entities, of which 2 triggers and 2 brush entities") && HAS("6 brushes, 7 drawn surfaces, 5 shaders"));
  EXPECT(HAS("timed run: 1 start, 1 stop, 2 checkpoint triggers") && !HAS("uses frags") && HAS("1 spawn point\n"));
  EXPECT(HAS("     2  rocket launcher  (1 handed out by a target_give)") && HAS("     1  plasma gun  (1 handed out by a target_give)"));
  EXPECT(HAS("     1  ammo_rockets") && HAS("     1  item_quad"));
  EXPECT(HAS("     2  door  (1 shootable: has health)") && HAS("     1  jump pad (trigger_push)") && HAS("     1  target_speed") && HAS("     2  checkpoint"));
  EXPECT(HAS("slick           4 brush sides (2 shaders)") && HAS("lava            3 brushes (1 shader)") && HAS("no fall damage  1 brush side "));
  EXPECT(HAS("     1  (no classname)") && HAS("     1  worldspawn"));
  EXPECT(HAS("mdf  vq3  0:18.800  (5 checkpoints)") && HAS("df   cpm  0:21.216\n"));
  EXPECT(strstr(sent, "which \"maps/testrun.bsp\""));
  RUN(cmd_mapinfo, "mapinfo", "fragmap"); EXPECT(HAS("=== fragmap ===") && HAS("no defrag timer") && HAS("uses frags / score: 1 target_fragsFilter, 1 target_score") && HAS("nothing special") && HAS("none recorded"));
  EXPECT(HAS("--- weapons ---\n  none\n"));
  RUN(cmd_mapinfo, "mapinfo", "maps/fragmap.bsp"); EXPECT(HAS("=== fragmap ==="));
  RUN(cmd_mapinfo, "mapinfo", "loose"); EXPECT(HAS("pk3       defrag/  (loose file, not in a pk3)"));
  RUN(cmd_mapinfo, "mapinfo", "nosuchmap"); EXPECT(HAS("maps/nosuchmap.bsp not found"));
  RUN(cmd_mapinfo, "mapinfo", "notabsp"); EXPECT(HAS("not a Quake 3 map"));
  RUN(cmd_mapinfo, "mapinfo", "../../etc/passwd"); EXPECT(HAS("is not a map name"));
  serverinfo[0] = 0; RUN(cmd_mapinfo, "mapinfo"); EXPECT(HAS("usage: mapinfo"));
  // --- damaged maps and archives must not crash (run under ASan/UBSan)
  {
    char src[4096], dst[4096];
    snprintf(src, sizeof src, "%s/maps/testrun.bsp", dir);
    snprintf(dst, sizeof dst, "%s/maps/fuzz.bsp", dir);
    FILE* f = fopen(src, "rb"); static unsigned char orig[1 << 16], buf[1 << 16]; size_t n = fread(orig, 1, sizeof orig, f); fclose(f);
    srand(1);
    for (int i = 0; i < 3000; i++)
    {
      memcpy(buf, orig, n);
      size_t len = n;
      int kind = rand() % 4;
      if (kind == 0) len = (size_t)rand() % (n + 1);                                             // truncated
      else if (kind == 1) for (int k = 0; k < 1 + rand() % 8; k++) buf[rand() % 144] = (unsigned char)rand(); // header
      else if (kind == 2) for (int k = 0; k < 1 + rand() % 64; k++) buf[rand() % n] = (unsigned char)rand(); // anywhere
      else { size_t o = 144 + (size_t)rand() % 400; for (int k = 0; k < 40 && o + k < n; k++) buf[o + k] = "{}\"\0 \n"[rand() % 7]; } // entity text
      f = fopen(dst, "wb"); fwrite(buf, 1, len, f); fclose(f);
      RUN(cmd_mapinfo, "mapinfo", "fuzz");
    }
    remove(dst);
    snprintf(src, sizeof src, "%s/zzz-maps.pk3", dir);
    snprintf(dst, sizeof dst, "%s/fuzz.pk3", dir);
    f = fopen(src, "rb"); n = fread(orig, 1, sizeof orig, f); fclose(f);
    for (int i = 0; i < 1500; i++)
    {
      memcpy(buf, orig, n);
      size_t len = rand() % 3 ? n : (size_t)rand() % (n + 1);
      for (int k = 0; k < 1 + rand() % 16; k++) buf[n - 1 - (size_t)rand() % (n < 600 ? n : 600)] = (unsigned char)rand(); // the directory is at the end
      f = fopen(dst, "wb"); fwrite(buf, 1, len, f); fclose(f);
      RUN(cmd_pk3search, "pk3search", "*");
    }
    remove(dst);
    passes++;
  }
  printf("%d passed, %d failed\n", passes, fails);
  return fails != 0;
}
