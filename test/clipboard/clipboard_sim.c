// Standalone check of cg_capture.c, cg_clipboard.c and cg_grep.c: a model of oDFe's console (word wrap, hard wrap,
// optional time stamps - CL_ConsolePrint), a command buffer with append semantics, condump, and a fake clipboard.
//   gcc -fsanitize=address,undefined -Iinclude -I<build dir> test/clipboard/clipboard_sim.c src/cg_capture.c
//       src/cg_clipboard.c src/cg_grep.c src/regex_lite.c src/q_shared.c
#include "cg_capture.h"
#include "cg_clipboard.h"
#include "cg_grep.h"
#include "cg_local.h"
#include "sys_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cgs_t cgs;
#define MAXW 600
static int   W = 100, stamps = 0, scale10 = 10;
static char  rows[20000][MAXW + 1]; static int cur = -1, cx, pending = 1;
static char  cbuf[1 << 18];
static char  argv_[32][1024]; static int argc_;
static char  clip[1 << 22]; static int clip_set, clip_fail;
static char* dump; static long dump_len = -1; static long rpos;
static char  written[1 << 16]; static char written_name[256];

static void con_reset(void) { cur = -1; cx = 0; pending = 1; memset(rows, 0, sizeof rows); }
static void new_line(void) { cur++; cx = 0; memset(rows[cur], ' ', (size_t)W); rows[cur][W] = 0; if (stamps) { memcpy(rows[cur], "12:34:56 ", 9); cx = 9; } }
static void linefeed(void) { if (pending) new_line(); else pending = 1; cx = 0; }
static int wordlen(const char* t) { int l = 0; while ((unsigned char)*t > ' ') { if (*t == '^' && t[1] && t[1] != '^') t += 2; else { l++; t++; } } return l; }
static void con_print(const char* t)
{
  int l = 0;
  while (*t) {
    if (*t == '^' && t[1] && t[1] != '^') { t += 2; continue; }
    if (*t == '\n') { linefeed(); t++; continue; }
    if (cx == W) linefeed();
    if (pending) { new_line(); pending = 0; }
    if (l == 0) { l = wordlen(t); if (!l) l = 1; if (cx + l > W && (stamps ? 9 : 0) + l < W) { linefeed(); l = 0; continue; } }
    rows[cur][cx++] = *t; l--; t++;
  }
}
void trap_Print(char const* s) { con_print(s); }
void trap_SendConsoleCommand(char const* t) { strcat(cbuf, t); }
int32_t trap_Argc(void) { return argc_; }
void trap_Argv(int32_t n, char* b, int32_t sz) { snprintf(b, sz, "%s", n < argc_ ? argv_[n] : ""); }
void trap_Cvar_VariableStringBuffer(char const* n, char* b, int32_t sz)
{
  if (!strcmp(n, "name")) snprintf(b, sz, "^1kai^7ros");
  else if (!strcmp(n, "con_timestamp")) snprintf(b, sz, "%d", stamps);
  else if (!strcmp(n, "con_scale")) snprintf(b, sz, "%d.%d", scale10 / 10, scale10 % 10);
  else snprintf(b, sz, "%s", "");
}
int32_t trap_FS_FOpenFile(char const* q, fileHandle_t* f, fsMode_t m)
{
  *f = 0;
  if (m == FS_WRITE && !strcmp(q, "mdd_console.txt")) { dump_len = 0; *f = 3; return 0; } // emptied before a dump
  if (m == FS_WRITE) { snprintf(written_name, sizeof written_name, "%s", q); written[0] = 0; *f = 2; return 0; }
  if (strcmp(q, "mdd_console.txt") || dump_len < 0) return -1;
  rpos = 0; *f = 1; return (int32_t)dump_len;
}
void trap_FS_Read(void* b, int32_t len, fileHandle_t f) { (void)f; memcpy(b, dump + rpos, len); rpos += len; }
void trap_FS_Write(void const* b, int32_t len, fileHandle_t f) { (void)f; strncat(written, b, (size_t)len); }
void trap_FS_FCloseFile(fileHandle_t f) { (void)f; }
int sys_set_clipboard(char const* t) { if (clip_fail) return 0; strcpy(clip, t); clip_set++; return 1; }
int sys_module_dir(char* o, size_t n) { (void)o; (void)n; return 0; }

