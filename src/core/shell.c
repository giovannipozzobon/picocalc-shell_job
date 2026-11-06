/* shell.c — the command line (shell.h). */

#include "keycodes.h"
#include "shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "clock.h"
#include "files.h"
#include "flavour.h"
#include "history.h"
#include "line.h"
#include "path.h"
#include "pipe.h"
#include "run.h"
#include "system.h"
#include "vars.h"

/* Whether the terminal's cursor is at a line's start, from the bytes
 * sent to it: a CR or LF puts it there, a character takes it away, and
 * of the escape sequences only RIS and a cursor move to column 1 put it
 * there. Enough to start the prompt on a line of its own after output
 * with no final line break. */
static uint8_t  s_esc;        /* 0, after ESC, or in a CSI */
static unsigned s_esc_col;    /* a CSI's second parameter so far */
static bool     s_esc_semi;

static void con_write(void *ctx, const char *buf, size_t n) {
    shell_t *sh = ctx;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (s_esc == 1) {
            s_esc = c == '[' ? 2 : 0;
            s_esc_col = 0;
            s_esc_semi = false;
            if (c == 'c') sh->at_bol = true;
        } else if (s_esc == 2) {
            if (c == ';') s_esc_semi = true;
            else if (c >= '0' && c <= '9' && s_esc_semi) s_esc_col = s_esc_col * 10u + (c - '0');
            else if (c >= 0x40 && c <= 0x7E) {
                if (c == 'H' || c == 'f') sh->at_bol = s_esc_col <= 1u;
                s_esc = 0;
            }
        } else if (c == 0x1B) {
            s_esc = 1;
        } else if (c == '\r' || c == '\n') {
            sh->at_bol = true;
        } else if (c >= 0x20) {
            sh->at_bol = false;
        }
    }
    sh->term.write(sh->term.ctx, buf, n);
}

void shell_init(shell_t *sh, const out_t *out, const shell_hooks_t *hooks, const char *version) {
    memset(sh, 0, sizeof *sh);
    sh->term = *out;
    sh->con = (out_t){ con_write, sh, out->raw };
    sh->out = sh->con;
    sh->out_is_con = true;
    sh->err = sh->con;
    sh->err_is_con = true;
    sh->at_bol = true;
    sh->hooks = *hooks;
    sh->version = version;
    strcpy(sh->cwd, "/");
    s_esc = 0;
    vars_init();
    flavour_init(sh);
    history_clear();
    line_start(sh, 0);
}

void shell_set_loader(shell_t *sh, const struct shell_loader *loader) {
    sh->loader = loader;
}

void shell_set_system(shell_t *sh, const shell_system_t *sys) {
    sh->sys = *sys;
}

void shell_set_memory(shell_t *sh, unsigned long used, unsigned long total, unsigned long psram) {
    sh->mem_used = used;
    sh->mem_total = total;
    sh->mem_psram = psram;
}

/* The prompt's width on its last row: what follows its last line
 * break, less escape sequences, a backspace taking one back. */
static size_t prompt_width(const char *p) {
    size_t w = 0;
    for (; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            w = 0;
        } else if (*p == '\b') {
            if (w) w--;
        } else if (*p == '\033') {
            if (p[1] == '[') {
                p += 2;
                while (*p && !(*p >= 0x40 && *p <= 0x7E)) p++;
                if (!*p) break;
            } else if (p[1]) {
                p++;
            }
        } else if ((unsigned char)*p >= 0x20) {
            w++;
        }
    }
    return w;
}

/* The flavour's prompt (design.md §4.4, §4.7). */
void shell_prompt(shell_t *sh) {
    static char buf[SHELL_PROMPT_MAX + 1];
    if (!sh->at_bol) out_puts(&sh->con, "\n");
    flavour_prompt(sh, buf, sizeof buf);
    out_puts(&sh->con, buf);
    line_start(sh, prompt_width(buf));
}

