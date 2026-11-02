/* line.c — the line typed at the prompt (line.h). */

#include "line.h"

#include <stdlib.h>
#include <string.h>

#include "files.h"
#include "flavour.h"
#include "history.h"
#include "keycodes.h"
#include "path.h"
#include "run.h"
#include "vars.h"

/* ---- drawing ---------------------------------------------------------- */

/* Move the terminal's cursor to `to`, counted from the prompt's start. */
static void go(shell_t *sh, size_t to) {
    line_t *l = &sh->line;
    size_t fr = l->cur / TERM_COLS, tr = to / TERM_COLS;
    if (to == l->cur) return;
    if (fr == tr) {
        if (to < l->cur && l->cur - to <= 4u) out_write(&sh->con, "\b\b\b\b", l->cur - to);
        else if (to < l->cur) out_printf(&sh->con, "\033[%uD", (unsigned)(l->cur - to));
        else out_printf(&sh->con, "\033[%uC", (unsigned)(to - l->cur));
        l->cur = to;
        return;
    }
    if (tr < fr) out_printf(&sh->con, "\033[%uA", (unsigned)(fr - tr));
    if (tr > fr) out_printf(&sh->con, "\033[%uB", (unsigned)(tr - fr));
    out_puts(&sh->con, "\r");
    if (to % TERM_COLS) out_printf(&sh->con, "\033[%uC", (unsigned)(to % TERM_COLS));
    l->cur = to;
}

/* Draw the line from `from` to its end, blank what is left of a longer
 * one, and put the cursor back at pos. */
static void redraw(shell_t *sh, size_t from) {
    line_t *l = &sh->line;
    go(sh, l->prompt + from);
    out_write(&sh->con, l->text + from, l->len - from);
    size_t end = l->prompt + l->len;
    static const char blanks[] = "                ";
    for (size_t at = end, old = l->prompt + l->shown; at < old;) {
        size_t n = old - at < sizeof blanks - 1 ? old - at : sizeof blanks - 1;
        out_write(&sh->con, blanks, n);
        at += n;
        end = at;
    }
    l->cur = end;
    l->shown = l->len;
    go(sh, l->prompt + l->pos);
}

/* The prompt and the line again, on a new row, after a listing. */
static void reprompt(shell_t *sh) {
    static char prompt[SHELL_PROMPT_MAX + 1];
    line_t *l = &sh->line;
    flavour_prompt(sh, prompt, sizeof prompt);
    out_puts(&sh->con, prompt);
    out_write(&sh->con, l->text, l->len);
    l->cur = l->prompt + l->len;
    l->shown = l->len;
    go(sh, l->prompt + l->pos);
}

void line_start(shell_t *sh, size_t prompt) {
    line_t *l = &sh->line;
    l->len = l->pos = l->shown = 0;
    l->text[0] = '\0';
    l->prompt = l->cur = prompt;
    l->hist = -1;
    l->tabbed = false;
}

/* ---- editing ---------------------------------------------------------- */

static void insert(shell_t *sh, const char *s, size_t n) {
    line_t *l = &sh->line;
    if (n > SHELL_LINE_MAX - l->len) n = SHELL_LINE_MAX - l->len;
    if (n == 0) return;
    memmove(l->text + l->pos + n, l->text + l->pos, l->len - l->pos + 1);
    memcpy(l->text + l->pos, s, n);
    l->len += n;
    l->pos += n;
    redraw(sh, l->pos - n);
}

/* Delete text[at, at + n), the cursor going to `at`. */
static void erase(shell_t *sh, size_t at, size_t n) {
    line_t *l = &sh->line;
    if (n == 0) return;
    memmove(l->text + at, l->text + at + n, l->len - at - n + 1);
    l->len -= n;
    l->pos = at;
    redraw(sh, at);
}

static void replace_all(shell_t *sh, const char *s) {
    line_t *l = &sh->line;
    size_t n = strlen(s);
    if (n > SHELL_LINE_MAX) n = SHELL_LINE_MAX;
    memcpy(l->text, s, n);
    l->text[n] = '\0';
    l->len = l->pos = n;
    redraw(sh, 0);
}

