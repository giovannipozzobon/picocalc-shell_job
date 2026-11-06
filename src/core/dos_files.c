/* dos_files.c — the DOS flavour's file commands (dos.h; design.md
 * §4.7): CD, MD, RD, DIR, COPY, XCOPY, DEL, DELTREE, REN, MOVE, ATTRIB
 * and VOL.
 *
 * They work on the card as the UNIX flavour's do, through files.h: a
 * copy goes through name.new (EL §8.6), and a walk down a tree uses
 * files.c's stack. What differs is DOS's: its wildcards, which each
 * command expands itself (dos_match), its switches, and its words.
 * Everything large is static, as in files.c: the stack is small. */

#include <stdio.h>
#include <string.h>

#include "clock.h"
#include "dos.h"
#include "files.h"
#include "line.h"
#include "path.h"

static DIR     s_dir;              /* the directory being listed or matched */
static FILINFO s_ent;              /* its entry */
static FILINFO s_fno;              /* an entry looked up by name */
static char    s_path[SHELL_PATH_MAX + 1];   /* the entry's whole path */
static char    s_a[SHELL_PATH_MAX + 1], s_b[SHELL_PATH_MAX + 1];
static char    s_to[SHELL_PATH_MAX + 1];

static char fold(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* An absolute path as DOS shows it; two at a time. */
static const char *show(const char *path) {
    static char bufs[2][SHELL_PATH_MAX + 3];
    static unsigned next;
    char *b = bufs[next++ & 1u];
    dos_show(b, sizeof bufs[0], path);
    return b;
}

/* 64 bits in decimal with the country's separator every three digits,
 * as DIR shows them (the board's printf may not do 64 bits). */
static const char *num(uint64_t v) {
    static char bufs[2][32];
    static unsigned next;
    char *b = bufs[next++ & 1u] + 31;
    unsigned digits = 0;
    *b = '\0';
    do {
        if (digits && digits % 3u == 0) *--b = dos_country()->thousands;
        *--b = (char)('0' + v % 10u);
        v /= 10u;
        digits++;
    } while (v);
    return b;
}

/* `arg` made absolute; false after DOS's error. */
static bool where(shell_t *sh, const char *arg, char *out) {
    if (dos_other_drive(arg)) {
        dos_error(sh, "Invalid drive specification", NULL);
        return false;
    }
    if (dos_resolve(out, sh->cwd, arg)) return true;
    const char *slash = strchr(arg, '/');
    if (slash) dos_error(sh, "Invalid switch", slash);
    else dos_error(sh, "Invalid path", arg);
    return false;
}

/* The directory part of an absolute path, into out. */
static void parent(char *out, const char *path) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n == 0) n = 1;
    memmove(out, path, n);
    out[n] = '\0';
}

/* Whether path names a directory, or with a wildcard, can only name
 * entries. */
static bool is_dir(const char *path) {
    return !dos_wild(path_base(path)) && files_is_dir(path);
}

static bool make_dirs(const char *path) {
    static char buf[SHELL_PATH_MAX + 1];
    strcpy(buf, path);
    for (char *p = buf + 1;; p++) {
        if (*p != '/' && *p) continue;
        char keep = *p;
        *p = '\0';
        FRESULT r = f_mkdir(buf);
        if (r != FR_OK && !(r == FR_EXIST && files_is_dir(buf))) return false;
        if (!keep) return true;
        *p = keep;
    }
}

/* ---- matching --------------------------------------------------------- */

/* What a command's operand names: a directory and a name or wildcard in
 * it. A directory named without a wildcard is itself, or with `all`,
 * everything in it (`*.*`). */
typedef struct {
    char dir[SHELL_PATH_MAX + 1];
    char pat[SHELL_PATH_MAX + 1];
    bool wild;
} spec_t;

static bool spec(shell_t *sh, const char *arg, bool all, spec_t *sp) {
    if (!where(sh, arg, sp->dir)) return false;
    if (all && is_dir(sp->dir)) {
        strcpy(sp->pat, "*.*");
    } else if (strcmp(sp->dir, "/") == 0) {
        sp->pat[0] = '\0';
    } else {
        strcpy(sp->pat, path_base(sp->dir));
        parent(sp->dir, sp->dir);
    }
    sp->wild = dos_wild(sp->pat);
    return true;
}

enum {
    EACH_DIRS   = 1u << 0,   /* a wildcard matches directories as well as files */
    EACH_HIDDEN = 1u << 1,   /* ...and hidden and system entries */
};

/* fn for each entry: with a wildcard, the files it matches (and as
 * `what` says, directories, and hidden and system entries); without
 * one, the entry it names, whatever it is. fn gets its whole path and
 * entry, and returns its status, or -1 to stop. The number of entries
 * found, or -1 if the directory could not be read; *status gathers
 * fn's. */
typedef int (*each_fn)(shell_t *sh, const char *path, const FILINFO *fno, void *ctx);

