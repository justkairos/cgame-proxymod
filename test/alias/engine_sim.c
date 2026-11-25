// Mini engine around cg_alias.c: command buffer + tokenizer + command/cvar/cgame dispatch order as in Cmd_ExecuteString,
// key binding dispatch as in Key_ParseBinding, kbutton as in IN_KeyDown/IN_KeyUp.
#include "cg_alias.h"
#include "cg_local.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char cbuf[1 << 20]; static size_t cbuf_len;
static char argv_[64][1024]; static int argc_;
static char logbuf[1 << 20]; static size_t log_len; static int log_overflow;
static int ms = 1000;
static char file[1 << 16]; static int file_len = -1; static int wpos, rpos; static int fail_write;
typedef struct { char name[64], val[512]; } cv; static cv cvars[64]; static int ncv;
typedef struct { int down[2]; int active; } kb; static kb button5, attack;
static char added[4096];

static void L(const char* s) { size_t n = strlen(s); if (log_len + n + 1 >= sizeof logbuf) { log_overflow = 1; return; } memcpy(logbuf + log_len, s, n + 1); log_len += n; }
static cv* getcv(const char* n) { for (int i = 0; i < ncv; i++) if (!Q_stricmp(cvars[i].name, n)) return &cvars[i]; return 0; }
static void setcv(const char* n, const char* v) { cv* c = getcv(n); if (!c) { c = &cvars[ncv++]; strcpy(c->name, n); } strcpy(c->val, v); }

void trap_Print(char const* s) { L("P:"); L(s); }
int32_t trap_Milliseconds(void) { return ms; }
void trap_SendConsoleCommand(char const* t) { size_t n = strlen(t); assert(cbuf_len + n < sizeof cbuf); memcpy(cbuf + cbuf_len, t, n); cbuf_len += n; }
void trap_Cvar_VariableStringBuffer(char const* n, char* b, int32_t sz) { cv* c = getcv(n); snprintf(b, sz, "%s", c ? c->val : ""); }
void trap_AddCommand(char const* n) { strcat(added, n); strcat(added, " "); }
int32_t trap_Argc(void) { return argc_; }
void trap_Argv(int32_t n, char* b, int32_t sz) { snprintf(b, sz, "%s", n >= 0 && n < argc_ ? argv_[n] : ""); }
int32_t trap_FS_FOpenFile(char const* q, fileHandle_t* f, fsMode_t m) { (void)q; if (m == FS_WRITE) { if (fail_write) { *f = 0; return -1; } file_len = 0; wpos = 0; *f = 1; return 0; } if (file_len < 0) { *f = 0; return -1; } rpos = 0; *f = 2; return file_len; }
void trap_FS_Read(void* b, int32_t len, fileHandle_t f) { (void)f; memcpy(b, file + rpos, len); rpos += len; }
void trap_FS_Write(void const* b, int32_t len, fileHandle_t f) { (void)f; memcpy(file + wpos, b, len); wpos += len; file_len = wpos; }
void trap_FS_FCloseFile(fileHandle_t f) { (void)f; }