static void history_move(shell_t *sh, int dir) {
    line_t *l = &sh->line;
    int count = (int)history_count();
    int to = l->hist;
    if (dir < 0) to = l->hist < 0 ? count - 1 : l->hist - 1;
    else if (l->hist >= 0) to = l->hist + 1 < count ? l->hist + 1 : -1;
    else return;
    if (to < 0 && dir < 0) return;   /* no history, or already the oldest */
    if (l->hist < 0) memcpy(l->typed, l->text, l->len + 1);
    l->hist = to;
    replace_all(sh, to < 0 ? l->typed : history_get((unsigned)to));
}

/* ---- completion ------------------------------------------------------- */

static char     s_names[SHELL_COMPLETE_NAMES];   /* packed, each NUL-ended */
static size_t   s_used;
static unsigned s_count;
typedef struct {
    uint16_t name;    /* offset in s_names */
    bool     dir;
} cand_t;
static cand_t   s_ents[SHELL_COMPLETE_MAX];
static char     s_word[SHELL_LINE_MAX + 1];      /* the word, escapes removed */
static char     s_dir[SHELL_PATH_MAX + 1];
static FILINFO  s_fno;

static int fold(int c) {
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

static bool has_prefix(const char *name, const char *prefix, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!name[i] || fold((unsigned char)name[i]) != fold((unsigned char)prefix[i])) return false;
    return true;
}

static const char *ent(unsigned i) {
    return s_names + s_ents[i].name;
}

static void offer(const char *name, size_t n, bool dir) {
    if (s_count == SHELL_COMPLETE_MAX || s_used + n + 1 > sizeof s_names) return;
    for (unsigned i = 0; i < s_count; i++)
        if (strlen(ent(i)) == n && has_prefix(ent(i), name, n)) return;   /* already offered */
    memcpy(s_names + s_used, name, n);
    s_names[s_used + n] = '\0';
    s_ents[s_count].name = (uint16_t)s_used;
    s_ents[s_count++].dir = dir;
    s_used += n + 1;
}

/* As ls sorts: case folded, then as spelled. */
static int ent_cmp(const void *a, const void *b) {
    const char *x = s_names + ((const cand_t *)a)->name;
    const char *y = s_names + ((const cand_t *)b)->name;
    for (size_t i = 0;; i++) {
        int d = fold((unsigned char)x[i]) - fold((unsigned char)y[i]);
        if (d || !x[i]) return d ? d : strcmp(x, y);
    }
}

/* The names in directory `dir` that start with prefix[0..n). With
 * `programs`, only *.uf2 files, offered without the extension. */
static void offer_dir(const char *dir, const char *prefix, size_t n, bool programs) {
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return;
    while (f_readdir(&d, &s_fno) == FR_OK && s_fno.fname[0]) {
        const char *name = s_fno.fname;
        if (files_hidden(&s_fno) && !(n > 0 && prefix[0] == '.' && name[0] == '.')) continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!has_prefix(name, prefix, n)) continue;
        bool dir_ = (s_fno.fattrib & AM_DIR) != 0;
        size_t len = strlen(name);
        if (programs) {
            size_t ext = sizeof SHELL_PROGRAM_EXT - 1;
            if (dir_ || len <= ext || !path_same(name + len - ext, SHELL_PROGRAM_EXT)) continue;
            len -= ext;
        }
        offer(name, len, dir_ && !programs);
    }
    f_closedir(&d);
}

static void offer_commands(shell_t *sh, const char *prefix, size_t n) {
    const char *name;
    for (unsigned i = 0; (name = shell_builtin_at(i)) != NULL; i++)
        if (has_prefix(name, prefix, n)) offer(name, strlen(name), false);
    /* An empty entry is the current directory, as in run.c's search. */
    for (const char *path = vars_get("PATH", 4); path;) {
        const char *colon = strchr(path, FLAVOUR_PATH_SEP);
        size_t len = colon ? (size_t)(colon - path) : strlen(path);
        char entry[SHELL_PATH_MAX + 1];
        if (len <= SHELL_PATH_MAX) {
            memcpy(entry, path, len);
            entry[len] = '\0';
            if (len == 0) strcpy(entry, ".");
            if (flavour_resolve(s_dir, sh->cwd, entry)) offer_dir(s_dir, prefix, n, true);
        }
        path = colon ? colon + 1 : NULL;
    }
}

