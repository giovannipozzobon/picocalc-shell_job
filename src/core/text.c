/* text.c — `more`, `grep`, `head`, `tail` and `wc` (text.h). */

#include "text.h"

#include <stdlib.h>
#include <string.h>

#include "editor_pattern.h"
#include "files.h"
#include "vi.h"
#include "flavour.h"
#include "keycodes.h"
#include "term.h"

static FIL      s_file;
static reader_t s_rd;
static char     s_line[SHELL_TEXT_LINE];
static uint8_t  s_buf[SHELL_COPY_BUF];

/* ---- more ------------------------------------------------------------- */

typedef struct {
    unsigned row, col;      /* rows shown since the last --More--, and the column */
    unsigned page;          /* rows to show before the next one */
    size_t   n;             /* bytes held in s_buf */
    bool     quit;
} pager_t;

static void flush(shell_t *sh, pager_t *pg) {
    out_write(&sh->con, (const char *)s_buf, pg->n);
    pg->n = 0;
}

/* --More-- and a key: whether to go on. */
static bool pager_wait(shell_t *sh, pager_t *pg) {
    flush(sh, pg);
    out_puts(&sh->con, "\033[7m--More--\033[0m");
    int key;
    do key = sh->hooks.getkey(sh->hooks.ctx);
    while (key == KEY_WHEEL_UP);   /* the wheel cannot go back */
    out_puts(&sh->con, "\r\033[K");
    pg->row = 0;
    pg->page = TERM_ROWS - 1u;
    switch (key) {
    case 'q':
    case 'Q':
    case 0x1B:
    case 0x03:
    case KEY_BREAK:
    case -1:
        return false;
    case '\r':
    case KEY_DOWN:
        pg->page = 1;
        return true;
    case KEY_WHEEL_DOWN:
        pg->page = SHELL_WHEEL_LINES;
        return true;
    default:
        return true;
    }
}

/* One byte to the screen, counting the rows it fills as term.c does. */
static void page_byte(shell_t *sh, pager_t *pg, char c) {
    if (pg->row >= pg->page && !pager_wait(sh, pg)) {
        pg->quit = true;
        return;
    }
    if (pg->n == sizeof s_buf) flush(sh, pg);
    s_buf[pg->n++] = (uint8_t)c;
    unsigned char u = (unsigned char)c;
    if (u == '\n') {
        pg->row++;
        pg->col = 0;
    } else if (u == '\r') {
        pg->col = 0;
    } else if (u == '\b') {
        if (pg->col > 0) pg->col--;
    } else if (u == '\t') {
        pg->col = (pg->col / 8u + 1u) * 8u;
        if (pg->col > TERM_COLS - 1u) pg->col = TERM_COLS - 1u;
    } else if (u >= 0x20 && u != 0x7F) {
        if (++pg->col == TERM_COLS) {
            pg->row++;
            pg->col = 0;
        }
    }
}

/* All of `in` to the screen a page at a time, or to sh->out as it is. */
static int more_in(shell_t *sh, const input_t *in, const char *shown, pager_t *pg) {
    for (;;) {
        UINT n;
        FRESULT r = input_read(in, s_line, sizeof s_line, &n);
        if (r != FR_OK) {
            if (pg) flush(sh, pg);
            shell_error(sh, "more: %s: %s\n", shown, files_why(r));
            return 1;
        }
        if (n == 0) return 0;
        if (!pg) {
            out_write(&sh->out, s_line, n);
            if (shell_interrupted(sh)) return 1;
            continue;
        }
        for (UINT i = 0; i < n && !pg->quit; i++) page_byte(sh, pg, s_line[i]);
        if (pg->quit) return 0;
    }
}

int cmd_more(shell_t *sh, int argc, char **argv) {
    pager_t pager = { .page = TERM_ROWS - 1u };
    pager_t *pg = sh->out_is_con && sh->hooks.getkey ? &pager : NULL;
    if (argc == 1) {
        if (!input_any(&sh->in)) {
            shell_error(sh, "usage: more file... (or more < file, or ... | more)\n");
            return 1;
        }
        int status = more_in(sh, &sh->in, "input", pg);
        if (pg) flush(sh, pg);
        return status;
    }
    int status = 0;
    for (int i = 1; i < argc && !sh->stop && !(pg && pg->quit); i++) {
        if (files_open_read(sh, "more", argv[i], &s_file) != FR_OK) {
            status = 1;
            continue;
        }
        if (argc > 2) {
            static const char rule[] = "::::::::::::::\n";
            const char *parts[] = { rule, argv[i], "\n", rule };
            for (unsigned k = 0; k < 4; k++) {
                if (!pg) out_puts(&sh->out, parts[k]);
                else
                    for (const char *c = parts[k]; *c && !pg->quit; c++) page_byte(sh, pg, *c);
            }
        }
        status |= more_in(sh, &(input_t){ &s_file, NULL }, argv[i], pg);
        f_close(&s_file);
    }
    if (pg) flush(sh, pg);
    return status;
}