static int each(shell_t *sh, const spec_t *sp, unsigned what, each_fn fn, void *ctx,
                int *status) {
    if (!sp->wild) {
        if (!sp->pat[0]) strcpy(s_path, "/");
        else if (!path_join(s_path, sp->dir, sp->pat)) return 0;
        if (files_stat(s_path, &s_fno) != FR_OK) return 0;
        int st = fn(sh, s_path, &s_fno, ctx);
        if (st > 0) *status |= st;
        return 1;
    }
    FRESULT r = f_opendir(&s_dir, sp->dir);
    if (r != FR_OK) {
        dos_error(sh, dos_why(r), show(sp->dir));
        return -1;
    }
    int found = 0;
    while ((r = f_readdir(&s_dir, &s_ent)) == FR_OK && s_ent.fname[0]) {
        if ((s_ent.fattrib & AM_DIR) && !(what & EACH_DIRS)) continue;
        if (files_hidden(&s_ent) && !(what & EACH_HIDDEN)) continue;
        if (!dos_match(sp->pat, s_ent.fname) &&
            !(s_ent.altname[0] && dos_match(sp->pat, s_ent.altname)))
            continue;
        if (!path_join(s_path, sp->dir, s_ent.fname)) continue;
        found++;
        int st = fn(sh, s_path, &s_ent, ctx);
        if (st < 0) break;
        *status |= st;
        if (shell_interrupted(sh)) break;
    }
    f_closedir(&s_dir);
    if (r != FR_OK) {
        dos_error(sh, dos_why(r), show(sp->dir));
        *status = 1;
    }
    return found;
}

/* ---- CD, MD, RD ------------------------------------------------------- */

int dos_cd(shell_t *sh, int argc, char **argv) {
    if (dos_too_many(sh, argc, argv, 2)) return 1;
    if (argc == 1 || path_same(argv[1], "C:")) {
        out_printf(&sh->out, "%s\n", show(sh->cwd));
        return 0;
    }
    if (!where(sh, argv[1], s_a)) return 1;
    if (!files_is_dir(s_a) || files_chdir(sh, s_a) != NULL) {
        dos_error(sh, "Invalid directory", NULL);
        return 1;
    }
    return 0;
}

int dos_md(shell_t *sh, int argc, char **argv) {
    if (argc < 2) {
        dos_error(sh, "Required parameter missing", NULL);
        return 1;
    }
    if (dos_too_many(sh, argc, argv, 2) || !where(sh, argv[1], s_a)) return 1;
    if (strcmp(s_a, "/") == 0 || f_mkdir(s_a) != FR_OK) {
        dos_error(sh, "Unable to create directory", NULL);
        return 1;
    }
    return 0;
}

int dos_rd(shell_t *sh, int argc, char **argv) {
    if (argc < 2) {
        dos_error(sh, "Required parameter missing", NULL);
        return 1;
    }
    if (dos_too_many(sh, argc, argv, 2) || !where(sh, argv[1], s_a)) return 1;
    if (strcmp(s_a, "/") != 0 && files_is_dir(s_a) && path_within(sh->cwd, s_a)) {
        dos_error(sh, "Attempt to remove current directory", show(s_a));
        return 1;
    }
    /* FatFs refuses a directory that is not empty. */
    if (strcmp(s_a, "/") == 0 || !files_is_dir(s_a) || f_unlink(s_a) != FR_OK) {
        dos_error(sh, "Invalid path, not directory,\nor directory not empty", NULL);
        return 1;
    }
    return 0;
}

/* ---- DIR -------------------------------------------------------------- */

#define DIR_TOTAL "%5u file(s) %14s bytes"   /* its number ends where "%28s bytes free"'s does */
#define DIR_WIDTH TERM_COLS   /* the screen's columns */

enum { DIR_W = 1u << 0, DIR_P = 1u << 1, DIR_S = 1u << 2, DIR_A = 1u << 3, DIR_B = 1u << 4 };

typedef struct {
    unsigned flags;
    const char *pat;
    unsigned rows;          /* rows since the last pause, for /P */
    unsigned col;           /* /W: the column the next name starts at */
    unsigned files;         /* in this directory */
    uint64_t bytes;
    unsigned all_files;     /* in all of them, for /S */
    uint64_t all_bytes;
    bool     stopped;
} dir_t;

/* The end of a row of DIR's output: /P pauses when the screen is full. */
static void dir_row(shell_t *sh, dir_t *d) {
    out_puts(&sh->out, "\n");
    if (!(d->flags & DIR_P) || !dos_paging(sh)) return;
    if (++d->rows < TERM_ROWS - 1u) return;
    d->rows = 0;
    if (!dos_pause(sh)) d->stopped = true;
}

static void dir_text(shell_t *sh, dir_t *d, const char *text) {
    out_puts(&sh->out, text);
    dir_row(sh, d);
}

/* NAME     EXT, 12 columns, as DOS lined them up, for a name that fits
 * 8.3: true. Any other name as it is: false. */
static bool dir_name(const char *name, char *buf, size_t size) {
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext = dot ? strlen(dot + 1) : 0;
    if (base >= 1 && base <= 8 && ext <= 3 && (!dot || strchr(name, '.') == dot)) {
        snprintf(buf, size, "%-8.*s %-3s", (int)base, name, dot ? dot + 1 : "");
        return true;
    }
    snprintf(buf, size, "%s", name);
    return false;
}

/* What follows the name: the size or <DIR> in 11 columns (wider for a
 * file of 1 GB or more), the date in 9 and the time in 7, or 6 on a
 * 24-hour clock (design.md §3.8). */
