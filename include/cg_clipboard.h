#ifndef CG_CLIPBOARD_H
#define CG_CLIPBOARD_H

// clipboard [<lines> | <cvar> | <file.cfg> | <command ...>]: console text, a cvar's value or a command's output
// to the system clipboard
void cmd_clipboard(void);

// The later steps of a clipboard request, queued behind the commands whose output is wanted.
#define CLIPBOARD_GRAB_COMMAND "mdd_clipboard_grab"
void cmd_clipboard_grab(void);

#endif // CG_CLIPBOARD_H
