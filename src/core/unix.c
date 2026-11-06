/* unix.c — the UNIX flavour's command language (flavour.h; design.md
 * §4.4): the parser (parse.h) and expansion (expand.h), `;`, `&&` and
 * `||`, NAME=value, `sh` scripts and /etc/profile, and its builtins:
 * the file commands (files.h), `more`, `grep`, `head`, `tail`, `wc`, `uniq` and `sort`
 * (text.h), the PicoCalc's own (system.h), `echo`, `set`, `unset`,
 * `export`, `sh`, `exit`, `test`, `which`, `history`, `menu`, `usbdisk`, `vi` and `view` (vi.h);
 * and `select` with `break`, its one compound command. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "expand.h"
#include "files.h"
#include "flavour.h"
#include "history.h"
#include "keycodes.h"
#include "line.h"
#include "ntp.h"
#include "parse.h"
#include "path.h"
#include "pipe.h"
#include "run.h"
#include "system.h"
#include "text.h"
#include "vars.h"
#include "vi.h"

#define PROFILE "/etc/profile"

static int script(shell_t *sh, const char *name);
static int cmd_select(shell_t *sh, int argc, char **argv);
static int cmd_break(shell_t *sh, int argc, char **argv);
static int cmd_exit(shell_t *sh, int argc, char **argv);

/* `exit` ran: the rest of the line, and of its script, is dropped. */
static bool s_exit;

static void banner(shell_t *sh) {
    out_printf(&sh->con, "\033[1mPicoCalc shell\033[0m %s\n\n", sh->version);
}

/* ---- commands --------------------------------------------------------- */

static int cmd_echo(shell_t *sh, int argc, char **argv) {
    /* -n: no newline at the end. */
    int first = (argc > 1 && strcmp(argv[1], "-n") == 0) ? 2 : 1;
    for (int i = first; i < argc; i++) {
        if (i > first) out_puts(&sh->out, " ");
        out_puts(&sh->out, argv[i]);
    }
    if (first == 1) out_puts(&sh->out, "\n");
    return 0;
}

/* NAME=value, as typed, into a variable. */
static int assign(shell_t *sh, const char *word, expand_t *e) {
    const char *eq;
    expand_is_assignment(word, &eq);
    e->used = 0;
    const char *value = expand_one(sh, eq + 1, strlen(eq + 1), e);
    if (!value) return 1;
    if (!vars_set(word, (size_t)(eq - word), value)) {
        shell_error(sh, "shell: no room for %.*s\n", (int)(eq - word), word);
        return 1;
    }
    return 0;
}

static int cmd_set(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc != 1) {
        shell_error(sh, "usage: set, to list the variables; NAME=value sets one\n");
        return 1;
    }
    const char *v;
    for (unsigned i = 0; (v = vars_at(i)) != NULL; i++) out_printf(&sh->out, "%s\n", v);
    return 0;
}

static int cmd_unset(shell_t *sh, int argc, char **argv) {
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (!vars_is_name(argv[i], strlen(argv[i]))) {
            shell_error(sh, "unset: %s: not a name\n", argv[i]);
            status = 1;
            continue;
        }
        vars_unset(argv[i], strlen(argv[i]));
    }
    return status;
}

/* export NAME=value sets it; export NAME is accepted and does nothing,
 * since every variable is the session's (vars.h); export alone lists. */
static int cmd_export(shell_t *sh, int argc, char **argv) {
    if (argc == 1) return cmd_set(sh, argc, argv);
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        size_t n = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!vars_is_name(argv[i], n)) {
            shell_error(sh, "export: %s: not a name\n", argv[i]);
            status = 1;
        } else if (eq && !vars_set(argv[i], n, eq + 1)) {
            shell_error(sh, "export: no room for %.*s\n", (int)n, argv[i]);
            status = 1;
        }
    }
    return status;
}

static int cmd_true(shell_t *sh, int argc, char **argv) {
    (void)sh;
    (void)argc;
    (void)argv;
    return 0;
}

static int cmd_false(shell_t *sh, int argc, char **argv) {
    (void)sh;
    (void)argc;
    (void)argv;
    return 1;
}

/* A whole decimal number, or false with the error printed. */
static bool test_number(shell_t *sh, const char *s, long *n) {
    char *end;
    *n = strtol(s, &end, 10);
    if (*s && !*end) return true;
    shell_error(sh, "test: %s: not a number\n", s);
    return false;
}

