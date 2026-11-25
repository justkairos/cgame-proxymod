#ifndef CG_ALIAS_H
#define CG_ALIAS_H

#include "q_shared.h"

// alias / unalias: user-defined commands, as in Quake 1 and 2, plus "+" aliases (see cg_alias.c)
void init_alias(void);

// Release every "+" variable alias that is still held (the DLL is unloaded on every map change).
void shutdown_alias(void);

// cmd is argv(0) of the console command being executed; returns qtrue if it was alias, unalias or an alias name.
qboolean alias_console_command(char const* cmd);

// cg_consolecmds.c: is `name` one of the proxymod's own console commands? (An alias can't have that name.)
qboolean is_proxymod_command(char const* name);

#endif // CG_ALIAS_H
