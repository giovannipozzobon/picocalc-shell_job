/* run.c — programs and `menu` (run.h). */

#include "run.h"

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "flavour.h"
#include "history.h"
#include "path.h"
#include "vars.h"

static FIL      s_prog;
static FILINFO  s_fno;
static char     s_path[SHELL_PATH_MAX + 1];
static char     s_sfn[SHELL_PATH_MAX + 1];
static char     s_spelled[SHELL_PATH_MAX + 1];

/* The path stage3 can open: each name of the absolute `path` as its 8.3
 * entry, as the loader's menu makes it (get_short_path in ui/uf2.c).
 * FatFs gives the 8.3 name as altname, or, for a name that has no long
 * one and no case of its own, leaves altname empty and fname is it. */
static FRESULT short_path(const char *path, char out[SHELL_PATH_MAX + 1]) {
    char prefix[SHELL_PATH_MAX + 1];
    size_t olen = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t end = slash ? (size_t)(slash - path) : strlen(path);
        memcpy(prefix, path, end);
        prefix[end] = '\0';
        FRESULT r = f_stat(prefix, &s_fno);
        if (r != FR_OK) return r;
        const char *name = s_fno.altname[0] ? s_fno.altname : s_fno.fname;
        size_t n = strlen(name);
        if (olen + 1 + n > SHELL_PATH_MAX) return FR_INVALID_NAME;
        out[olen++] = '/';
        memcpy(out + olen, name, n);
        olen += n;
        p = slash ? slash + 1 : path + end;
    }
    out[olen] = '\0';
    return FR_OK;
}

static bool stop(void *ctx) {
    return shell_interrupted(ctx);
}

typedef struct {
    shell_t          *sh;
    const uf2_info_t *info;
} progress_t;

/* Before each sector is written: count it on the line, which has no
 * name so it stays short of the width and CR can rewrite it, and keep
 * the keyboard polled; a stop is ignored. */
static void tick(void *ctx) {
    progress_t *p = ctx;
    shell_t *sh = p->sh;
    out_printf(&sh->con, "\rwriting %lu of %lu sectors", (unsigned long)p->info->written + 1,
               (unsigned long)p->info->to_write);
    if (sh->hooks.interrupted) sh->hooks.interrupted(sh->hooks.ctx);
}

static uint32_t since(shell_t *sh, uint64_t t0) {
    return (uint32_t)(sh->hooks.now_us(sh->hooks.ctx) - t0);
}

/* The last steps before the reboot, in order: what a program that takes
 * arguments is to be given, without which it must not start; then the
 * directory to come back to, and the history. False, with the error
 * printed, if the program must not start. */
static bool leave_for(shell_t *sh, const char *name, const char *path, const uf2_info_t *info,
                      int argc, char **argv) {
    if (info->args_name[0]) {
        const char *err = files_spelled(path, s_spelled);
        if (!err) err = history_keep_args(sh->cwd, s_spelled, info->args_name, argc, argv);
        if (err) {
            shell_error(sh, "%s: %s: %s\n", name, ARGS_FILE, err);
            return false;
        }
    }
    shell_keep_cwd(sh);
    shell_leaving(sh);
    return true;
}

/* Check, flash if need be, and reboot into the program at `path`, which
 * exists, giving it argv[1..argc-1] if it declared that it takes them.
 * `ram_only` for the menu, which must never be flashed. */