static void tokenize(const char* t) { argc_ = 0; while (1) { while (*t && (unsigned char)*t <= ' ') t++; if (!*t) return; char* o = argv_[argc_++]; if (*t == '"') { t++; while (*t && *t != '"') *o++ = *t++; if (*t) t++; } else { while ((unsigned char)*t > ' ' && *t != '"') *o++ = *t++; } *o = 0; } }
static int have_silent = 1, quiet, announced;
static void condump(void)
{
  if (!quiet) { con_print("Dumped console text to mdd_console.txt.\n"); announced++; }
  quiet = 0;
  free(dump); dump = malloc((size_t)(cur + 2) * (MAXW + 2) + 1); char* o = dump;
  for (int i = 0; i <= cur; i++) { int n = W; while (n && rows[i][n - 1] == ' ') n--; memcpy(o, rows[i], (size_t)n); o += n; *o++ = '\r'; *o++ = '\n'; }
  dump_len = o - dump;
}
static char** feed; static int nfeed; // lines the "feed" command prints
static void exec_line(const char* l)
{
  tokenize(l); if (!argc_) return;
  const char* c = argv_[0];
  if (!strcmp(c, "condump")) { condump(); return; }
  // defrag's "silent <command>": a cgame command, so what it runs is queued behind everything already waiting
  if (!strcmp(c, "silent") && have_silent) { const char* rest = strstr(l, "silent") + 7; strcat(cbuf, "__quiet "); strcat(cbuf, rest); strcat(cbuf, "\n"); return; }
  if (!strcmp(c, "__quiet")) { quiet = 1; exec_line(strstr(l, "__quiet") + 8); return; }
  if (!strcmp(c, "clipboard")) { cmd_clipboard(); return; }
  if (!strcmp(c, "grep")) { cmd_grep(); return; }
  if (!strcmp(c, CAPTURE_STEP_COMMAND)) { cmd_capture_step(); return; }
  if (!strcmp(c, "echo")) { static char line[4096]; line[0] = 0; for (int i = 1; i < argc_; i++) { strcat(line, argv_[i]); strcat(line, i + 1 < argc_ ? " " : ""); } strcat(line, "\n"); con_print(line); return; }
  if (!strcmp(c, "three")) { con_print("^3one\ntwo\nthree\n"); return; }
  if (!strcmp(c, "feed")) { for (int i = 0; i < nfeed; i++) { con_print(feed[i]); con_print("\n"); } return; }
  if (!strcmp(c, "quiet")) return;
  if (!strcmp(c, "nested")) { con_print("direct\n"); trap_SendConsoleCommand("echo child; nested2\n"); return; } // queues like search / mapinfo do
  if (!strcmp(c, "nested2")) { trap_SendConsoleCommand("echo grandchild\n"); return; }
  if (!strcmp(c, "exec")) { char l2[1100]; snprintf(l2, sizeof l2, "from %s\n", argv_[1]); con_print(l2); return; }
  if (!strcmp(c, "cvars")) { con_print("     cg_centertime \"3\"\n     cg_drawTimer \"0\"\n S   sv_fps \"125\"\n     cg_lagometertime 12:01.050 and 0:18.800\n"); return; }
  static char l2[4200]; snprintf(l2, sizeof l2, "unknown: %s\n", l); con_print(l2);
}
static void run(void) { while (cbuf[0]) { static char line[8192]; size_t i = 0; int q = 0; while (cbuf[i] && cbuf[i] != '\n' && (q || cbuf[i] != ';')) { if (cbuf[i] == '"') q = !q; i++; } memcpy(line, cbuf, i); line[i] = 0; memmove(cbuf, cbuf + i + (cbuf[i] ? 1 : 0), strlen(cbuf + i + (cbuf[i] ? 1 : 0)) + 1); exec_line(line); } }
static void type(const char* s) { static char echo[4200]; snprintf(echo, sizeof echo, "]\\%s\n", s); con_print(echo); strcat(cbuf, s); strcat(cbuf, "\n"); clip[0] = 0; written[0] = written_name[0] = 0; run(); }
static int fails, passes;
#define EXPECT(c) do { if (!(c)) { printf("FAIL line %d (W %d stamps %d): %s\n--- clipboard:\n%.700s\n--- last rows:\n%.200s\n%.200s\n%.200s\n", __LINE__, W, stamps, #c, clip, rows[cur - 2], rows[cur - 1], rows[cur]); fails++; } else passes++; } while (0)
#define LASTROW(s) (strstr(flat_tail(), s) != NULL)
static const char* tail_rows(int n);
// the last few rows as one line (a message can be wrapped in a narrow console)
static const char* flat_tail(void) { static char t[8192]; strcpy(t, tail_rows(cur < 3 ? cur : 3)); for (char* c = t; *c; c++) if (*c == '\n') *c = ' '; return t; }
// rows above the last one, as one string
static const char* tail_rows(int n) { static char t[8192]; t[0] = 0; for (int i = cur - n; i <= cur; i++) { char r[MAXW + 1]; strcpy(r, rows[i] + (stamps ? 9 : 0)); int k = (int)strlen(r); while (k && r[k - 1] == ' ') r[--k] = 0; strcat(t, r); strcat(t, "\n"); } return t; }

