// Standalone check of cg_clipboard.c: a small console (rows of fixed width, like the engine's), a command buffer
// with append semantics, condump writing the rows to a file, and a fake clipboard.
//   gcc -fsanitize=address,undefined -Iinclude -I<build dir> test/clipboard/clipboard_sim.c src/cg_clipboard.c src/q_shared.c
#include "cg_clipboard.h"
#include "cg_local.h"
#include "sys_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 100
static char  rows[4096][WIDTH + 1]; static int nrows = 3; // a few unused rows at the top, like a fresh console
static char  cbuf[1 << 16];
static char  argv_[32][1024]; static int argc_;
static char  clip[1 << 22]; static int clip_set, clip_fail;
static char* dump; static long dump_len = -1; static long rpos;

static void con_print(const char* s) // strips ^x colours, wraps at WIDTH
{
  static int x;
  for (; *s; s++) {
    if (*s == '^' && s[1]) { s++; continue; }
    if (*s == '\n') { nrows++; x = 0; continue; }
    if (x == WIDTH) { nrows++; x = 0; }
    rows[nrows][x++] = *s; rows[nrows][x] = 0;
  }
}
void trap_Print(char const* s) { con_print(s); }
void trap_SendConsoleCommand(char const* t) { strcat(cbuf, t); }
int32_t trap_Argc(void) { return argc_; }
void trap_Argv(int32_t n, char* b, int32_t sz) { snprintf(b, sz, "%s", n < argc_ ? argv_[n] : ""); }
void trap_Cvar_VariableStringBuffer(char const* n, char* b, int32_t sz) { snprintf(b, sz, "%s", !strcmp(n, "name") ? "^1kai^7ros" : ""); }
int32_t trap_FS_FOpenFile(char const* q, fileHandle_t* f, fsMode_t m) { (void)m; *f = 0; if (strcmp(q, "mdd_clipboard.txt") || dump_len < 0) return -1; rpos = 0; *f = 1; return (int32_t)dump_len; }
void trap_FS_Read(void* b, int32_t len, fileHandle_t f) { (void)f; memcpy(b, dump + rpos, len); rpos += len; }
void trap_FS_FCloseFile(fileHandle_t f) { (void)f; }
int sys_set_clipboard(char const* t) { if (clip_fail) return 0; strcpy(clip, t); clip_set++; return 1; }
int sys_module_dir(char* o, size_t n) { (void)o; (void)n; return 0; }

static void tokenize(const char* t) { argc_ = 0; while (1) { while (*t && (unsigned char)*t <= ' ') t++; if (!*t) return; char* o = argv_[argc_++]; if (*t == '"') { t++; while (*t && *t != '"') *o++ = *t++; if (*t) t++; } else { while ((unsigned char)*t > ' ' && *t != '"') *o++ = *t++; } *o = 0; } }
static void condump(void) { con_print("Dumped console text to mdd_clipboard.txt.\n"); free(dump); dump = malloc((size_t)(nrows + 1) * (WIDTH + 2) + 1); char* o = dump; for (int i = 0; i <= nrows; i++) o += sprintf(o, "%s\r\n", rows[i]); dump_len = o - dump; }
static void exec_line(const char* l)
{
  tokenize(l); if (!argc_) return;
  const char* c = argv_[0];
  if (!strcmp(c, "condump")) return condump();
  if (!strcmp(c, "clipboard")) return cmd_clipboard();
  if (!strcmp(c, CLIPBOARD_GRAB_COMMAND)) return cmd_clipboard_grab();
  if (!strcmp(c, "echo")) { char line[2048] = ""; for (int i = 1; i < argc_; i++) { strcat(line, argv_[i]); strcat(line, i + 1 < argc_ ? " " : ""); } strcat(line, "\n"); return con_print(line); }
  if (!strcmp(c, "three")) return con_print("^3one\ntwo\nthree\n");
  if (!strcmp(c, "long")) { char l2[200]; memset(l2, 'a', 150); strcpy(l2 + 150, "\n"); return con_print(l2); }
  if (!strcmp(c, "quiet")) return;
  if (!strcmp(c, "nested")) { con_print("direct\n"); trap_SendConsoleCommand("echo child; nested2\n"); return; } // queues like search / mapinfo do
  if (!strcmp(c, "nested2")) { trap_SendConsoleCommand("echo grandchild\n"); return; }
  if (!strcmp(c, "exec")) { char l2[1100]; snprintf(l2, sizeof l2, "from %s\n", argv_[1]); return con_print(l2); }
  if (!strcmp(c, "flood")) { for (int i = 0; i < 3000; i++) { char l2[64]; sprintf(l2, "flood line %d\n", i); con_print(l2); } return; }
  char l2[4200]; snprintf(l2, sizeof l2, "unknown: %s\n", l); con_print(l2);
}
static void run(void) { while (cbuf[0]) { char line[4096]; size_t i = 0; int q = 0; while (cbuf[i] && cbuf[i] != '\n' && (q || cbuf[i] != ';')) { if (cbuf[i] == '"') q = !q; i++; } memcpy(line, cbuf, i); line[i] = 0; memmove(cbuf, cbuf + i + (cbuf[i] ? 1 : 0), strlen(cbuf + i + (cbuf[i] ? 1 : 0)) + 1); exec_line(line); } }
static void type(const char* s) { char echo[1100]; snprintf(echo, sizeof echo, "]\\%s\n", s); con_print(echo); strcat(cbuf, s); strcat(cbuf, "\n"); clip[0] = 0; run(); }
static int fails, passes;
#define EXPECT(c) do { if (!(c)) { printf("FAIL line %d: %s\n--- clipboard:\n%.600s\n--- last rows:\n%s\n%s\n", __LINE__, #c, clip, rows[nrows - 2], rows[nrows - 1]); fails++; } else passes++; } while (0)
#define LASTROW(s) (strstr(rows[nrows - 1], s) != NULL)