void shell_start(shell_t *sh) {
    files_mount();
    /* DOS starts at the root, as DOS did; UNIX in $HOME. */
    const char *home = SHELL_DOS ? "/" : vars_get("HOME", 4);
    if (!home) home = SHELL_HOME;
    strcpy(sh->cwd, files_is_dir(home) ? home : "/");
    out_puts(&sh->con, "\033c");   /* RIS: a clean screen whatever ran before */
    history_load();
    /* Back from a restart: the directory left, once the startup file has
     * run from the usual place. */
    static char kept[SHELL_PATH_MAX + 1];
    bool back = history_take_cwd(kept);
    history_drop_args();
    flavour_start(sh);
    if (back && files_is_dir(kept)) files_chdir(sh, kept);
    sh->stop = false;
    shell_prompt(sh);
}

void shell_leaving(shell_t *sh) {
    clock_leaving();
    const char *err = history_save();
    if (err) shell_error(sh, "shell: %s: %s\n", HISTORY_FILE, err);
}

void shell_keep_cwd(shell_t *sh) {
    const char *err = history_keep_cwd(sh->cwd);
    if (err) shell_error(sh, "shell: %s: %s\n", CWD_FILE, err);
}

void shell_error(shell_t *sh, const char *fmt, ...) {
    char buf[SHELL_PRINTF_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    out_write(&sh->err, buf, (size_t)n);
}

bool shell_interrupted(shell_t *sh) {
    if (!sh->stop && sh->hooks.interrupted && sh->hooks.interrupted(sh->hooks.ctx)) sh->stop = true;
    return sh->stop;
}

int shell_getkey(shell_t *sh) {
    if (sh->wheel_left) {
        int up = sh->wheel_left > 0;
        sh->wheel_left += up ? -1 : 1;
        return up ? KEY_UP : KEY_DOWN;
    }
    int k = sh->hooks.getkey(sh->hooks.ctx);
    if (k == KEY_WHEEL_UP || k == KEY_WHEEL_DOWN) {
        int up = k == KEY_WHEEL_UP;
        sh->wheel_left = up ? SHELL_WHEEL_LINES - 1 : -(SHELL_WHEEL_LINES - 1);
        return up ? KEY_UP : KEY_DOWN;
    }
    return k;
}

/* ---- commands --------------------------------------------------------- */

int cmd_usbdisk(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc != 1) {
        shell_error(sh, "usage: usbdisk\n");
        return 1;
    }
    if (!sh->hooks.usbdisk) {
        shell_error(sh, "usbdisk: no USB here\n");
        return 1;
    }
    shell_leaving(sh);
    out_puts(&sh->con, "The card is on USB. Eject it on the computer to come back;\n"
                       "Esc comes back at once, which may lose what it was writing.\n");
    files_unmount();
    const char *err = sh->hooks.usbdisk(sh->hooks.ctx);
    files_mount();
    sh->stop = false;   /* Esc is how it ends, not a reason to drop the line */
    if (err) {
        shell_error(sh, "usbdisk: %s\n", err);
        return 1;
    }
    /* The card may have changed under the shell. */
    if (!files_is_dir(sh->cwd)) strcpy(sh->cwd, "/");
    history_load();
    return 0;
}

/* A builtin's name: exact in the UNIX flavour; in DOS's, letters
 * without regard to case. */
static bool same_name(const char *a, const char *b) {
    return SHELL_DOS ? path_same(a, b) : strcmp(a, b) == 0;
}

static int index_of(const char *name) {
    for (unsigned i = 0; i < flavour_nbuiltins; i++)
        if (same_name(name, flavour_builtins[i].name)) return (int)i;
    return -1;
}

shell_cmd_fn shell_builtin(const char *name) {
    int i = index_of(name);
    return i < 0 ? NULL : flavour_builtins[i].fn;
}

bool shell_is_builtin(const char *name) {
    return index_of(name) >= 0;
}

int shell_builtin_index(const char *name) {
    return index_of(name);
}

const char *shell_builtin_at(unsigned i) {
    return i < flavour_nbuiltins ? flavour_builtins[i].name : NULL;
}

const char *shell_builtin_usage(unsigned i) {
    return i < flavour_nbuiltins ? flavour_builtins[i].usage : NULL;
}

const char *shell_builtin_about(unsigned i) {
    return i < flavour_nbuiltins ? flavour_builtins[i].about : NULL;
}

/* ---- running a command ----------------------------------------------- */