/* 0 true, 1 false, 2 a mistake. */
static int test_eval(shell_t *sh, int n, char **a) {
    if (n == 0) return 1;
    if (strcmp(a[0], "!") == 0 && n > 1) {
        int st = test_eval(sh, n - 1, a + 1);
        return st == 2 ? 2 : !st;
    }
    if (n == 1) return a[0][0] ? 0 : 1;
    if (n == 2) {
        const char *op = a[0];
        if (strcmp(op, "-n") == 0) return a[1][0] ? 0 : 1;
        if (strcmp(op, "-z") == 0) return a[1][0] ? 1 : 0;
        if (strchr("efdsrw", op[1]) && op[0] == '-' && op[1] && !op[2]) {
            static char path[SHELL_PATH_MAX + 1];
            static FILINFO fno;
            if (!path_resolve(path, sh->cwd, a[1])) return 1;
            bool root = strcmp(path, "/") == 0;
            if (!root && files_stat(path, &fno) != FR_OK) return 1;
            bool dir = root || (fno.fattrib & AM_DIR);
            switch (op[1]) {
            case 'f': return dir ? 1 : 0;
            case 'd': return dir ? 0 : 1;
            case 's': return !dir && fno.fsize > 0 ? 0 : 1;
            default:  return 0;
            }
        }
    } else if (n == 3) {
        const char *op = a[1];
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return strcmp(a[0], a[2]) ? 1 : 0;
        if (strcmp(op, "!=") == 0) return strcmp(a[0], a[2]) ? 0 : 1;
        static const char ops[][4] = { "-eq", "-ne", "-lt", "-le", "-gt", "-ge" };
        for (unsigned k = 0; k < 6; k++) {
            if (strcmp(op, ops[k]) != 0) continue;
            long x, y;
            if (!test_number(sh, a[0], &x) || !test_number(sh, a[2], &y)) return 2;
            bool yes = k == 0 ? x == y : k == 1 ? x != y : k == 2 ? x < y
                     : k == 3 ? x <= y : k == 4 ? x > y : x >= y;
            return yes ? 0 : 1;
        }
    }
    shell_error(sh, "test: not understood; see man test\n");
    return 2;
}

static int cmd_test(shell_t *sh, int argc, char **argv) {
    return test_eval(sh, argc - 1, argv + 1);
}

static int cmd_bracket(shell_t *sh, int argc, char **argv) {
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
        shell_error(sh, "[: no closing ]\n");
        return 2;
    }
    return test_eval(sh, argc - 2, argv + 1);
}