qboolean is_proxymod_command(char const* n) { return !Q_stricmp(n, "search") || !Q_stricmp(n, "mapinfo"); }
// Cmd_TokenizeString (quotes, whitespace <= ' ')
static void tokenize(const char* t) { argc_ = 0; while (1) { while (*t && (unsigned char)*t <= ' ') t++; if (!*t) return; char* o = argv_[argc_++]; if (*t == '"') { t++; while (*t && *t != '"') *o++ = *t++; if (*t) t++; } else { while ((unsigned char)*t > ' ' && *t != '"') *o++ = *t++; } *o = 0; } }
static void kdown(kb* b, const char* nm) { int k = argv_[1][0] && argc_ > 1 ? atoi(argv_[1]) : -1; if (k == b->down[0] || k == b->down[1]) return; if (!b->down[0]) b->down[0] = k; else if (!b->down[1]) b->down[1] = k; else return; if (b->active) return; b->active = 1; L("B:+"); L(nm); L("\n"); }
static void kup(kb* b, const char* nm) { if (!(argc_ > 1 && argv_[1][0])) { b->down[0] = b->down[1] = 0; if (b->active) { L("B:-"); L(nm); L("\n"); } b->active = 0; return; } int k = atoi(argv_[1]); if (b->down[0] == k) b->down[0] = 0; else if (b->down[1] == k) b->down[1] = 0; else return; if (b->down[0] || b->down[1]) return; b->active = 0; L("B:-"); L(nm); L("\n"); }
static long execs;
static void exec_line(const char* line) {
  tokenize(line); if (!argc_) return; execs++;
  const char* c = argv_[0];
  if (!Q_stricmp(c, "+button5")) return kdown(&button5, "button5");
  if (!Q_stricmp(c, "-button5")) return kup(&button5, "button5");
  if (!Q_stricmp(c, "+attack")) return kdown(&attack, "attack");
  if (!Q_stricmp(c, "-attack")) return kup(&attack, "attack");
  if (!Q_stricmp(c, "screenshotjpeg") || !Q_stricmp(c, "echo")) { L("E:"); L(line); L("\n"); return; }
  cv* v = getcv(c); if (v) { if (argc_ > 1) strcpy(v->val, argv_[1]); return; }
  if (alias_console_command(c)) return;
  L("U:"); L(line); L("\n");
}
// Cbuf_Execute: split on \n and on ; outside quotes
static void run(void) { size_t pos = 0; while (pos < cbuf_len) { size_t i = pos; int q = 0; for (; i < cbuf_len; i++) { if (cbuf[i] == '"') q = !q; if (!q && cbuf[i] == ';') break; if (cbuf[i] == '\n') break; } char line[4096]; size_t n = i - pos; assert(n < sizeof line); memcpy(line, cbuf + pos, n); line[n] = 0; pos = i + 1; exec_line(line); assert(execs < 5000000); } cbuf_len = 0; }
static void con(const char* s) { trap_SendConsoleCommand(s); trap_SendConsoleCommand("\n"); run(); }
// Key_ParseBinding
static void key(const char* binding, int k, int down) { char buf[1024]; strcpy(buf, binding); char* p = buf; while (1) { while (isspace((unsigned char)*p)) p++; char* end = strchr(p, ';'); if (end) *end = 0; char cmd[1100]; if (*p == '+') { sprintf(cmd, "%c%s %d %d\n", down ? '+' : '-', p + 1, k, 12345); trap_SendConsoleCommand(cmd); } else if (down) { sprintf(cmd, "%s\n", p); trap_SendConsoleCommand(cmd); } if (!end) break; p = end + 1; } run(); }
static void clr(void) { log_len = 0; logbuf[0] = 0; }
#define EXPECT(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n--- log:\n%s\n", __LINE__, #cond, logbuf); fails++; } else passes++; } while (0)
static int fails, passes;
#define HAS(s) (strstr(logbuf, s) != NULL)
#define CV(n) (getcv(n)->val)