int shell_command(shell_t *sh, int argc, char **argv, const char *in, const char *out,
                  bool append, pipe_t *from, pipe_t *to) {
    out_t saved_out = sh->out;
    bool saved_is_con = sh->out_is_con;
    input_t saved_in = sh->in;
    if (from) sh->in = (input_t){ NULL, from };
    if (to) {
        sh->out = pipe_out(to);
        sh->out_is_con = false;
    }
    /* Anything that leaves the shell must not leave a redirection's
     * name.new or a pipe's file behind, here or at a level above. */
    bool redirected = in || out || !sh->out_is_con || !sh->err_is_con || input_any(&sh->in);
    bool err_to_out = sh->err_to_out;
    sh->err_to_out = false;

    int status;
    shell_cmd_fn fn = shell_builtin(argv[0]);
    if (!fn) {
        status = run_program(sh, argc, argv, redirected);
    } else if ((fn == cmd_menu || fn == cmd_usbdisk || system_leaves(fn)) && redirected) {
        shell_error(sh, "%s: not with a redirection or a pipe\n", argv[0]);
        status = 1;
    } else if (in && !files_input(sh, in)) {
        status = 1;
    } else if (out && !files_redirect(sh, out, append)) {
        if (in) files_input_end(sh);
        status = 1;
    } else {
        if (err_to_out) {
            sh->err = sh->out;
            sh->err_is_con = sh->out_is_con;
        }
        status = fn(sh, argc, argv);
        if (out && !files_redirect_end(sh) && status == 0) status = 1;
        if (in) files_input_end(sh);
    }
    sh->out = saved_out;
    sh->out_is_con = saved_is_con;
    sh->in = saved_in;
    return status;
}

static void discard(void *ctx, const char *buf, size_t n) {
    (void)ctx;
    (void)buf;
    (void)n;
}

const out_t shell_null_out = { discard, NULL, true };

bool shell_leaves(const char *name) {
    shell_cmd_fn fn = shell_builtin(name);
    return !fn || fn == cmd_menu || fn == cmd_usbdisk || system_leaves(fn);
}

int shell_command_null(shell_t *sh, int argc, char **argv, const char *in,
                       pipe_t *from, pipe_t *to) {
    /* For a program or a command that leaves the shell the redirection is
     * dropped instead: the null device leaves no file behind, which is
     * why those refuse a redirection, and what they say before leaving
     * is little. */
    if (shell_leaves(argv[0]))
        return shell_command(sh, argc, argv, in, NULL, false, from, to);
    out_t saved = sh->out;
    bool saved_is_con = sh->out_is_con;
    sh->out = shell_null_out;
    sh->out_is_con = false;
    int status = shell_command(sh, argc, argv, in, NULL, false, from, to);
    sh->out = saved;
    sh->out_is_con = saved_is_con;
    return status;
}

int shell_pipeline(shell_t *sh, int n, shell_stage_fn stage, void *ctx) {
    pipe_t *from = NULL;
    int status = 0;
    for (int k = 0; k < n; k++) {
        pipe_t *to = NULL;
        const char *why;
        if (k + 1 < n && !(to = pipe_open(&why))) {
            shell_error(sh, "shell: %s\n", why);
            status = 1;
            break;
        }
        status = stage(sh, k, from, to, ctx);
        if (from) pipe_close(from);
        from = to;
        /* Output lost on the way: the next stage would read part of it. */
        if (to && (why = pipe_written(to)) != NULL) {
            shell_error(sh, "shell: pipe: %s\n", why);
            status = 1;
            break;
        }
        if (sh->stop) break;
    }
    if (from) pipe_close(from);
    return status;
}

int shell_run(shell_t *sh, const char *line) {
    sh->stop = false;
    flavour_run_line(sh, line);
    if (sh->stop) {
        out_puts(&sh->con, "^C\n");
        sh->status = 130;
    }
    sh->stop = false;
    return sh->status;
}

/* ---- the prompt ------------------------------------------------------- */

void shell_input(shell_t *sh, int key) {
    if (!line_key(sh, key)) return;
    history_add(sh->line.text);
    shell_run(sh, sh->line.text);
    shell_prompt(sh);
}