/* ---- grep ------------------------------------------------------------- */

enum { GREP_V = 1u << 0, GREP_N = 1u << 1, GREP_C = 1u << 2, GREP_I = 1u << 3 };

/* The lines of `in` that are wanted; how many, or -1 on an error. */
static long grep_in(shell_t *sh, const input_t *in, const char *name, const char *pat,
                    unsigned flags) {
    reader_init(&s_rd, in);
    size_t plen = strlen(pat);
    long found = 0;
    unsigned long lineno = 0;
    bool whole = true;
    long n;
    for (;;) {
        bool start = whole;   /* a line's first piece */
        n = reader_line(&s_rd, s_line, sizeof s_line, &whole);
        if (n < 0) break;
        if (start) lineno++;
        EditorPatternGroups g;
        bool complex_ = false;
        bool hit = editor_pattern_search(pat, plen, s_line, (size_t)n, 0, g, &complex_);
        if (complex_) {
            shell_error(sh, "grep: the pattern is too complex for line %lu\n", lineno);
            return -1;
        }
        if (hit == !(flags & GREP_V)) {
            found++;
            if (!(flags & GREP_C)) {
                if (name) out_printf(&sh->out, "%s:", name);
                if (flags & GREP_N) out_printf(&sh->out, "%lu:", lineno);
                out_write(&sh->out, s_line, (size_t)n);
                out_puts(&sh->out, "\n");
            }
        }
        if (shell_interrupted(sh)) return -1;
    }
    if (s_rd.r != FR_OK) {
        shell_error(sh, "grep: %s: %s\n", name ? name : "input", files_why(s_rd.r));
        return -1;
    }
    if (flags & GREP_C) {
        if (name) out_printf(&sh->out, "%s:", name);
        out_printf(&sh->out, "%ld\n", found);
    }
    return found;
}

int cmd_grep(shell_t *sh, int argc, char **argv) {
    static const char use[] = "grep [-v] [-n] [-c] [-i] pattern [file...]";
    unsigned flags;
    int first = files_options(sh, argc, argv, "vnci", &flags, use);
    if (first < 0) return 2;
    if (first == argc) {
        shell_error(sh, "usage: %s\n", use);
        return 2;
    }
    const char *pat = argv[first++];
    if (!editor_pattern_valid(pat, strlen(pat))) {
        shell_error(sh, "grep: %s: not a pattern\n", pat);
        return 2;
    }
    if (first == argc) {
        if (!input_any(&sh->in)) {
            shell_error(sh, "usage: %s (or < file, or ... | grep)\n", use);
            return 2;
        }
        long n = grep_in(sh, &sh->in, NULL, pat, flags);
        return n < 0 ? 2 : n > 0 ? 0 : 1;
    }
    bool names = argc - first > 1;
    bool any = false, failed = false;
    for (int i = first; i < argc && !sh->stop; i++) {
        if (files_open_read(sh, "grep", argv[i], &s_file) != FR_OK) {
            failed = true;
            continue;
        }
        long n = grep_in(sh, &(input_t){ &s_file, NULL }, names ? argv[i] : NULL, pat, flags);
        f_close(&s_file);
        if (n < 0) failed = true;
        if (n > 0) any = true;
    }
    return failed ? 2 : any ? 0 : 1;
}

/* ---- head, tail ------------------------------------------------------- */

/* "N" as a count of lines: false if it is not a number. */
static bool count(const char *s, unsigned long *n) {
    if (!*s) return false;
    unsigned long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10u + (unsigned long)(*s - '0');
        if (v > 999999999ul) v = 999999999ul;
    }
    *n = v;
    return true;
}

/* -n N, -nN or -N before the operands, as many as given, the last
 * winning, and "--" to end them: the index of the first operand, or -1
 * after the usage. */
