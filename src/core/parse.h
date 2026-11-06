/* parse.h — one typed line into commands (design.md §4.4).
 *
 * Words with '…' and "…" quoting and \ escapes, `#` comments, the
 * separators `;`, `&&`, `||` and `|`, and the redirections `>`, `>>`,
 * `<`, `2>`, `2>>`, `2>&1` and `>&2` (with `1>` for `>`). Words are kept as typed, quotes and all: variables and globs are
 * expanded, and quotes removed, as each command runs (expand.h), so
 * `cd x; ls *` and `false; echo $?` see what the command before did.
 *
 * The whole line is parsed before anything runs, so a mistake at its end
 * stops all of it, as in sh. */
#ifndef SHELL_PARSE_H
#define SHELL_PARSE_H

#include <stdbool.h>

#include "config.h"

typedef enum {
    SEP_FIRST,      /* the line's first command */
    SEP_SEMI,       /* ;  */
    SEP_AND,        /* && */
    SEP_OR,         /* || */
    SEP_PIPE,       /* |: its input is the command before's output */
} sep_t;

typedef struct {
    sep_t  sep;                 /* what joins it to the command before */
    int    argc;
    char **argv;                /* into parse_t.words, as typed */
    char  *in;                  /* < file, as typed, or NULL */
    char  *out;                 /* > or >> file, as typed, or NULL */
    bool   append;              /* >> */
    bool   out_to_err;          /* >&2 */
    char  *err;                 /* 2> or 2>> file, as typed, or NULL */
    bool   err_append;          /* 2>> */
    bool   err_to_out;          /* 2>&1: to the output... */
    bool   err_to_file;         /* ...as `> file` before it left it */
    unsigned start, end;        /* where it is in the line: its first character, and past its last */
} parse_cmd_t;

typedef struct {
    int         ncmds;
    parse_cmd_t cmds[SHELL_CMDS_MAX];
    char       *words[SHELL_WORDS_MAX + SHELL_CMDS_MAX];   /* argv arrays, NULL-ended */
    char        text[SHELL_LINE_MAX + 1];                  /* the words, NUL-ended */
    const char *error;          /* what was wrong, when parse_line fails */
} parse_t;

/* Parse `line`, of at most SHELL_LINE_MAX characters. True with
 * p->ncmds commands, possibly none for a blank line or a comment; false
 * with p->error set. The line is not changed. */
bool parse_line(parse_t *p, const char *line);

#endif /* SHELL_PARSE_H */