/* What needs a '\' before it; DOS has no escapes. */
static bool special(char c) {
    return !SHELL_DOS && strchr(" \t'\"\\;&|<>#$*?[", c) != NULL;
}

/* After a directory's name: DOS's separator, or UNIX's. */
static const char k_sep[] = { SHELL_DOS ? '\\' : '/', '\0' };

/* Whether c ends a directory in a path being completed: in DOS, '\',
 * '/' and the ':' of C:. */
static bool is_sep(char c) {
    return c == '/' || (SHELL_DOS && (c == '\\' || c == ':'));
}

/* Put name[from, to) in at the cursor, escaped. */
static void insert_escaped(shell_t *sh, const char *name, size_t from, size_t to) {
    static char buf[2 * SHELL_LINE_MAX];
    size_t n = 0;
    for (size_t i = from; i < to && n + 2 <= sizeof buf; i++) {
        if (special(name[i])) buf[n++] = '\\';
        buf[n++] = name[i];
    }
    insert(sh, buf, n);
}

/* The names, down the columns, as ls shows them, each row short of the
 * last column (files.c). */
static void list(shell_t *sh) {
    size_t wmax = 0;
    for (unsigned i = 0; i < s_count; i++) {
        size_t w = strlen(ent(i)) + s_ents[i].dir;
        if (w > wmax) wmax = w;
    }
    unsigned colw = (unsigned)wmax + 2u;
    unsigned cols = (TERM_COLS + 1u) / colw;
    if (cols == 0) cols = 1;
    unsigned rows = (s_count + cols - 1u) / cols;
    go(sh, sh->line.prompt + sh->line.len);
    out_puts(&sh->con, "\n");
    for (unsigned r = 0; r < rows; r++) {
        for (unsigned i = r; i < s_count; i += rows) {
            out_puts(&sh->con, ent(i));
            size_t w = strlen(ent(i));
            if (s_ents[i].dir) out_puts(&sh->con, k_sep), w++;
            if (i + rows < s_count)
                for (; w < colw; w++) out_puts(&sh->con, " ");
        }
        out_puts(&sh->con, "\n");
    }
    reprompt(sh);
}

static bool word_break(char c) {
    return c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|' || c == '<' || c == '>';
}

static void complete(shell_t *sh) {
    line_t *l = &sh->line;
    /* The word before the cursor, and whether it names a command. */
    size_t start = l->pos;
    while (start > 0 && !(word_break(l->text[start - 1]) &&
                          (SHELL_DOS || start < 2 || l->text[start - 2] != '\\')))
        start--;
    size_t before = start;
    while (before > 0 && (l->text[before - 1] == ' ' || l->text[before - 1] == '\t')) before--;
    bool command = before == 0 || strchr(";&|", l->text[before - 1]) != NULL;

    size_t n = 0, base = 0, raw_base = start;
    for (size_t i = start; i < l->pos; i++) {
        if (!SHELL_DOS && l->text[i] == '\\' && i + 1 < l->pos) i++;
        else if (l->text[i] == '\'' || l->text[i] == '"') continue;   /* quotes are not completed */
        s_word[n++] = l->text[i];
        if (is_sep(l->text[i])) {
            base = n;
            raw_base = i + 1;
        }
    }
    s_word[n] = '\0';

    s_used = 0;
    s_count = 0;
    if (command && base == 0) {
        offer_commands(sh, s_word, n);
    } else {
        char keep = s_word[base];
        s_word[base] = '\0';
        bool ok = flavour_resolve(s_dir, sh->cwd, base ? s_word : ".");
        s_word[base] = keep;
        if (ok) offer_dir(s_dir, s_word + base, n - base, false);
    }
    if (s_count == 0) return;
    qsort(s_ents, s_count, sizeof s_ents[0], ent_cmp);

    /* What every name shares. */
    size_t common = strlen(ent(0));
    for (unsigned i = 1; i < s_count; i++) {
        const char *p = ent(i);
        size_t k = 0;
        while (k < common && p[k] && fold((unsigned char)p[k]) == fold((unsigned char)ent(0)[k])) k++;
        common = k;
    }
    size_t typed = n - base;
    if (s_count > 1 && common == typed) {
        if (l->tabbed) list(sh);
        l->tabbed = true;
        return;
    }
    /* The base as typed goes, and comes back as the card spells it. */
    erase(sh, raw_base, l->pos - raw_base);
    insert_escaped(sh, ent(0), 0, common);
    if (s_count == 1) insert(sh, s_ents[0].dir ? k_sep : " ", 1);
}

