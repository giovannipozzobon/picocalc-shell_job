/* dos_batch.c — batch files and AUTOEXEC.BAT (dos.h; design.md §4.7):
 * running a line, with %0-%9 and %NAME%, REM, IF and FOR, and the
 * commands that steer a batch file, GOTO, SHIFT and CALL, and CHOICE.
 *
 * A batch file runs a line at a time at the level below the command
 * that ran it, as `sh` does in the UNIX flavour (unix.c), each level
 * with its own file, parameters and FOR. IF, FOR and REM are taken from
 * the start of a line before it is parsed, since what follows them is a
 * line of its own; elsewhere they are a syntax error. */

#include <string.h>

#include "dos.h"
#include "files.h"
#include "flavour.h"
#include "keycodes.h"
#include "path.h"
#include "vars.h"

#define AUTOEXEC "/AUTOEXEC.BAT"

/* Each level's batch file, if it runs one, its line, and its FOR. */
typedef struct {
    bool        open;           /* a batch file runs at this level */
    bool        done;           /* it is to stop: GOTO found no label */
    bool        jumped;         /* GOTO, or another batch file in its place */
    FIL         fil;
    reader_t    reader;
    char        text[SHELL_LINE_MAX + 2];   /* the line read */
    char        args[SHELL_LINE_MAX + 1];   /* %0 and the parameters, each NUL-ended */
    const char *param[SHELL_WORDS_MAX];
    unsigned    nparams, shift;
    char        line[SHELL_LINE_MAX + 1];   /* the line, expanded */
    bool        in_for;
    char        item[SHELL_LINE_MAX + 1];   /* FOR's word of the set */
    char        pat[SHELL_PATH_MAX + 1];    /* ...and its wildcard, */
    DIR         dir;                        /* in this directory */
    FILINFO     ent;
    char        for_line[SHELL_LINE_MAX + 1];   /* FOR's command for one of its set */
} level_t;

static level_t s_levels[SHELL_DEPTH];

/* Buffers a function is done with before anything it runs could use
 * them again, kept off the stack, which batch files nested through CALL
 * would otherwise fill (design.md §4.7). */
static char    s_found[SHELL_PATH_MAX + 1];   /* dos_batch_find's */
static char    s_path[SHELL_PATH_MAX + 1];
static char    s_name[SHELL_PATH_MAX + 1];
static char    s_word[2][SHELL_LINE_MAX + 1];
static char    s_prompt[SHELL_PROMPT_MAX + 1];
static DIR     s_dir;
static FILINFO s_fno;

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool blank(char c) {
    return c == ' ' || c == '\t';
}

static char *skip_blanks(char *s) {
    while (blank(*s)) s++;
    return s;
}

/* Whether *sp starts with the word `kw`, in either case, followed by a
 * blank, '(' or the end; if so *sp moves past it and the blanks after. */
static bool keyword(char **sp, const char *kw) {
    char *s = *sp;
    size_t n = strlen(kw);
    for (size_t i = 0; i < n; i++)
        if (upper(s[i]) != kw[i]) return false;
    if (s[n] && !blank(s[n]) && s[n] != '(') return false;
    *sp = skip_blanks(s + n);
    return true;
}

/* A word from *sp into buf, up to a blank outside "...", the quotes
 * kept, or with `eq`, up to "==" as well. *sp moves past it. */
static void take_word(char **sp, char *buf, size_t size, bool eq) {
    char *s = *sp;
    size_t n = 0;
    bool quoted = false;
    while (*s && (quoted || !(blank(*s) || (eq && s[0] == '=' && s[1] == '=')))) {
        if (*s == '"') quoted = !quoted;
        if (n + 1 < size) buf[n++] = *s;
        s++;
    }
    buf[n] = '\0';
    *sp = s;
}

static int syntax(shell_t *sh) {
    shell_error(sh, "Syntax error\n");
    return sh->status = 2;
}

/* The line as a batch file shows it with ECHO ON: a blank line, the
 * prompt, the line. */
static void show_line(shell_t *sh, const char *text) {
    flavour_prompt(sh, s_prompt, sizeof s_prompt);
    out_puts(&sh->con, sh->at_bol ? "\n" : "\n\n");
    out_puts(&sh->con, s_prompt);
    out_puts(&sh->con, text);
    out_puts(&sh->con, "\n");
}