static void dir_details(shell_t *sh, const FILINFO *fno) {
    if (fno->fattrib & AM_DIR) out_puts(&sh->out, " <DIR>     ");
    else out_printf(&sh->out, "%11s", num(fno->fsize));
    if (!fno->fdate) return;   /* an entry with no date */
    clock_tm_t tm;
    char date[16], time[16];
    clock_from_fat(fno->fdate, fno->ftime, &tm);
    dos_format_date(date, sizeof date, &tm, false);
    dos_format_time(time, sizeof time, &tm, false);
    out_printf(&sh->out, " %s %s", date, time);
}

static bool dir_wants(const dir_t *d, const FILINFO *fno) {
    if (files_hidden(fno) && !(d->flags & DIR_A)) return false;
    return dos_match(d->pat, fno->fname) || (fno->altname[0] && dos_match(d->pat, fno->altname));
}

/* One directory's entries that match: false if there were none. */
static bool dir_list(shell_t *sh, dir_t *d, const char *dir, bool header) {
    FRESULT r = f_opendir(&s_dir, dir);
    bool any = false;
    d->files = 0;
    d->bytes = 0;
    d->col = 0;
    while (!d->stopped && (r = f_readdir(&s_dir, &s_ent)) == FR_OK && s_ent.fname[0]) {
        if (!dir_wants(d, &s_ent)) continue;
        bool sub = (s_ent.fattrib & AM_DIR) != 0;
        if (!any && header) {
            out_printf(&sh->out, " Directory of %s", show(dir));
            dir_row(sh, d);
            dir_row(sh, d);
        }
        any = true;
        if (!sub) {
            d->files++;
            d->bytes += s_ent.fsize;
        }
        if (d->flags & DIR_B) {
            if ((d->flags & DIR_S) && path_join(s_path, dir, s_ent.fname))
                dir_text(sh, d, show(s_path));
            else
                dir_text(sh, d, s_ent.fname);
        } else if (d->flags & DIR_W) {
            char name[SHELL_PATH_MAX + 4];
            snprintf(name, sizeof name, sub ? "[%s]" : "%s", s_ent.fname);
            size_t w = strlen(name);
            size_t cell = (w / 13u + 1u) * 13u;
            if (d->col > 0 && d->col + w >= DIR_WIDTH) {
                dir_row(sh, d);
                d->col = 0;
            }
            out_puts(&sh->out, name);
            d->col += (unsigned)cell;
            if (d->col + 13u > DIR_WIDTH) {
                dir_row(sh, d);
                d->col = 0;
            } else {
                for (; w < cell; w++) out_puts(&sh->out, " ");
            }
        } else {
            /* DOS's row in 39 columns, short of the 40th, where the
             * terminal wraps. A name that is not 8.3, or a size too wide
             * for its field, leaves the rest to the next row, where the
             * size still ends in its column. */
            char name[SHELL_PATH_MAX + 16];
            bool fits = dir_name(s_ent.fname, name, sizeof name) &&
                        (sub || strlen(num(s_ent.fsize)) <= 11u);
            out_puts(&sh->out, name);
            if (!fits) {
                dir_row(sh, d);
                size_t n = sub ? 11u : strlen(num(s_ent.fsize));
                out_printf(&sh->out, "%*s", (int)(n <= 11u ? 12u : 23u - n), "");
            }
            dir_details(sh, &s_ent);
            dir_row(sh, d);
        }
        if (shell_interrupted(sh)) d->stopped = true;
    }
    f_closedir(&s_dir);
    if (d->col > 0) dir_row(sh, d);
    if (r != FR_OK) dos_error(sh, dos_why(r), show(dir));
    if (any && !(d->flags & DIR_B) && !d->stopped) {
        out_printf(&sh->out, DIR_TOTAL, d->files, num(d->bytes));
        dir_row(sh, d);
        if (d->flags & DIR_S) dir_row(sh, d);
    }
    d->all_files += d->files;
    d->all_bytes += d->bytes;
    return any;
}

/* The volume's label and serial number, as VOL and DIR show them. */
static FRESULT volume(shell_t *sh, dir_t *d) {
    char label[24];
    DWORD vsn;
    FRESULT r = f_getlabel("", label, &vsn);
    if (r != FR_OK) return r;
    if (label[0]) out_printf(&sh->out, " Volume in drive C is %s", label);
    else out_puts(&sh->out, " Volume in drive C has no label");
    if (d) dir_row(sh, d);
    else out_puts(&sh->out, "\n");
    out_printf(&sh->out, " Volume Serial Number is %04lX-%04lX", (unsigned long)(vsn >> 16),
               (unsigned long)(vsn & 0xFFFFu));
    if (d) dir_row(sh, d);
    else out_puts(&sh->out, "\n");
    return FR_OK;
}

