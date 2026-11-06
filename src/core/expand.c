/* expand.c — a command's words as it runs (expand.h). */

#include "expand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "files.h"
#include "path.h"
#include "vars.h"

/* The word being expanded, as a glob pattern: its active `*`, `?` and
 * `[` as themselves, every other one of those escaped. A word longer
 * than a path is never a glob. */
static char    s_pat[2u * SHELL_PATH_MAX + 2u];
static char    s_word[SHELL_PATH_MAX + 1];   /* a glob's word, kept while it is expanded */
static char    s_dir[SHELL_PATH_MAX + 1];
static FILINFO s_fno;

typedef struct {
    size_t pat_len;
    size_t tslash, pslash;   /* just past the last '/' in the text and the pattern */
    bool   quoted;           /* had quotes, so kept even if empty */
    bool   glob;             /* has an active *, ? or [ */
    bool   glob_dir;         /* ... before a '/', which is not expanded */
    bool   long_;            /* too long for s_pat: not a glob */
} word_t;

/* One character of the word: `active` if it is a glob's. */
static bool emit(expand_t *e, word_t *w, char c, bool active) {
    if (e->used >= sizeof e->text) return false;
    e->text[e->used++] = c;
    if (w->pat_len + 3u > sizeof s_pat) w->long_ = true;
    if (w->long_) return true;
    if (!active && (c == '*' || c == '?' || c == '[' || c == '\\')) s_pat[w->pat_len++] = '\\';
    s_pat[w->pat_len++] = c;
    if (active) w->glob = true;
    if (c == '/') {
        if (w->glob) w->glob_dir = true;
        w->pslash = w->pat_len;
        w->tslash = e->used;   /* relative to the word's start, fixed by the caller */
    }
    return true;
}

static bool emit_str(expand_t *e, word_t *w, const char *s) {
    for (; *s; s++)
        if (!emit(e, w, *s, false)) return false;
    return true;
}

static bool name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* A variable's value; $PWD is always where the shell is. */
static const char *value_of(shell_t *sh, const char *name, size_t len) {
    if (len == 3 && memcmp(name, "PWD", 3) == 0) return sh->cwd;
    return vars_get(name, len);
}

/* `$` at *sp: a variable's value, `$?`, or the `$` itself. */
static bool dollar(shell_t *sh, const char **sp, const char *end, expand_t *e, word_t *w) {
    const char *s = *sp + 1;
    const char *val = NULL;
    char num[12];
    if (s < end && *s == '?') {
        snprintf(num, sizeof num, "%d", sh->status);
        val = num;
        s++;
    } else if (s < end && *s == '{') {
        const char *close = memchr(s, '}', (size_t)(end - s));
        if (close && vars_is_name(s + 1, (size_t)(close - s - 1))) {
            val = value_of(sh, s + 1, (size_t)(close - s - 1));
            s = close + 1;
        }
    } else {
        const char *name = s;
        while (s < end && name_char(*s)) s++;
        if (vars_is_name(name, (size_t)(s - name))) val = value_of(sh, name, (size_t)(s - name));
        else s = name;
    }
    if (s == *sp + 1) {   /* nothing a variable: the '$' as it is */
        *sp = s;
        return emit(e, w, '$', false);
    }
    *sp = s;
    return emit_str(e, w, val ? val : "");
}

/* raw[0..len) into e->text, quotes removed and variables replaced. */
static bool scan(shell_t *sh, const char *raw, size_t len, expand_t *e, word_t *w, bool glob) {
    const char *s = raw, *end = raw + len;
    bool dq = false;
    /* ~ and ~/ at a word's start, unquoted, are $HOME. */
    if (len > 0 && raw[0] == '~' && (len == 1 || raw[1] == '/')) {
        const char *home = vars_get("HOME", 4);
        if (home) {
            if (!emit_str(e, w, home)) return false;
            s++;
        }
    }
    while (s < end) {
        char c = *s;
        if (c == '\'' && !dq) {
            /* The parser has checked that it closes. */
            const char *q = memchr(s + 1, '\'', (size_t)(end - s - 1));
            if (!q) q = end;
            for (s++; s < q; s++)
                if (!emit(e, w, *s, false)) return false;
            s = q < end ? q + 1 : end;
            w->quoted = true;
        } else if (c == '"') {
            dq = !dq;
            s++;
            w->quoted = true;
        } else if (c == '\\' && s + 1 < end) {
            /* In double quotes \ escapes only what is special there. */
            if (dq && s[1] != '"' && s[1] != '\\' && s[1] != '$') {
                if (!emit(e, w, '\\', false)) return false;
                s++;
            } else {
                if (!emit(e, w, s[1], false)) return false;
                s += 2;
            }
        } else if (c == '$') {
            if (!dollar(sh, &s, end, e, w)) return false;
        } else {
            if (!emit(e, w, c, glob && !dq && (c == '*' || c == '?' || c == '['))) return false;
            s++;
        }
    }
    return true;
}