int main(void) {
  setcv("cg_fov", "110"); setcv("sensitivity", "3"); setcv("name", "kairos"); setcv("empty", "");
  init_alias(); EXPECT(HAS("") && strstr(added, "alias ") && strstr(added, "unalias ") && strstr(added, "mdd_alias "));
  // --- plain alias
  clr(); con("alias ss screenshotjpeg"); EXPECT(HAS("P:alias ss \"screenshotjpeg\""));
  clr(); con("ss"); EXPECT(HAS("E:screenshotjpeg\n")); EXPECT(!HAS("U:"));
  clr(); key("ss", 50, 1); key("ss", 50, 1); key("ss", 50, 0); { int n = 0; for (char* p = logbuf; (p = strstr(p, "E:screenshotjpeg")); p++) n++; EXPECT(n == 2); }
  clr(); con("SS"); EXPECT(HAS("E:screenshotjpeg"));
  clr(); con("alias multi \"echo one; echo two\""); con("multi"); EXPECT(HAS("E:echo one\nE: echo two\n"));
  clr(); con("alias sayit echo hello   world"); con("sayit"); EXPECT(HAS("E:echo hello world\n"));
  clr(); con("alias q \"echo \"a;b\"\""); // odd quoting must not crash
  // --- listing
  clr(); con("alias"); EXPECT(HAS("alias ss \"screenshotjpeg\"") && HAS("alias multi \"echo one; echo two\""));
  clr(); con("alias ss"); EXPECT(HAS("P:alias ss \"screenshotjpeg\""));
  clr(); con("alias nope"); EXPECT(HAS("no alias nope"));
  // --- refused names
  clr(); con("alias quit echo x"); EXPECT(HAS("can't create quit") && HAS("engine command"));
  clr(); con("alias +attack echo x"); EXPECT(HAS("can't create +attack"));
  clr(); con("alias -Attack echo x"); EXPECT(HAS("can't create -Attack"));
  clr(); con("alias cg_fov echo x"); EXPECT(HAS("can't create cg_fov") && HAS("cvar"));
  clr(); con("alias mdd_help echo x"); EXPECT(HAS("proxymod name"));
  clr(); con("alias +mdd_jumpshoot echo x"); EXPECT(HAS("proxymod name"));
  clr(); con("alias alias echo x"); EXPECT(HAS("proxymod name"));
  clr(); con("alias search echo x"); EXPECT(HAS("proxymod name"));
  clr(); con("alias mapinfo echo x"); EXPECT(HAS("proxymod name"));
  clr(); con("alias + echo x"); EXPECT(HAS("can't create +"));
  clr(); con("alias \"a b\" echo x"); EXPECT(HAS("can't create a b"));
  clr(); con("alias aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa echo x"); EXPECT(HAS("too long")); // 32 chars
  clr(); con("alias aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa echo x"); EXPECT(HAS("P:alias aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")); // 31
  clr(); con("alias empty2 \"\""); EXPECT(HAS("nothing to run"));
  clr(); con("alias empty2 \"   \""); EXPECT(HAS("nothing to run"));
  { char big[2000] = "alias big \""; for (int i = 0; i < 600; i++) strcat(big, "x"); strcat(big, "\""); clr(); con(big); EXPECT(HAS("commands too long")); }
  { char big[2000] = "alias ok511 \""; for (int i = 0; i < 511; i++) strcat(big, "x"); strcat(big, "\""); clr(); con(big); EXPECT(HAS("P:alias ok511")); clr(); con("ok511"); EXPECT(HAS("U:xxxxx") && strlen(strstr(logbuf, "U:")) == 2 + 511 + 1); con("unalias ok511"); }
  // --- + alias to a button, bound to a key
  clr(); con("alias +grapple +button5"); EXPECT(HAS("P:alias +grapple \"+button5\"") && strstr(added, "+grapple ") && strstr(added, "-grapple "));
  clr(); key("+grapple", 100, 1); EXPECT(HAS("B:+button5") && button5.active && button5.down[0] == 100);
  key("+grapple", 100, 1); key("+grapple", 100, 1); EXPECT(button5.active);
  clr(); key("+grapple", 100, 0); EXPECT(HAS("B:-button5") && !button5.active); EXPECT(!HAS("U:"));
  // two keys
  clr(); key("+grapple", 100, 1); key("+grapple", 101, 1); key("+grapple", 100, 0); EXPECT(button5.active); key("+grapple", 101, 0); EXPECT(!button5.active);
  // typed in console
  clr(); con("+grapple"); EXPECT(button5.active); con("-grapple"); EXPECT(!button5.active);
  // mixed commands: non-+ part only on press
  clr(); con("alias +combo \"+attack; echo fired; +button5\""); key("+combo", 7, 1); EXPECT(attack.active && button5.active && HAS("E:echo fired")); clr(); key("+combo", 7, 0); EXPECT(!attack.active && !button5.active && !HAS("echo fired"));
  // explicit release alias wins
  clr(); con("alias -grapple \"-button5; echo released\""); key("+grapple", 9, 1); EXPECT(button5.active); clr(); key("+grapple", 9, 0); EXPECT(!button5.active && HAS("E:echo released"));
  con("unalias -grapple"); clr(); key("+grapple", 9, 1); key("+grapple", 9, 0); EXPECT(!button5.active && !HAS("released"));
  // nested + alias
  clr(); con("alias +outer +grapple"); key("+outer", 3, 1); EXPECT(button5.active && button5.down[0] == 3); key("+outer", 3, 0); EXPECT(!button5.active);
  // unalias while held unsticks
  key("+grapple", 9, 1); EXPECT(button5.active); con("unalias +grapple"); EXPECT(!button5.active); clr(); key("+grapple", 9, 0); EXPECT(HAS("U:-grapple"));
  con("alias +grapple +button5");
  // --- + alias to a variable, two values
  clr(); con("alias +zoomfov cg_fov 60 120"); EXPECT(HAS("P:alias +zoomfov cg_fov \"60\" \"120\""));
  key("+zoomfov", 20, 1); EXPECT(!strcmp(CV("cg_fov"), "60")); setcv("cg_fov", "61"); key("+zoomfov", 20, 1); EXPECT(!strcmp(CV("cg_fov"), "61")); // repeat does not re-apply
  key("+zoomfov", 20, 0); EXPECT(!strcmp(CV("cg_fov"), "120"));
  con("+zoomfov"); EXPECT(!strcmp(CV("cg_fov"), "60")); con("-zoomfov"); EXPECT(!strcmp(CV("cg_fov"), "120"));
  setcv("cg_fov", "100"); con("-zoomfov"); EXPECT(!strcmp(CV("cg_fov"), "120")); // release alone still sets the second value
  setcv("cg_fov", "100"); key("+zoomfov", 77, 0); EXPECT(!strcmp(CV("cg_fov"), "100")); // key up without a down: nothing
  // two keys hold it
  key("+zoomfov", 20, 1); key("+zoomfov", 21, 1); key("+zoomfov", 20, 0); EXPECT(!strcmp(CV("cg_fov"), "60")); key("+zoomfov", 21, 0); EXPECT(!strcmp(CV("cg_fov"), "120"));
  // --- one value: restore what it was at the press
  setcv("cg_fov", "105"); clr(); con("alias +zoomfov cg_fov 40"); EXPECT(HAS("alias +zoomfov cg_fov \"40\"") && !strcmp(CV("cg_fov"), "120") == 0);
  setcv("cg_fov", "105"); key("+zoomfov", 20, 1); EXPECT(!strcmp(CV("cg_fov"), "40")); key("+zoomfov", 20, 1); key("+zoomfov", 20, 0); EXPECT(!strcmp(CV("cg_fov"), "105"));
  setcv("cg_fov", "90"); key("+zoomfov", 20, 1); key("+zoomfov", 20, 0); EXPECT(!strcmp(CV("cg_fov"), "90"));
  con("-zoomfov"); EXPECT(!strcmp(CV("cg_fov"), "90")); // not held, nothing saved: nothing happens
  // values with spaces, restore of a value with spaces
  con("alias +nm name \"a b\""); key("+nm", 5, 1); EXPECT(!strcmp(CV("name"), "a b")); key("+nm", 5, 0); EXPECT(!strcmp(CV("name"), "kairos"));
  setcv("name", "x y"); key("+nm", 5, 1); key("+nm", 5, 0); EXPECT(!strcmp(CV("name"), "x y"));
  // empty current value is restored as empty
  con("alias +em empty 1"); key("+em", 5, 1); EXPECT(!strcmp(CV("empty"), "1")); key("+em", 5, 0); EXPECT(!strcmp(CV("empty"), ""));
  // redefine while held lets go first
  setcv("cg_fov", "90"); key("+zoomfov", 20, 1); EXPECT(!strcmp(CV("cg_fov"), "40")); con("alias +zoomfov cg_fov 30 100"); EXPECT(!strcmp(CV("cg_fov"), "90")); key("+zoomfov", 20, 0); EXPECT(!strcmp(CV("cg_fov"), "90"));
  // explicit -alias with a variable alias: runs instead, state cleared
  con("alias -zoomfov \"cg_fov 95; sensitivity 5\""); key("+zoomfov", 20, 1); EXPECT(!strcmp(CV("cg_fov"), "30")); key("+zoomfov", 20, 0); EXPECT(!strcmp(CV("cg_fov"), "95") && !strcmp(CV("sensitivity"), "5"));
  key("+zoomfov", 20, 1); EXPECT(!strcmp(CV("cg_fov"), "30")); key("+zoomfov", 20, 0); con("unalias -zoomfov");
  // quoted + form is a plain command, not a variable
  clr(); con("alias +say2 \"echo a b\""); key("+say2", 1, 1); key("+say2", 1, 0); EXPECT(HAS("E:echo a b\n") && !HAS("U:"));
  // shutdown releases held variable aliases
  setcv("cg_fov", "90"); con("alias +zoomfov cg_fov 40"); key("+zoomfov", 20, 1); EXPECT(!strcmp(CV("cg_fov"), "40")); shutdown_alias(); run(); EXPECT(!strcmp(CV("cg_fov"), "90"));
  // --- persistence: reload
  char before[65536]; clr(); con("alias"); strcpy(before, logbuf); added[0] = 0;
  init_alias(); clr(); con("alias"); EXPECT(!strcmp(before, logbuf)); EXPECT(strstr(added, "ss ") && strstr(added, "+zoomfov ") && strstr(added, "-zoomfov "));
  clr(); con("ss"); EXPECT(HAS("E:screenshotjpeg")); key("+zoomfov", 20, 1); EXPECT(!strcmp(CV("cg_fov"), "40")); key("+zoomfov", 20, 0); EXPECT(!strcmp(CV("cg_fov"), "90"));
  key("+zoomfov", 20, 0); // stray key up after a reload
  // file edited by hand: CRLF, stripped trailing tab, junk lines
  strcpy(file, "mdd aliases 1\r\nC\tss\tscreenshotjpeg\r\nV\t+z\tcg_fov\t50\t0\r\njunk\r\nX\ta\tb\r\nC\tquit\techo no\r\nV\tnoplus\tcg_fov\t1\t0\t\r\nC\t\techo noname\r\n\r\nC\tlast\techo last"); file_len = strlen(file);
  init_alias(); clr(); con("alias"); EXPECT(HAS("alias ss") && HAS("alias +z cg_fov \"50\"") && HAS("alias last \"echo last\"") && HAS("3 aliases")); 
  strcpy(file, "garbage\nC\tss\tscreenshotjpeg\n"); file_len = strlen(file); init_alias(); clr(); con("alias"); EXPECT(HAS("no aliases"));
  file_len = 0; init_alias(); file_len = -1; init_alias(); clr(); con("alias"); EXPECT(HAS("no aliases"));
  // --- loops must terminate
  con("alias a a"); clr(); execs = 0; con("a"); EXPECT(HAS("runs in a loop") && execs < 5000);
  ms += 1000; con("alias f \"f;f\""); clr(); execs = 0; con("f"); EXPECT(HAS("runs in a loop") && execs < 100000); EXPECT(!log_overflow);
  ms += 1000; con("alias +l +l"); clr(); execs = 0; key("+l", 4, 1); EXPECT(HAS("runs in a loop")); ms += 1000; key("+l", 4, 0);
  ms += 1000; con("alias p1 p2"); con("alias p2 p1"); clr(); con("p1"); EXPECT(HAS("runs in a loop"));
  ms += 1000; clr(); con("a2"); EXPECT(HAS("U:a2")); // works again in the next window
  // --- unalias
  clr(); con("unalias a f nope"); EXPECT(HAS("removed alias a") && HAS("removed alias f") && HAS("no alias nope")); clr(); con("a"); EXPECT(HAS("U:a"));
  clr(); con("unalias"); EXPECT(HAS("usage: unalias"));
  // --- table full
  ms += 1000; for (int i = 0; i < 300; i++) { char c[64]; sprintf(c, "alias t%d echo %d", i, i); con(c); } clr(); con("alias zz echo z"); EXPECT(HAS("too many aliases")); clr(); con("alias t5 echo five"); EXPECT(HAS("P:alias t5 \"echo five\""));
  init_alias(); clr(); con("t5"); EXPECT(HAS("E:echo five")); clr(); con("t200"); EXPECT(HAS("E:echo 200"));
  // write failure is reported, alias still works this session
  fail_write = 1; con("unalias t0"); clr(); con("alias t0 echo zero"); EXPECT(HAS("can't write")); clr(); con("t0"); EXPECT(HAS("E:echo zero")); fail_write = 0;
  printf("%d passed, %d failed\n", passes, fails); return fails != 0;
}