/* ---- IF ---------------------------------------------------------------- */

/* Whether a file or directory is there; with a wildcard, whether any
 * file it matches is. */
static bool exists(shell_t *sh, const char *name) {
    char *path = s_path, *pat = s_name;
    if (!dos_resolve(path, sh->cwd, name)) return false;
    if (!dos_wild(path)) return strcmp(path, "/") == 0 || f_stat(path, &s_fno) == FR_OK;
    char *slash = strrchr(path, '/');
    strcpy(pat, slash + 1);
    if (slash == path) slash[1] = '\0';
    else *slash = '\0';
    bool found = false;
    if (f_opendir(&s_dir, path) != FR_OK) return false;
    while (!found && f_readdir(&s_dir, &s_fno) == FR_OK && s_fno.fname[0])
        found = !(s_fno.fattrib & AM_DIR) && !files_hidden(&s_fno) &&
                (dos_match(pat, s_fno.fname) ||
                 (s_fno.altname[0] && dos_match(pat, s_fno.altname)));
    f_closedir(&s_dir);
    return found;
}

static int bad_if(shell_t *sh) {
    syntax(sh);
    return -1;
}

/* IF [NOT] EXIST name, ERRORLEVEL n or a==b, then a command: 1 if it
 * is to run, with *sp at it; 0 if not; -1 after a syntax error. */
static int if_test(shell_t *sh, char **sp) {
    char *s = *sp;
    char *a = s_word[0], *b = s_word[1];
    bool not = keyword(&s, "NOT");
    bool truth;
    if (keyword(&s, "EXIST")) {
        take_word(&s, a, sizeof s_word[0], false);
        if (!*a) return bad_if(sh);
        truth = exists(sh, a);
    } else if (keyword(&s, "ERRORLEVEL")) {
        take_word(&s, a, sizeof s_word[0], false);
        long n = 0;
        if (!*a) return bad_if(sh);
        for (const char *d = a; *d; d++) {
            if (*d < '0' || *d > '9') return bad_if(sh);
            if (n < 100000) n = n * 10 + (*d - '0');
        }
        truth = sh->status >= n;
    } else {
        take_word(&s, a, sizeof s_word[0], true);
        s = skip_blanks(s);
        if (!*a || s[0] != '=' || s[1] != '=') return bad_if(sh);
        s = skip_blanks(s + 2);
        take_word(&s, b, sizeof s_word[1], false);
        if (!*b) return bad_if(sh);
        truth = strcmp(a, b) == 0;
    }
    s = skip_blanks(s);
    if (!*s) return bad_if(sh);
    *sp = s;
    return truth != not;
}

static int control(shell_t *sh, char *s);

/* ---- FOR --------------------------------------------------------------- */

/* FOR's command for one item: each %x replaced by it, then run. False
 * to stop: the line did not fit, or it ended the loop. */
static bool for_one(shell_t *sh, level_t *lv, char var, const char *cmd, const char *item,
                    bool quiet) {
    size_t n = 0, len = strlen(item);
    for (const char *c = cmd; *c; c++) {
        const char *piece = c;
        size_t k = 1;
        if (c[0] == '%' && c[1] == var) {
            piece = item;
            k = len;
            c++;
        }
        if (n + k > SHELL_LINE_MAX) {
            shell_error(sh, "Line too long\n");
            sh->status = 2;
            return false;
        }
        memcpy(lv->for_line + n, piece, k);
        n += k;
    }
    lv->for_line[n] = '\0';
    if (dos_echo_on && !quiet) show_line(sh, lv->for_line);
    control(sh, lv->for_line);
    return !shell_interrupted(sh) && !lv->jumped && !lv->done;
}

/* Each file a wildcard matches, as typed before its name: SUB\*.TXT
 * gives SUB\A.TXT. */