/* ---- keys ------------------------------------------------------------- */

bool line_key(shell_t *sh, int key) {
    line_t *l = &sh->line;
    bool tab = key == KEY_TAB;
    if (!tab) l->tabbed = false;
    switch (key) {
    case '\r':
        go(sh, l->prompt + l->len);
        out_puts(&sh->con, "\n");
        return true;
    case 0x03:   /* Ctrl-C: drop the line */
        go(sh, l->prompt + l->len);
        out_puts(&sh->con, "^C\n");
        shell_prompt(sh);
        break;
    case KEY_BACKSPACE:
        if (l->pos > 0) erase(sh, l->pos - 1, 1);
        break;
    case KEY_DEL:
        if (l->pos < l->len) erase(sh, l->pos, 1);
        break;
    case KEY_LEFT:
    case 0x02:
        if (l->pos > 0) go(sh, l->prompt + --l->pos);
        break;
    case KEY_RIGHT:
    case 0x06:
        if (l->pos < l->len) go(sh, l->prompt + ++l->pos);
        break;
    case KEY_HOME:
    case 0x01:
        l->pos = 0;
        go(sh, l->prompt);
        break;
    case KEY_END:
    case 0x05:
        l->pos = l->len;
        go(sh, l->prompt + l->len);
        break;
    case 0x15:   /* Ctrl-U */
        erase(sh, 0, l->pos);
        break;
    case 0x0B:   /* Ctrl-K */
        erase(sh, l->pos, l->len - l->pos);
        break;
    case KEY_UP:
    case KEY_WHEEL_UP:
    case 0x10:
        history_move(sh, -1);
        break;
    case KEY_DOWN:
    case KEY_WHEEL_DOWN:
    case 0x0E:
        history_move(sh, +1);
        break;
    case KEY_TAB:
        complete(sh);
        break;
    case KEY_F3:   /* DOS: the rest of the line before, from the cursor's column */
        if (SHELL_DOS && history_count() > 0) {
            const char *last = history_get(history_count() - 1u);
            size_t n = strlen(last);
            if (n > l->len) {
                l->pos = l->len;
                insert(sh, last + l->len, n - l->len);
            }
        }
        break;
    case 0x1B:   /* DOS: Esc drops what is typed */
        if (SHELL_DOS) erase(sh, 0, l->len);
        break;
    default:
        if (key >= 0x20 && key < 0x7F) {
            char c = (char)key;
            insert(sh, &c, 1);
        }
        break;
    }
    return false;
}

int line_read(shell_t *sh, const char *prompt) {
    if (!sh->at_bol) out_puts(&sh->con, "\n");
    out_puts(&sh->con, prompt);
    line_start(sh, strlen(prompt));
    for (;;) {
        int key = sh->hooks.getkey(sh->hooks.ctx);
        if (key < 0) {
            out_puts(&sh->con, "\n");
            return 0;
        }
        if (key == 0x03 || key == 0x1B || key == KEY_BREAK) {
            line_key(sh, KEY_END);
            sh->stop = true;
            return -1;
        }
        if (line_key(sh, key)) return 1;
    }
}
