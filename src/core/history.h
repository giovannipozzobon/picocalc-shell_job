/* history.h — the lines typed at the prompt (design.md §4.3, §4.4).
 *
 * Kept packed in SHELL_HISTORY_BYTES, the oldest dropped to make room. A
 * blank line, and one the same as the line before it, are not kept.
 * The history is read from /.shell/history when the shell starts and
 * written back, through name.new, when the shell is left for a program,
 * the menu or USB (shell_leaving), and only if it changed: one write
 * per session, not one per command. Lines typed since the last such
 * write are lost if the power goes. */
#ifndef SHELL_HISTORY_H
#define SHELL_HISTORY_H

#include <stdbool.h>

#include "shell.h"

#define HISTORY_FILE "/.shell/history"
#define CWD_FILE     "/.shell/cwd"
#define ARGS_FILE    "/.shell/args"

void history_clear(void);
void history_add(const char *line);

/* How many lines there are; the i-th, 0 the oldest, or NULL. */
unsigned history_count(void);
const char *history_get(unsigned i);

/* Read HISTORY_FILE, if there is one, in place of what is kept. */
void history_load(void);

/* Write HISTORY_FILE if the history changed since it was read or
 * written: NULL, or why not. */
const char *history_save(void);

/* The current directory, for the shell's next start: written to CWD_FILE
 * when the shell restarts for a program, the menu or a reboot, and taken
 * (read and removed) by the next start, so a start after the power was
 * switched off at the prompt finds none (design.md §4.3). NULL or why
 * not; true if there was one. */
const char *history_keep_cwd(const char *cwd);
bool history_take_cwd(char out[SHELL_PATH_MAX + 1]);

/* What a program that declared it takes arguments is given, in ARGS_FILE,
 * for that program to read and remove (design.md §3.5): the format's
 * version, the flavour, the current directory, the program's path, the
 * name it declared and argv[1..argc-1], one a line. NULL or why not. */
const char *history_keep_args(const char *cwd, const char *program, const char *name, int argc,
                              char **argv);

/* Remove what a program did not take, as when the power was cut before
 * it started, or it failed first: at each start of the shell. */
void history_drop_args(void);

int cmd_history(shell_t *sh, int argc, char **argv);

#endif /* SHELL_HISTORY_H */
