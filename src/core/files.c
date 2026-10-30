/* files.c — the card's files (files.h).
 *
 * Everything large is static: the shell runs on a 4 KB stack (scratch Y),
 * so FILs, FILINFOs, path buffers and the copy buffer live here, each
 * owned by one step at a time. cp -r and rm -r walk the tree with an
 * explicit stack of open directories rather than by recursion. */

#include "files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "flavour.h"
#include "path.h"
#include "vars.h"

/* At once: cp -r's walk and its two files, and at each level a script,
 * its < and its > (shell.c), and every pipe's file (pipe.c). */
_Static_assert(FF_FS_LOCK >= SHELL_TREE_DEPTH + 2u + 3u * SHELL_DEPTH + SHELL_PIPES,
               "FatFs's lock table must hold everything that can be open at once");

static FILINFO s_fno;
static FIL     s_src;      /* the file cat or cp reads */
static FIL     s_in[SHELL_DEPTH];          /* <, at each level */
static files_safe_t s_redir[SHELL_DEPTH];   /* > and >>, at each level */
static files_safe_t s_copy;     /* cp's destination */
static uint8_t s_buf[SHELL_COPY_BUF];

/* Paths, each owned by one command at a time. */
static char s_a[SHELL_PATH_MAX + 1], s_b[SHELL_PATH_MAX + 1], s_c[SHELL_PATH_MAX + 1];

/* cp -r and rm -r: the directories open on the way down, and the length
 * of each walked path at each level. */
static DIR    s_dirs[SHELL_TREE_DEPTH];
static size_t s_srclen[SHELL_TREE_DEPTH], s_dstlen[SHELL_TREE_DEPTH];
static char   s_wsrc[SHELL_PATH_MAX + 1], s_wdst[SHELL_PATH_MAX + 1];

bool files_is_dir(const char *path) {
    return files_stat(path, &s_fno) == FR_OK && (s_fno.fattrib & AM_DIR);
}

/* `arg` as typed, in the flavour's paths, made absolute. */
static bool resolve(shell_t *sh, const char *cmd, const char *arg, char *out) {
    if (flavour_resolve(out, sh->cwd, arg)) return true;
    flavour_file_error(sh, cmd, arg, "name too long");
    return false;
}

int files_options(shell_t *sh, int argc, char **argv, const char *allowed, unsigned *flags,
                   const char *usage) {
    *flags = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) return i + 1;
        for (const char *c = argv[i] + 1; *c; c++) {
            const char *p = strchr(allowed, *c);
            if (!p) {
                shell_error(sh, "%s: -%c: unknown option\nusage: %s\n", argv[0], *c, usage);
                return -1;
            }
            *flags |= 1u << (p - allowed);
        }
    }
    return i;
}

static int usage(shell_t *sh, const char *text) {
    shell_error(sh, "usage: %s\n", text);
    return 1;
}

/* ---- writing through name.new ----------------------------------------- */

/* ---- redirection ------------------------------------------------------ */

static void redir_write(void *ctx, const char *buf, size_t n) {
    files_safe_write(ctx, buf, n);
}

FRESULT files_open_startup(FIL *f, const char *path) {
    FRESULT r = f_open(f, path, FA_READ);
    if (r != FR_NO_FILE || strlen(path) + 4u > SHELL_PATH_MAX) return r;
    strcpy(s_c, path);
    strcat(s_c, ".new");
    FRESULT r2 = f_open(f, s_c, FA_READ);
    return r2 == FR_OK ? FR_OK : r;
}

bool files_redirect(shell_t *sh, const char *arg, bool append) {
    files_safe_t *w = &s_redir[sh->depth];
    if (!resolve(sh, "shell", arg, s_a)) return false;
    const char *err = files_safe_open(w, s_a);
    if (!err && append) {
        /* >> copies what is there first, so the old file stays whole
         * until the new one is. */
        FRESULT r = f_open(&s_src, s_a, FA_READ);
        if (r == FR_OK) {
            UINT n;
            while ((r = f_read(&s_src, s_buf, sizeof s_buf, &n)) == FR_OK && n > 0)
                files_safe_write(w, s_buf, n);
            f_close(&s_src);
        }
        if (r != FR_OK && r != FR_NO_FILE) w->why = files_why(r);
        err = w->why;
        if (err) files_safe_abort(w);
    }
    if (err) {
        flavour_file_error(sh, "shell", arg, err);
        return false;
    }
    sh->out = (out_t){ redir_write, w, true };
    sh->out_is_con = false;
    return true;
}

bool files_redirect_end(shell_t *sh) {
    files_safe_t *w = &s_redir[sh->depth];
    const char *err = files_safe_commit(w);
    if (err) flavour_file_error(sh, "shell", w->path, err);
    return err == NULL;
}