static int name_cmp(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    for (size_t i = 0;; i++) {
        int cx = (unsigned char)x[i], cy = (unsigned char)y[i];
        if (cx >= 'a' && cx <= 'z') cx -= 'a' - 'A';
        if (cy >= 'a' && cy <= 'z') cy -= 'a' - 'A';
        if (cx != cy) return cx - cy;
        if (!cx) return strcmp(x, y);
    }
}

static bool too_many(shell_t *sh) {
    shell_error(sh, "shell: too many arguments\n");
    return false;
}

static bool too_long(shell_t *sh) {
    shell_error(sh, "shell: arguments too long\n");
    return false;
}

/* The word at e->text + start as a glob: the names that match, added to
 * argv and sorted. 0 if none does, with the word left as it was; -1,
 * with the error printed, if they do not fit. */
static int glob(shell_t *sh, expand_t *e, size_t start, const word_t *w) {
    size_t wlen = strlen(e->text + start);
    memcpy(s_word, e->text + start, wlen + 1);
    size_t plen = w->tslash;   /* the directory as typed, its '/' included */
    char keep = s_word[plen];
    s_word[plen] = '\0';
    bool ok = path_resolve(s_dir, sh->cwd, plen ? s_word : ".");
    s_word[plen] = keep;
    DIR d;
    if (!ok || f_opendir(&d, s_dir) != FR_OK) return 0;

    const char *bpat = s_pat + w->pslash;
    bool dot = bpat[0] == '.';
    int first = e->argc, n = 0;
    e->used = start;
    while (f_readdir(&d, &s_fno) == FR_OK && s_fno.fname[0]) {
        const char *name = s_fno.fname;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (files_hidden(&s_fno) && !(dot && name[0] == '.')) continue;
        if (!path_match(bpat, name)) continue;
        size_t nl = strlen(name);
        if (e->argc == (int)SHELL_ARGS_MAX || e->used + plen + nl + 1u > sizeof e->text) {
            f_closedir(&d);
            return e->argc == (int)SHELL_ARGS_MAX ? (too_many(sh), -1) : (too_long(sh), -1);
        }
        e->argv[e->argc++] = e->text + e->used;
        memcpy(e->text + e->used, s_word, plen);
        memcpy(e->text + e->used + plen, name, nl + 1);
        e->used += plen + nl + 1u;
        n++;
    }
    f_closedir(&d);
    if (n == 0) {
        memcpy(e->text + start, s_word, wlen + 1);
        e->used = start + wlen + 1u;
        return 0;
    }
    qsort(&e->argv[first], (size_t)n, sizeof e->argv[0], name_cmp);
    return n;
}

bool expand_args(shell_t *sh, char *const *raw, expand_t *e) {
    e->argc = 0;
    e->used = 0;
    for (; *raw; raw++) {
        word_t w = { 0 };
        size_t start = e->used;
        if (!scan(sh, *raw, strlen(*raw), e, &w, true) || e->used >= sizeof e->text)
            return too_long(sh);
        e->text[e->used++] = '\0';
        w.tslash = w.tslash ? w.tslash - start : 0;
        s_pat[w.pat_len] = '\0';
        if (e->text[start] == '\0' && !w.quoted) {
            e->used = start;
            continue;
        }
        if (w.glob && !w.glob_dir && !w.long_ && e->used - start <= sizeof s_word) {
            int n = glob(sh, e, start, &w);
            if (n < 0) return false;
            if (n > 0) continue;
        }
        if (e->argc == (int)SHELL_ARGS_MAX) return too_many(sh);
        e->argv[e->argc++] = e->text + start;
    }
    e->argv[e->argc] = NULL;
    return true;
}

char *expand_one(shell_t *sh, const char *raw, size_t len, expand_t *e) {
    word_t w = { 0 };
    size_t start = e->used;
    if (!scan(sh, raw, len, e, &w, false) || e->used >= sizeof e->text) {
        too_long(sh);
        return NULL;
    }
    e->text[e->used++] = '\0';
    return e->text + start;
}

bool expand_is_assignment(const char *raw, const char **eq) {
    const char *e = strchr(raw, '=');
    if (!e || !vars_is_name(raw, (size_t)(e - raw))) return false;
    *eq = e;
    return true;
}