static bool for_wild(shell_t *sh, level_t *lv, char var, const char *cmd, const char *item,
                     bool quiet) {
    char *dir = s_path, *pat = lv->pat, *name = s_name;
    if (!dos_resolve(dir, sh->cwd, item)) return true;
    char *slash = strrchr(dir, '/');
    strcpy(pat, slash + 1);
    if (slash == dir) slash[1] = '\0';
    else *slash = '\0';
    size_t prefix = 0;
    for (size_t i = 0; item[i]; i++)
        if (item[i] == '\\' || item[i] == ':') prefix = i + 1;
    if (f_opendir(&lv->dir, dir) != FR_OK) return true;
    bool go = true;
    while (go && f_readdir(&lv->dir, &lv->ent) == FR_OK && lv->ent.fname[0]) {
        const FILINFO *e = &lv->ent;
        if ((e->fattrib & AM_DIR) || files_hidden(e)) continue;
        if (!dos_match(pat, e->fname) && !(e->altname[0] && dos_match(pat, e->altname))) continue;
        if (prefix + strlen(e->fname) >= sizeof s_name) continue;
        memcpy(name, item, prefix);
        strcpy(name + prefix, e->fname);
        go = for_one(sh, lv, var, cmd, name, quiet);
    }
    f_closedir(&lv->dir);
    return go;
}

static bool set_delim(char c) {
    return blank(c) || c == ',' || c == ';' || c == '=';
}

/* FOR %x IN (set) DO command. A wildcard in the set is each file it
 * matches. */
static int for_line(shell_t *sh, char *s, bool quiet) {
    level_t *lv = &s_levels[sh->depth];
    if (lv->in_for) {
        shell_error(sh, "FOR cannot be nested\n");
        return sh->status = 2;
    }
    if (s[0] != '%' || !s[1] || blank(s[1]) || !blank(s[2])) return syntax(sh);
    char var = s[1];
    s = skip_blanks(s + 2);
    if (!keyword(&s, "IN") || *s != '(') return syntax(sh);
    char *set = s + 1;
    char *close = strchr(set, ')');
    if (!close) return syntax(sh);
    s = skip_blanks(close + 1);
    if (!keyword(&s, "DO") || !*s) return syntax(sh);
    const char *cmd = s;
    *close = '\0';

    lv->in_for = true;
    lv->jumped = false;
    char *item = lv->item;
    for (char *e = set; !sh->stop;) {
        while (set_delim(*e)) e++;
        if (!*e) break;
        size_t n = 0;
        while (*e && !set_delim(*e)) item[n++] = *e++;
        item[n] = '\0';
        bool go = dos_wild(item) ? for_wild(sh, lv, var, cmd, item, quiet)
                                 : for_one(sh, lv, var, cmd, item, quiet);
        if (!go) break;
    }
    lv->in_for = false;
    return sh->status;
}

/* ---- a line ------------------------------------------------------------ */

/* A line, expanded: REM, IF and FOR, or else the parser's. IF after IF
 * is taken in turn, not by recursion, to spare the stack. */
static int control(shell_t *sh, char *s) {
    s = skip_blanks(s);
    bool quiet = *s == '@';
    if (quiet) s = skip_blanks(s + 1);
    while (keyword(&s, "IF")) {
        int run = if_test(sh, &s);
        if (run <= 0) return sh->status;
    }
    if (keyword(&s, "REM")) return sh->status;
    if (keyword(&s, "FOR")) return for_line(sh, s, quiet);
    return dos_commands(sh, s);
}

int dos_line(shell_t *sh, const char *line, bool echo) {
    level_t *lv = &s_levels[sh->depth];
    const char *params[10] = { 0 };
    if (lv->open)
        for (unsigned i = 0; i < 10 && lv->shift + i < lv->nparams; i++)
            params[i] = lv->param[lv->shift + i];
    if (!dos_expand(lv->line, sizeof lv->line, line, lv->open ? params : NULL)) {
        shell_error(sh, "Line too long\n");
        return sh->status = 2;
    }
    if (echo) show_line(sh, lv->line);
    return control(sh, lv->line);
}

/* IF, FOR anywhere but at the start of a line: after a '|'. */
int dos_control(shell_t *sh, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return syntax(sh);
}

int dos_rem(shell_t *sh, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return sh->status;
}

/* ---- batch files ------------------------------------------------------- */

/* `file` in `dir` as typed, into s_found if it is a file there. */
static bool batch_in(shell_t *sh, const char *dir, const char *file) {
    return dos_resolve(s_path, sh->cwd, dir) && path_join(s_found, s_path, file) &&
           f_stat(s_found, &s_fno) == FR_OK && !(s_fno.fattrib & AM_DIR);
}