bool files_input(shell_t *sh, const char *arg) {
    if (!resolve(sh, "shell", arg, s_a)) return false;
    FIL *f = &s_in[sh->depth];
    FRESULT r = f_open(f, s_a, FA_READ);
    if (r != FR_OK) {
        bool dir = r == FR_NO_FILE && files_is_dir(s_a);
        flavour_file_error(sh, "shell", arg, dir ? "is a directory" : files_why(r));
        return false;
    }
    sh->in = (input_t){ f, NULL };
    return true;
}

void files_input_end(shell_t *sh) {
    f_close(&s_in[sh->depth]);
}

/* ---- pwd, cd ---------------------------------------------------------- */

int cmd_pwd(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc != 1) return usage(sh, "pwd");
    out_printf(&sh->out, "%s\n", sh->cwd);
    return 0;
}

int cmd_cd(shell_t *sh, int argc, char **argv) {
    if (argc > 2) return usage(sh, "cd [dir]");
    const char *arg = argc == 2 ? argv[1] : vars_get("HOME", 4);
    if (!arg) {
        shell_error(sh, "cd: HOME is not set\n");
        return 1;
    }
    if (!resolve(sh, "cd", arg, s_a)) return 1;
    FRESULT r = files_stat(s_a, &s_fno);
    if (r != FR_OK) {
        shell_error(sh, "cd: %s: %s\n", arg, files_why(r));
        return 1;
    }
    if (!(s_fno.fattrib & AM_DIR)) {
        shell_error(sh, "cd: %s: not a directory\n", arg);
        return 1;
    }
    const char *why = files_chdir(sh, s_a);
    if (why) {
        shell_error(sh, "cd: %s: %s\n", arg, why);
        return 1;
    }
    return 0;
}

const char *files_chdir(shell_t *sh, const char *path) {
    /* The prompt shows each name as the card spells it, not as typed.
     * f_stat gives a long name back as it was asked for, so each one is
     * found in its directory's listing. */
    FRESULT r;
    if (path != s_a) strcpy(s_a, path);
    size_t len = 0;
    s_b[0] = '\0';
    for (char *p = s_a + 1; *p;) {
        char *e = strchr(p, '/');
        if (e) *e = '\0';
        DIR d;
        r = f_opendir(&d, len ? s_b : "/");
        while (r == FR_OK && (r = f_readdir(&d, &s_fno)) == FR_OK && s_fno.fname[0])
            if (path_same(s_fno.fname, p) || path_same(s_fno.altname, p)) break;
        f_closedir(&d);
        if (r == FR_OK && !s_fno.fname[0]) r = FR_NO_PATH;
        if (r != FR_OK) return files_why(r);
        size_t n = strlen(s_fno.fname);
        if (len + 1u + n > SHELL_PATH_MAX) return "name too long";
        s_b[len++] = '/';
        memcpy(s_b + len, s_fno.fname, n + 1);
        len += n;
        p = e ? e + 1 : p + strlen(p);
    }
    strcpy(sh->cwd, len ? s_b : "/");
    return NULL;
}

/* ---- ls --------------------------------------------------------------- */

typedef struct {
    uint16_t name;     /* offset in s_names */
    uint8_t  attr;
    uint32_t size;
    uint16_t fdate, ftime;   /* FAT's, in local time */
} ent_t;

static ent_t s_ents[SHELL_LS_MAX];
static char  s_names[SHELL_LS_NAMES];

enum { LS_L = 1u << 0, LS_A = 1u << 1 };

