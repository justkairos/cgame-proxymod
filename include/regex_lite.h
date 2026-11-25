#ifndef REGEX_LITE_H
#define REGEX_LITE_H

#include <stddef.h>

/*
  A small backtracking regular expression matcher (there is no <regex.h> on Windows). No engine headers.

  Supported, with Perl / Python meaning:
    literals   .   [abc] [a-z] [^...]   \d \D \w \W \s \S   \t \n \r \xHH   \<punctuation>
    ^ $  \b \B
    ( ) (?: )   |
    * + ? {n} {n,} {n,m}, each also lazy with a trailing ?
    (?i) at the very start: ignore case
  Not supported: backreferences, lookaround, named groups, Unicode classes. Groups only group: a search
  returns the whole match.
*/

#define RE_MAX_PROG    4096
#define RE_MAX_CLASSES 128

typedef struct
{
  unsigned char  op;
  unsigned char  c;    // RE_CHAR: the character
  unsigned short cls;  // RE_CLASS: index into classes
  int            x, y; // RE_JMP: x; RE_SPLIT: x is tried first, then y (offsets relative to this instruction)
} re_inst_t;

typedef struct
{
  re_inst_t     prog[RE_MAX_PROG];
  int           len;
  unsigned char classes[RE_MAX_CLASSES][32];
  int           num_classes;
  int           icase;
  char          error[128]; // why re_compile failed
} re_t;

// Returns 1, or 0 with re->error set.
int re_compile(re_t* re, char const* pattern);

// Leftmost match in text[0..len) that starts at or after `from`. Returns 1 and the match [*start, *end),
// 0 if there is none, -1 if the pattern needs too much backtracking on this text.
int re_search(re_t const* re, char const* text, size_t len, size_t from, size_t* start, size_t* end);

#endif // REGEX_LITE_H
