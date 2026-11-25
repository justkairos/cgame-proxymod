/*
  Compiles a pattern into a small program and runs it with backtracking (the "recursive backtracking" machine of
  Russ Cox's regexp articles, with an explicit stack).

    RE_CHAR c     the next character is c
    RE_ANY        any character
    RE_CLASS n    the next character is in classes[n]
    RE_BOL RE_EOL start / end of the text
    RE_WORDB RE_NWORDB   at / not at a word boundary
    RE_JMP x      continue at pc + x
    RE_SPLIT x y  continue at pc + x; if that fails, at pc + y
    RE_MATCH      done

  All jumps are relative, and every construct compiles to one contiguous block that only jumps inside itself or
  to its own end. That is what lets a block be moved (to put a SPLIT in front of it) or copied (for {n,m}).
*/
#include "regex_lite.h"

#include <stdio.h>
#include <string.h>

enum
{
  RE_CHAR,
  RE_ANY,
  RE_CLASS,
  RE_BOL,
  RE_EOL,
  RE_WORDB,
  RE_NWORDB,
  RE_JMP,
  RE_SPLIT,
  RE_MATCH,
};

#define RE_MAX_REPEAT 1000
#define RE_MAX_STACK  16384
#define RE_MAX_STEPS  2000000

typedef struct
{
  re_t*       re;
  char const* p; // rest of the pattern
  int         depth;
} parser_t;

static int fail(parser_t* ps, char const* why)
{
  if (!ps->re->error[0]) snprintf(ps->re->error, sizeof(ps->re->error), "%s", why);
  return 0;
}

static int emit(parser_t* ps, int op, int x, int y)
{
  re_t* const re = ps->re;
  if (re->len >= RE_MAX_PROG) return fail(ps, "pattern too large");
  re_inst_t* const i = &re->prog[re->len++];
  i->op              = (unsigned char)op;
  i->c               = 0;
  i->cls             = 0;
  i->x               = x;
  i->y               = y;
  return 1;
}

// Makes room for one instruction at `at`, moving the block behind it up.
static int insert(parser_t* ps, int at, int op, int x, int y)
{
  re_t* const re = ps->re;
  if (re->len >= RE_MAX_PROG) return fail(ps, "pattern too large");
  memmove(&re->prog[at + 1], &re->prog[at], (size_t)(re->len - at) * sizeof(re->prog[0]));
  ++re->len;
  re->prog[at].op  = (unsigned char)op;
  re->prog[at].c   = 0;
  re->prog[at].cls = 0;
  re->prog[at].x   = x;
  re->prog[at].y   = y;
  return 1;
}