static int fold(int c) {
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

/* Names in order without regard to case, as FAT compares them. */
static int ent_cmp(const void *a, const void *b) {
    const char *x = s_names + ((const ent_t *)a)->name;
    const char *y = s_names + ((const ent_t *)b)->name;
    for (;; x++, y++) {
        int d = fold((unsigned char)*x) - fold((unsigned char)*y);
        if (d || !*x) return d ? d : strcmp(s_names + ((const ent_t *)a)->name,
                                            s_names + ((const ent_t *)b)->name);
    }
}

bool files_hidden(const FILINFO *fno) {
    return fno->fname[0] == '.' || (fno->fattrib & (AM_HID | AM_SYS));
}

/* About six months, as ls takes it: half of an average Gregorian year. */
#define SIX_MONTHS 15778476

/* One entry of ls -l: mode, size, date and name (design.md §3.8). The
 * date is POSIX's %b %e %H:%M, or the year in place of the time for one
 * more than six months from now, either way. */
static void ls_long(shell_t *sh, const char *name, const FILINFO *fno) {
    uint8_t attr = fno->fattrib;
    char mode[4] = { (attr & AM_DIR) ? 'd' : '-', 'r', (attr & AM_RDO) ? '-' : 'w', '\0' };
    if (attr & AM_DIR) out_printf(&sh->out, "%s %8s ", mode, "");
    else out_printf(&sh->out, "%s %8lu ", mode, (unsigned long)fno->fsize);
    if (fno->fdate) {
        clock_tm_t now, t;
        clock_now(&now);
        clock_from_fat(fno->fdate, fno->ftime, &t);
        int64_t away = clock_seconds(&t) - clock_seconds(&now);
        out_printf(&sh->out, "%s %2d ", clock_mon_name(t.mon), t.mday);
        if (away > SIX_MONTHS || away < -SIX_MONTHS) out_printf(&sh->out, " %4d ", t.year);
        else out_printf(&sh->out, "%02d:%02d ", t.hour, t.min);
    } else {
        out_printf(&sh->out, "%13s", "");
    }
    out_puts(&sh->out, name);
    out_puts(&sh->out, (attr & AM_DIR) ? "/\n" : "\n");
}

/* A directory's name ends in '/', as ls -F shows it. */
static size_t ent_width(const ent_t *e) {
    return strlen(s_names + e->name) + ((e->attr & AM_DIR) ? 1u : 0u);
}

static void ls_name(shell_t *sh, const ent_t *e) {
    out_puts(&sh->out, s_names + e->name);
    if (e->attr & AM_DIR) out_puts(&sh->out, "/");
}

/* Down the columns, as ls does on a terminal, and one per line when the
 * output is a file. Each row stops short of the last column: the terminal
 * wraps as soon as a row is full, so a full row and its line break would
 * be two rows. */
static void ls_names(shell_t *sh, unsigned n) {
    if (!sh->out_is_con) {
        for (unsigned i = 0; i < n; i++) {
            ls_name(sh, &s_ents[i]);
            out_puts(&sh->out, "\n");
        }
        return;
    }
    size_t wmax = 0;
    for (unsigned i = 0; i < n; i++) {
        size_t w = ent_width(&s_ents[i]);
        if (w > wmax) wmax = w;
    }
    unsigned colw = (unsigned)wmax + 2u;
    unsigned cols = (TERM_COLS + 1u) / colw;
    if (cols == 0) cols = 1;
    unsigned rows = (n + cols - 1u) / cols;
    for (unsigned r = 0; r < rows; r++) {
        for (unsigned i = r; i < n; i += rows) {
            ls_name(sh, &s_ents[i]);
            if (i + rows < n)
                for (size_t pad = ent_width(&s_ents[i]); pad < colw; pad++) out_puts(&sh->out, " ");
        }
        out_puts(&sh->out, "\n");
    }
}

static int ls_dir(shell_t *sh, const char *shown, const char *path, unsigned flags) {
    DIR d;
    FRESULT r = f_opendir(&d, path);
    unsigned n = 0;
    size_t used = 0;
    bool full = false;
    while (r == FR_OK && (r = f_readdir(&d, &s_fno)) == FR_OK && s_fno.fname[0]) {
        if (!(flags & LS_A) && files_hidden(&s_fno)) continue;
        size_t len = strlen(s_fno.fname) + 1u;
        if (n == SHELL_LS_MAX || used + len > sizeof s_names) {
            full = true;
            break;
        }
        memcpy(s_names + used, s_fno.fname, len);
        s_ents[n++] = (ent_t){ (uint16_t)used, s_fno.fattrib, (uint32_t)s_fno.fsize,
                               s_fno.fdate, s_fno.ftime };
        used += len;
    }
    f_closedir(&d);
    if (r != FR_OK) {
        shell_error(sh, "ls: %s: %s\n", shown, files_why(r));
        return 1;
    }

    if (full) {
        /* Too many to sort: the lot, in the card's order, one per line. */
        shell_error(sh, "ls: %s: more than %u names, not sorted\n", shown, SHELL_LS_MAX);
        r = f_opendir(&d, path);
        while (r == FR_OK && (r = f_readdir(&d, &s_fno)) == FR_OK && s_fno.fname[0]) {
            if (!(flags & LS_A) && files_hidden(&s_fno)) continue;
            if (flags & LS_L) ls_long(sh, s_fno.fname, &s_fno);
            else out_printf(&sh->out, "%s%s\n", s_fno.fname, (s_fno.fattrib & AM_DIR) ? "/" : "");
            if (shell_interrupted(sh)) break;
        }
        f_closedir(&d);
        return r == FR_OK ? 0 : 1;
    }

    qsort(s_ents, n, sizeof s_ents[0], ent_cmp);
    if (flags & LS_L) {
        for (unsigned i = 0; i < n; i++) {
            const ent_t *e = &s_ents[i];
            s_fno.fattrib = e->attr;
            s_fno.fsize = e->size;
            s_fno.fdate = e->fdate;
            s_fno.ftime = e->ftime;
            ls_long(sh, s_names + e->name, &s_fno);
        }
    } else {
        ls_names(sh, n);
    }
    return 0;
}

int cmd_ls(shell_t *sh, int argc, char **argv) {
    unsigned flags;
    int first = files_options(sh, argc, argv, "la", &flags, "ls [-l] [-a] [path...]");
    if (first < 0) return 1;
    if (first == argc) return ls_dir(sh, ".", sh->cwd, flags);

    int status = 0;
    bool headers = argc - first > 1;
    for (int i = first; i < argc && !sh->stop; i++) {
        if (!resolve(sh, "ls", argv[i], s_a)) {
            status = 1;
            continue;
        }
        FRESULT r = files_stat(s_a, &s_fno);
        if (r != FR_OK) {
            shell_error(sh, "ls: %s: %s\n", argv[i], files_why(r));
            status = 1;
        } else if (!(s_fno.fattrib & AM_DIR)) {
            if (flags & LS_L) ls_long(sh, argv[i], &s_fno);
            else out_printf(&sh->out, "%s\n", argv[i]);
        } else {
            if (headers) out_printf(&sh->out, "%s%s:\n", i > first ? "\n" : "", argv[i]);
            status |= ls_dir(sh, argv[i], s_a, flags);
        }
    }
    return status;
}

/* ---- cat -------------------------------------------------------------- */

static int cat_in(shell_t *sh, const input_t *in, const char *shown) {
    for (;;) {
        UINT n;
        FRESULT r = input_read(in, s_buf, sizeof s_buf, &n);
        if (r != FR_OK) {
            shell_error(sh, "cat: %s: %s\n", shown, files_why(r));
            return 1;
        }
        if (n == 0) return 0;
        out_write(&sh->out, (const char *)s_buf, n);
        if (shell_interrupted(sh)) return 1;
    }
}

FRESULT files_open_read(shell_t *sh, const char *cmd, const char *arg, FIL *f) {
    if (!resolve(sh, cmd, arg, s_a)) return FR_INVALID_NAME;
    FRESULT r = f_open(f, s_a, FA_READ);
    if (r != FR_OK) {
        /* FatFs opens a directory as no file at all. */
        bool dir = r == FR_NO_FILE && files_is_dir(s_a);
        flavour_file_error(sh, cmd, arg, dir ? "is a directory" : files_why(r));
    }
    return r;
}

int cmd_cat(shell_t *sh, int argc, char **argv) {
    if (argc == 1) {
        if (!input_any(&sh->in)) return usage(sh, "cat file... (or cat < file)");
        return cat_in(sh, &sh->in, "input");
    }
    int status = 0;
    for (int i = 1; i < argc && !sh->stop; i++) {
        if (files_open_read(sh, "cat", argv[i], &s_src) != FR_OK) {
            status = 1;
            continue;
        }
        status |= cat_in(sh, &(input_t){ &s_src, NULL }, argv[i]);
        f_close(&s_src);
    }
    return status;
}

/* ---- cp --------------------------------------------------------------- */

const char FILES_STOPPED[] = "stopped";

const char *files_copy(shell_t *sh, const char *src, const char *dst, const char **which) {
    *which = src;
    FRESULT r = f_open(&s_src, src, FA_READ);
    if (r != FR_OK) return files_why(r);
    *which = dst;
    const char *err = files_safe_open(&s_copy, dst);
    if (err) {
        f_close(&s_src);
        return err;
    }
    for (;;) {
        UINT n;
        r = f_read(&s_src, s_buf, sizeof s_buf, &n);
        if (r != FR_OK) {
            f_close(&s_src);
            files_safe_abort(&s_copy);
            *which = src;
            return files_why(r);
        }
        if (n == 0) break;
        files_safe_write(&s_copy, s_buf, n);
        if (s_copy.why) break;
        if (shell_interrupted(sh)) {
            /* Stopped: nothing half-copied is put in place. */
            f_close(&s_src);
            files_safe_abort(&s_copy);
            return FILES_STOPPED;
        }
    }
    f_close(&s_src);
    return files_safe_commit(&s_copy);
}

/* One file to `dst`, both absolute, through dst.new. */
static int copy_file(shell_t *sh, const char *src, const char *dst) {
    if (path_same(src, dst)) {
        shell_error(sh, "cp: %s and %s are the same file\n", src, dst);
        return 1;
    }
    const char *which;
    const char *err = files_copy(sh, src, dst, &which);
    if (err && err != FILES_STOPPED) shell_error(sh, "cp: %s: %s\n", which, err);
    return err ? 1 : 0;
}

/* mkdir that accepts a directory already there. */
static FRESULT make_dir(const char *path) {
    FRESULT r = f_mkdir(path);
    if (r == FR_EXIST) return files_is_dir(path) ? FR_OK : FR_EXIST;
    return r;
}

/* Append `name` to a walked path whose length at this level is *len. */
static bool walk_join(char *buf, size_t len, const char *name) {
    size_t n = strlen(name);
    if (len + 1u + n > SHELL_PATH_MAX) return false;
    buf[len] = '/';
    memcpy(buf + len + 1, name, n + 1);
    return true;
}

/* cp -r: src's tree as dst, made or merged into. Both absolute. */
static int copy_tree(shell_t *sh, const char *src, const char *dst) {
    if (path_within(dst, src)) {
        shell_error(sh, "cp: cannot copy %s into itself\n", src);
        return 1;
    }
    FRESULT r = make_dir(dst);
    if (r != FR_OK) {
        shell_error(sh, "cp: %s: %s\n", dst, r == FR_EXIST ? "not a directory" : files_why(r));
        return 1;
    }
    strcpy(s_wsrc, src);
    strcpy(s_wdst, dst);
    int status = 0;
    unsigned depth = 0;
    s_srclen[0] = strlen(s_wsrc);
    s_dstlen[0] = strlen(s_wdst);
    r = f_opendir(&s_dirs[0], s_wsrc);
    if (r != FR_OK) {
        shell_error(sh, "cp: %s: %s\n", src, files_why(r));
        return 1;
    }
    depth = 1;
    while (depth > 0) {
        unsigned top = depth - 1;
        s_wsrc[s_srclen[top]] = '\0';
        s_wdst[s_dstlen[top]] = '\0';
        if (sh->stop || shell_interrupted(sh)) {
            while (depth > 0) f_closedir(&s_dirs[--depth]);
            return 1;
        }
        r = f_readdir(&s_dirs[top], &s_fno);
        if (r != FR_OK || !s_fno.fname[0]) {
            if (r != FR_OK) {
                shell_error(sh, "cp: %s: %s\n", s_wsrc, files_why(r));
                status = 1;
            }
            f_closedir(&s_dirs[--depth]);
            continue;
        }
        if (!walk_join(s_wsrc, s_srclen[top], s_fno.fname) ||
            !walk_join(s_wdst, s_dstlen[top], s_fno.fname)) {
            shell_error(sh, "cp: %s/%s: name too long\n", s_wsrc, s_fno.fname);
            status = 1;
            continue;
        }
        if (!(s_fno.fattrib & AM_DIR)) {
            status |= copy_file(sh, s_wsrc, s_wdst);
            continue;
        }
        r = make_dir(s_wdst);
        if (r != FR_OK) {
            shell_error(sh, "cp: %s: %s\n", s_wdst, r == FR_EXIST ? "not a directory" : files_why(r));
            status = 1;
            continue;
        }
        if (depth == SHELL_TREE_DEPTH) {
            shell_error(sh, "cp: %s: more than %u levels deep\n", s_wsrc, SHELL_TREE_DEPTH);
            status = 1;
            continue;
        }
        r = f_opendir(&s_dirs[depth], s_wsrc);
        if (r != FR_OK) {
            shell_error(sh, "cp: %s: %s\n", s_wsrc, files_why(r));
            status = 1;
            continue;
        }
        s_srclen[depth] = strlen(s_wsrc);
        s_dstlen[depth] = strlen(s_wdst);
        depth++;
    }
    return status;
}

int cmd_cp(shell_t *sh, int argc, char **argv) {
    static const char use[] = "cp [-r] from... to";
    unsigned flags;
    int first = files_options(sh, argc, argv, "r", &flags, use);
    if (first < 0) return 1;
    if (argc - first < 2) return usage(sh, use);

    const char *to = argv[argc - 1];
    if (!resolve(sh, "cp", to, s_b)) return 1;
    FRESULT r = files_stat(s_b, &s_fno);
    bool to_dir = r == FR_OK && (s_fno.fattrib & AM_DIR);
    if (r != FR_OK && r != FR_NO_FILE) {
        shell_error(sh, "cp: %s: %s\n", to, files_why(r));
        return 1;
    }
    if (argc - first > 2 && !to_dir) {
        shell_error(sh, "cp: %s: not a directory\n", to);
        return 1;
    }

    int status = 0;
    for (int i = first; i < argc - 1 && !sh->stop; i++) {
        if (!resolve(sh, "cp", argv[i], s_a)) {
            status = 1;
            continue;
        }
        r = files_stat(s_a, &s_fno);
        if (r != FR_OK) {
            shell_error(sh, "cp: %s: %s\n", argv[i], files_why(r));
            status = 1;
            continue;
        }
        bool dir = s_fno.fattrib & AM_DIR;
        if (dir && !flags) {
            shell_error(sh, "cp: %s: is a directory (use cp -r)\n", argv[i]);
            status = 1;
            continue;
        }
        if (to_dir) {
            if (!path_join(s_c, s_b, path_base(s_a))) {
                shell_error(sh, "cp: %s: name too long\n", to);
                status = 1;
                continue;
            }
        } else {
            strcpy(s_c, s_b);
        }
        status |= dir ? copy_tree(sh, s_a, s_c) : copy_file(sh, s_a, s_c);
    }
    return status;
}

/* ---- mv --------------------------------------------------------------- */

int cmd_mv(shell_t *sh, int argc, char **argv) {
    if (argc < 3) return usage(sh, "mv from... to");
    const char *to = argv[argc - 1];
    if (!resolve(sh, "mv", to, s_b)) return 1;
    FRESULT r = files_stat(s_b, &s_fno);
    bool to_dir = r == FR_OK && (s_fno.fattrib & AM_DIR);
    if (r != FR_OK && r != FR_NO_FILE) {
        shell_error(sh, "mv: %s: %s\n", to, files_why(r));
        return 1;
    }
    if (argc > 3 && !to_dir) {
        shell_error(sh, "mv: %s: not a directory\n", to);
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc - 1 && !sh->stop; i++) {
        if (!resolve(sh, "mv", argv[i], s_a)) {
            status = 1;
            continue;
        }
        if (strcmp(s_a, "/") == 0) {
            shell_error(sh, "mv: cannot move /\n");
            status = 1;
            continue;
        }
        r = files_stat(s_a, &s_fno);
        if (r != FR_OK) {
            shell_error(sh, "mv: %s: %s\n", argv[i], files_why(r));
            status = 1;
            continue;
        }
        bool dir = s_fno.fattrib & AM_DIR;
        /* `mv name NAME` renames, though FAT finds NAME as name. */
        if (to_dir && !path_same(s_a, s_b)) {
            if (!path_join(s_c, s_b, path_base(s_a))) {
                shell_error(sh, "mv: %s: name too long\n", to);
                status = 1;
                continue;
            }
        } else {
            strcpy(s_c, s_b);
        }
        if (strcmp(s_a, s_c) == 0) {
            shell_error(sh, "mv: %s and %s are the same file\n", argv[i], to);
            status = 1;
            continue;
        }
        if (dir && path_within(s_c, s_a) && !path_same(s_c, s_a)) {
            shell_error(sh, "mv: cannot move %s into itself\n", argv[i]);
            status = 1;
            continue;
        }
        if (!path_same(s_a, s_c)) {
            r = files_stat(s_c, &s_fno);
            if (r == FR_OK) {
                /* A file replaces a file; nothing replaces a directory. */
                if ((s_fno.fattrib & AM_DIR) || dir) {
                    shell_error(sh, "mv: %s: %s\n", s_c,
                                (s_fno.fattrib & AM_DIR) ? "is a directory" : "file exists");
                    status = 1;
                    continue;
                }
                r = f_unlink(s_c);
            } else if (r == FR_NO_FILE) {
                r = FR_OK;
            }
            if (r != FR_OK) {
                shell_error(sh, "mv: %s: %s\n", s_c, files_why(r));
                status = 1;
                continue;
            }
        }
        r = f_rename(s_a, s_c);
        if (r != FR_OK) {
            shell_error(sh, "mv: %s: %s\n", argv[i], files_why(r));
            status = 1;
        }
    }
    return status;
}

/* ---- rm, rmdir -------------------------------------------------------- */

/* rm -r: everything below `path`, then path itself. */
static int remove_tree(shell_t *sh, const char *path) {
    strcpy(s_wsrc, path);
    s_srclen[0] = strlen(s_wsrc);
    FRESULT r = f_opendir(&s_dirs[0], s_wsrc);
    if (r != FR_OK) {
        shell_error(sh, "rm: %s: %s\n", path, files_why(r));
        return 1;
    }
    unsigned depth = 1;
    int status = 0;
    while (depth > 0) {
        unsigned top = depth - 1;
        s_wsrc[s_srclen[top]] = '\0';
        if (sh->stop || shell_interrupted(sh)) {
            while (depth > 0) f_closedir(&s_dirs[--depth]);
            return 1;
        }
        r = f_readdir(&s_dirs[top], &s_fno);
        if (r != FR_OK || !s_fno.fname[0]) {
            f_closedir(&s_dirs[--depth]);
            /* The directory itself, once empty. A failure below it has
             * been reported and leaves it not empty. */
            if (r == FR_OK) r = f_unlink(s_wsrc);
            if (r != FR_OK) {
                if (status == 0) shell_error(sh, "rm: %s: %s\n", s_wsrc, files_why(r));
                status = 1;
            }
            continue;
        }
        if (!walk_join(s_wsrc, s_srclen[top], s_fno.fname)) {
            shell_error(sh, "rm: %s/%s: name too long\n", s_wsrc, s_fno.fname);
            status = 1;
            continue;
        }
        if (!(s_fno.fattrib & AM_DIR)) {
            r = f_unlink(s_wsrc);
            if (r != FR_OK) {
                shell_error(sh, "rm: %s: %s\n", s_wsrc,
                            (s_fno.fattrib & AM_RDO) ? "read-only" : files_why(r));
                status = 1;
            }
            continue;
        }
        if (depth == SHELL_TREE_DEPTH) {
            shell_error(sh, "rm: %s: more than %u levels deep\n", s_wsrc, SHELL_TREE_DEPTH);
            status = 1;
            continue;
        }
        r = f_opendir(&s_dirs[depth], s_wsrc);
        if (r != FR_OK) {
            shell_error(sh, "rm: %s: %s\n", s_wsrc, files_why(r));
            status = 1;
            continue;
        }
        s_srclen[depth] = strlen(s_wsrc);
        depth++;
    }
    return status;
}

/* ---- a walk down a tree, for the DOS flavour ------------------------- */

/* It uses cp -r's and rm -r's stack, and an entry of its own, since the
 * caller may stat or copy between steps. */
static FILINFO  s_wfno;
static unsigned s_wdepth;

FRESULT files_walk(const char *root) {
    files_walk_end();
    strcpy(s_wsrc, root);
    FRESULT r = f_opendir(&s_dirs[0], s_wsrc);
    /* Names are joined after a '/', so the root's is empty. */
    s_srclen[0] = strcmp(root, "/") == 0 ? 0 : strlen(s_wsrc);
    if (r == FR_OK) s_wdepth = 1;
    return r;
}

walk_t files_walk_next(const char **path, const FILINFO **fno, FRESULT *r) {
    *path = s_wsrc;
    *fno = &s_wfno;
    *r = FR_OK;
    if (s_wdepth == 0) return WALK_DONE;
    unsigned top = s_wdepth - 1u;
    s_wsrc[s_srclen[top]] = '\0';
    FRESULT rr = f_readdir(&s_dirs[top], &s_wfno);
    if (rr != FR_OK || !s_wfno.fname[0]) {
        f_closedir(&s_dirs[top]);
        s_wdepth--;
        *r = rr;
        if (!s_wsrc[0]) strcpy(s_wsrc, "/");
        return WALK_LEAVE;
    }
    bool dir = (s_wfno.fattrib & AM_DIR) != 0;
    if (!walk_join(s_wsrc, s_srclen[top], s_wfno.fname)) {
        *r = FR_INVALID_NAME;
        return dir ? WALK_DIR : WALK_FILE;
    }
    if (!dir) return WALK_FILE;
    if (s_wdepth == SHELL_TREE_DEPTH) {
        *r = FILES_TOO_DEEP;
        return WALK_DIR;
    }
    rr = f_opendir(&s_dirs[s_wdepth], s_wsrc);
    if (rr != FR_OK) {
        *r = rr;
        return WALK_DIR;
    }
    s_srclen[s_wdepth++] = strlen(s_wsrc);
    return WALK_DIR;
}

void files_walk_skip(void) {
    if (s_wdepth > 0) f_closedir(&s_dirs[--s_wdepth]);
}

void files_walk_end(void) {
    while (s_wdepth > 0) f_closedir(&s_dirs[--s_wdepth]);
}

int cmd_rm(shell_t *sh, int argc, char **argv) {
    static const char use[] = "rm [-r] path...";
    unsigned flags;
    int first = files_options(sh, argc, argv, "r", &flags, use);
    if (first < 0) return 1;
    if (first == argc) return usage(sh, use);
    int status = 0;
    for (int i = first; i < argc && !sh->stop; i++) {
        if (!resolve(sh, "rm", argv[i], s_a)) {
            status = 1;
            continue;
        }
        if (strcmp(s_a, "/") == 0) {
            shell_error(sh, "rm: refusing to remove /\n");
            status = 1;
            continue;
        }
        FRESULT r = files_stat(s_a, &s_fno);
        if (r == FR_OK && (s_fno.fattrib & AM_DIR)) {
            if (!flags) {
                shell_error(sh, "rm: %s: is a directory (use rm -r)\n", argv[i]);
                status = 1;
            } else {
                status |= remove_tree(sh, s_a);
            }
            continue;
        }
        if (r == FR_OK) r = f_unlink(s_a);
        if (r != FR_OK) {
            shell_error(sh, "rm: %s: %s\n", argv[i],
                        r == FR_DENIED && (s_fno.fattrib & AM_RDO) ? "read-only" : files_why(r));
            status = 1;
        }
    }
    return status;
}

int cmd_rmdir(shell_t *sh, int argc, char **argv) {
    if (argc < 2) return usage(sh, "rmdir dir...");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (!resolve(sh, "rmdir", argv[i], s_a)) {
            status = 1;
            continue;
        }
        FRESULT r = files_stat(s_a, &s_fno);
        const char *err = NULL;
        if (r != FR_OK) err = files_why(r);
        else if (!(s_fno.fattrib & AM_DIR)) err = "not a directory";
        else if (strcmp(s_a, "/") == 0) err = "cannot remove the root";
        else {
            DIR d;
            r = f_opendir(&d, s_a);
            if (r == FR_OK) {
                r = f_readdir(&d, &s_fno);
                f_closedir(&d);
            }
            if (r != FR_OK) err = files_why(r);
            else if (s_fno.fname[0]) err = "directory not empty";
            else if ((r = f_unlink(s_a)) != FR_OK) err = files_why(r);
        }
        if (err) {
            shell_error(sh, "rmdir: %s: %s\n", argv[i], err);
            status = 1;
        }
    }
    return status;
}