int main(void)
{
  con_print("^2first line\nsecond line\n");
  type("clipboard"); EXPECT(!strcmp(clip, "first line\nsecond line")); EXPECT(LASTROW("copied the console (2 lines, 22 characters)"));
  type("echo third"); type("clipboard 2"); EXPECT(strstr(clip, "]\\echo third\nthird") == clip); // the two rows above the typed line
  type("clipboard 1"); EXPECT(LASTROW("copied the end of the console (1 line"));
  type("clipboard 99999"); EXPECT(strstr(clip, "first line\nsecond line\n") == clip);
  type("clipboard 0"); EXPECT(!clip[0] && LASTROW("nothing to copy"));
  type("clipboard name"); EXPECT(!strcmp(clip, "^1kai^7ros") && LASTROW("copied the value of name (1 line, 10 characters)"));
  type("clipboard three"); EXPECT(!strcmp(clip, "one\ntwo\nthree")); EXPECT(LASTROW("copied the command's output (3 lines, 13 characters)"));
  type("clipboard echo hello   world"); EXPECT(!strcmp(clip, "hello world"));
  type("clipboard echo \"a  b\" c"); EXPECT(!strcmp(clip, "a  b c"));
  type("clipboard nested"); EXPECT(!strcmp(clip, "direct\nchild\ngrandchild")); // queued follow-ups are waited for
  type("clipboard my.cfg"); EXPECT(!strcmp(clip, "from my.cfg"));
  type("clipboard long"); EXPECT(strlen(clip) == 151 && clip[100] == '\n'); // copied as the console shows it: two rows
  type("clipboard quiet"); EXPECT(!clip[0] && LASTROW("the command printed nothing"));
  type("clipboard nosuchthing 1 2"); EXPECT(!strcmp(clip, "unknown: nosuchthing 1 2"));
  type("clipboard clipboard"); EXPECT(!clip[0] && LASTROW("can't copy the output of clipboard itself"));
  type("clipboard help"); EXPECT(!clip[0] && LASTROW("clipboard <command> ..."));
  // two requests in one line: the newer one wins, nothing is copied twice or mixed
  { int before = clip_set; type("clipboard three; clipboard echo second"); EXPECT(!strcmp(clip, "second") && clip_set == before + 1); }
  type("clipboard flood"); EXPECT(strstr(clip, "flood line 0\n") == clip && strstr(clip, "\nflood line 2999") && LASTROW("3000 lines"));
  clip_fail = 1; type("clipboard three"); EXPECT(!clip[0] && LASTROW("could not put the text on the clipboard")); clip_fail = 0;
  { char big[1300] = "clipboard echo "; memset(big + 15, 'x', 1200); big[1215] = 0; type(big); EXPECT(LASTROW("command too long") || clip[0] == 'x'); }
  // damaged / missing dump
  dump_len = -1; strcat(cbuf, CLIPBOARD_GRAB_COMMAND " 1 2 3\n" CLIPBOARD_GRAB_COMMAND " x y\n" CLIPBOARD_GRAB_COMMAND "\n"); run();
  printf("%d passed, %d failed\n", passes, fails);
  return fails != 0;
}
