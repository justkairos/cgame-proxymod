#ifndef HELP_H
#define HELP_H

#include "cg_cvar.h"

typedef enum
{
  // X, Y, W and H can be combined, hence the spacing.
  X              = 1,
  Y              = 2,
  W              = 4,
  H              = 8,
  BINARY_LITERAL = 16,
  RGBA,
  RGBAS,
  COUNT // a single plain non-negative integer, not a screen coordinate (see X/Y/W/H above)
} cvarKind_t;

typedef struct
{
  cvarTable_t const* cvarTable;
  cvarKind_t         kind;
  char const*        message[7];
} help_t;

void init_help(help_t const* help, size_t size);

void del_help(void);

void cvar_help(char const* cvarName);

void cvars_with_help(void);

#endif // HELP_H