static int cmd_which(shell_t *sh, int argc, char **argv) {
    if (argc < 2) {
        shell_error(sh, "usage: which name...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *path;
        if (shell_is_builtin(argv[i])) {
            out_printf(&sh->out, "%s: builtin\n", argv[i]);
        } else if ((path = run_which(sh, argv[i])) != NULL) {
            out_printf(&sh->out, "%s\n", path);
        } else {
            status = 1;
        }
    }
    return status;
}

static int cmd_sh(shell_t *sh, int argc, char **argv) {
    if (argc != 2) {
        shell_error(sh, "usage: sh file\n");
        return 1;
    }
    return script(sh, argv[1]);
}

/* Each builtin, with its usage and a line on what it does for `man`;
 * in order, as `man` and Tab list them. */
const shell_builtin_t flavour_builtins[] = {
    { "backlight",  cmd_backlight,  "backlight [lcd|kbd] [0-255]", "Shows or sets a backlight" },
    { "battery",    cmd_battery,    "battery", "Displays the battery's charge" },
#if SHELL_NTP
    { "bt",         cmd_bt,         "bt", "Shows the Bluetooth keyboard's state" },
#endif
    { "bootsel",    cmd_bootsel,    "bootsel", "Reboot into bootloader mode (BOOTSEL)" },
    { "break",      cmd_break,      "break [N]", "Leaves a select, or N of them" },
    { "cat",        cmd_cat,        "cat file... (or cat < file)", "Concatenate and print files" },
    { "cd",         cmd_cd,         "cd [dir | -]", "Change directory; - for the last one" },
    { "clear",      cmd_clear,      "clear", "Clears the screen" },
    { "cp",         cmd_cp,         "cp [-rRf] from... to", "Copies files, -r directories" },
    { "date",       cmd_date,       "date [-u] [mmddHHMM[[cc]yy]]", "Displays or sets the date and time" },
    { "df",         cmd_df,         "df", "Displays the card's size and free space" },
    { "echo",       cmd_echo,       "echo [-n] [word...]", "Print words; -n no line break" },
    { "exit",       cmd_exit,       "exit [N]", "Ends a script; the prompt stays" },
    { "export",     cmd_export,     "export [NAME=value...]", "Displays, sets or lists variables" },
    { "false",      cmd_false,      "false", "Does nothing, and fails" },
    { "find",       cmd_find,       "find [path] [-name pattern] [-type f|d]", "Lists files in and below a directory" },
    { "grep",       cmd_grep,       "grep [-v] [-n] [-c] [-i] pattern [file...]", "File pattern searcher" },
    { "hdmi",       cmd_hdmi,       "hdmi", "Resends the screen to HDMI" },
    { "head",       cmd_head,       "head [-n N] [file...]", "Displays the first lines of a file" },
    { "history",    cmd_history,    "history", "Displays the lines typed" },
    { "info",       cmd_info,       "info", "Displays diagnostic information" },
    { "less",       cmd_more,       "less file...", "The same as more" },
    { "ls",         cmd_ls,         "ls [-l] [-a] [path...]", "Lists directory contents" },
    { "man",        cmd_help,       "man [command]", "Display manual pages" },
    { "menu",       cmd_menu,       "menu", "UF2 Loader's menu" },
    { "mkdir",      cmd_mkdir,      "mkdir [-p] dir...", "Makes directories" },
    { "more",       cmd_more,       "more file... (or < file, or | more)", "Displays text a screen at a time" },
    { "mv",         cmd_mv,         "mv [-f] from... to", "Move or rename" },
#if SHELL_NTP
    { "ntp",        cmd_ntp,        "ntp [-f]", "Sets the clock from the network, once" },
#endif
    { "phosphor",   cmd_phosphor,   "phosphor [white|green|amber|default]", "Sets the colour of the text" },
    { "poweroff",   cmd_poweroff,   "poweroff", "Switches the PicoCalc off" },
    { "pwd",        cmd_pwd,        "pwd", "Print the current directory" },
    { "reboot",     cmd_reboot,     "reboot", "Restarts the shell" },
    { "rm",         cmd_rm,         "rm [-rRf] path...", "Removes files, -r directories" },
    { "rmdir",      cmd_rmdir,      "rmdir dir...", "Removes empty directories" },
    { "select",     cmd_select,     "select NAME in WORD...; do LIST; done", "Offers the words as a menu" },
    { "set",        cmd_set,        "set", "Lists the variables" },
    { "sh",         cmd_sh,         "sh file", "Runs a script" },
    { "sort",       cmd_sort,       "sort [-r] [-n] [-u] [file...]", "Sorts lines, up to 64 KB" },
    { "tail",       cmd_tail,       "tail [-n N] [file...]", "Displays the last lines, 10 by default" },
    { "tee",        cmd_tee,        "tee [-a] [file]", "Copies its input to a file as well" },
    { "[",          cmd_bracket,    "[ expression ]", "The same as test" },
    { "test",       cmd_test,       "test [!] -f|-d|-e|-s file\n  test [!] -n|-z text, or a = b, a != b\n  test [!] a -eq|-ne|-lt|-le|-gt|-ge b", "Checks a file, text or numbers" },
    { "touch",      cmd_touch,      "touch file...", "Creates empty files" },
    { "true",       cmd_true,       "true", "Does nothing, and works" },
    { "uniq",       cmd_uniq,       "uniq [-c] [-d] [file]", "Drops repeats of the line before" },
    { "unset",      cmd_unset,      "unset NAME...", "Removes variables" },
    { "usbdisk",    cmd_usbdisk,    "usbdisk", "Exposes the SD Card to a computer" },
    { "vi",         cmd_vi,         "vi [-R] file", "Edits a text file" },
    { "view",       cmd_view,       "view file", "Reads a text file in vi" },
    { "wc",         cmd_wc,         "wc [-l] [-w] [-c] [file...]", "Counts lines, words and bytes" },
#if SHELL_NTP
    { "web",        cmd_web,        "web", "Manages the SD Card from a browser" },
#endif
    { "which",      cmd_which,      "which name...", "Shows where a command is found" },
};

const unsigned flavour_nbuiltins = sizeof flavour_builtins / sizeof flavour_builtins[0];

/* ---- running a line --------------------------------------------------- */

/* Each level's line, parsed, and the words of the command it is running.
 * The parser's output is large, and the shell's stack small. */
static struct {
    parse_t  parse;
    expand_t exp;
} s_frames[SHELL_DEPTH];

/* Whether two names typed are one file. */
static bool same_file(shell_t *sh, const char *a, const char *b) {
    static char pa[SHELL_PATH_MAX + 1], pb[SHELL_PATH_MAX + 1];
    return path_resolve(pa, sh->cwd, a) && path_resolve(pb, sh->cwd, b) && path_same(pa, pb);
}

/* A script run by its name is `sh name`: into e. False, with the error
 * printed, if it cannot be. */
static bool script_name(shell_t *sh, expand_t *e) {
    if (shell_is_builtin(e->argv[0])) return true;
    const char *path = run_script_path(sh, e->argv[0]);
    if (!path) return true;
    if (e->argc > 1) {
        shell_error(sh, "%s: a script takes no arguments\n", e->argv[0]);
        return false;
    }
    size_t n = strlen(path) + 1, k = sizeof "sh";
    if (e->used + n + k > sizeof e->text) {
        shell_error(sh, "shell: arguments too long\n");
        return false;
    }
    e->argv[0] = memcpy(e->text + e->used, "sh", k);
    e->argv[1] = memcpy(e->text + e->used + k, path, n);
    e->argv[2] = NULL;
    e->used += n + k;
    e->argc = 2;
    return true;
}

/* One command with its redirections, reading `from` and writing `to`
 * when it is in a pipeline. */
static int run_cmd(shell_t *sh, const parse_cmd_t *c, pipe_t *from, pipe_t *to) {
    expand_t *e = &s_frames[sh->depth].exp;
    const char *eq;
    if (expand_is_assignment(c->argv[0], &eq)) {
        if (c->in || c->out) {
            shell_error(sh, "shell: %s: NAME=value with a redirection is not supported\n",
                        c->argv[0]);
            return 1;
        }
        for (int i = 0; i < c->argc; i++) {
            if (!expand_is_assignment(c->argv[i], &eq)) {
                shell_error(sh, "shell: %s: NAME=value before a command is not supported\n",
                            c->argv[0]);
                return 1;
            }
        }
        for (int i = 0; i < c->argc; i++)
            if (assign(sh, c->argv[i], e)) return 1;
        return 0;
    }

    if (!expand_args(sh, c->argv, e)) return 1;
    char *in = NULL, *out = NULL;
    if (c->in && !(in = expand_one(sh, c->in, strlen(c->in), e))) return 1;
    if (c->out && !(out = expand_one(sh, c->out, strlen(c->out), e))) return 1;
    if ((in && !*in) || (out && !*out)) {
        shell_error(sh, "shell: a redirection with no file\n");
        return 1;
    }
    if (e->argc == 0) return 0;   /* only empty variables */
    if (!script_name(sh, e)) return 1;

    /* A program or what leaves the shell can leave no file behind. */
    char *err = NULL;
    if (c->err && !(err = expand_one(sh, c->err, strlen(c->err), e))) return 1;
    if (err && !*err) {
        shell_error(sh, "shell: a redirection with no file\n");
        return 1;
    }
    bool err_null = err && strcmp(err, "/dev/null") == 0;
    if (err && !err_null && shell_leaves(e->argv[0])) {
        shell_error(sh, "%s: not with a redirection or a pipe\n", e->argv[0]);
        return 1;
    }
    bool out_null = out && strcmp(out, "/dev/null") == 0;
    if (err && !err_null && out && !out_null && same_file(sh, err, out)) {
        shell_error(sh, "shell: %s: both > and 2>; use > %s 2>&1\n", out, out);
        return 1;
    }

    const out_t saved_out = sh->out, saved_err = sh->err;
    const bool saved_out_con = sh->out_is_con, saved_err_con = sh->err_is_con;
    int status = 1;
    pipe_t *empty = NULL;
    /* < /dev/null is an input with nothing in it. */
    if (in && strcmp(in, "/dev/null") == 0) {
        const char *why;
        if (!(empty = pipe_open(&why))) {
            shell_error(sh, "shell: %s\n", why);
            return 1;
        }
        from = empty;
        in = NULL;
    }
    if (err_null) {
        if (!shell_leaves(e->argv[0])) {
            sh->err = shell_null_out;
            sh->err_is_con = false;
        }
    } else if (err && !files_redirect_err(sh, err, c->err_append)) {
        goto done;
    }
    if (c->err_to_out) {
        if (c->err_to_file && out_null) {
            sh->err = shell_null_out;
            sh->err_is_con = false;
        } else if (c->err_to_file) {
            sh->err_to_out = true;
        } else {
            sh->err = to ? pipe_out(to) : sh->out;
            sh->err_is_con = to ? false : sh->out_is_con;
        }
    }
    if (c->out_to_err) {
        sh->out = sh->err;
        sh->out_is_con = sh->err_is_con;
    }
    /* > /dev/null throws the output away; there is no /dev on the card. */
    status = out_null ? shell_command_null(sh, e->argc, e->argv, in, from, to)
                      : shell_command(sh, e->argc, e->argv, in, out, c->append, from, to);
    if (err && !err_null && !files_redirect_err_end(sh, &saved_err) && status == 0) status = 1;
done:
    sh->err_to_out = false;
    sh->out = saved_out;
    sh->out_is_con = saved_out_con;
    sh->err = saved_err;
    sh->err_is_con = saved_err_con;
    if (empty) pipe_close(empty);
    return status;
}

/* The k-th command of the pipeline that starts at `first`. */
typedef struct {
    const parse_t *p;
    int            first;
} stages_t;

static int stage(shell_t *sh, int k, pipe_t *from, pipe_t *to, void *ctx) {
    const stages_t *s = ctx;
    return run_cmd(sh, &s->p->cmds[s->first + k], from, to);
}

/* A parsed line's commands, at this level. */
static int run_parsed(shell_t *sh, const parse_t *p);

/* A line at this level. `where` names a script's file and line for a
 * mistake in it; NULL for a typed line. */
static int run_line(shell_t *sh, const char *line, const char *where) {
    parse_t *p = &s_frames[sh->depth].parse;
    if (!parse_line(p, line)) {
        if (where) shell_error(sh, "sh: %s: %s\n", where, p->error);
        else shell_error(sh, "shell: %s\n", p->error);
        return sh->status = 2;
    }
    return run_parsed(sh, p);
}

/* Each level's script and its reader. */
static FIL      s_script[SHELL_DEPTH];
static reader_t s_reader[SHELL_DEPTH];
static char     s_text[SHELL_DEPTH][SHELL_LINE_MAX + 2];
static char     s_where[SHELL_DEPTH][SHELL_PATH_MAX + 16];

/* ---- select (design.md §4.4) ------------------------------------------ */

/* A select is kept as the pieces of its lines between the words select,
 * do and done, each piece of commands a line of its own to run_line. */
typedef enum { PART_CMDS, PART_SELECT, PART_DO, PART_DONE } part_kind_t;

typedef struct {
    uint8_t  kind;
    uint16_t text;              /* PART_CMDS, PART_SELECT: into compound_t.text */
    uint16_t line;              /* its line in the script; 0 at the prompt */
} part_t;

/* Each level's select, from `select` to its `done`, while it is read and
 * while it runs, and the words offered by it and those inside it. */
typedef struct {
    char        text[SHELL_SELECT_TEXT];
    size_t      used;
    part_t      parts[SHELL_SELECT_PARTS];
    unsigned    nparts;         /* 0: none being read or run */
    unsigned    open;           /* selects read and not yet ended by done */
    bool        want_do;        /* the last piece was a select's words */
    bool        broken;         /* a mistake in it: the script ends */
    unsigned    line;           /* the first select's line */
    const char *name;           /* the script's; NULL at the prompt */
    char        words[SHELL_SELECT_WORDS];
    size_t      wused;
    uint16_t    items[SHELL_SELECT_ITEMS];   /* into words */
    unsigned    nitems;
    unsigned    loops;          /* selects running */
    unsigned    brk;            /* selects `break` is leaving */
} compound_t;

static compound_t s_comp[SHELL_DEPTH];

typedef enum { KW_NONE, KW_SELECT, KW_DO, KW_DONE } kw_t;

/* select, do and done count only unquoted, as a command's first word. */
static kw_t keyword(const parse_cmd_t *c) {
    const char *w = c->argv[0];
    if (strcmp(w, "select") == 0) return KW_SELECT;
    if (strcmp(w, "do") == 0) return KW_DO;
    if (strcmp(w, "done") == 0) return KW_DONE;
    return KW_NONE;
}

static bool has_keyword(const parse_t *p) {
    for (int i = 0; i < p->ncmds; i++)
        if (keyword(&p->cmds[i]) != KW_NONE) return true;
    return false;
}

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

/* A decimal number of digits alone, or 0. */
static unsigned long number(const char *s) {
    unsigned long n = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > 99999999ul) return 0;
        n = n * 10u + (unsigned long)(*s - '0');
    }
    return n;
}

