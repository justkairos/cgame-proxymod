#ifndef CG_SEARCH_H
#define CG_SEARCH_H

// search / mdd_search <pattern> ...: cvar names, command names, engine cvar descriptions and help files
void cmd_search(void);

// Second half of a search. It is queued behind cvarlist and cmdlist so that its output comes after theirs.
#define SEARCH_TEXT_COMMAND "mdd_search_text"
void cmd_search_text(void);

#endif // CG_SEARCH_H