/* Whether the search ends in `dir`: DOS looked in one directory at a
 * time, a program before a batch file, so *found is the batch file, or
 * NULL for a program, which run.c then finds in the same place. `prog`
 * is NULL when the name was typed with .BAT. */
static bool ends_in(shell_t *sh, const char *dir, const char *prog, const char *file,
                    const char **found) {
    *found = NULL;
    if (prog && batch_in(sh, dir, prog)) return true;
    if (!batch_in(sh, dir, file)) return false;
    *found = s_found;
    return true;
}

const char *dos_batch_find(shell_t *sh, const char *name) {
    static char prog[SHELL_PATH_MAX + 1];
    char *file = s_name;
    const char *base = name;
    for (const char *s = name; *s; s++)
        if (*s == '\\' || *s == ':') base = s + 1;
    const char *dot = strrchr(base, '.');
    if (dot && !path_same(dot, ".BAT")) return NULL;
    size_t n = strlen(name);
    if (n + (dot ? 1 : 5) > sizeof s_name) return NULL;
    memcpy(file, name, n + 1);
    if (!dot) {
        memcpy(file + n, ".BAT", 5);
        memcpy(prog, name, n);
        memcpy(prog + n, ".UF2", 5);
    }

    if (flavour_is_path(name)) {
        if (!dot && dos_resolve(s_found, sh->cwd, prog) && f_stat(s_found, &s_fno) == FR_OK &&
            !(s_fno.fattrib & AM_DIR))
            return NULL;
        if (!dos_resolve(s_found, sh->cwd, file) || f_stat(s_found, &s_fno) != FR_OK ||
            (s_fno.fattrib & AM_DIR))
            return NULL;
        return s_found;
    }
    const char *found;
    const char *uf2 = dot ? NULL : prog;
    if (ends_in(sh, ".", uf2, file, &found)) return found;
    char *dir = s_word[0];
    for (const char *p = vars_get("PATH", 4); p;) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len > 0 && len <= SHELL_PATH_MAX) {
            memcpy(dir, p, len);
            dir[len] = '\0';
            if (ends_in(sh, dir, uf2, file, &found)) return found;
        }
        p = semi ? semi + 1 : NULL;
    }
    return NULL;
}

static bool add_param(level_t *lv, size_t *n, const char *s, size_t len) {
    if (*n + len + 1 > sizeof lv->args || lv->nparams == SHELL_WORDS_MAX) return false;
    memcpy(lv->args + *n, s, len);
    lv->args[*n + len] = '\0';
    lv->param[lv->nparams++] = lv->args + *n;
    *n += len + 1;
    return true;
}

/* %0, the name as typed, and the parameters, quotes kept, into the level. */
static void set_params(level_t *lv, const char *name, const char *tail) {
    size_t n = 0;
    lv->nparams = 0;
    lv->shift = 0;
    add_param(lv, &n, name, strlen(name));
    for (const char *s = tail;;) {
        while (set_delim(*s)) s++;
        if (!*s) break;
        const char *start = s;
        bool quoted = false;
        while (*s && (quoted || !set_delim(*s))) {
            if (*s == '"') quoted = !quoted;
            s++;
        }
        if (!add_param(lv, &n, start, (size_t)(s - start))) break;
    }
}

/* Open the batch file at `path` at level `d`. */
static bool open_batch(shell_t *sh, unsigned d, const char *path, const char *name,
                       const char *tail, bool startup) {
    level_t *lv = &s_levels[d];
    FRESULT r = startup ? files_open_startup(&lv->fil, path)
                        : files_open_path(sh, name, path, &lv->fil);
    if (r != FR_OK) {
        if (startup && r != FR_NO_FILE && r != FR_NO_PATH && r != FR_NOT_READY)
            flavour_file_error(sh, name, path, files_why(r));
        return false;
    }
    reader_init(&lv->reader, &(input_t){ &lv->fil, NULL });
    set_params(lv, name, tail);
    lv->open = true;
    lv->done = false;
    lv->jumped = true;
    return true;
}

/* Level d's batch file, a line at a time, to its end, a stop or a GOTO
 * with no label; then closed. */