static int lines_option(shell_t *sh, int argc, char **argv, const char *use, unsigned long *n) {
    *n = 10;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) return i + 1;
        bool ok;
        if (strcmp(a, "-n") == 0) {
            ok = i + 1 < argc && count(argv[i + 1], n);
            i++;
        } else if (a[1] == 'n') {
            ok = count(a + 2, n);
        } else {
            ok = count(a + 1, n);
        }
        if (!ok) {
            shell_error(sh, "usage: %s\n", use);
            return -1;
        }
    }
    return i;
}

/* The first `n` lines of `in`: 0, or 1 on an error. */
static int head_in(shell_t *sh, const input_t *in, const char *name, unsigned long n) {
    while (n > 0) {
        UINT got;
        FRESULT r = input_read(in, s_line, sizeof s_line, &got);
        if (r != FR_OK) {
            shell_error(sh, "head: %s: %s\n", name, files_why(r));
            return 1;
        }
        if (got == 0) break;
        UINT end = 0;
        while (end < got && n > 0)
            if (s_line[end++] == '\n') n--;
        out_write(&sh->out, s_line, end);
        if (shell_interrupted(sh)) return 1;
    }
    return 0;
}

/* tail keeps the end of its input in s_buf, as a ring: `total` bytes
 * have gone in, and the last sizeof s_buf of them are kept. */
static unsigned long s_total;

static void ring_put(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) s_buf[(s_total++) % sizeof s_buf] = (uint8_t)p[i];
}

/* The i-th byte of those kept, oldest first. */
static uint8_t ring_at(size_t i) {
    size_t oldest = s_total > sizeof s_buf ? s_total % sizeof s_buf : 0;
    return s_buf[(oldest + i) % sizeof s_buf];
}

static int tail_in(shell_t *sh, const input_t *in, const char *name, unsigned long n) {
    s_total = 0;
    for (;;) {
        UINT got;
        FRESULT r = input_read(in, s_line, sizeof s_line, &got);
        if (r != FR_OK) {
            shell_error(sh, "tail: %s: %s\n", name, files_why(r));
            return 1;
        }
        if (got == 0) break;
        ring_put(s_line, got);
        if (shell_interrupted(sh)) return 1;
    }
    size_t kept = s_total < sizeof s_buf ? (size_t)s_total : sizeof s_buf;
    if (n == 0 || kept == 0) return 0;
    /* Back from the end, past a last line break, to the n-th before it. */
    size_t end = kept;
    if (ring_at(end - 1) == '\n') end--;
    size_t start = end;
    unsigned long seen = 0;
    bool found = false;
    while (start > 0) {
        if (ring_at(start - 1) == '\n' && ++seen == n) {
            found = true;
            break;
        }
        start--;
    }
    if (!found && s_total > sizeof s_buf) {
        /* The lines asked for began before what was kept: from the first
         * whole line kept, if there is one. */
        size_t i = 0;
        while (i < end && ring_at(i) != '\n') i++;
        start = i < end ? i + 1 : 0;
        shell_error(sh, "tail: %s: only the last %u bytes are kept\n", name,
                    (unsigned)sizeof s_buf);
    }
    for (size_t i = start; i < kept;) {
        char chunk[64];
        size_t k = 0;
        while (i < kept && k < sizeof chunk) chunk[k++] = (char)ring_at(i++);
        out_write(&sh->out, chunk, k);
    }
    return 0;
}

typedef int (*lines_fn)(shell_t *sh, const input_t *in, const char *name, unsigned long n);

/* head and tail: the files named, each under a header if there are
 * several, or else the input. */
static int lines_cmd(shell_t *sh, int argc, char **argv, const char *use, lines_fn fn) {
    unsigned long n;
    int first = lines_option(sh, argc, argv, use, &n);
    if (first < 0) return 1;
    if (first == argc) {
        if (!input_any(&sh->in)) {
            shell_error(sh, "usage: %s (or < file, or ... | %s)\n", use, argv[0]);
            return 1;
        }
        return fn(sh, &sh->in, "input", n);
    }
    int status = 0;
    for (int i = first; i < argc && !sh->stop; i++) {
        if (files_open_read(sh, argv[0], argv[i], &s_file) != FR_OK) {
            status = 1;
            continue;
        }
        if (argc - first > 1) out_printf(&sh->out, "%s==> %s <==\n", i > first ? "\n" : "", argv[i]);
        status |= fn(sh, &(input_t){ &s_file, NULL }, argv[i], n);
        f_close(&s_file);
    }
    return status;
}