static bool mistake(shell_t *sh, const char *where, const char *why) {
    if (where) shell_error(sh, "sh: %s: %s\n", where, why);
    else shell_error(sh, "shell: %s\n", why);
    return false;
}

static bool add_part(shell_t *sh, compound_t *k, part_kind_t kind, const char *s, size_t n,
                     unsigned line, const char *where) {
    part_t *pt = &k->parts[k->nparts];
    if (k->nparts == SHELL_SELECT_PARTS || (s && k->used + n + 1u > sizeof k->text))
        return mistake(sh, where, "select too long");
    pt->kind = (uint8_t)kind;
    pt->line = (uint16_t)line;
    pt->text = (uint16_t)k->used;
    if (s) {
        memcpy(k->text + k->used, s, n);
        k->text[k->used + n] = '\0';
        k->used += n + 1u;
    }
    k->nparts++;
    return true;
}

/* The commands in s[0..n): after a keyword's command when `lead`, so
 * with the ';' after it, and before one when `trail`, with the ';'
 * before it. Blank, or only a comment, they are no piece. */
static bool add_cmds(shell_t *sh, compound_t *k, const char *s, size_t n, bool lead, bool trail,
                     unsigned line, const char *where) {
    while (n && is_blank(*s)) s++, n--;
    if (lead && n && *s == ';') s++, n--;
    while (n && is_blank(*s)) s++, n--;
    while (n && is_blank(s[n - 1])) n--;
    if (trail && n && s[n - 1] == ';') n--;
    if (n == 0 || *s == '#') return true;
    if (k->want_do) return mistake(sh, where, "select: do expected");
    return add_part(sh, k, PART_CMDS, s, n, line, where);
}

