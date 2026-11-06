/* expand.h — a command's words as it runs: variables, quotes, globs
 * (design.md §4.4).
 *
 * The parser keeps each word as typed (parse.h). Here, as the command
 * runs:
 *   - `$NAME`, `${NAME}` and `$?` are replaced, outside single quotes. A
 *     `$` before anything else is itself. An unset variable is empty.
 *   - `~` or `~/` starting a word, unquoted, is $HOME, and $PWD is always
 *     the current directory.
 *   - quotes and escapes are removed.
 *   - a word with `*`, `?` or `[` outside quotes, in its last name, is a
 *     glob: it becomes the names in that directory that match, sorted as
 *     ls sorts them, or stays as it is if none does. Hidden names
 *     (design.md §4.2) match only a pattern that starts with '.'.
 *   - a word left empty that had no quotes is dropped, as in sh.
 * A variable's value is never split into words or globbed. */
#ifndef SHELL_EXPAND_H
#define SHELL_EXPAND_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "shell.h"

typedef struct {
    int   argc;
    char *argv[SHELL_ARGS_MAX + 1];   /* NULL-ended */
    char  text[SHELL_ARGS_TEXT];
    size_t used;
} expand_t;

/* The words `raw` (NULL-ended) as argv, globs included. False, with the
 * error printed, if they do not fit. */
bool expand_args(shell_t *sh, char *const *raw, expand_t *e);

/* One word as a single string, with no glob: a redirection's file or an
 * assignment's value. It is added to e->text; NULL, with the error
 * printed, if it does not fit. */
char *expand_one(shell_t *sh, const char *raw, size_t len, expand_t *e);

/* Whether a word as typed is NAME=value, with NAME unquoted; *eq is
 * set to the '='. */
bool expand_is_assignment(const char *raw, const char **eq);

#endif /* SHELL_EXPAND_H */