static int run_level(shell_t *sh, unsigned d) {
    level_t *lv = &s_levels[d];
    unsigned up = sh->depth;
    bool echo = dos_echo_on;
    sh->depth = d;
    long n;
    bool whole;
    /* Break, Esc or Ctrl-C stops it, even in a loop of commands that
     * never look. */
    while (!shell_interrupted(sh) && !lv->done &&
           (n = reader_line(&lv->reader, lv->text, sizeof lv->text, &whole)) >= 0) {
        bool piece = !whole;
        while (!whole && reader_line(&lv->reader, lv->text, sizeof lv->text, &whole) >= 0) {}
        if (piece || (size_t)n > SHELL_LINE_MAX) {
            shell_error(sh, "Line too long\n");
            sh->status = 2;
            continue;
        }
        lv->jumped = false;
        char *s = skip_blanks(lv->text);
        if (*s == ':') continue;   /* a label */
        bool quiet = *s == '@';
        if (quiet) s++;
        if (!*skip_blanks(s)) continue;
        dos_line(sh, s, dos_echo_on && !quiet);
    }
    if (lv->reader.r != FR_OK) {
        dos_error(sh, dos_why(lv->reader.r), NULL);
        sh->status = 1;
    }
    f_close(&lv->fil);
    lv->open = false;
    sh->depth = up;
    /* Back at the prompt, ECHO is as it was there. */
    if (up == 0) dos_echo_on = echo;
    return sh->status;
}

int dos_batch_run(shell_t *sh, const char *path, const char *name, const char *tail, bool call) {
    unsigned d = sh->depth;
    level_t *lv = &s_levels[d];
    if (lv->open && !call) {
        /* In place of the one running: its loop goes on with the new
         * file's first line, or if it cannot be opened, ends. */
        f_close(&lv->fil);
        if (!open_batch(sh, d, path, name, tail, false)) {
            lv->done = true;
            return 1;
        }
        return sh->status;
    }
    if (d + 1 >= SHELL_DEPTH) {
        shell_error(sh, "Batch files nested more than %u deep\n", SHELL_DEPTH - 1u);
        return 1;
    }
    if (!open_batch(sh, d + 1, path, name, tail, false)) return 1;
    return run_level(sh, d + 1);
}

void dos_autoexec(shell_t *sh) {
    if (open_batch(sh, 1, AUTOEXEC, "AUTOEXEC", "", true)) run_level(sh, 1);
}

/* ---- CALL, GOTO, SHIFT ------------------------------------------------- */

/* CALL name [parameters]: a batch file at the level below, back here
 * after; anything else as if typed. */
int dos_call(shell_t *sh, int argc, char **argv) {
    if (argc < 2) return 0;
    /* What followed the name, as typed. */
    const char *t = dos_tail;
    while (blank(*t)) t++;
    bool quoted = false;
    while (*t && (quoted || !blank(*t))) {
        if (*t == '"') quoted = !quoted;
        t++;
    }
    const char *bat = dos_batch_find(sh, argv[1]);
    if (bat) return dos_batch_run(sh, bat, argv[1], t, true);
    dos_tail = t;
    return shell_command(sh, argc - 1, argv + 1, NULL, NULL, false, NULL, NULL);
}

/* A label's name, `s` after its ':', against `want`, in either case, to
 * a blank or the end. */
static bool same_label(const char *s, const char *want) {
    for (; *want; s++, want++)
        if (upper(*s) != upper(*want)) return false;
    return !*s || blank(*s);
}

/* GOTO label, or GOTO :label: on from the line after it. At the prompt
 * it does nothing, as in DOS. */
int dos_goto(shell_t *sh, int argc, char **argv) {
    level_t *lv = &s_levels[sh->depth];
    if (!lv->open) return 0;
    const char *label = argc > 1 ? argv[1] : "";
    if (*label == ':') label++;
    lv->jumped = true;
    if (*label && f_lseek(&lv->fil, 0) == FR_OK) {
        reader_init(&lv->reader, &(input_t){ &lv->fil, NULL });
        bool whole;
        while (reader_line(&lv->reader, lv->text, sizeof lv->text, &whole) >= 0) {
            char *s = skip_blanks(lv->text);
            if (*s == ':' && same_label(s + 1, label)) return 0;
        }
    }
    dos_error(sh, "Label not found", NULL);
    lv->done = true;
    return 1;
}

/* SHIFT: %1 becomes %0, %2 %1, and so on, with the parameters past %9
 * coming in. */