static int run_path(shell_t *sh, const char *name, const char *path, bool ram_only, int argc,
                    char **argv) {
    const shell_loader_t *ld = sh->loader;
    if (!ld || ld->problem) {
        shell_error(sh, "%s: cannot run programs: %s\n", name,
                    ld ? ld->problem : "no UF2 Loader here");
        return 1;
    }
    FRESULT fr = f_open(&s_prog, path, FA_READ);
    if (fr != FR_OK) {
        shell_error(sh, "%s: %s: %s\n", name, path, files_why(fr));
        return 1;
    }
    uf2_info_t info;
    uint64_t t0 = sh->hooks.now_us(sh->hooks.ctx);
    uf2_result_t r = uf2_check(&s_prog, &ld->flash, &info, stop, sh);
    info.check_us = since(sh, t0);
    /* Refused before flash is touched, not started without what was asked
     * for (design.md §3.5). */
    if ((r == UF2_FLASH || r == UF2_RAM) && argc > 1 && !info.args_name[0]) {
        f_close(&s_prog);
        shell_error(sh, "%s: takes no arguments\n", name);
        return 2;
    }
    if (r == UF2_FLASH && ram_only) {
        f_close(&s_prog);
        shell_error(sh, "%s: %s: not a RAM program\n", name, path);
        return 1;
    }
    if (r == UF2_FLASH && info.differ) {
        progress_t p = { sh, &info };
        t0 = sh->hooks.now_us(sh->hooks.ctx);
        r = uf2_write(&s_prog, &ld->flash, &info, tick, &p);
        info.write_us = since(sh, t0);
        out_puts(&sh->con, "\n");
    }
    f_close(&s_prog);

    switch (r) {
    case UF2_FLASH:
        if (!leave_for(sh, name, path, &info, argc, argv)) return 1;
        ld->boot(ld->ctx, NULL, &info);
        return 0;
    case UF2_RAM:
        /* Before anything is written: nothing may fail once the file is
         * there, or it waits for a program started from the loader's menu. */
        fr = short_path(path, s_sfn);
        if (fr != FR_OK) {
            shell_error(sh, "%s: %s\n", name, files_why(fr));
            return 1;
        }
        if (!leave_for(sh, name, path, &info, argc, argv)) return 1;
        ld->boot(ld->ctx, s_sfn, &info);
        return 0;
    case UF2_NOT_UF2:
        shell_error(sh, "%s: not a UF2 program\n", name);
        return 126;
    case UF2_NOT_HERE:
        shell_error(sh, "%s: not for this device\n", name);
        return 126;
    case UF2_BAD:
        shell_error(sh, "%s: damaged UF2: blocks missing or out of order\n", name);
        return 126;
    case UF2_CARD:
        shell_error(sh, "%s: %s\n", name, files_why(info.fr));
        return 1;
    case UF2_STOPPED:
        return 130;
    case UF2_WRITE_FAILED:
        break;
    }
    shell_error(sh, "%s: writing flash failed (%s); flash may not hold a whole program\n", name,
                info.fr != FR_OK ? files_why(info.fr) : "flash error");
    return 1;
}

/* `name.uf2` in the directory `dir`, as typed in $PATH: into s_path if
 * it is there. */
static bool in_dir(shell_t *sh, const char *dir, const char *file) {
    static char abs[SHELL_PATH_MAX + 1];
    return flavour_resolve(abs, sh->cwd, dir) && path_join(s_path, abs, file) &&
           f_stat(s_path, &s_fno) == FR_OK && !(s_fno.fattrib & AM_DIR);
}

static bool ends_with(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && path_same(name + n - e, ext);
}

/* `name`, a path, into s_path if it is a file there: with no extension,
 * name.uf2 first, as C:\WP51\WP was WP.EXE and as a name on $PATH is. */
static bool at_path(void) {
    size_t len = strlen(s_path);
    if (!strchr(path_base(s_path), '.') && len + sizeof SHELL_PROGRAM_EXT <= sizeof s_path) {
        memcpy(s_path + len, SHELL_PROGRAM_EXT, sizeof SHELL_PROGRAM_EXT);
        if (f_stat(s_path, &s_fno) == FR_OK && !(s_fno.fattrib & AM_DIR)) return true;
        s_path[len] = '\0';
    }
    return f_stat(s_path, &s_fno) == FR_OK && !(s_fno.fattrib & AM_DIR);
}

/* `file` in the first directory of $PATH that has it, into s_path, the
 * directories separated by ':' (DOS: ';'; after the current directory,
 * as DOS looked). An empty entry is the current directory, as in sh. */
static bool on_path(shell_t *sh, const char *file) {
    static char dir[SHELL_PATH_MAX + 1];
    if (SHELL_DOS && in_dir(sh, ".", file)) return true;
    for (const char *p = vars_get("PATH", 4); p;) {
        const char *colon = strchr(p, FLAVOUR_PATH_SEP);
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len < sizeof dir) {
            memcpy(dir, p, len);
            dir[len] = '\0';
            if (len == 0) strcpy(dir, ".");
            if (in_dir(sh, dir, file)) return true;
        }
        p = colon ? colon + 1 : NULL;
    }
    return false;
}