static void basics(void)
{
  con_reset();
  con_print("^2first line\nsecond line\n");
  type("clipboard"); EXPECT(!strcmp(clip, "first line\nsecond line")); EXPECT(LASTROW("copied the console (2 lines, 22 characters)"));
  type("echo third"); type("clipboard 2"); EXPECT(strstr(clip, "]\\echo third\nthird") == clip);
  type("clipboard 1"); EXPECT(LASTROW("copied the end of the console (1 line"));
  type("clipboard 99999"); EXPECT(strstr(clip, "first line\nsecond line\n") == clip);
  type("clipboard 0"); EXPECT(!clip[0] && LASTROW("nothing to copy"));
  type("clipboard name"); EXPECT(!strcmp(clip, "^1kai^7ros") && LASTROW("copied the value of name (1 line, 10 characters)"));
  type("clipboard three"); EXPECT(!strcmp(clip, "one\ntwo\nthree")); EXPECT(LASTROW("copied the command's output (3 lines, 13 characters)"));
  type("clipboard echo hello   world"); EXPECT(!strcmp(clip, "hello world"));
  type("clipboard echo \"a  b\" c"); EXPECT(!strcmp(clip, "a  b c"));
  type("clipboard nested"); EXPECT(!strcmp(clip, "direct\nchild\ngrandchild"));
  type("clipboard my.cfg"); EXPECT(!strcmp(clip, "from my.cfg"));
  type("clipboard quiet"); EXPECT(!clip[0] && LASTROW("the command printed nothing, nothing copied"));
  type("clipboard nosuchthing 1 2"); EXPECT(!strcmp(clip, "unknown: nosuchthing 1 2"));
  type("clipboard clipboard"); EXPECT(!clip[0] && LASTROW("can't copy the output of clipboard or grep"));
  { int before = clip_set; type("clipboard three; clipboard echo second"); EXPECT(!strcmp(clip, "second") && clip_set == before + 1); }
  clip_fail = 1; type("clipboard three"); EXPECT(!clip[0]); clip_fail = 0;
  // the whole console again, now that it holds markers and reports: nothing of clipboard's own last request in it
  type("echo END"); type("clipboard 1"); EXPECT(!strcmp(clip, "END"));
  EXPECT(have_silent ? announced == 0 : announced > 0); // with "silent" the engine never announces a dump

  // --- wrapped lines come back whole
  static char w1[1400], w2[1400], w3[400];
  strcpy(w1, "word"); while ((int)strlen(w1) < W * 2 + 20) strcat(w1, " word"); // wraps at words, twice
  memset(w2, 'x', (size_t)(W * 2 + 7)); w2[W * 2 + 7] = 0;                     // no spaces: runs into the edge twice
  strcpy(w3, "short line");
  char* f1[] = { w1, w3, w2, w3, w1 }; feed = f1; nfeed = 5;
  type("clipboard feed");
  { static char want[8000]; snprintf(want, sizeof want, "%s\n%s\n%s\n%s\n%s", w1, w3, w2, w3, w1); EXPECT(!strcmp(clip, want)); EXPECT(LASTROW("(5 lines,")); }
  type("clipboard 3"); // last three lines of the console: the report line, and above it ... count whole lines, not rows
  type("echo A"); feed = f1; nfeed = 1; type("feed"); type("echo B"); type("clipboard 4");
  { static char want[4000]; snprintf(want, sizeof want, "]\\feed\n%s\n]\\echo B\nB", w1); EXPECT(!strcmp(clip, want)); }
  // a line that is exactly as wide as the console, then another line: the known blind spot is only the join of these two
  // short lines stay separate, also when followed by a long word
  char* f2[] = { "one two", "averyveryverylongwordthatisnotwrappedbecausetherewasroom", "three" }; feed = f2; nfeed = 3;
  type("clipboard feed"); EXPECT(!strcmp(clip, "one two\naveryveryverylongwordthatisnotwrappedbecausetherewasroom\nthree"));

  // --- grep
  type("grep \"cvars\" r'cg_\\w*time\\w*'"); EXPECT(!strcmp(tail_rows(2), "cg_centertime\ncg_lagometertime\ngrep: 2 matches in 2 of 4 lines\n"));
  type("grep \"cvars\" r'\\d+:\\d+\\.\\d+'"); EXPECT(!strcmp(tail_rows(2), "12:01.050\n0:18.800\ngrep: 2 matches in 1 of 4 lines\n"));
  type("grep \"cvars\" r'.*fps.*'"); EXPECT(!strcmp(tail_rows(1), " S   sv_fps \"125\"\ngrep: 1 match in 1 of 4 lines\n"));
  type("grep \"cvars\" r'(?i)TIMER'"); EXPECT(!strcmp(tail_rows(1), "Timer\ngrep: 1 match in 1 of 4 lines\n"));
  type("grep \"cvars\" r'nothinglikethis'"); EXPECT(LASTROW("grep: 0 matches in 0 of 4 lines"));
  type("grep \"echo it's a 'test'\" r'\\'\\w+\\''"); EXPECT(!strcmp(tail_rows(1), "'test'\ngrep: 1 match in 1 of 1 lines\n"));
  type("grep \"echo one two  three\" r'two three'"); EXPECT(!strcmp(tail_rows(1), "two three\ngrep: 1 match in 1 of 1 lines\n"));
  { char* f3[] = { "a;b // c" }; feed = f3; nfeed = 1;
    type("grep \"feed\" \"r'a;b'\""); EXPECT(!strcmp(tail_rows(1), "a;b\ngrep: 1 match in 1 of 1 lines\n"));
    type("grep \"feed\" r'a\\x3bb \\x2f+'"); EXPECT(!strcmp(tail_rows(1), "a;b //\ngrep: 1 match in 1 of 1 lines\n"));
    type("grep \"echo one; echo two\" r'o\\w+|\\w+o'"); EXPECT(!strcmp(tail_rows(2), "one\ntwo\ngrep: 2 matches in 2 of 2 lines\n")); }
  type("grep \"echo back\\slash\" r'k\\\\s'"); EXPECT(!strcmp(tail_rows(1), "k\\s\ngrep: 1 match in 1 of 1 lines\n"));
  feed = f1; nfeed = 5; type("grep \"feed\" r'^short.*$'"); EXPECT(!strcmp(tail_rows(2), "short line\nshort line\ngrep: 2 matches in 2 of 5 lines\n"));
  type("grep \"feed\" r'x{150,}'"); EXPECT(LASTROW("grep: 1 match in 1 of 5 lines")); // found only if the row was put back together
  type("grep \"cvars\" r'cg_\\w+' out.txt"); EXPECT(!strcmp(written_name, "out.txt") && !strcmp(written, "cg_centertime\ncg_drawTimer\ncg_lagometertime\n") && LASTROW("written to out.txt"));
  type("grep \"cvars\" r'sv_\\w+' (logs/fps)"); EXPECT(!strcmp(written_name, "logs/fps.txt") && !strcmp(written, "sv_fps\n"));
  type("grep \"cvars\" r'x' ../evil.txt"); EXPECT(!written_name[0] && LASTROW("can't use ../evil.txt as a file name"));
  type("grep \"cvars\" r'x' q3config.cfg"); EXPECT(!written_name[0] && LASTROW("can't use q3config.cfg"));
  type("grep \"cvars\" r'x' a b"); EXPECT(LASTROW("unexpected text after the regex: a b"));
  type("grep \"cvars\" r'(unclosed'"); EXPECT(LASTROW("bad regex (unclosed: missing )"));
  type("grep \"cvars\" r'no end"); EXPECT(LASTROW("has no closing '"));
  type("grep \"cvars\" cg_"); EXPECT(strstr(tail_rows(cur < 14 ? cur : 14), "the regex must be written as r'...'"));
  type("grep \"cvars\" r''"); EXPECT(LASTROW("the regex is empty"));
  type("grep cvars"); EXPECT(strstr(tail_rows(cur < 14 ? cur : 14), "usage: grep"));
  type("grep \"grep x\" r'x'"); EXPECT(LASTROW("can't search the output of grep or clipboard"));
  type("grep \"quiet\" r'x'"); EXPECT(LASTROW("grep: the command printed nothing"));
  type("grep \"\" r'x'"); EXPECT(strstr(tail_rows(cur < 14 ? cur : 14), "usage: grep") || LASTROW("the command is empty"));
  type("grep \"feed\" r'(x*)*y'"); EXPECT(LASTROW("backtracks too much"));
}