/* The pieces of one line of a select, added to k. False, with the
 * mistake reported, if it is not one. */
static bool split(shell_t *sh, compound_t *k, const char *text, unsigned line, const char *where) {
    parse_t *p = &s_frames[sh->depth].parse;
again:
    if (!parse_line(p, text)) return mistake(sh, where, p->error);
    unsigned from = 0;
    for (int i = 0; i < p->ncmds; i++) {
        const parse_cmd_t *c = &p->cmds[i];
        kw_t kw = keyword(c);
        if (kw == KW_NONE) continue;
        if (c->sep != SEP_FIRST && c->sep != SEP_SEMI)
            return mistake(sh, where, "select, do and done may follow only ; or a line's start");
        if (!add_cmds(sh, k, text + from, c->start - from, from > 0, true, line, where)) return false;
        if (kw != KW_DO) {
            if (i + 1 < p->ncmds && p->cmds[i + 1].sep != SEP_SEMI)
                return mistake(sh, where, "only ; or a line's end may follow select's words or done");
            if (c->in || c->out || c->out_to_err || c->err || c->err_to_out)
                return mistake(sh, where, "a select cannot be redirected");
        }
        switch (kw) {
        case KW_SELECT:
            if (k->want_do) return mistake(sh, where, "select: do expected");
            if (c->argc < 3 || strcmp(c->argv[2], "in") != 0)
                return mistake(sh, where, "usage: select NAME in WORD...; do LIST; done");
            if (!vars_is_name(c->argv[1], strlen(c->argv[1])))
                return mistake(sh, where, "select: not a name");
            if (k->open == 0) k->line = line;
            if (!add_part(sh, k, PART_SELECT, text + c->start, c->end - c->start, line, where))
                return false;
            k->open++;
            k->want_do = true;
            break;
        case KW_DO:
            if (!k->want_do) return mistake(sh, where, "do without select");
            k->want_do = false;
            if (!add_part(sh, k, PART_DO, NULL, 0, line, where)) return false;
            text += c->start + 2u;   /* the LIST may start on its line */
            goto again;
        case KW_DONE:
            if (k->open == 0) return mistake(sh, where, "done without select");
            if (k->want_do) return mistake(sh, where, "select: do expected");
            if (c->argc > 1) return mistake(sh, where, "done: a word after it");
            if (!add_part(sh, k, PART_DONE, NULL, 0, line, where)) return false;
            k->open--;
            break;
        case KW_NONE:
            break;
        }
        from = c->end;
    }
    return add_cmds(sh, k, text + from, strlen(text) - from, from > 0, false, line, where);
}

