// Standalone check of cg_search.c: stub syscalls, help files read from the directory given as argv[1].
//   gcc -fsanitize=address,undefined -Iinclude -I<build dir> test/search/search_sim.c src/cg_search.c src/q_shared.c
#include "cg_local.h"
#include "cg_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char  argv_[16][1024];
static int   argc_;
static char  out[1 << 22];
static char  sent[1 << 16];
static char* dir;
static FILE* fh;
static int   opened;

void    trap_Print(char const* s) { strcat(out, s); }
void    trap_SendConsoleCommand(char const* t) { strcat(sent, t); }
int32_t trap_Argc(void) { return argc_; }
void    trap_Argv(int32_t n, char* b, int32_t sz) { snprintf(b, sz, "%s", n < argc_ ? argv_[n] : ""); }
void    trap_Cvar_VariableStringBuffer(char const* n, char* b, int32_t sz) { snprintf(b, sz, "%s", !strcmp(n, "m_side") ? "0.01" : ""); }
int32_t trap_FS_FOpenFile(char const* q, fileHandle_t* f, fsMode_t m)
{
  (void)m;
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, q + strlen("system/help/"));
  fh = fopen(path, "rb");
  *f = 0;
  if (!fh) return -1;
  ++opened;
  fseek(fh, 0, SEEK_END);
  long n = ftell(fh);
  fseek(fh, 0, SEEK_SET);
  *f = 1;
  return (int32_t)n;
}
void trap_FS_Read(void* b, int32_t len, fileHandle_t f) { (void)f; if (fread(b, 1, len, fh) != (size_t)len) abort(); }
void trap_FS_FCloseFile(fileHandle_t f) { (void)f; if (fh) fclose(fh); fh = NULL; }

static int fails, passes;
#define EXPECT(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } else passes++; } while (0)
static int count(const char* hay, const char* needle) { int n = 0; for (const char* p = hay; (p = strstr(p, needle)); p++) n++; return n; }

// search <args> as the engine would run it: cmd_search queues, then the queued mdd_search_text lines run
static void search(int n, const char** args)
{
  out[0] = sent[0] = 0;
  argc_ = n + 1;
  strcpy(argv_[0], "search");
  for (int i = 0; i < n; i++) strcpy(argv_[i + 1], args[i]);
  cmd_search();
  char* copy = strdup(sent);
  for (char* l = strtok(copy, "\n"); l; l = strtok(NULL, "\n"))
  {
    if (strncmp(l, SEARCH_TEXT_COMMAND " \"", strlen(SEARCH_TEXT_COMMAND) + 2)) continue;
    char* q = l + strlen(SEARCH_TEXT_COMMAND) + 2;
    *strrchr(q, '"') = 0;
    argc_ = 2;
    strcpy(argv_[0], SEARCH_TEXT_COMMAND);
    strcpy(argv_[1], q);
    cmd_search_text();
  }
  free(copy);
}
#define S1(a) search(1, (const char*[]){ a })

int main(int c, char** v)
{
  if (c < 2) return 2;
  dir = v[1];
  S1("mouse");
  EXPECT(strstr(sent, "cvarlist \"*mouse*\"\n") && strstr(sent, "cmdlist \"*mouse*\"\n") && strstr(sent, "echo \"^3--- cvars matching *mouse* ---\"\n"));
  EXPECT(strstr(out, "^3m_side^7 is \"0.01^7\"  Set the strafe movement distance of the player in relation to how much the mouse moves.\n"));
  EXPECT(strstr(out, "^3m_forward^7 is \"") && strstr(out, "^3sensitivity^7") && !strstr(out, "m_accel"));
  EXPECT(strstr(out, "description") && strstr(out, "help file"));
  if (c > 2) puts(out);
  S1("m_s?de"); EXPECT(count(out, "^7 is \"") == 1 && strstr(out, "^3m_side^7"));
  S1("[Mm]_si[a-e]e"); EXPECT(count(out, "^7 is \"") == 1 && strstr(out, "^3m_side^7"));
  S1("m_side"); EXPECT(strstr(out, "^3m_side^7") && strstr(sent, "cvarlist \"*m_side*\""));
  S1("M_SIDE"); EXPECT(strstr(out, "^3m_side^7"));
  S1("r_*gamma*"); EXPECT(strstr(out, "^3r_gamma^7"));
  S1("zzzznothing"); EXPECT(strstr(out, "0 descriptions, 0 help files"));
  // help files: the proxymod's own, found by text that is not in any name
  S1("usercmd"); EXPECT(strstr(out, "^5mdd_jumpshoot^7  ") && strstr(out, "^5mdd_input_stagger_min^7  "));
  S1("screenshotjpeg"); EXPECT(strstr(out, "^5alias^7  "));
  S1("mouse moves"); EXPECT(strstr(out, "^3m_side^7") && strstr(sent, "\"*mouse moves*\""));
  S1("strafe*mouse"); EXPECT(strstr(out, "^3m_side^7") && !strstr(out, "^3m_forward^7"));
  // the title box is skipped: a name-only pattern must not list every file through its title
  S1("COMMAND: alias"); EXPECT(!strstr(out, "^5alias^7  "));
  // junk and edge patterns
  S1(";\"\n$"); EXPECT(!sent[0] && !out[0]);
  S1("["); S1("[a"); S1("[]"); S1("*"); S1("?"); S1("***?***"); S1("[a-"); S1("a["); S1("  x  ");
  EXPECT(strstr(sent, "cvarlist \"*x*\""));
  { char big[400]; memset(big, 'a', 399); big[399] = 0; S1(big); EXPECT(strlen(strstr(sent, "cvarlist \"") + 10) > 60); EXPECT(strstr(out, "0 descriptions")); }
  { char big[400]; memset(big, '*', 399); big[399] = 0; S1(big); }
  { char big[400]; for (int i = 0; i < 399; i++) big[i] = "a*"[i & 1]; big[399] = 0; S1(big); } // many stars: must stay fast
  search(3, (const char*[]){ "mouse", "m_side", "usercmd" }); EXPECT(count(sent, "cvarlist") == 3 && count(out, "--- help files matching") == 3);
  search(0, NULL); EXPECT(strstr(out, "usage: search"));
  { const char* many[10] = { "a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9", "a10" }; search(10, many); EXPECT(count(sent, "cvarlist") == 8); }
  EXPECT(opened > 60);
  printf("%d passed, %d failed\n", passes, fails);
  return fails != 0;
}