/* ---- mkdir, touch ----------------------------------------------------- */

int cmd_mkdir(shell_t *sh, int argc, char **argv) {
    static const char use[] = "mkdir [-p] dir...";
    unsigned flags;
    int first = files_options(sh, argc, argv, "p", &flags, use);
    if (first < 0) return 1;
    if (first == argc) return usage(sh, use);
    int status = 0;
    for (int i = first; i < argc; i++) {
        if (!resolve(sh, "mkdir", argv[i], s_a)) {
            status = 1;
            continue;
        }
        FRESULT r;
        if (strcmp(s_a, "/") == 0) {
            r = flags ? FR_OK : FR_EXIST;
        } else if (!flags) {
            r = f_mkdir(s_a);
        } else {
            /* -p: each directory on the way, and no complaint for one
             * that is there. */
            r = FR_OK;
            for (char *p = s_a + 1; r == FR_OK;) {
                char *e = strchr(p, '/');
                if (e) *e = '\0';
                r = make_dir(s_a);
                if (!e) break;
                *e = '/';
                p = e + 1;
            }
            if (r == FR_EXIST) r = FR_NO_PATH;   /* a file in the way */
        }
        if (r != FR_OK) {
            shell_error(sh, "mkdir: %s: %s\n", argv[i],
                        r == FR_NO_PATH && flags ? "not a directory" : files_why(r));
            status = 1;
        }
    }
    return status;
}