int dos_dir(shell_t *sh, int argc, char **argv) {
    dir_t d = { 0 };
    if (!dos_switches(sh, &argc, argv, "WPSAB", true, &d.flags)) return 1;
    if (dos_too_many(sh, argc, argv, 2)) return 1;
    spec_t sp;
    if (!spec(sh, argc == 2 ? argv[1] : ".", true, &sp)) return 1;
    /* DIR NAME is DIR NAME.*, as in DOS. */
    if (!strchr(sp.pat, '.') && strlen(sp.pat) + 2u < sizeof sp.pat) strcat(sp.pat, ".*");
    d.pat = sp.pat;
    if (!files_is_dir(sp.dir)) {
        dos_error(sh, "File not found", NULL);
        return 1;
    }
    bool brief = d.flags & DIR_B;
    if (!brief) {
        FRESULT r = volume(sh, &d);
        if (r != FR_OK) {
            dos_error(sh, dos_why(r), NULL);
            return 1;
        }
        if (!(d.flags & DIR_S)) {
            out_printf(&sh->out, " Directory of %s", show(sp.dir));
            dir_row(sh, &d);
            dir_row(sh, &d);
        }
    }
    bool any = dir_list(sh, &d, sp.dir, !brief && (d.flags & DIR_S));
    if (d.flags & DIR_S) {
        FRESULT r = files_walk(sp.dir);
        const char *path;
        const FILINFO *fno;
        walk_t ev;
        while (r == FR_OK && !d.stopped && (ev = files_walk_next(&path, &fno, &r)) != WALK_DONE) {
            if (ev == WALK_DIR && r == FR_OK) {
                if (files_hidden(fno) && !(d.flags & DIR_A)) files_walk_skip();
                else any |= dir_list(sh, &d, path, !brief);
            } else if (r != FR_OK) {
                dos_error(sh, r == FILES_TOO_DEEP ? "Too many levels" : dos_why(r), show(path));
            }
            r = FR_OK;
            if (shell_interrupted(sh)) d.stopped = true;
        }
        files_walk_end();
        if (any && !brief && !d.stopped) {
            dir_text(sh, &d, "Total files listed:");
            out_printf(&sh->out, DIR_TOTAL, d.all_files, num(d.all_bytes));
            dir_row(sh, &d);
        }
    }
    if (!any && !d.stopped) {
        dos_error(sh, "File not found", NULL);
        return 1;
    }
    if (!brief && !d.stopped) {
        const char *type;
        uint64_t total, avail;
        if (files_space(&type, &total, &avail) == FR_OK) {
            out_printf(&sh->out, "%28s bytes free", num(avail));
            dir_row(sh, &d);
        }
    }
    return 0;
}

int dos_vol(shell_t *sh, int argc, char **argv) {
    if (dos_too_many(sh, argc, argv, 2)) return 1;
    if (argc == 2 && !path_same(argv[1], "C:")) {
        dos_error(sh, "Invalid drive specification", NULL);
        return 1;
    }
    out_puts(&sh->out, "\n");
    FRESULT r = volume(sh, NULL);
    if (r != FR_OK) {
        dos_error(sh, dos_why(r), NULL);
        return 1;
    }
    out_puts(&sh->out, "\n");
    return 0;
}

/* ---- COPY ------------------------------------------------------------- */

typedef struct {
    const char *to;     /* the destination, absolute */
    bool to_dir;        /* a directory to copy into */
    const char *mask;   /* or a wildcard for the new names, or NULL */
    bool list;          /* name each file, as DOS did for several */
    int device;         /* or a device, not a file: DEV_CON or DEV_NUL */
    unsigned copied;
} copy_t;

enum { DEV_NONE, DEV_CON, DEV_NUL };

/* The device COPY's operand names: CON or NUL, with DOS's ':' or not.
 * Only COPY's: elsewhere, but for > NUL, these are names of files. */
static int copy_device(const char *arg) {
    if (path_same(arg, "CON") || path_same(arg, "CON:")) return DEV_CON;
    if (path_same(arg, "NUL") || path_same(arg, "NUL:")) return DEV_NUL;
    return DEV_NONE;
}

/* One file to dst, keeping its date and time, as DOS's COPY and XCOPY
 * did; DOS's error and -1 to stop, or 0. */
static int copy_one(shell_t *sh, const char *src, const char *dst) {
    static FILINFO when;
    if (path_same(src, dst)) {
        dos_error(sh, "File cannot be copied onto itself", NULL);
        return -1;
    }
    const char *which;
    const char *why = files_copy(sh, src, dst, &which);
    if (why == FILES_STOPPED) return -1;
    if (why) {
        dos_error(sh, dos_say(why), show(which));
        return -1;
    }
    if (f_stat(src, &when) == FR_OK) f_utime(dst, &when);
    return 0;
}

/* The destination for one source file. */
static bool copy_target(const copy_t *c, const char *name, char *out) {
    if (c->mask) {
        char renamed[SHELL_PATH_MAX + 1];
        static char dir[SHELL_PATH_MAX + 1];
        parent(dir, c->to);
        return dos_rename(renamed, sizeof renamed, c->mask, name) && path_join(out, dir, renamed);
    }
    if (c->to_dir) return path_join(out, c->to, name);
    strcpy(out, c->to);
    return true;
}

static int copy_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    copy_t *c = ctx;
    if (fno->fattrib & AM_DIR) return 0;
    if (!copy_target(c, fno->fname, s_to)) {
        dos_error(sh, "Invalid path", NULL);
        return -1;
    }
    if (c->list) out_printf(&sh->con, "%s\n", fno->fname);
    if (copy_one(sh, path, s_to) < 0) return -1;
    c->copied++;
    return 0;
}

/* COPY file CON and COPY file NUL: read to the screen, or to nothing. */
static int read_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    copy_t *c = ctx;
    if (fno->fattrib & AM_DIR) return 0;
    if (c->list) out_printf(&sh->con, "%s\n", fno->fname);
    const char *why = files_read_out(sh, path, c->device == DEV_CON ? &sh->con : NULL);
    if (why == FILES_STOPPED) return -1;
    if (why) {
        dos_error(sh, dos_say(why), show(path));
        return -1;
    }
    c->copied++;
    return 0;
}