int cmd_head(shell_t *sh, int argc, char **argv) {
    return lines_cmd(sh, argc, argv, "head [-n N] [file...]", head_in);
}

int cmd_tail(shell_t *sh, int argc, char **argv) {
    return lines_cmd(sh, argc, argv, "tail [-n N] [file...]", tail_in);
}

/* ---- wc --------------------------------------------------------------- */

enum { WC_L = 1u << 0, WC_W = 1u << 1, WC_C = 1u << 2 };

typedef struct {
    unsigned long lines, words, bytes;
} wc_t;

static int wc_in(shell_t *sh, const input_t *in, const char *name, wc_t *c) {
    bool word = false;
    for (;;) {
        UINT got;
        FRESULT r = input_read(in, s_line, sizeof s_line, &got);
        if (r != FR_OK) {
            shell_error(sh, "wc: %s: %s\n", name, files_why(r));
            return 1;
        }
        if (got == 0) return 0;
        c->bytes += got;
        for (UINT i = 0; i < got; i++) {
            char ch = s_line[i];
            bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
                         ch == '\f';
            if (ch == '\n') c->lines++;
            if (!space && !word) c->words++;
            word = !space;
        }
        if (shell_interrupted(sh)) return 1;
    }
}

/* The counts asked for; padded unless there is only one and no name. */
static void wc_print(shell_t *sh, const wc_t *c, unsigned flags, const char *name) {
    const unsigned long v[3] = { c->lines, c->words, c->bytes };
    bool pad = name || (flags != WC_L && flags != WC_W && flags != WC_C);
    bool first = true;
    for (unsigned k = 0; k < 3; k++) {
        if (!(flags & (1u << k))) continue;
        out_printf(&sh->out, pad ? "%s%7lu" : "%s%lu", first ? "" : " ", v[k]);
        first = false;
    }
    if (name) out_printf(&sh->out, " %s", name);
    out_puts(&sh->out, "\n");
}

int cmd_wc(shell_t *sh, int argc, char **argv) {
    static const char use[] = "wc [-l] [-w] [-c] [file...]";
    unsigned flags;
    int first = files_options(sh, argc, argv, "lwc", &flags, use);
    if (first < 0) return 1;
    if (!flags) flags = WC_L | WC_W | WC_C;
    if (first == argc) {
        if (!input_any(&sh->in)) {
            shell_error(sh, "usage: %s (or < file, or ... | wc)\n", use);
            return 1;
        }
        wc_t c = { 0, 0, 0 };
        if (wc_in(sh, &sh->in, "input", &c)) return 1;
        wc_print(sh, &c, flags, NULL);
        return 0;
    }
    int status = 0;
    wc_t total = { 0, 0, 0 };
    for (int i = first; i < argc && !sh->stop; i++) {
        if (files_open_read(sh, "wc", argv[i], &s_file) != FR_OK) {
            status = 1;
            continue;
        }
        wc_t c = { 0, 0, 0 };
        int r = wc_in(sh, &(input_t){ &s_file, NULL }, argv[i], &c);
        f_close(&s_file);
        if (r) {
            status = 1;
            continue;
        }
        wc_print(sh, &c, flags, argv[i]);
        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
    }
    if (argc - first > 1) wc_print(sh, &total, flags, "total");
    return status;
}

#if !SHELL_DOS   /* uniq and sort are the UNIX flavour's */

/* ---- uniq ------------------------------------------------------------- */

enum { UNIQ_C = 1u << 0, UNIQ_D = 1u << 1 };

static void uniq_show(shell_t *sh, const char *line, unsigned long count, unsigned flags) {
    if ((flags & UNIQ_D) && count < 2) return;
    if (flags & UNIQ_C) out_printf(&sh->out, "%7lu ", count);
    out_puts(&sh->out, line);
    out_puts(&sh->out, "\n");
}