static const char *where_of(shell_t *sh, const compound_t *k, unsigned line) {
    if (!k->name) return NULL;
    snprintf(s_where[sh->depth], sizeof s_where[sh->depth], "%s: line %u", k->name, line);
    return s_where[sh->depth];
}

static bool push_word(compound_t *k, const char *w) {
    size_t n = strlen(w) + 1u;
    if (k->wused + n > sizeof k->words) return false;
    memcpy(k->words + k->wused, w, n);
    k->wused += n;
    return true;
}

/* The menu, to the terminal: `N) word` down the columns, as ls lays out
 * names, each row short of the last column: the terminal wraps as soon as
 * a row is full, so a full row and its line break would be two rows. */
static void show_menu(shell_t *sh, const compound_t *k, unsigned first, unsigned n) {
    unsigned nw = 1;
    for (unsigned v = n; v >= 10u; v /= 10u) nw++;
    size_t wmax = 0;
    for (unsigned i = 0; i < n; i++) {
        size_t w = nw + 2u + strlen(k->words + k->items[first + i]);
        if (w > wmax) wmax = w;
    }
    unsigned colw = (unsigned)wmax + 2u;
    unsigned cols = (TERM_COLS + 1u) / colw;
    if (cols == 0) cols = 1;
    unsigned rows = (n + cols - 1u) / cols;
    if (!sh->at_bol) out_puts(&sh->con, "\n");
    for (unsigned r = 0; r < rows; r++) {
        for (unsigned i = r; i < n; i += rows) {
            const char *w = k->words + k->items[first + i];
            out_printf(&sh->con, "%*u) %s", (int)nw, i + 1u, w);
            if (i + rows < n)
                for (size_t pad = nw + 2u + strlen(w); pad < colw; pad++) out_puts(&sh->con, " ");
        }
        out_puts(&sh->con, "\n");
    }
}

/* A line typed after $PS3, into sh->line (line_read). */
static int read_reply(shell_t *sh) {
    const char *ps3 = vars_get("PS3", 3);
    return line_read(sh, ps3 ? ps3 : "#? ");
}

static int run_parts(shell_t *sh, compound_t *k, unsigned i, unsigned end);