/* COPY CON to `to`, a file through name.new, or a device: lines typed,
 * ended with CR LF, until Ctrl+Z. Stopped, nothing is put in place. COPY
 * NUL has nothing to read, so a file comes out empty. */
static int copy_from_device(shell_t *sh, int from, const char *to, int to_device) {
    static files_safe_t save;
    if (from == DEV_CON && !sh->hooks.getkey) {
        dos_error(sh, "Device not ready", "CON");
        return 1;
    }
    if (!to_device) {
        const char *err = files_safe_open(&save, to);
        if (err) {
            dos_error(sh, dos_say(err), show(to));
            return 1;
        }
    }
    int got = from == DEV_CON ? 1 : 0;
    while (got > 0) {
        got = line_read_text(sh);
        if (got < 0) break;
        const char *text = sh->line.text;
        size_t n = strlen(text);
        if (!to_device) {
            files_safe_write(&save, text, n);
            if (got) files_safe_write(&save, "\r\n", 2);
            if (save.why) break;
        } else if (to_device == DEV_CON && (got || n)) {
            out_printf(&sh->con, "%s\n", text);
        }
    }
    /* Stopped: the shell says ^C, and DOS said nothing more. */
    if (got < 0) {
        if (!to_device) files_safe_abort(&save);
        return 1;
    }
    unsigned copied = 1;
    if (!to_device) {
        const char *err = files_safe_commit(&save);
        if (err) {
            dos_error(sh, dos_say(err), show(to));
            copied = 0;
        }
    }
    out_printf(&sh->out, "%9u file(s) copied\n", copied);
    return copied ? 0 : 1;
}

/* COPY NAME+,, gives each file the time now, as touch does: left as it
 * is with the clock not set (design.md §3.8). */
static int touch_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    static FILINFO when;
    copy_t *c = ctx;
    if (fno->fattrib & AM_DIR) return 0;
    if (c->list) out_printf(&sh->con, "%s\n", fno->fname);
    clock_tm_t now;
    if (clock_now(&now)) {
        when = *fno;
        clock_to_fat(&now, &when.fdate, &when.ftime);
        FRESULT r = f_utime(path, &when);
        if (r != FR_OK) {
            dos_error(sh, dos_why(r), show(path));
            return -1;
        }
    }
    c->copied++;
    return 0;
}

/* The name in COPY's operands if they are NAME+,, (in one word or
 * several), DOS's way to give a file the time now; NULL if not. */
static const char *touch_name(int argc, char **argv) {
    static char name[SHELL_PATH_MAX + 4];
    size_t n = 0;
    for (int i = 1; i < argc; i++) {
        size_t k = strlen(argv[i]);
        if (n + k >= sizeof name) return NULL;
        memcpy(name + n, argv[i], k);
        n += k;
    }
    name[n] = '\0';
    if (n <= 3 || strcmp(name + n - 3, "+,,") != 0) return NULL;
    name[n - 3] = '\0';
    return strchr(name, '+') ? NULL : name;
}

int dos_copy(shell_t *sh, int argc, char **argv) {
    unsigned flags;
    /* /V: every copy is checked; /B: a copy is always the bytes. */
    if (!dos_switches(sh, &argc, argv, "VB", true, &flags)) return 1;
    if (argc < 2) {
        dos_error(sh, "Required parameter missing", NULL);
        return 1;
    }
    spec_t sp;
    const char *touch = touch_name(argc, argv);
    if (touch) {
        if (!spec(sh, touch, false, &sp)) return 1;
        copy_t c = { .list = sp.wild };
        int status = 0;
        int found = each(sh, &sp, 0, touch_each, &c, &status);
        if (found == 0) dos_error(sh, "File not found", touch);
        out_printf(&sh->out, "%9u file(s) copied\n", c.copied);
        return found > 0 && c.copied == (unsigned)found ? status : 1;
    }
    if (dos_too_many(sh, argc, argv, 3)) return 1;
    int from = copy_device(argv[1]);
    int to = argc == 3 ? copy_device(argv[2]) : DEV_NONE;
    if (from && argc == 2) {
        dos_error(sh, "File cannot be copied onto itself", NULL);
        return 1;
    }
    if (from) {
        if (!to && !where(sh, argv[2], s_b)) return 1;
        return copy_from_device(sh, from, s_b, to);
    }
    if (!spec(sh, argv[1], true, &sp)) return 1;
    if (to) {
        copy_t c = { .list = sp.wild, .device = to };
        int status = 0;
        int found = each(sh, &sp, 0, read_each, &c, &status);
        if (found == 0) dos_error(sh, "File not found", argv[1]);
        if (!sh->at_bol) out_puts(&sh->con, "\n");
        out_printf(&sh->out, "%9u file(s) copied\n", c.copied);
        return found > 0 && c.copied == (unsigned)found ? status : 1;
    }
    if (!where(sh, argc == 3 ? argv[2] : ".", s_b)) return 1;
    copy_t c = { .to = s_b, .list = sp.wild };
    if (dos_wild(path_base(s_b))) c.mask = path_base(s_b);
    else c.to_dir = files_is_dir(s_b);
    if (sp.wild && !c.to_dir && !c.mask) {
        dos_error(sh, "Cannot copy several files into one file", NULL);
        return 1;
    }
    int status = 0;
    int found = each(sh, &sp, 0, copy_each, &c, &status);
    if (found == 0) dos_error(sh, "File not found", argv[1]);
    out_printf(&sh->out, "%9u file(s) copied\n", c.copied);
    return found > 0 && c.copied == (unsigned)found ? status : 1;
}