/* What to look for on $PATH: name.uf2, or a name typed with .uf2 (or
 * .sh, UNIX's scripts) as it is. False if it cannot fit. */
static bool search_name(char file[SHELL_PATH_MAX + 1], const char *name) {
    size_t n = strlen(name);
    if (n + sizeof SHELL_PROGRAM_EXT > SHELL_PATH_MAX + 1) return false;
    memcpy(file, name, n + 1);
    if (!ends_with(name, SHELL_PROGRAM_EXT) && (SHELL_DOS || !ends_with(name, ".sh")))
        memcpy(file + n, SHELL_PROGRAM_EXT, sizeof SHELL_PROGRAM_EXT);
    return true;
}

/* `name` as a program: a path if it has a '/' (in DOS, a '\' or a
 * drive), otherwise along $PATH. False, with the error printed, if none. */
static bool find(shell_t *sh, const char *name) {
    static char file[SHELL_PATH_MAX + 1];
    if (flavour_is_path(name)) {
        if (!flavour_resolve(s_path, sh->cwd, name)) {
            if (SHELL_DOS) flavour_not_found(sh, name);
            else shell_error(sh, "%s: name too long\n", name);
            return false;
        }
        if (at_path()) return true;
        FRESULT r = f_stat(s_path, &s_fno);
        if (r == FR_OK) shell_error(sh, "%s: is a directory\n", name);
        else shell_error(sh, "%s: %s\n", name, files_why(r));
        return false;
    }
    if (search_name(file, name) && on_path(sh, file)) return true;
    /* A name not found is "not found", whatever stopped the search, as
     * in sh. */
    flavour_not_found(sh, name);
    return false;
}

const char *run_which(shell_t *sh, const char *name) {
    static char file[SHELL_PATH_MAX + 1];
    if (flavour_is_path(name))
        return flavour_resolve(s_path, sh->cwd, name) && at_path() ? s_path : NULL;
    return search_name(file, name) && on_path(sh, file) ? s_path : NULL;
}

const char *run_script_path(shell_t *sh, const char *name) {
    static char file[SHELL_PATH_MAX + 1];
    if (flavour_is_path(name)) {
        if (!flavour_resolve(s_path, sh->cwd, name) || f_stat(s_path, &s_fno) != FR_OK ||
            (s_fno.fattrib & AM_DIR))
            return NULL;
        if (ends_with(s_path, ".sh")) return s_path;
        /* A file with no .sh is a script if it says so. */
        char start[2];
        UINT got = 0;
        if (f_open(&s_prog, s_path, FA_READ) != FR_OK) return NULL;
        f_read(&s_prog, start, sizeof start, &got);
        f_close(&s_prog);
        return got == 2 && start[0] == '#' && start[1] == '!' ? s_path : NULL;
    }
    return ends_with(name, ".sh") && search_name(file, name) && on_path(sh, file) ? s_path : NULL;
}

int run_program(shell_t *sh, int argc, char **argv, bool redirected) {
    if (!find(sh, argv[0])) return 127;
    if (redirected) {
        shell_error(sh, "%s: a program takes no redirection or pipe\n", argv[0]);
        return 2;
    }
    return run_path(sh, argv[0], s_path, false, argc, argv);
}

int run_ram(shell_t *sh, const char *name, const char *path) {
    snprintf(s_path, sizeof s_path, "%s", path);
    return run_path(sh, name, s_path, true, 0, NULL);
}

int run_ram_program(shell_t *sh, const char *name) {
    if (!find(sh, name)) return 127;
    return run_path(sh, name, s_path, true, 0, NULL);
}

int cmd_menu(shell_t *sh, int argc, char **argv) {
    /* A redirection is refused by run_cmd, before it is opened. */
    if (argc > 1) {
        shell_error(sh, "usage: menu, with no operand or redirection\n");
        return 1;
    }
    (void)argv;
    return run_ram(sh, "menu", SHELL_MENU_UF2);
}