/* The select whose words are part i and whose done is part j. */
static void run_select(shell_t *sh, compound_t *k, unsigned i, unsigned j) {
    unsigned d = sh->depth;
    parse_t *p = &s_frames[d].parse;
    expand_t *e = &s_frames[d].exp;
    if (!sh->hooks.getkey) {
        shell_error(sh, "select: no keyboard\n");
        sh->status = 1;
        return;
    }
    if (!parse_line(p, k->text + k->parts[i].text)) {   /* it parsed when it was read */
        mistake(sh, where_of(sh, k, k->parts[i].line), p->error);
        sh->status = 2;
        return;
    }
    const parse_cmd_t *c = &p->cmds[0];
    if (!expand_args(sh, c->argv + 3, e)) {
        sh->status = 1;
        return;
    }
    /* The LIST uses p and e: keep NAME and the words. */
    size_t w0 = k->wused;
    unsigned n0 = k->nitems, n = (unsigned)e->argc;
    size_t nlen = strlen(c->argv[1]);
    bool fits = push_word(k, c->argv[1]) && n0 + n <= SHELL_SELECT_ITEMS;
    for (unsigned w = 0; fits && w < n; w++) {
        k->items[n0 + w] = (uint16_t)k->wused;
        fits = push_word(k, e->argv[w]);
    }
    const char *name = k->words + w0;
    sh->status = 0;
    if (!fits) {
        shell_error(sh, "select: more words than it can offer\n");
        sh->status = 1;
    } else if (n > 0) {
        k->nitems = n0 + n;
        k->loops++;
        show_menu(sh, k, n0, n);
        while (read_reply(sh) > 0) {
            const char *reply = sh->line.text;
            unsigned long v = number(reply);
            const char *word = v >= 1u && v <= n ? k->words + k->items[n0 + v - 1u] : "";
            if (!vars_set("REPLY", 5, reply) || !vars_set(name, nlen, word)) {
                shell_error(sh, "select: no room for %s\n", name);
                sh->status = 1;
                break;
            }
            if (!*reply) {
                show_menu(sh, k, n0, n);
                continue;
            }
            run_parts(sh, k, i + 2u, j);
            if (k->brk) {
                k->brk--;
                break;
            }
            if (sh->stop) break;
        }
        if (--k->loops == 0) k->brk = 0;
    }
    k->wused = w0;
    k->nitems = n0;
}

/* Parts [i, end) of this level's select. */
static int run_parts(shell_t *sh, compound_t *k, unsigned i, unsigned end) {
    while (i < end && !sh->stop && !k->brk && !s_exit) {
        const part_t *pt = &k->parts[i];
        if (pt->kind == PART_CMDS) {
            run_line(sh, k->text + pt->text, where_of(sh, k, pt->line));
            i++;
            continue;
        }
        /* A select's words; its do and done are taken with it. */
        unsigned j = i + 2u, open = 1;
        for (;; j++) {
            if (k->parts[j].kind == PART_SELECT) open++;
            else if (k->parts[j].kind == PART_DONE && --open == 0) break;
        }
        run_select(sh, k, i, j);
        i = j + 1u;
    }
    return sh->status;
}

static void select_reset(compound_t *k) {
    k->nparts = k->open = 0;
    k->used = 0;
    k->want_do = false;
}

/* A line at this level, typed (name NULL), or line `line` of the script
 * `name`, with `where` naming it. A select is read to its done over the
 * script's lines, and then run; at the prompt it fits on one. */
static int run_top(shell_t *sh, const char *text, const char *name, unsigned line,
                   const char *where) {
    compound_t *k = &s_comp[sh->depth];
    if (k->nparts == 0) {
        parse_t *p = &s_frames[sh->depth].parse;
        if (!parse_line(p, text)) {
            mistake(sh, where, p->error);
            return sh->status = 2;
        }
        if (!has_keyword(p)) return run_parsed(sh, p);
        select_reset(k);
        k->name = name;
    }
    if (!split(sh, k, text, line, where)) {
        select_reset(k);
        k->broken = name != NULL;
        return sh->status = 2;
    }
    if (k->open > 0) {
        if (name) return sh->status;   /* the rest is on the lines to come */
        mistake(sh, NULL, "select with no done");
        select_reset(k);
        return sh->status = 2;
    }
    run_parts(sh, k, 0, k->nparts);
    select_reset(k);
    return sh->status;
}

static int cmd_select(shell_t *sh, int argc, char **argv) {
    (void)argc;
    (void)argv;
    shell_error(sh, "select: only as a command's first word, unquoted\n");
    return 2;
}

static int cmd_break(shell_t *sh, int argc, char **argv) {
    compound_t *k = &s_comp[sh->depth];
    unsigned long n = argc == 2 ? number(argv[1]) : 1u;
    if (argc > 2 || n == 0) {
        shell_error(sh, "usage: break [N]\n");
        return 1;
    }
    if (k->loops == 0) {
        shell_error(sh, "break: not in a select\n");
        return 1;
    }
    k->brk = n < k->loops ? (unsigned)n : k->loops;
    return 0;
}

/* exit [N]: ends this script, the rest of it dropped, with N for $? or
 * the last command's status. At the prompt there is nothing to end: the
 * banner says the shell is still here, and nothing else changes. */
static int cmd_exit(shell_t *sh, int argc, char **argv) {
    int status = sh->status;
    if (argc > 2) {
        shell_error(sh, "usage: exit [N]\n");
        return 2;
    }
    if (argc == 2) {
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (!*argv[1] || *end) {
            shell_error(sh, "exit: %s: not a number\n", argv[1]);
            status = 2;
        } else {
            status = (int)(n & 0xFF);
        }
    }
    compound_t *k = &s_comp[sh->depth];
    k->brk = k->loops;   /* out of any select */
    s_exit = true;
    if (sh->depth == 0) banner(sh);
    return status;
}

