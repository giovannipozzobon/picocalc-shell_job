/* text.h — `more`, `grep`, `head`, `tail` and `wc` (design.md §4.5).
 *
 * Each reads the files named, or else its input (`<` or a pipe).
 *
 * `more` shows a screenful, TERM_ROWS - 1 rows, then waits at
 * --More--: Space for the next screenful, Enter or Down for one more
 * row, q, Esc, Brk or Ctrl-C to stop. A long line counts as the rows it
 * wraps to. When its output is not the terminal it copies, as cat does.
 *
 * `grep [-v] [-n] [-c] [-i] pattern [file...]` prints the lines that
 * match. The pattern is vi's (editor_pattern.h: ^ $ . * [...] \< \>
 * \( \) \1-\9), so it matches without regard to case, as `/` in vi
 * does; -i is accepted and changes nothing. -v the lines that do not
 * match, -n with their numbers, -c only how many. With more than one
 * file each line starts with its file's name. A line longer than
 * SHELL_TEXT_LINE - 1 is matched in pieces of that length. The status
 * is 0 if a line was found, 1 if none, 2 on an error.
 *
 * `head [-n N] [file...]` and `tail [-n N] [file...]` show the first or
 * last N lines, 10 by default; -N is the same as -n N. With several
 * files each starts with "==> name <==". tail keeps the last
 * SHELL_COPY_BUF bytes of its input, more's buffer, so it takes no
 * RAM of its own: if the lines asked for are longer than that, it
 * shows the whole lines that were kept, and says so.
 *
 * `wc [-l] [-w] [-c] [file...]` counts lines (line breaks), words and
 * bytes, all three if none is asked for, with a total for several
 * files. A single count of the input is printed alone, unpadded. */
#ifndef SHELL_TEXT_H
#define SHELL_TEXT_H

#include "shell.h"

int cmd_more(shell_t *sh, int argc, char **argv);
int cmd_grep(shell_t *sh, int argc, char **argv);
int cmd_head(shell_t *sh, int argc, char **argv);
int cmd_tail(shell_t *sh, int argc, char **argv);
int cmd_wc(shell_t *sh, int argc, char **argv);
int cmd_uniq(shell_t *sh, int argc, char **argv);
int cmd_sort(shell_t *sh, int argc, char **argv);

#endif /* SHELL_TEXT_H */
