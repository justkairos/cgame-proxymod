#ifndef CG_SEARCH_H
#define CG_SEARCH_H

#include "q_shared.h"

// search / mdd_search <pattern> ...: cvar names, command names, engine cvar descriptions and help files
void cmd_search(void);

// Second half of a search. It is queued behind cvarlist and cmdlist so that its output comes after theirs.
#define SEARCH_TEXT_COMMAND "mdd_search_text"
void cmd_search_text(void);

// Does the whole of `text` match `pattern`? * any run of characters, ? one character, [a-z] one of a set;
// case-insensitive, linear time.
qboolean glob_match(char const* pattern, char const* text);

#endif // CG_SEARCH_H