/* ---- XCOPY ------------------------------------------------------------ */

static int xcopy_file(shell_t *sh, copy_t *c, const char *src, const char *rel) {
    if (!path_join(s_to, c->to, rel)) {
        dos_error(sh, "Invalid path", NULL);
        return -1;
    }
    static char dir[SHELL_PATH_MAX + 1];
    parent(dir, s_to);
    if (!make_dirs(dir)) {
        dos_error(sh, "Unable to create directory", show(dir));
        return -1;
    }
    char shown[SHELL_PATH_MAX + 3];
    snprintf(shown, sizeof shown, "%s", rel);
    for (char *s = shown; *s; s++)
        if (*s == '/') *s = '\\';
    out_printf(&sh->con, "%s\n", shown);
    if (copy_one(sh, src, s_to) < 0) return -1;
    c->copied++;
    return 0;
}

static int xcopy_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    if (fno->fattrib & AM_DIR) return 0;
    return xcopy_file(sh, ctx, path, fno->fname);
}

int dos_xcopy(shell_t *sh, int argc, char **argv) {
    unsigned flags;
    if (!dos_switches(sh, &argc, argv, "S", true, &flags)) return 4;
    if (argc < 2) {
        dos_error(sh, "Required parameter missing", NULL);
        return 4;
    }
    if (dos_too_many(sh, argc, argv, 3)) return 4;
    if (!where(sh, argv[1], s_a) || !where(sh, argc == 3 ? argv[2] : ".", s_b)) return 4;
    spec_t sp;
    bool src_dir = is_dir(s_a);
    if (!spec(sh, argv[1], true, &sp)) return 4;
    bool tree = src_dir || sp.wild || flags;
    copy_t c = { .to = s_b, .to_dir = true };
    out_puts(&sh->con, "Reading source file(s)...\n");

    if (!tree) {
        /* One file: into a directory, or as the name given. */
        if (files_is_dir(s_b)) {
            if (!path_join(s_to, s_b, path_base(s_a))) return 4;
        } else {
            strcpy(s_to, s_b);
        }
        if (files_stat(s_a, &s_fno) != FR_OK) {
            dos_error(sh, "File not found", argv[1]);
            out_puts(&sh->out, "0 File(s) copied\n");
            return 1;
        }
        out_printf(&sh->con, "%s\n", s_fno.fname);
        int st = copy_one(sh, s_a, s_to);
        out_printf(&sh->out, "%d File(s) copied\n", st < 0 ? 0 : 1);
        return st < 0 ? 4 : 0;
    }
    if (files_stat(s_b, &s_fno) == FR_OK && !(s_fno.fattrib & AM_DIR)) {
        dos_error(sh, "Cannot copy into a file", show(s_b));
        return 4;
    }
    if (flags && path_within(s_b, sp.dir)) {
        dos_error(sh, "Cannot perform a cyclic copy", NULL);
        return 4;
    }

    int status = 0;
    bool stopped = false;
    if (!flags) {
        if (each(sh, &sp, 0, xcopy_each, &c, &status) < 0) status = 4;
        stopped = sh->stop;
    } else {
        size_t root = strcmp(sp.dir, "/") == 0 ? 1u : strlen(sp.dir) + 1u;
        FRESULT r = files_walk(sp.dir);
        if (r != FR_OK) {
            dos_error(sh, dos_why(r), show(sp.dir));
            return 4;
        }
        const char *path;
        const FILINFO *fno;
        walk_t ev;
        while (!stopped && (ev = files_walk_next(&path, &fno, &r)) != WALK_DONE) {
            if (r != FR_OK) {
                dos_error(sh, r == FILES_TOO_DEEP ? "Too many levels" : dos_why(r), show(path));
                status = 4;
            } else if (ev == WALK_DIR && files_hidden(fno)) {
                files_walk_skip();
            } else if (ev == WALK_FILE && !files_hidden(fno) &&
                       (dos_match(sp.pat, fno->fname) ||
                        (fno->altname[0] && dos_match(sp.pat, fno->altname)))) {
                if (xcopy_file(sh, &c, path, path + root) < 0) stopped = true;
            }
            if (shell_interrupted(sh)) stopped = true;
        }
        files_walk_end();
    }
    if (c.copied == 0 && !stopped && status == 0) {
        dos_error(sh, "File not found", argv[1]);
        status = 1;
    }
    out_printf(&sh->out, "%u File(s) copied\n", c.copied);
    return stopped ? 4 : status;
}

/* ---- DEL -------------------------------------------------------------- */

typedef struct {
    bool ask;
    unsigned deleted;
} del_t;

static int del_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    del_t *d = ctx;
    if (fno->fattrib & AM_DIR) return 0;
    if (d->ask) {
        char q[SHELL_PATH_MAX + 32];
        snprintf(q, sizeof q, "%s,    Delete (Y/N)?", show(path));
        if (!dos_ask(sh, q)) return sh->stop ? -1 : 0;
    }
    if (fno->fattrib & AM_RDO) {
        dos_error(sh, "Access denied", show(path));
        return 1;
    }
    FRESULT r = f_unlink(path);
    if (r != FR_OK) {
        dos_error(sh, dos_why(r), show(path));
        return 1;
    }
    d->deleted++;
    return 0;
}