int cmd_uniq(shell_t *sh, int argc, char **argv) {
    static const char use[] = "uniq [-c] [-d] [file]";
    static char prev[SHELL_TEXT_LINE];
    unsigned flags;
    int first = files_options(sh, argc, argv, "cd", &flags, use);
    if (first < 0) return 1;
    bool file = argc > first;
    if (argc - first > 1 || (!file && !input_any(&sh->in))) {
        shell_error(sh, "usage: %s (or < file, or ... | uniq)\n", use);
        return 1;
    }
    if (file && files_open_read(sh, "uniq", argv[first], &s_file) != FR_OK) return 1;
    reader_init(&s_rd, file ? &(input_t){ &s_file, NULL } : &sh->in);
    unsigned long count = 0;
    bool whole;
    while (!shell_interrupted(sh) && reader_line(&s_rd, s_line, sizeof s_line, &whole) >= 0) {
        if (count && strcmp(prev, s_line) == 0) {
            count++;
            continue;
        }
        if (count) uniq_show(sh, prev, count, flags);
        strcpy(prev, s_line);
        count = 1;
    }
    if (count && !sh->stop) uniq_show(sh, prev, count, flags);
    if (file) f_close(&s_file);
    if (s_rd.r != FR_OK) {
        shell_error(sh, "uniq: %s\n", files_why(s_rd.r));
        return 1;
    }
    return 0;
}

/* ---- sort ------------------------------------------------------------- */

/* vi's buffer, idle while a command runs, so sort costs no RAM of its
 * own: the lines from its start, their offsets from its end. More is
 * refused, not spilled to the card. */
static char    *s_sort;
static size_t   s_sort_size;
static size_t   s_sort_used;
static unsigned s_sort_lines;
static unsigned s_sort_flags;
enum { SORT_R = 1u << 0, SORT_N = 1u << 1, SORT_U = 1u << 2 };

static uint16_t *sort_index(void) {
    return (uint16_t *)(void *)(s_sort + s_sort_size) - s_sort_lines;
}

static int sort_cmp(const void *a, const void *b) {
    const char *x = s_sort + *(const uint16_t *)a, *y = s_sort + *(const uint16_t *)b;
    int d = 0;
    if (s_sort_flags & SORT_N) {
        long nx = strtol(x, NULL, 10), ny = strtol(y, NULL, 10);
        d = nx < ny ? -1 : nx > ny;
    }
    if (!d) d = strcmp(x, y);
    return (s_sort_flags & SORT_R) ? -d : d;
}

/* An input's lines, added: false, with the error printed, if they do not
 * fit or cannot be read. */
static bool sort_in(shell_t *sh, const input_t *in, const char *name) {
    bool whole;
    long n;
    reader_init(&s_rd, in);
    while (!shell_interrupted(sh) && (n = reader_line(&s_rd, s_line, sizeof s_line, &whole)) >= 0) {
        size_t room = s_sort_size - (s_sort_lines + 1u) * sizeof(uint16_t);
        if (s_sort_used + (size_t)n + 1u > room) {
            shell_error(sh, "sort: more than %u bytes to sort\n", (unsigned)s_sort_size);
            return false;
        }
        memcpy(s_sort + s_sort_used, s_line, (size_t)n + 1u);
        s_sort_lines++;
        sort_index()[0] = (uint16_t)s_sort_used;
        s_sort_used += (size_t)n + 1u;
    }
    if (s_rd.r != FR_OK) {
        shell_error(sh, "sort: %s: %s\n", name, files_why(s_rd.r));
        return false;
    }
    return true;
}

int cmd_sort(shell_t *sh, int argc, char **argv) {
    static const char use[] = "sort [-r] [-n] [-u] [file...]";
    int first = files_options(sh, argc, argv, "rnu", &s_sort_flags, use);
    if (first < 0) return 1;
    if (first == argc && !input_any(&sh->in)) {
        shell_error(sh, "usage: %s (or < file, or ... | sort)\n", use);
        return 1;
    }
    s_sort = vi_scratch(&s_sort_size);
    s_sort_size &= ~(size_t)1u;   /* the index is 16-bit, aligned from the end */
    s_sort_used = 0;
    s_sort_lines = 0;
    bool ok = first == argc ? sort_in(sh, &sh->in, "input") : true;
    for (int i = first; i < argc && ok; i++) {
        if (files_open_read(sh, "sort", argv[i], &s_file) != FR_OK) return 1;
        ok = sort_in(sh, &(input_t){ &s_file, NULL }, argv[i]);
        f_close(&s_file);
    }
    if (!ok || sh->stop) return 1;
    uint16_t *index = sort_index();
    qsort(index, s_sort_lines, sizeof index[0], sort_cmp);
    const char *last = NULL;
    for (unsigned i = 0; i < s_sort_lines && !shell_interrupted(sh); i++) {
        const char *line = s_sort + index[i];
        if ((s_sort_flags & SORT_U) && last && strcmp(last, line) == 0) continue;
        out_puts(&sh->out, line);
        out_puts(&sh->out, "\n");
        last = line;
    }
    return 0;
}

#endif /* !SHELL_DOS */
