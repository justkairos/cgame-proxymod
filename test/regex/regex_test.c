// Checks src/regex_lite.c against Python's re:
//   python3 test/regex/gen_cases.py > cases.txt
//   gcc -fsanitize=address,undefined -Iinclude test/regex/regex_test.c src/regex_lite.c -o regex_test && ./regex_test cases.txt
#include "regex_lite.h"
#include <stdio.h>
#include <string.h>

static void untab(char* s) { for (; *s; s++) if (*s == 2) *s = '\t'; }

int main(int c, char** v)
{
  if (c < 2) return 2;
  FILE* f = fopen(v[1], "rb");
  if (!f) return 2;
  static char line[8192];
  static re_t re;
  int pass = 0, failed = 0, errors = 0;
  while (fgets(line, sizeof line, f))
  {
    line[strcspn(line, "\n")] = 0;
    char* text = strchr(line, '\t'); *text++ = 0;
    char* want = strchr(text, '\t'); *want++ = 0;
    untab(line); untab(text); untab(want);
    char got[8192] = "";
    if (!re_compile(&re, line)) { strcpy(got, "E"); errors++; }
    else
    {
      strcpy(got, "M");
      size_t len = strlen(text), from = 0, s, e; int first = 1, r;
      while (from <= len && (r = re_search(&re, text, len, from, &s, &e)) == 1)
      {
        if (e > s) { if (!first) strcat(got, "\x01"); strncat(got, text + s, e - s); first = 0; from = e; }
        else from = s + 1; // empty match: step on, like finditer
      }
      if (r < 0) strcpy(got, "TOO COMPLEX");
    }
    if (strcmp(got, want)) { if (failed++ < 400) printf("FAIL /%s/ on \"%s\"\n   want %s\n   got  %s  (%s)\n", line, text, want, got, re.error); }
    else pass++;
  }
  // runaway patterns must give up, not hang
  const char* evil[] = { "(a*)*b", "(a+)+$", "(a|aa)+$", "(x*)*y", "((a*)*)*c" };
  for (int i = 0; i < 5; i++)
  {
    char text[80]; memset(text, 'a', 60); text[59] = '!'; text[60] = 0; size_t s, e;
    if (!re_compile(&re, evil[i])) { printf("FAIL compile %s\n", evil[i]); failed++; continue; }
    int r = re_search(&re, text, 60, 0, &s, &e);
    if (r != -1 && r != 0 && r != 1) { printf("FAIL evil %s -> %d\n", evil[i], r); failed++; } else pass++;
  }
  printf("%d passed, %d failed (%d patterns rejected as expected or not)\n", pass, failed, errors);
  return failed != 0;
}