static int lower(int c)
{
  return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int upper(int c)
{
  return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int is_word(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void set_bit(unsigned char* cls, int c)
{
  cls[(c & 255) >> 3] |= (unsigned char)(1 << (c & 7));
}

static int has_bit(unsigned char const* cls, int c)
{
  return cls[(c & 255) >> 3] >> (c & 7) & 1;
}

// \d \w \s and their negations, added to cls. Returns 0 if `letter` is none of them.
static int add_shorthand(unsigned char* cls, int letter)
{
  int const kind = lower(letter);
  if (kind != 'd' && kind != 'w' && kind != 's') return 0;
  int const negate = letter != kind;
  for (int c = 0; c < 256; ++c)
  {
    int in;
    if (kind == 'd')
      in = c >= '0' && c <= '9';
    else if (kind == 'w')
      in = is_word(c);
    else
      in = c == ' ' || (c >= '\t' && c <= '\r');
    if (in != negate) set_bit(cls, c);
  }
  return 1;
}

static int hex_digit(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  c = lower(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// The character an escape stands for, with ps->p on the letter after the backslash. -1 on a bad \x.
static int escaped_char(parser_t* ps)
{
  int const c = (unsigned char)*ps->p++;
  switch (c)
  {
  case 't':
    return '\t';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 'x':
  {
    int const hi = hex_digit((unsigned char)ps->p[0]);
    int const lo = hi < 0 ? -1 : hex_digit((unsigned char)ps->p[1]);
    if (lo < 0) return -1;
    ps->p += 2;
    return hi * 16 + lo;
  }
  default:
    return c;
  }
}

static int new_class(parser_t* ps)
{
  if (ps->re->num_classes >= RE_MAX_CLASSES)
  {
    fail(ps, "too many character classes");
    return -1;
  }
  memset(ps->re->classes[ps->re->num_classes], 0, 32);
  return ps->re->num_classes++;
}

static int emit_class(parser_t* ps, int cls)
{
  if (!emit(ps, RE_CLASS, 0, 0)) return 0;
  ps->re->prog[ps->re->len - 1].cls = (unsigned short)cls;
  return 1;
}

static int emit_char(parser_t* ps, int c)
{
  if (ps->re->icase && lower(c) != upper(c))
  {
    int const cls = new_class(ps);
    if (cls < 0) return 0;
    set_bit(ps->re->classes[cls], lower(c));
    set_bit(ps->re->classes[cls], upper(c));
    return emit_class(ps, cls);
  }
  if (!emit(ps, RE_CHAR, 0, 0)) return 0;
  ps->re->prog[ps->re->len - 1].c = (unsigned char)c;
  return 1;
}

// [...] with ps->p just behind the [
static int parse_class(parser_t* ps)
{
  int const n = new_class(ps);
  if (n < 0) return 0;
  unsigned char* const cls    = ps->re->classes[n];
  int                  negate = 0;
  if (*ps->p == '^')
  {
    negate = 1;
    ++ps->p;
  }
  for (int first = 1;; first = 0)
  {
    if (!*ps->p) return fail(ps, "missing ]");
    if (*ps->p == ']' && !first)
    {
      ++ps->p;
      break;
    }
    int lo;
    if (*ps->p == '\\')
    {
      ++ps->p;
      if (!*ps->p) return fail(ps, "pattern ends with a backslash");
      if (add_shorthand(cls, (unsigned char)*ps->p))
      {
        ++ps->p;
        continue;
      }
      lo = escaped_char(ps);
      if (lo < 0) return fail(ps, "\\x needs two hex digits");
    }
    else
    {
      lo = (unsigned char)*ps->p++;
    }
    int hi = lo;
    if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']')
    {
      ++ps->p;
      if (*ps->p == '\\')
      {
        ++ps->p;
        if (!*ps->p) return fail(ps, "pattern ends with a backslash");
        hi = escaped_char(ps);
        if (hi < 0) return fail(ps, "\\x needs two hex digits");
      }
      else
      {
        hi = (unsigned char)*ps->p++;
      }
      if (hi < lo) return fail(ps, "bad range in [ ]");
    }
    for (int c = lo; c <= hi; ++c)
    {
      set_bit(cls, c);
      if (ps->re->icase)
      {
        set_bit(cls, lower(c));
        set_bit(cls, upper(c));
      }
    }
  }
  if (negate)
  {
    for (int i = 0; i < 32; ++i) cls[i] = (unsigned char)~cls[i];
  }
  return emit_class(ps, n);
}

static int parse_alt(parser_t* ps);

// One thing that can be repeated. Returns 0 on error.
static int parse_atom(parser_t* ps)
{
  int const c = (unsigned char)*ps->p++;
  switch (c)
  {
  case '(':
  {
    if (ps->p[0] == '?')
    {
      if (ps->p[1] != ':') return fail(ps, "only (?: ) groups are supported, and (?i) at the very start");
      ps->p += 2;
    }
    if (++ps->depth > 100) return fail(ps, "groups nested too deep");
    if (!parse_alt(ps)) return 0;
    --ps->depth;
    if (*ps->p != ')') return fail(ps, "missing )");
    ++ps->p;
    return 1;
  }
  case '[':
    return parse_class(ps);
  case '.':
    return emit(ps, RE_ANY, 0, 0);
  case '^':
    return emit(ps, RE_BOL, 0, 0);
  case '$':
    return emit(ps, RE_EOL, 0, 0);
  case '*':
  case '+':
  case '?':
    return fail(ps, "nothing to repeat");
  case '\\':
  {
    if (!*ps->p) return fail(ps, "pattern ends with a backslash");
    int const e = (unsigned char)*ps->p;
    if (e == 'b' || e == 'B')
    {
      ++ps->p;
      return emit(ps, e == 'b' ? RE_WORDB : RE_NWORDB, 0, 0);
    }
    if (lower(e) == 'd' || lower(e) == 'w' || lower(e) == 's')
    {
      int const n = new_class(ps);
      if (n < 0) return 0;
      add_shorthand(ps->re->classes[n], e);
      ++ps->p;
      return emit_class(ps, n);
    }
    int const lit = escaped_char(ps);
    if (lit < 0) return fail(ps, "\\x needs two hex digits");
    return emit_char(ps, lit);
  }
  default:
    return emit_char(ps, c);
  }
}

// {n}, {n,}, {n,m} at ps->p (on the {). Returns 0 if it is not a repeat (then { is a literal, as in Perl).
static int parse_braces(parser_t* ps, int* min, int* max)
{
  char const* p = ps->p + 1;
  if ((*p < '0' || *p > '9') && !(p[0] == ',' && p[1] >= '0' && p[1] <= '9')) return 0; // {,m} is {0,m}
  int lo = 0;
  while (*p >= '0' && *p <= '9' && lo <= RE_MAX_REPEAT) lo = lo * 10 + (*p++ - '0');
  int hi = lo;
  if (*p == ',')
  {
    ++p;
    if (*p == '}')
      hi = -1;
    else
    {
      if (*p < '0' || *p > '9') return 0;
      hi = 0;
      while (*p >= '0' && *p <= '9' && hi <= RE_MAX_REPEAT) hi = hi * 10 + (*p++ - '0');
    }
  }
  if (*p != '}') return 0;
  ps->p = p + 1;
  *min  = lo;
  *max  = hi;
  return 1;
}

// An atom with its repeat count.
static int parse_piece(parser_t* ps)
{
  re_t* const re    = ps->re;
  int const   start = re->len;
  if (!parse_atom(ps)) return 0;

  int min, max; // max -1: no limit
  if (*ps->p == '*')
  {
    min = 0, max = -1;
    ++ps->p;
  }
  else if (*ps->p == '+')
  {
    min = 1, max = -1;
    ++ps->p;
  }
  else if (*ps->p == '?')
  {
    min = 0, max = 1;
    ++ps->p;
  }
  else if (*ps->p != '{' || !parse_braces(ps, &min, &max))
    return 1;
  if (min > RE_MAX_REPEAT || max > RE_MAX_REPEAT) return fail(ps, "repeat count too large (1000 at most)");
  if (max >= 0 && max < min) return fail(ps, "bad repeat count: {n,m} with m < n");
  int lazy = 0;
  if (*ps->p == '?')
  {
    lazy = 1;
    ++ps->p;
  }
  if (*ps->p == '*' || *ps->p == '+' || *ps->p == '?')
    return fail(ps, "repeat of a repeat: put ( ) around the first one");

  // take the atom's block out and put down what the count asks for
  int const k = re->len - start;
  if (k > RE_MAX_PROG / 2) return fail(ps, "pattern too large");
  static re_inst_t block[RE_MAX_PROG / 2 + 1];
  memcpy(block, &re->prog[start], (size_t)k * sizeof(block[0]));
  re->len = start;
#define COPY_BLOCK()                                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    if (re->len + k > RE_MAX_PROG) return fail(ps, "pattern too large");                                               \
    memcpy(&re->prog[re->len], block, (size_t)k * sizeof(block[0]));                                                   \
    re->len += k;                                                                                                      \
  } while (0)
  for (int i = 0; i < min; ++i) COPY_BLOCK();
  if (max < 0)
  {
    // loop: SPLIT body, out; body; JMP loop
    int const in = 1, out_ = k + 2;
    if (!emit(ps, RE_SPLIT, lazy ? out_ : in, lazy ? in : out_)) return 0;
    COPY_BLOCK();
    if (!emit(ps, RE_JMP, -(k + 1), 0)) return 0;
  }
  else
  {
    // (body(body(body)?)?)? : each SPLIT can leave to the common end
    for (int left = max - min; left > 0; --left)
    {
      int const in = 1, out_ = left * (k + 1);
      if (!emit(ps, RE_SPLIT, lazy ? out_ : in, lazy ? in : out_)) return 0;
      COPY_BLOCK();
    }
  }
#undef COPY_BLOCK
  return 1;
}

static int parse_seq(parser_t* ps)
{
  while (*ps->p && *ps->p != '|' && *ps->p != ')')
  {
    if (!parse_piece(ps)) return 0;
  }
  return 1;
}

static int parse_alt(parser_t* ps)
{
  re_t* const re    = ps->re;
  int const   start = re->len;
  if (!parse_seq(ps)) return 0;
  while (*ps->p == '|')
  {
    ++ps->p;
    // SPLIT left, right; left...; JMP end; right...
    if (!insert(ps, start, RE_SPLIT, 1, 0)) return 0;
    int const jmp = re->len;
    if (!emit(ps, RE_JMP, 0, 0)) return 0;
    re->prog[start].y = re->len - start;
    if (!parse_seq(ps)) return 0;
    re->prog[jmp].x = re->len - jmp;
  }
  return 1;
}

int re_compile(re_t* re, char const* pattern)
{
  re->len         = 0;
  re->num_classes = 0;
  re->icase       = 0;
  re->error[0]    = 0;
  parser_t ps     = { re, pattern, 0 };
  if (!strncmp(ps.p, "(?i)", 4))
  {
    re->icase = 1;
    ps.p += 4;
  }
  if (!parse_alt(&ps)) return 0;
  if (*ps.p == ')') return fail(&ps, "unmatched )");
  if (*ps.p) return fail(&ps, "unexpected character");
  return emit(&ps, RE_MATCH, 0, 0);
}

typedef struct
{
  int    pc;
  size_t sp;
} thread_t;

// Does the program match starting exactly at `at`? 1 with *end, 0, or -1 when out of budget.
static int run(re_t const* re, char const* text, size_t len, size_t at, size_t* end, long* steps)
{
  static thread_t stack[RE_MAX_STACK];
  int             depth = 0;
  stack[depth++]        = (thread_t){ 0, at };
  while (depth)
  {
    thread_t const t  = stack[--depth];
    int            pc = t.pc;
    size_t         sp = t.sp;
    for (;;)
    {
      if (++*steps > RE_MAX_STEPS) return -1;
      re_inst_t const* const i  = &re->prog[pc];
      int                    ok = 1;
      switch (i->op)
      {
      case RE_CHAR:
        ok = sp < len && (unsigned char)text[sp] == i->c;
        if (ok) ++sp, ++pc;
        break;
      case RE_ANY:
        ok = sp < len;
        if (ok) ++sp, ++pc;
        break;
      case RE_CLASS:
        ok = sp < len && has_bit(re->classes[i->cls], (unsigned char)text[sp]);
        if (ok) ++sp, ++pc;
        break;
      case RE_BOL:
        ok = sp == 0;
        ++pc;
        break;
      case RE_EOL:
        ok = sp == len;
        ++pc;
        break;
      case RE_WORDB:
      case RE_NWORDB:
      {
        int const before = sp > 0 && is_word((unsigned char)text[sp - 1]);
        int const after  = sp < len && is_word((unsigned char)text[sp]);
        ok               = (before != after) == (i->op == RE_WORDB);
        ++pc;
        break;
      }
      case RE_JMP:
        pc += i->x;
        break;
      case RE_SPLIT:
        if (depth >= RE_MAX_STACK) return -1;
        stack[depth++] = (thread_t){ pc + i->y, sp };
        pc += i->x;
        break;
      case RE_MATCH:
        *end = sp;
        return 1;
      default:
        return 0;
      }
      if (!ok) break;
    }
  }
  return 0;
}

int re_search(re_t const* re, char const* text, size_t len, size_t from, size_t* start, size_t* end)
{
  long steps = 0;
  for (size_t at = from; at <= len; ++at)
  {
    int const r = run(re, text, len, at, end, &steps);
    if (r < 0) return -1;
    if (r)
    {
      *start = at;
      return 1;
    }
  }
  return 0;
}
