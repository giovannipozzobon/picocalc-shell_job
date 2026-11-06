/* input.h — what a command reads: a file, `<` or a pipe (design.md §4.4).
 *
 * The keyboard is never a command's input, but for DOS's COPY CON: `cat` with no file and no
 * `<` or pipe says how to use it. */
#ifndef SHELL_INPUT_H
#define SHELL_INPUT_H

#include <stdbool.h>
#include <stddef.h>

#include "config.h"
#include "ff.h"

struct pipe;

typedef struct {
    FIL         *file;   /* an open file, or NULL */
    struct pipe *pipe;   /* a pipe's reading end, or NULL */
} input_t;

static inline bool input_any(const input_t *in) {
    return in->file || in->pipe;
}

/* Up to n bytes into buf, *got of them; *got 0 at the end. */
FRESULT input_read(const input_t *in, void *buf, UINT n, UINT *got);

/* Lines from an input, as grep, more and scripts take them. */
typedef struct {
    input_t in;
    FRESULT r;                      /* the first failure, or FR_OK */
    UINT    pos, len;
    char    buf[SHELL_TEXT_LINE];
} reader_t;

void reader_init(reader_t *rd, const input_t *in);

/* The next line into line[0..size), NUL-ended, without its LF or a CR
 * before it; its length, or -1 at the end or on a failure (rd->r). A
 * line longer than size - 1 comes as pieces of that length; *whole is
 * false for each piece but the last. */
long reader_line(reader_t *rd, char *line, size_t size, bool *whole);

#endif /* SHELL_INPUT_H */