int cmd_touch(shell_t *sh, int argc, char **argv) {
    if (argc < 2) return usage(sh, "touch file...");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (!resolve(sh, "touch", argv[i], s_a)) {
            status = 1;
            continue;
        }
        /* A file already there gets the time now, if the clock is set;
         * otherwise it is left as it is (design.md §3.8). */
        FRESULT r = files_stat(s_a, &s_fno);
        if (r == FR_NO_FILE) {
            r = f_open(&s_src, s_a, FA_CREATE_NEW | FA_WRITE);
            if (r == FR_OK) r = f_close(&s_src);
        } else if (r == FR_OK && strcmp(s_a, "/") != 0) {
            clock_tm_t now;
            if (clock_now(&now)) {
                clock_to_fat(&now, &s_fno.fdate, &s_fno.ftime);
                r = f_utime(s_a, &s_fno);
            }
        }
        if (r != FR_OK) {
            shell_error(sh, "touch: %s: %s\n", argv[i], files_why(r));
            status = 1;
        }
    }
    return status;
}

/* ---- df --------------------------------------------------------------- */

/* Bytes as df -h gives them: "512K", "1.5M", "29G". */
static void human(char *buf, size_t size, uint64_t bytes) {
    static const char units[] = "KMGT";
    uint64_t tenths = (bytes * 10u + 512u) / 1024u;   /* of a KB */
    unsigned u = 0;
    while (tenths >= 10240u && u + 2u < sizeof units) {
        tenths = (tenths + 512u) / 1024u;
        u++;
    }
    if (tenths < 100u)
        snprintf(buf, size, "%lu.%lu%c", (unsigned long)(tenths / 10u),
                 (unsigned long)(tenths % 10u), units[u]);
    else
        snprintf(buf, size, "%lu%c", (unsigned long)((tenths + 5u) / 10u), units[u]);
}

int cmd_df(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc != 1) return usage(sh, "df");
    const char *type;
    uint64_t total, avail;
    FRESULT r = files_space(&type, &total, &avail);
    if (r != FR_OK) {
        shell_error(sh, "df: %s\n", files_why(r));
        return 1;
    }
    uint64_t used = total - avail;
    char a[16], b[16], c[16];
    human(a, sizeof a, total);
    human(b, sizeof b, used);
    human(c, sizeof c, avail);
    unsigned pct = total ? (unsigned)((used * 100u + total - 1u) / total) : 0;
    out_printf(&sh->out, "Type    Size  Used Avail Use%%\n");
    out_printf(&sh->out, "%-5s %6s %5s %5s %3u%%\n", type, a, b, c, pct);
    return 0;
}

FRESULT files_card(char *buf, size_t size) {
    const char *type;
    uint64_t total, avail;
    FRESULT r = files_space(&type, &total, &avail);
    if (r != FR_OK) return r;
    char a[16], c[16];
    human(a, sizeof a, total);
    human(c, sizeof c, avail);
    snprintf(buf, size, "%s, %s, %s free", type, a, c);
    return FR_OK;
}
