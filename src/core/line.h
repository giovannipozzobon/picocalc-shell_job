/* line.h — the line typed at the prompt (design.md §4.4).
 *
 * Keys:
 *   Left, Right, Home, End, and Ctrl-B, F, A, E   move
 *   Backspace, Del                                delete
 *   Ctrl-U, Ctrl-K                                delete to the start, to the end
 *   Up, Down, and Ctrl-P, N                       the history (history.h)
 *   Tab                                           complete (below)
 *   Ctrl-C                                        drop the line
 *   Enter                                         run it
 *   F3, Esc (the DOS flavour)                     the rest of the last line from
 *                                                 the cursor's column; drop the line
 *
 * Tab completes the word before the cursor: a command name (a builtin or
 * a program in $PATH) as the first word of a command, and a path
 * anywhere else. One match is put in whole, followed by '/' for a
 * directory and a space for anything else; several put in what they
 * share, and a second Tab with nothing to add lists them. Names are
 * matched without regard to case and put in as the card spells them,
 * with a '\' before any character the shell would take as special. In
 * the DOS flavour a directory gets a '\' after it, and nothing is put
 * before a special character: DOS has no escapes.
 *
 * The line may run over several rows. The terminal wraps as soon as a
 * row is full (term.c), so the cursor's place is worked out from the
 * prompt's width, and moved there with relative escape sequences. */
#ifndef SHELL_LINE_H
#define SHELL_LINE_H

#include <stdbool.h>

#include "shell.h"

/* A new, empty line after a prompt `prompt` columns wide. */
void line_start(shell_t *sh, size_t prompt);

/* One key. True on Enter, with the line in sh->line.text and the cursor
 * at the start of the next row. */
bool line_key(shell_t *sh, int key);

/* A line typed after `prompt`, for a command that asks, such as select
 * or DOS's DATE, into sh->line.text: 1; 0 at the end of the input; -1
 * when stopped by Break, Esc or Ctrl-C, with sh->stop set. The caller
 * makes sure there is a keyboard (sh->hooks.getkey). */
int line_read(shell_t *sh, const char *prompt);

/* A line of text typed for DOS's COPY CON, into sh->line.text: 1; 0 at
 * Ctrl+Z or F6, or the end of the input, with what was typed before it
 * still in sh->line.text; -1 when stopped by Break or Ctrl-C, with
 * sh->stop set. Esc only drops the line, as at the prompt. The caller
 * makes sure there is a keyboard (sh->hooks.getkey). */
int line_read_text(shell_t *sh);

#endif /* SHELL_LINE_H */