int dos_shift(shell_t *sh, int argc, char **argv) {
    (void)argc;
    (void)argv;
    level_t *lv = &s_levels[sh->depth];
    if (lv->open && lv->shift < lv->nparams) lv->shift++;
    return 0;
}

/* ---- CHOICE ------------------------------------------------------------ */

static bool key_is(char a, char b, bool exact) {
    return exact ? a == b : upper(a) == upper(b);
}

/* CHOICE [/C[:]keys] [/N] [/S] [/T[:]c,nn] [text]: one of the keys; the
 * status is its place among them, 1 for the first, and 255 for a
 * mistake, as DOS 6's (design.md §4.7). */
int dos_choice(shell_t *sh, int argc, char **argv) {
    const char *keys = "YN";
    bool list = true, exact = false, timed = false;
    char dflt = 0;
    unsigned secs = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '/'; i++) {
        const char *a = argv[i], *v = a + 2;
        char sw = upper(a[1]);
        if ((sw == 'C' || sw == 'T') && *v == ':') v++;
        if (sw == 'C') {
            if (!*v) {
                dos_error(sh, "CHOICE: invalid choice switch syntax. Expected form: /C[:]choices", NULL);
                return 255;
            }
            keys = v;
        } else if (sw == 'T') {
            /* c,nn: a key, a comma, and one or two digits. */
            if (!v[0] || v[1] != ',' || v[2] < '0' || v[2] > '9' ||
                (v[3] && (v[3] < '0' || v[3] > '9' || v[4]))) {
                dos_error(sh, "CHOICE: Incorrect timeout syntax. Expected form Tc,nn or T:c,nn", NULL);
                return 255;
            }
            dflt = v[0];
            secs = (unsigned)(v[2] - '0');
            if (v[3]) secs = secs * 10u + (unsigned)(v[3] - '0');
            timed = true;
        } else if ((sw == 'N' || sw == 'S') && !a[2]) {
            if (sw == 'N') list = false;
            else exact = true;
        } else {
            dos_error(sh, "CHOICE: invalid switch", a);
            return 255;
        }
    }
    for (const char *k = keys; *k; k++) {
        if (*k <= ' ' || *k > '~' || *k == '/') {
            dos_error(sh, "CHOICE: invalid choice switch syntax. Expected form: /C[:]choices", NULL);
            return 255;
        }
    }
    if (timed) {
        const char *k = keys;
        while (*k && !key_is(*k, dflt, exact)) k++;
        if (!*k) {
            dos_error(sh, "CHOICE: Timeout default not in specified (or default) choices.", NULL);
            return 255;
        }
    }
    if (!sh->hooks.getkey) {
        dos_error(sh, "CHOICE: no keyboard", NULL);
        return 255;
    }

    for (int w = i; w < argc; w++) out_printf(&sh->con, w > i ? " %s" : "%s", argv[w]);
    if (list) {
        out_puts(&sh->con, "[");
        for (const char *k = keys; *k; k++)
            out_printf(&sh->con, k > keys ? ",%c" : "%c", exact ? *k : upper(*k));
        out_puts(&sh->con, "]?");
    }

    /* A time limit needs both a clock and a key that does not wait. */
    timed = timed && sh->hooks.pollkey && sh->hooks.now_us;
    uint64_t end = timed ? sh->hooks.now_us(sh->hooks.ctx) + (uint64_t)secs * 1000000u : 0;
    for (;;) {
        int key;
        if (timed) {
            key = sh->hooks.pollkey(sh->hooks.ctx);
            if (key < 0) {
                if (sh->hooks.now_us(sh->hooks.ctx) < end) continue;
                key = (unsigned char)dflt;
            }
        } else {
            key = sh->hooks.getkey(sh->hooks.ctx);
        }
        if (key < 0 || key == 0x03 || key == 0x1B || key == KEY_BREAK) {
            out_puts(&sh->con, "\n");
            sh->stop = true;
            return 0;
        }
        for (const char *k = keys; *k; k++) {
            if (key < 0x80 && key_is(*k, (char)key, exact)) {
                out_printf(&sh->con, "%c\n", exact ? *k : upper(*k));
                return (int)(k - keys) + 1;
            }
        }
        out_puts(&sh->con, "\a");
    }
}