// random text through the console and back: nothing lost, nothing added, only line breaks may differ where a
// line ended right at the edge (the documented blind spot)
static void random_text(unsigned seed)
{
  con_reset();
  srand(seed);
  static char* ls[60]; static char store[60][700];
  int n = 20 + rand() % 40; long joined = 0;
  for (int i = 0; i < n; i++) {
    char* o = store[i]; int words = 1 + rand() % 30;
    for (int w = 0; w < words; w++) { int len = 1 + rand() % (rand() % 5 ? 9 : 40); if (w) *o++ = ' '; for (int k = 0; k < len; k++) *o++ = (char)('a' + rand() % 26); }
    *o = 0; ls[i] = store[i];
  }
  feed = ls; nfeed = n;
  type("clipboard feed");
  static char want[60 * 700], got[60 * 700];
  want[0] = 0; for (int i = 0; i < n; i++) { strcat(want, ls[i]); strcat(want, i + 1 < n ? "\n" : ""); }
  strcpy(got, clip);
  int same = !strcmp(want, got);
  if (!same) { // allowed difference: a newline of `want` became a space or vanished
    const char *a = want, *b = got; same = 1;
    while (*a && same) { if (*a == *b) { a++; b++; } else if (*a == '\n' && *b == ' ') { a++; b++; joined++; } else if (*a == '\n') { a++; joined++; } else same = 0; }
    if (*b) same = 0;
  }
  if (!same) { printf("FAIL random seed %u W %d stamps %d\n", seed, W, stamps); fails++; } else passes++;
  static long total_joined, total_lines; total_joined += joined; total_lines += n;
  if (seed == 0xffff) printf("  blind spot: %ld of %ld line breaks joined\n", total_joined, total_lines);
}