int dos_del(shell_t *sh, int argc, char **argv) {
    unsigned flags;
    if (!dos_switches(sh, &argc, argv, "P", true, &flags)) return 1;
    if (argc < 2) {
        dos_error(sh, "Required parameter missing", NULL);
        return 1;
    }
    if (dos_too_many(sh, argc, argv, 2)) return 1;
    spec_t sp;
    if (!spec(sh, argv[1], true, &sp)) return 1;
    del_t d = { .ask = flags != 0 };
    if (!d.ask && (strcmp(sp.pat, "*.*") == 0 || strcmp(sp.pat, "*") == 0) &&
        !dos_ask(sh, "All files in directory will be deleted!\nAre you sure (Y/N)?"))
        return 0;
    int status = 0;
    int found = each(sh, &sp, 0, del_each, &d, &status);
    if (found == 0) {
        dos_error(sh, "File not found", NULL);
        return 1;
    }
    return found < 0 ? 1 : status;
}

/* ---- DELTREE ---------------------------------------------------------- */

/* A file or an empty directory, whatever its attributes. */
static FRESULT remove_any(const char *path) {
    f_chmod(path, 0, AM_RDO | AM_HID | AM_SYS);
    return f_unlink(path);
}

static int deltree_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    bool ask = *(bool *)ctx;
    bool dir = (fno->fattrib & AM_DIR) != 0;
    static char target[SHELL_PATH_MAX + 1];
    strcpy(target, path);
    if (strcmp(target, "/") == 0) {
        dos_error(sh, "Cannot delete the root directory", NULL);
        return 1;
    }
    if (dir && path_within(sh->cwd, target)) {
        dos_error(sh, "Attempt to remove current directory", show(target));
        return 1;
    }
    if (ask) {
        char q[SHELL_PATH_MAX + 64];
        snprintf(q, sizeof q, dir ? "Delete directory \"%s\" and all its subdirectories? [yn] "
                                  : "Delete file \"%s\"? [yn] ",
                 fno->fname);
        if (!dos_ask(sh, q)) return sh->stop ? -1 : 0;
    }
    out_printf(&sh->con, "Deleting %s...\n", fno->fname);
    if (!dir) {
        FRESULT r = remove_any(target);
        if (r == FR_OK) return 0;
        dos_error(sh, dos_why(r), show(target));
        return 1;
    }
    int status = 0;
    FRESULT r = files_walk(target);
    const char *p;
    const FILINFO *f;
    walk_t ev;
    while (r == FR_OK && (ev = files_walk_next(&p, &f, &r)) != WALK_DONE) {
        if (r == FR_OK && ev == WALK_FILE) r = remove_any(p);
        else if (r == FR_OK && ev == WALK_LEAVE) r = remove_any(p);
        if (r != FR_OK) {
            dos_error(sh, r == FILES_TOO_DEEP ? "Too many levels" : dos_why(r), show(p));
            status = 1;
            r = FR_OK;
        }
        if (shell_interrupted(sh)) {
            files_walk_end();
            return -1;
        }
    }
    files_walk_end();
    if (r != FR_OK) {
        dos_error(sh, dos_why(r), show(target));
        status = 1;
    }
    return status;
}

int dos_deltree(shell_t *sh, int argc, char **argv) {
    unsigned flags;
    if (!dos_switches(sh, &argc, argv, "Y", true, &flags)) return 1;
    if (argc < 2) {
        dos_error(sh, "Required parameter missing", NULL);
        return 1;
    }
    bool ask = !flags;
    int status = 0;
    for (int i = 1; i < argc && !sh->stop; i++) {
        spec_t sp;
        if (!spec(sh, argv[i], false, &sp)) {
            status = 1;
            continue;
        }
        /* each() lists the directory while deltree_each walks below it:
         * the walk has a stack of its own. */
        int found = each(sh, &sp, EACH_DIRS, deltree_each, &ask, &status);
        if (found < 0) status = 1;
    }
    return status;
}

/* ---- REN -------------------------------------------------------------- */

static int ren_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    const char *mask = ctx;
    char name[SHELL_PATH_MAX + 1];
    if (!dos_rename(name, sizeof name, mask, fno->fname) || !path_join(s_to, s_a, name)) {
        dos_error(sh, "Invalid filename", NULL);
        return 1;
    }
    if (strcmp(path, s_to) == 0) return 0;
    /* `REN readme.txt README.TXT` changes the case; anything else that
     * is there already is in the way. */
    if (!path_same(path, s_to) && files_stat(s_to, &s_fno) == FR_OK) {
        dos_error(sh, "Duplicate file name or file not found", NULL);
        return 1;
    }
    FRESULT r = f_rename(path, s_to);
    if (r != FR_OK) {
        dos_error(sh, dos_why(r), show(path));
        return 1;
    }
    return 0;
}

