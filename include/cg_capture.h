#ifndef CG_CAPTURE_H
#define CG_CAPTURE_H

#include "q_shared.h"

/*
  Reads text back out of the console: all of it, or what one command printed (see cg_capture.c).

  The result arrives later, when the queued steps have run: done(NULL, lines, num) with the text as whole
  lines - rows that the console had wrapped are put back together - or done(error, NULL, 0). The lines are only
  valid during the call. One request at a time; a new one cancels the one in flight.
*/
typedef void (*capture_done_t)(char const* error, char** lines, size_t num);

// Everything in the console. `self` is the command the user typed to get here; its echo is left out.
void capture_console(char const* self, capture_done_t done);

// What `command` prints (one console line, no line break in it).
void capture_command(char const* command, capture_done_t done);

// The queued steps.
#define CAPTURE_STEP_COMMAND "mdd_capture_step"
void cmd_capture_step(void);

#endif // CG_CAPTURE_H