static int run_parsed(shell_t *sh, const parse_t *p) {
    const compound_t *k = &s_comp[sh->depth];
    for (int i = 0; i < p->ncmds && !sh->stop && !k->brk && !s_exit;) {
        int end = i + 1;
        while (end < p->ncmds && p->cmds[end].sep == SEP_PIPE) end++;
        /* A pipeline skipped by && or || leaves the status as it was. */
        sep_t sep = p->cmds[i].sep;
        if (!(sep == SEP_AND && sh->status != 0) && !(sep == SEP_OR && sh->status == 0)) {
            stages_t s = { p, i };
            sh->status = shell_pipeline(sh, end - i, stage, &s);
        }
        i = end;
    }
    return sh->status;
}

int flavour_run_line(shell_t *sh, const char *line) {
    int status = run_top(sh, line, NULL, 0, NULL);
    s_exit = false;
    return status;
}

/* Run the file `name` a line at a time at the level below, to its end
 * or a stop; the last command's status. /etc/profile, if there is none,
 * is nothing. */
static int script(shell_t *sh, const char *name) {
    unsigned d = sh->depth + 1u;
    bool profile = strcmp(name, PROFILE) == 0 && sh->depth == 0;
    if (d >= SHELL_DEPTH) {
        shell_error(sh, "sh: %s: scripts nested more than %u deep\n", name, SHELL_DEPTH - 1u);
        return 2;
    }
    FRESULT r = profile ? files_open_startup(&s_script[d], name)
                        : files_open_read(sh, "sh", name, &s_script[d]);
    if (r != FR_OK) {
        if (profile && r != FR_NO_FILE && r != FR_NO_PATH && r != FR_NOT_READY)
            shell_error(sh, "sh: %s: %s\n", name, files_why(r));
        return profile ? 0 : 1;
    }
    reader_init(&s_reader[d], &(input_t){ &s_script[d], NULL });
    sh->depth = d;
    int status = 0;
    unsigned lineno = 0;
    bool whole = true;
    long n;
    while (!sh->stop && !s_exit &&
           (n = reader_line(&s_reader[d], s_text[d], sizeof s_text[d], &whole)) >= 0) {
        bool piece = !whole;
        lineno++;
        while (!whole && reader_line(&s_reader[d], s_text[d], sizeof s_text[d], &whole) >= 0) {}
        snprintf(s_where[d], sizeof s_where[d], "%s: line %u", name, lineno);
        if (piece || (size_t)n > SHELL_LINE_MAX) {
            shell_error(sh, "sh: %s: line too long\n", s_where[d]);
            status = sh->status = 2;
            if (s_comp[d].nparts == 0) continue;
            select_reset(&s_comp[d]);
            break;
        }
        status = run_top(sh, s_text[d], name, lineno, s_where[d]);
        /* A mistake in a select ends the script, as one in sh's grammar does. */
        if (s_comp[d].broken) break;
    }
    s_comp[d].broken = false;
    if (s_comp[d].nparts) {
        shell_error(sh, "sh: %s: line %u: select with no done\n", name, s_comp[d].line);
        select_reset(&s_comp[d]);
        status = sh->status = 2;
    }
    if (s_reader[d].r != FR_OK) {
        shell_error(sh, "sh: %s: %s\n", name, files_why(s_reader[d].r));
        status = 1;
    }
    f_close(&s_script[d]);
    sh->depth = d - 1u;
    s_exit = false;   /* it ended this script, and no more */
    return status;
}

/* ---- the rest of flavour.h -------------------------------------------- */

void flavour_init(shell_t *sh) {
    (void)sh;
    vars_set("HOME", 4, SHELL_HOME);
    vars_set("PATH", 4, SHELL_PROGRAM_DIR);
}

void flavour_start(shell_t *sh) {
    banner(sh);
    const char *note = clock_note();
    if (note) shell_error(sh, "shell: %s\n", note);
    script(sh, PROFILE);
}

/* The current directory followed by '>' (design.md §4.4). */
void flavour_prompt(shell_t *sh, char *buf, size_t size) {
    snprintf(buf, size, "%s>", sh->cwd);
}

bool flavour_resolve(char out[SHELL_PATH_MAX + 1], const char *cwd, const char *arg) {
    return path_resolve(out, cwd, arg);
}

bool flavour_is_path(const char *name) {
    return strchr(name, '/') != NULL;
}

void flavour_file_error(shell_t *sh, const char *cmd, const char *name, const char *why) {
    shell_error(sh, "%s: %s: %s\n", cmd, name, why);
}

void flavour_not_found(shell_t *sh, const char *name) {
    shell_error(sh, "%s: not found\n", name);
}