int main(void)
{
  int widths[] = { 100, 78, 238, 318 };
  for (int s = 0; s < 2; s++)
    for (int w = 0; w < 4; w++) {
      W = widths[w]; stamps = s; cgs.glconfig.vidWidth = (W + 2) * 8; scale10 = 10;
      basics();
    }
  // a mod without "silent": the dump is repeated the plain way
  W = 100; stamps = 0; cgs.glconfig.vidWidth = (W + 2) * 8; have_silent = 0;
  con_reset(); type("clipboard three"); EXPECT(strstr(clip, "one\ntwo\nthree") == clip); // the first one still shows the failed try
  basics(); announced = 0;
  // the width guess is wrong: scaled display (console narrower than computed), and a console wider than computed
  W = 100; stamps = 0; cgs.glconfig.vidWidth = 1920; basics();
  W = 238; stamps = 1; cgs.glconfig.vidWidth = 640; basics();
  W = 119; stamps = 0; cgs.glconfig.vidWidth = 1936; scale10 = 20; basics(); scale10 = 10;
  EXPECT(announced > 0); // (the plain way stays in use for the rest of this run)
  for (unsigned seed = 1; seed <= 300; seed++) { W = widths[seed % 4]; stamps = (int)(seed / 4 % 2); cgs.glconfig.vidWidth = (W + 2) * 8; random_text(seed); }
  random_text(0xffff);
  printf("%d passed, %d failed\n", passes, fails);
  return fails != 0;
}