int dos_ren(shell_t *sh, int argc, char **argv) {
    if (argc < 3) {
        dos_error(sh, "Required parameter missing", NULL);
        return 1;
    }
    if (dos_too_many(sh, argc, argv, 3)) return 1;
    if (strpbrk(argv[2], "\\/:")) {
        dos_error(sh, "Invalid parameter", NULL);
        return 1;
    }
    spec_t sp;
    if (!spec(sh, argv[1], false, &sp)) return 1;
    strcpy(s_a, sp.dir);
    int status = 0;
    int found = each(sh, &sp, 0, ren_each, argv[2], &status);
    if (found == 0) {
        dos_error(sh, "Duplicate file name or file not found", NULL);
        return 1;
    }
    return found < 0 ? 1 : status;
}

/* ---- MOVE ------------------------------------------------------------- */

typedef struct {
    bool to_dir;
    unsigned moved;
} move_t;

static int move_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    move_t *m = ctx;
    bool dir = (fno->fattrib & AM_DIR) != 0;
    if (m->to_dir && !path_same(path, s_b)) {
        if (!path_join(s_to, s_b, fno->fname)) {
            dos_error(sh, "Invalid path", NULL);
            return 1;
        }
    } else {
        strcpy(s_to, s_b);
    }
    static char from[SHELL_PATH_MAX + 1];
    strcpy(from, path);
    const char *why = NULL;
    FRESULT r = FR_OK;
    if (strcmp(from, "/") == 0) {
        why = "Cannot move the root directory";
    } else if (dir && path_within(s_to, from) && !path_same(s_to, from)) {
        why = "Cannot move a directory into itself";
    } else if (!path_same(from, s_to) && files_stat(s_to, &s_fno) == FR_OK) {
        /* A file replaces a file, as DOS 6's MOVE did; nothing replaces
         * a directory. */
        if ((s_fno.fattrib & AM_DIR) || dir) why = "Access denied";
        else r = f_unlink(s_to);
    }
    if (!why && r == FR_OK) r = f_rename(from, s_to);
    if (!why && r != FR_OK) why = dos_why(r);
    out_printf(&sh->out, "%s => %s [%s]\n", show(from), show(s_to), why ? why : "ok");
    if (why) return 1;
    m->moved++;
    return 0;
}

int dos_move(shell_t *sh, int argc, char **argv) {
    if (argc < 3) {
        dos_error(sh, "Required parameter missing", NULL);
        return 1;
    }
    if (!where(sh, argv[argc - 1], s_b)) return 1;
    move_t m = { .to_dir = files_is_dir(s_b) };
    /* Sources may be several words, and each a list with commas. */
    bool many = argc > 3 || strchr(argv[1], ',') || dos_wild(argv[1]);
    if (many && !m.to_dir) {
        dos_error(sh, "Cannot move multiple files to a single file", NULL);
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc - 1 && !sh->stop; i++) {
        for (char *s = argv[i]; s && *s && !sh->stop;) {
            char *comma = strchr(s, ',');
            if (comma) *comma = '\0';
            spec_t sp;
            if (spec(sh, s, false, &sp)) {
                int found = each(sh, &sp, 0, move_each, &m, &status);
                if (found == 0) {
                    dos_error(sh, "File not found", s);
                    status = 1;
                } else if (found < 0) {
                    status = 1;
                }
            } else {
                status = 1;
            }
            s = comma ? comma + 1 : NULL;
        }
    }
    return status;
}

/* ---- ATTRIB ----------------------------------------------------------- */

typedef struct {
    BYTE set, clear;
} attrib_t;

static int attrib_each(shell_t *sh, const char *path, const FILINFO *fno, void *ctx) {
    const attrib_t *a = ctx;
    if (a->set || a->clear) {
        FRESULT r = f_chmod(path, a->set, (BYTE)(a->set | a->clear));
        if (r != FR_OK) {
            dos_error(sh, dos_why(r), show(path));
            return 1;
        }
        return 0;
    }
    BYTE at = fno->fattrib;
    out_printf(&sh->out, "%c  %c%c%c     %s\n", (at & AM_ARC) ? 'A' : ' ', (at & AM_SYS) ? 'S' : ' ',
               (at & AM_HID) ? 'H' : ' ', (at & AM_RDO) ? 'R' : ' ', show(path));
    return 0;
}

int dos_attrib(shell_t *sh, int argc, char **argv) {
    attrib_t a = { 0, 0 };
    const char *arg = NULL;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        static const char letters[] = "RASH";
        static const BYTE bits[] = { AM_RDO, AM_ARC, AM_SYS, AM_HID };
        const char *which = s[1] ? strchr(letters, fold(s[1])) : NULL;
        if ((s[0] == '+' || s[0] == '-') && which && !s[2]) {
            BYTE bit = bits[which - letters];
            if (s[0] == '+') a.set |= bit;
            else a.clear |= bit;
        } else if (s[0] == '+' || s[0] == '-' || s[0] == '/') {
            dos_error(sh, "Parameter format not correct", s);
            return 1;
        } else if (arg) {
            dos_error(sh, "Too many parameters", s);
            return 1;
        } else {
            arg = s;
        }
    }
    spec_t sp;
    if (!spec(sh, arg ? arg : "*.*", false, &sp)) return 1;
    int status = 0;
    int found = each(sh, &sp, EACH_HIDDEN, attrib_each, &a, &status);
    if (found == 0) {
        dos_error(sh, "File not found", arg ? arg : NULL);
        return 1;
    }
    return found < 0 ? 1 : status;
}
