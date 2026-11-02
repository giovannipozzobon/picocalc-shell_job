/* edit.c — EDIT (edit.h).
 *
 * Its own key layer and screen over the file code it shares with vi
 * (editor_file.c) and vi's line memo (editor_lines.c). The screen is
 * built as cells, then each row that differs from what the terminal
 * shows is sent, as vi does. A menu or a box is drawn over the text and
 * runs its own loop of keys until it is closed.
 *
 * Everything large is static, as in vi.c: the shell's stack is small
 * (design.md §7). */

#include "edit.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dos.h"
#include "editor_file.h"
#include "editor_lines.h"
#include "files.h"
#include "flavour.h"
#include "keycodes.h"
#include "keys.h"

#define COLS       TERM_COLS
#define MENU_ROW   0
#define TITLE_ROW  1
#define TEXT_ROW   2
#define STATUS_ROW (TEXT_ROW + EDIT_TEXT_ROWS)
_Static_assert(STATUS_ROW + 1 == TERM_ROWS, "the bars and the text fill the screen");

#define ESC     0x1B
#define CTRL(c) ((c) - '@')

/* A cell's style, and a flag for a line-drawing glyph (DEC 0x5F-0x7E). */
enum { ST_TEXT, ST_SEL, ST_BAR, ST_HOT, ST_PICK, ST_STATUS, ST_KEY, ST_N };
#define ST_LINE  0x80u
#define NO_GLYPH 0x7Fu   /* sent as 0x80: the terminal's checkerboard */

/* Each style's SGR: white on blue, or the phosphor on black (Options,
 * Display). */
static const char *const s_sgr[2][ST_N] = {
    { "\033[0;37;44m", "\033[0;34;47m", "\033[0;30;48;5;248m", "\033[0;97;48;5;248m",
      "\033[0;97;40m", "\033[0;30;46m", "\033[0;31;48;5;248m" },
    { "\033[0m", "\033[0;7m", "\033[0;7m", "\033[0;4;7m", "\033[0m", "\033[0;7m", "\033[0;4;7m" },
};

typedef struct {
    uint8_t ch, st;
} cell_t;

/* ---- the menus ---------------------------------------------------------- */

enum {
    C_NONE, C_NEW, C_OPEN, C_SAVE, C_SAVE_AS, C_EXIT, C_CUT, C_COPY, C_PASTE, C_CLEAR,
    C_FIND, C_AGAIN, C_CHANGE, C_DISPLAY, C_HELP,
};

/* An item's label has '&' before its letter. */
typedef struct {
    const char *label, *keys;
    int cmd;
} item_t;

typedef struct {
    const char   *name;   /* its letter is its first */
    const item_t *items;
    int           n;
} menu_t;

static const item_t s_file[] = {
    { "&New", NULL, C_NEW }, { "&Open...", NULL, C_OPEN }, { "&Save", NULL, C_SAVE },
    { "Save &As...", NULL, C_SAVE_AS }, { "E&xit", NULL, C_EXIT },
};
static const item_t s_edit[] = {
    { "Cu&t", "Ctrl+X", C_CUT }, { "&Copy", "Ctrl+C", C_COPY },
    { "&Paste", "Ctrl+V", C_PASTE }, { "Cl&ear", "Del", C_CLEAR },
};
static const item_t s_search[] = {
    { "&Find...", NULL, C_FIND }, { "&Repeat Last Find", "F3", C_AGAIN },
    { "&Change...", NULL, C_CHANGE },
};
static const item_t s_options[] = { { "&Display...", NULL, C_DISPLAY } };
static const item_t s_help[] = { { "&Keys", "F1", C_HELP } };

#define MENUS 5
static const menu_t s_menus[MENUS] = {
    { "File", s_file, 5 }, { "Edit", s_edit, 4 }, { "Search", s_search, 3 },
    { "Options", s_options, 1 }, { "Help", s_help, 1 },
};

/* At most 36 columns, to fit a box. */
static const char *const s_keys_help[] = {
    "Tap Alt          the menu bar",
    "Alt+F,E,S,O,H    a menu",
    "Arrows           move",
    "Ctrl+Left,Right  a word",
    "Shift+Tab        Home: line's start",
    "Shift+Del        End: line's end",
    "Ctrl+Home, End   file's start, end",
    "Shift+Up, Down   PgUp and PgDn",
    "Alt+arrows       select",
    "Ctrl+X, C, V     cut, copy, paste",
    "Del              clear",
    "Ctrl+Y           cut the line",
    "Alt+I (Insert)   insert, overtype",
    "F3               find again",
    "Esc              close a menu, box",
    "Brk (Shift+Esc)  exit",
};
#define KEYS_HELP_LINES (sizeof s_keys_help / sizeof s_keys_help[0])

/* ---- the state ---------------------------------------------------------- */

typedef struct {
    shell_t      *sh;
    char          path[SHELL_PATH_MAX + 1];   /* absolute, when named */
    bool          named;                      /* else UNTITLED */
    editor_file_t file;
    size_t        len, cursor, anchor, copy_len;
    bool          sel;        /* the text from anchor to the cursor is selected */
    bool          modified, overtype, phosphor, done, eof;
    unsigned      goal;       /* the screen column Up and Down keep to */
    int           top;        /* the first line on screen */
    unsigned      left;       /* the first column on screen */
    const char   *msg;        /* the status row's, until the next key */
    EditorLineIndex lines;
} edit_t;

static edit_t ed;
static char   s_buf[VI_BUF_SIZE];
static char   s_copy[VI_COPY_SIZE];
static char   s_find[EDIT_FIND_MAX + 1], s_change[EDIT_FIND_MAX + 1];
static char   s_name[SHELL_PATH_MAX + 1];   /* what is typed in a box */
static char   s_indent[COLS + 1];           /* a line break and its indentation */
static char   s_msg[80];                     /* a message; a box wraps it */
static cell_t s_scr[TERM_ROWS][COLS], s_shown[TERM_ROWS][COLS];
static char   s_out[1024];
static size_t s_out_len;
static bool   s_cursor_on;

/* ---- the buffer --------------------------------------------------------- */

static size_t line_start_at(size_t pos) {
    while (pos > 0 && s_buf[pos - 1] != '\n') pos--;
    return pos;
}

static size_t line_end_at(size_t pos) {
    while (pos < ed.len && s_buf[pos] != '\n') pos++;
    return pos;
}

/* `del` bytes at pos give way to `n` bytes of text, which must not be in
 * the buffer. False, having changed nothing, if they would not fit. */
static bool splice(size_t pos, size_t del, const char *text, size_t n) {
    if (del == 0 && n == 0) return true;
    if (ed.len - del + n >= VI_BUF_SIZE) {
        ed.msg = "The file would be too large";
        return false;
    }
    editor_lines_edit(&ed.lines, pos);
    memmove(&s_buf[pos + n], &s_buf[pos + del], ed.len - pos - del);
    if (n) memcpy(&s_buf[pos], text, n);
    ed.len = ed.len - del + n;
    s_buf[ed.len] = '\0';
    ed.modified = true;
    return true;
}

static bool selection(size_t *lo, size_t *hi) {
    if (!ed.sel || ed.anchor == ed.cursor) return false;
    *lo = ed.anchor < ed.cursor ? ed.anchor : ed.cursor;
    *hi = ed.anchor < ed.cursor ? ed.cursor : ed.anchor;
    return true;
}

/* The selection deleted; false if there was none. */
static bool delete_selection(void) {
    size_t lo, hi;
    if (!selection(&lo, &hi)) {
        ed.sel = false;
        return false;
    }
    splice(lo, hi - lo, NULL, 0);
    ed.cursor = lo;
    ed.sel = false;
    return true;
}

static bool copy_range(size_t lo, size_t hi) {
    if (hi - lo > sizeof s_copy) {
        ed.msg = "Too much: the clipboard holds 8 KB";
        return false;
    }
    memcpy(s_copy, &s_buf[lo], hi - lo);
    ed.copy_len = hi - lo;
    return true;
}

static void cut(bool keep) {
    size_t lo, hi;
    if (!selection(&lo, &hi) || !copy_range(lo, hi)) return;
    if (!keep) delete_selection();
}

static void paste(void) {
    if (ed.copy_len == 0) return;
    size_t at = ed.cursor, lo, hi, del = 0;
    if (selection(&lo, &hi)) {
        at = lo;
        del = hi - lo;
    }
    if (splice(at, del, s_copy, ed.copy_len)) {
        ed.cursor = at + ed.copy_len;
        ed.sel = false;
    }
}

/* Ctrl+Y: the cursor's line and its line break, to the clipboard. */
static void cut_line(void) {
    size_t start = line_start_at(ed.cursor), end = line_end_at(ed.cursor);
    if (end - start + 1 > sizeof s_copy) {
        ed.msg = "Too much: the clipboard holds 8 KB";
        return;
    }
    memcpy(s_copy, &s_buf[start], end - start);
    s_copy[end - start] = '\n';
    ed.copy_len = end - start + 1;
    if (end < ed.len) end++;
    else if (start > 0) start--;   /* the last line takes the break before it */
    splice(start, end - start, NULL, 0);
    ed.cursor = line_start_at(start);
    ed.sel = false;
}

static void type(char c) {
    delete_selection();
    size_t del = ed.overtype && ed.cursor < line_end_at(ed.cursor) ? 1 : 0;
    if (splice(ed.cursor, del, &c, 1)) ed.cursor++;
}

/* Enter: a line break, with the line's indentation carried down. */
static void new_line(void) {
    delete_selection();
    size_t start = line_start_at(ed.cursor), n = 1;
    s_indent[0] = '\n';
    while (start + n - 1 < ed.cursor && n < sizeof s_indent &&
           (s_buf[start + n - 1] == ' ' || s_buf[start + n - 1] == '\t')) {
        s_indent[n] = s_buf[start + n - 1];
        n++;
    }
    if (splice(ed.cursor, 0, s_indent, n)) ed.cursor += n;
}

/* ---- columns ------------------------------------------------------------ */

/* How many columns a byte takes at column col: a tab to the next stop, a
 * control character two (^X), any other one. */
static unsigned width_at(char c, unsigned col) {
    unsigned char u = (unsigned char)c;
    if (c == '\t') return VI_TAB_STOP - col % VI_TAB_STOP;
    if (u < 0x20 || u == 0x7F) return 2;
    return 1;
}

static unsigned col_of(size_t pos) {
    unsigned col = 0;
    for (size_t p = line_start_at(pos); p < pos; p++) col += width_at(s_buf[p], col);
    return col;
}

/* The byte of the line at `start` at or before screen column `goal`. */
static size_t pos_at_col(size_t start, unsigned goal) {
    size_t p = start, end = line_end_at(start);
    unsigned col = 0;
    while (p < end) {
        unsigned w = width_at(s_buf[p], col);
        if (col + w > goal) break;
        col += w;
        p++;
    }
    return p;
}

/* The place `delta` lines from the cursor's, at the goal column. */
static size_t lines_away(int delta) {
    size_t p = line_start_at(ed.cursor);
    for (; delta < 0 && p > 0; delta++) p = line_start_at(p - 1);
    for (; delta > 0; delta--) {
        size_t e = line_end_at(p);
        if (e >= ed.len) break;
        p = e + 1;
    }
    return pos_at_col(p, ed.goal);
}

/* Move the cursor; with `extend`, the selection grows with it. */
static void move_to(size_t pos, bool extend) {
    if (extend && !ed.sel) {
        ed.sel = true;
        ed.anchor = ed.cursor;
    } else if (!extend) {
        ed.sel = false;
    }
    ed.cursor = pos;
}

/* ---- finding ------------------------------------------------------------ */

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool match_at(size_t p, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (lower(s_buf[p + i]) != lower(s[i])) return false;
    return true;
}

/* The first match of s_find at or after `from`, wrapping if `wrap`. */
static bool find_from(size_t from, bool wrap, size_t *at) {
    size_t n = strlen(s_find);
    if (n == 0 || n > ed.len) return false;
    for (size_t p = from; p + n <= ed.len; p++)
        if (match_at(p, s_find, n)) return *at = p, true;
    if (wrap)
        for (size_t p = 0; p < from && p + n <= ed.len; p++)
            if (match_at(p, s_find, n)) return *at = p, true;
    return false;
}

/* ---- the screen --------------------------------------------------------- */

static void put(unsigned r, unsigned c, uint8_t ch, uint8_t st) {
    if (r < TERM_ROWS && c < COLS) s_scr[r][c] = (cell_t){ ch, st };
}

static void fill(unsigned r, unsigned c, unsigned n, uint8_t ch, uint8_t st) {
    for (unsigned i = 0; i < n; i++) put(r, c + i, ch, st);
}

/* Text at r, c; returns the column after it. */
static unsigned text(unsigned r, unsigned c, const char *s, uint8_t st) {
    for (; *s; s++) put(r, c++, (uint8_t)*s, st);
    return c;
}

/* A label with '&' before its letter, which is drawn as `hot`. */
static unsigned label(unsigned r, unsigned c, const char *s, uint8_t st, uint8_t hot) {
    for (; *s; s++) {
        if (*s == '&' && s[1]) {
            put(r, c++, (uint8_t)*++s, hot);
            continue;
        }
        put(r, c++, (uint8_t)*s, st);
    }
    return c;
}

static unsigned label_len(const char *s) {
    unsigned n = 0;
    for (; *s; s++) n += *s != '&';
    return n;
}

static char label_key(const char *s) {
    const char *a = strchr(s, '&');
    return a ? lower(a[1]) : 0;
}

/* A box with a border, filled with the bar's style, and a title on its
 * top edge. */
static void box(unsigned r, unsigned c, unsigned h, unsigned w, const char *title) {
    for (unsigned i = 0; i < h; i++) fill(r + i, c, w, ' ', ST_BAR);
    fill(r, c + 1, w - 2, 'q', ST_BAR | ST_LINE);
    fill(r + h - 1, c + 1, w - 2, 'q', ST_BAR | ST_LINE);
    for (unsigned i = 1; i + 1 < h; i++) {
        put(r + i, c, 'x', ST_BAR | ST_LINE);
        put(r + i, c + w - 1, 'x', ST_BAR | ST_LINE);
    }
    put(r, c, 'l', ST_BAR | ST_LINE);
    put(r, c + w - 1, 'k', ST_BAR | ST_LINE);
    put(r + h - 1, c, 'm', ST_BAR | ST_LINE);
    put(r + h - 1, c + w - 1, 'j', ST_BAR | ST_LINE);
    if (title && *title) {
        unsigned n = (unsigned)strlen(title) + 2;
        unsigned at = c + (w - n) / 2;
        put(r, at, ' ', ST_BAR);
        text(r, at + 1, title, ST_BAR);
        put(r, at + n - 1, ' ', ST_BAR);
    }
}

static unsigned menu_col(int m) {
    unsigned c = 2;
    for (int i = 0; i < m; i++) c += (unsigned)strlen(s_menus[i].name) + 2;
    return c;
}

/* The menu bar, with menu `open` shown chosen (-1 for none). */
static void compose_bar(int open) {
    fill(MENU_ROW, 0, COLS, ' ', ST_BAR);
    for (int m = 0; m < MENUS; m++) {
        unsigned c = menu_col(m);
        const char *name = s_menus[m].name;
        uint8_t st = m == open ? ST_PICK : ST_BAR;
        put(MENU_ROW, c - 1, ' ', st);
        put(MENU_ROW, c, (uint8_t)name[0], m == open ? ST_PICK : ST_HOT);
        c = text(MENU_ROW, c + 1, name + 1, st);
        put(MENU_ROW, c, ' ', st);
    }
}

static void compose_title(void) {
    fill(TITLE_ROW, 0, COLS, 'q', ST_TEXT | ST_LINE);
    char name[24];
    if (!ed.named) {
        strcpy(name, "UNTITLED");
    } else {
        const char *base = strrchr(ed.path, '/');
        base = base && base[1] ? base + 1 : ed.path;
        size_t i = 0;
        for (; base[i] && i + 1 < sizeof name; i++)
            name[i] = base[i] >= 'a' && base[i] <= 'z' ? (char)(base[i] - 'a' + 'A') : base[i];
        name[i] = '\0';
    }
    unsigned n = (unsigned)strlen(name) + 2;
    unsigned at = (COLS - n) / 2;
    put(TITLE_ROW, at, ' ', ST_SEL);
    text(TITLE_ROW, at + 1, name, ST_SEL);
    put(TITLE_ROW, at + n - 1, ' ', ST_SEL);
}

/* The text rows; the cursor's place on screen in *crow, *ccol. */
static void compose_text(unsigned *crow, unsigned *ccol) {
    int line = editor_lines_at_pos(&ed.lines, s_buf, ed.len, ed.cursor);
    int last = editor_lines_at_pos(&ed.lines, s_buf, ed.len, ed.len);
    if (ed.top > last) ed.top = last;
    if (line < ed.top) ed.top = line;
    if (line >= ed.top + EDIT_TEXT_ROWS) ed.top = line - EDIT_TEXT_ROWS + 1;
    unsigned col = col_of(ed.cursor);
    if (col < ed.left) ed.left = col;
    if (col >= ed.left + COLS) ed.left = col - COLS + 1;
    *crow = TEXT_ROW + (unsigned)(line - ed.top);
    *ccol = col - ed.left;

    size_t lo = 0, hi = 0;
    selection(&lo, &hi);
    for (int r = 0; r < EDIT_TEXT_ROWS; r++) {
        unsigned row = TEXT_ROW + (unsigned)r;
        fill(row, 0, COLS, ' ', ST_TEXT);
        if (ed.top + r > last) continue;
        size_t p = editor_lines_start(&ed.lines, s_buf, ed.len, ed.top + r);
        size_t end = line_end_at(p);
        unsigned c = 0;
        for (; p < end && c < ed.left + COLS; p++) {
            unsigned char u = (unsigned char)s_buf[p];
            uint8_t st = lo <= p && p < hi ? ST_SEL : ST_TEXT;
            unsigned w = width_at((char)u, c);
            for (unsigned i = 0; i < w; i++) {
                uint8_t g = u == '\t'        ? ' '
                          : w == 2 && i == 0 ? '^'
                          : w == 2           ? (u == 0x7F ? '?' : (uint8_t)(u ^ 0x40))
                          : u >= 0x80        ? NO_GLYPH
                                             : u;
                if (c + i >= ed.left) put(row, c + i - ed.left, g, st);
            }
            c += w;
        }
        /* A selected line break shows as one cell after the line. */
        if (p == end && end < ed.len && lo <= end && end < hi && c >= ed.left)
            put(row, c - ed.left, ' ', ST_SEL);
    }
}

/* The message or the keys, and the ruler. The ruler goes over the keys
 * if it must, but a message too long for both hides it until the next
 * key. */
static void compose_status(void) {
    fill(STATUS_ROW, 0, COLS, ' ', ST_STATUS);
    unsigned end = text(STATUS_ROW, 1, ed.msg ? ed.msg : "F1=Help  Alt=Menu", ST_STATUS);
    char ruler[32];
    int line = editor_lines_at_pos(&ed.lines, s_buf, ed.len, ed.cursor);
    int n = snprintf(ruler, sizeof ruler, "%s Line:%d Col:%u", ed.overtype ? "OVR" : "   ", line + 1,
                     col_of(ed.cursor) + 1);
    if (n > 0 && (unsigned)n + 2 < COLS && (!ed.msg || end + 1 + (unsigned)n < COLS))
        text(STATUS_ROW, COLS - 1 - (unsigned)n, ruler, ST_STATUS);
}

/* The whole screen as EDIT shows it with nothing open over the text. */
static void compose(unsigned *crow, unsigned *ccol) {
    compose_bar(-1);
    compose_title();
    compose_text(crow, ccol);
    compose_status();
}

static void flush(void) {
    out_write(&ed.sh->con, s_out, s_out_len);
    s_out_len = 0;
}

static void add(const char *s, size_t n) {
    if (s_out_len + n > sizeof s_out) flush();
    memcpy(s_out + s_out_len, s, n);
    s_out_len += n;
}

static void add_str(const char *s) {
    add(s, strlen(s));
}

/* Row r, if it differs from what the terminal shows. The status row's
 * last cell is never written, where the terminal would wrap and scroll;
 * an erase to the end of the row colours it. */
static void send_row(unsigned r) {
    if (memcmp(s_shown[r], s_scr[r], sizeof s_scr[r]) == 0) return;
    memcpy(s_shown[r], s_scr[r], sizeof s_scr[r]);
    unsigned n = r == STATUS_ROW ? COLS - 1 : COLS;
    char pos[16];
    add(pos, (size_t)snprintf(pos, sizeof pos, "\033[%u;1H", r + 1));
    int st = -1;
    bool line = false;
    for (unsigned c = 0; c < n; c++) {
        cell_t cell = s_scr[r][c];
        int want = cell.st & ~ST_LINE;
        if (want != st) {
            add_str(s_sgr[ed.phosphor][want]);
            st = want;
        }
        bool l = (cell.st & ST_LINE) != 0;
        if (l != line) {
            add_str(l ? "\033(0" : "\033(B");
            line = l;
        }
        char ch = cell.ch == NO_GLYPH ? (char)0x80 : (char)cell.ch;
        add(&ch, 1);
    }
    if (line) add_str("\033(B");
    if (n < COLS) {
        add_str(s_sgr[ed.phosphor][s_scr[r][n].st & ~ST_LINE]);
        add_str("\033[K");
    }
}

/* Send what changed, and put the cursor at crow, ccol; a crow past the
 * screen hides it. */
static void show(unsigned crow, unsigned ccol) {
    for (unsigned r = 0; r < TERM_ROWS; r++) send_row(r);
    bool on = crow < TERM_ROWS;
    if (on) {
        char pos[16];
        add(pos, (size_t)snprintf(pos, sizeof pos, "\033[%u;%uH", crow + 1, ccol + 1));
    }
    if (on != s_cursor_on) add_str(on ? "\033[?25h" : "\033[?25l");
    s_cursor_on = on;
    flush();
}

static void redraw_all(void) {
    memset(s_shown, 0xFF, sizeof s_shown);
}

/* ---- keys, menus and boxes ---------------------------------------------- */

static int key(void) {
    if (ed.eof) return -1;
    int k = shell_getkey(ed.sh);
    if (k < 0) ed.eof = true;
    if (k == KEY_ESC) k = ESC;
    return k;
}

/* Menu `m` open until an item is chosen (its command) or it is closed
 * (C_NONE). */
static int menu(int m) {
    int pick = 0;
    for (;;) {
        const menu_t *mu = &s_menus[m];
        unsigned crow, ccol, w = 0;
        compose(&crow, &ccol);
        compose_bar(m);
        for (int i = 0; i < mu->n; i++) {
            unsigned n = label_len(mu->items[i].label);
            if (mu->items[i].keys) n += 3 + (unsigned)strlen(mu->items[i].keys);
            if (n > w) w = n;
        }
        w += 4;
        unsigned c = menu_col(m) - 1;
        if (c + w > COLS) c = COLS - w;
        box(MENU_ROW + 1, c, (unsigned)mu->n + 2, w, NULL);
        for (int i = 0; i < mu->n; i++) {
            unsigned r = MENU_ROW + 2 + (unsigned)i;
            uint8_t st = i == pick ? ST_PICK : ST_BAR;
            fill(r, c + 1, w - 2, ' ', st);
            label(r, c + 2, mu->items[i].label, st, i == pick ? ST_PICK : ST_HOT);
            const char *keys = mu->items[i].keys;
            if (keys) text(r, c + w - 2 - (unsigned)strlen(keys), keys, st);
        }
        show(TERM_ROWS, 0);

        int k = key();
        if (k < 0 || k == ESC || k == KEYS_ALT_TAP || k == KEY_BREAK) return C_NONE;
        if (k == KEY_LEFT || k == KEY_RIGHT) {
            m = (m + (k == KEY_LEFT ? MENUS - 1 : 1)) % MENUS;
            pick = 0;
        } else if (k == KEY_UP || k == KEY_DOWN) {
            pick = (pick + (k == KEY_UP ? mu->n - 1 : 1)) % mu->n;
        } else if (k == '\r') {
            return mu->items[pick].cmd;
        } else if (k & KEYS_ALT) {
            /* Alt still held from opening the menu: an item's letter
             * first, then another menu's. */
            for (int i = 0; i < mu->n; i++)
                if (label_key(mu->items[i].label) == (char)(k & 0xFF)) return mu->items[i].cmd;
            for (int i = 0; i < MENUS; i++)
                if (lower(s_menus[i].name[0]) == (char)(k & 0xFF)) m = i, pick = 0;
        } else if (k < 0x80) {
            for (int i = 0; i < mu->n; i++)
                if (label_key(mu->items[i].label) == lower((char)k)) return mu->items[i].cmd;
        }
    }
}

/* Lines broken at a space, or where they must, to be at most `max`
 * columns: up to WRAP_MAX of them in s_wrap. */
#define WRAP_MAX 24
typedef struct {
    const char *s;
    unsigned    n;
} span_t;
static span_t s_wrap[WRAP_MAX];

static unsigned wrap(const char *const *lines, unsigned n, unsigned max) {
    unsigned k = 0;
    for (unsigned i = 0; i < n && k < WRAP_MAX; i++) {
        const char *s = lines[i];
        size_t left = strlen(s);
        do {
            size_t len = left;
            if (len > max) {
                len = max;
                while (len > 0 && s[len] != ' ') len--;
                if (len == 0) len = max;
            }
            s_wrap[k++] = (span_t){ s, (unsigned)len };
            s += len;
            left -= len;
            while (left > 0 && *s == ' ') s++, left--;
        } while (left > 0 && k < WRAP_MAX);
    }
    return k;
}

/* A box of text lines with `buttons` under them, each button's letter
 * marked with '&' and drawn in red; the box's row and column. A line
 * wider than the screen is wrapped. */
static void message_box(const char *title, const char *const *lines, unsigned n,
                        const char *buttons, unsigned *row, unsigned *col) {
    unsigned crow, ccol, w = label_len(buttons);
    compose(&crow, &ccol);
    n = wrap(lines, n, COLS - 4);
    for (unsigned i = 0; i < n; i++)
        if (s_wrap[i].n > w) w = s_wrap[i].n;
    w += 4;
    unsigned h = n + 4, r = (TERM_ROWS - h) / 2, c = (COLS - w) / 2;
    box(r, c, h, w, title);
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j < s_wrap[i].n; j++)
            put(r + 1 + i, c + 2 + j, (uint8_t)s_wrap[i].s[j], ST_BAR);
    fill(r + n + 1, c + 1, w - 2, 'q', ST_BAR | ST_LINE);
    put(r + n + 1, c, 't', ST_BAR | ST_LINE);
    put(r + n + 1, c + w - 1, 'u', ST_BAR | ST_LINE);
    label(r + n + 2, c + (w - label_len(buttons)) / 2, buttons, ST_BAR, ST_KEY);
    *row = r;
    *col = c;
}

/* A question; the letter of the button chosen, in lower case (Enter is
 * the first button), ESC, or -1 at the end of input. */
static int choose(const char *question, const char *buttons) {
    char keys[8];
    unsigned nk = 0;
    for (const char *a = buttons; (a = strchr(a, '&')) && nk + 1 < sizeof keys; a++)
        keys[nk++] = lower(a[1]);
    keys[nk] = '\0';
    for (;;) {
        unsigned r, c;
        message_box(NULL, &question, 1, buttons, &r, &c);
        show(TERM_ROWS, 0);
        int k = key();
        if (k < 0) return -1;
        if (k == ESC || k == KEY_BREAK) return ESC;
        if (k == '\r') return keys[0];
        if (k > 0 && k < 0x80 && strchr(keys, lower((char)k))) return lower((char)k);
    }
}

static void say(const char *what) {
    unsigned r, c;
    message_box(NULL, &what, 1, "< OK >", &r, &c);
    show(TERM_ROWS, 0);
    key();
}

/* A name or a string typed into `buf`, which holds `cap` bytes and starts
 * with what is there, offered: the first letter typed replaces it, and
 * Backspace edits it. False for Esc, or for nothing typed unless
 * `empty` allows it. */
static bool ask(const char *title, const char *prompt, char *buf, size_t cap, bool empty) {
    size_t n = strlen(buf);
    bool offered = n > 0;
    for (;;) {
        unsigned crow, ccol, w = COLS - 8, r = 12, c = 4;
        compose(&crow, &ccol);
        box(r, c, 7, w, title);
        text(r + 2, c + 2, prompt, ST_BAR);
        unsigned fw = w - 4;
        size_t from = n + 1 > fw ? n + 1 - fw : 0;
        fill(r + 3, c + 2, fw, ' ', ST_PICK);
        text(r + 3, c + 2, buf + from, offered ? ST_SEL : ST_PICK);
        text(r + 5, c + 2, "Enter=OK  Esc=Cancel", ST_BAR);
        show(r + 3, c + 2 + (unsigned)(n - from));

        int k = key();
        if (k < 0 || k == ESC || k == KEY_BREAK) return false;
        if (k == '\r') return n > 0 || empty;
        if (offered && k >= 0x20 && k < 0x7F) n = 0;
        offered = false;
        if (k == KEY_BACKSPACE) {
            if (n > 0) buf[--n] = '\0';
        } else if (k >= 0x20 && k < 0x7F && n + 1 < cap) {
            buf[n++] = (char)k;
            buf[n] = '\0';
        }
    }
}

/* ---- the file ----------------------------------------------------------- */

/* The view and the cursor at the top of a buffer of `len` bytes. */
static void start_over(size_t len) {
    ed.len = len;
    s_buf[len] = '\0';
    ed.cursor = ed.anchor = 0;
    ed.sel = ed.modified = false;
    ed.top = 0;
    ed.left = ed.goal = 0;
    editor_lines_reset(&ed.lines);
}

static void untitled(void) {
    start_over(0);
    ed.named = false;
    ed.path[0] = '\0';
    ed.file = (editor_file_t){ .eol = true, .was_empty = true, .is_new = true };
}

/* `name` as typed into the buffer: NULL, or why not. A failed read
 * leaves the buffer as it was, or empty and untitled. */
static const char *open_file(const char *name) {
    static char path[SHELL_PATH_MAX + 1];
    if (!flavour_resolve(path, ed.sh->cwd, name)) return "Invalid path";
    editor_file_t f = { 0 };
    size_t len = ed.len;
    unsigned long bytes;
    const char *why = editor_file_load(path, s_buf, sizeof s_buf, &len, &f, &bytes, s_msg,
                                       sizeof s_msg);
    if (why) {
        if (len != ed.len) untitled();
        return why;
    }
    start_over(len);
    ed.file = f;
    strcpy(ed.path, path);
    ed.named = true;
    if (f.readonly) ed.msg = "Read-only: changes can't be saved";
    return NULL;
}

/* Save, under a new name if `as` or there is none yet. */
static bool save_file(bool as) {
    if (as || !ed.named) {
        static char path[SHELL_PATH_MAX + 1];
        s_name[0] = '\0';
        if (!ask("Save As", "File Name:", s_name, sizeof s_name, false)) return false;
        if (!flavour_resolve(path, ed.sh->cwd, s_name)) {
            say("Invalid path");
            return false;
        }
        static FILINFO fno;
        if (strcmp(path, ed.path) != 0 && files_stat(path, &fno) == FR_OK &&
            choose("The file exists. Overwrite it?", "< &Yes >  < &No >") != 'y')
            return false;
        strcpy(ed.path, path);
        ed.named = true;
        ed.file.readonly = false;
    }
    if (ed.file.readonly) {
        say("The file is read-only; save it under another name");
        return false;
    }
    unsigned long bytes;
    const char *why = editor_file_save(ed.path, s_buf, ed.len, &ed.file, &bytes);
    if (why) {
        snprintf(s_msg, sizeof s_msg, "Not saved: %s", why);
        say(s_msg);
        return false;
    }
    ed.modified = false;
    snprintf(s_msg, sizeof s_msg, "Saved, %lu bytes", bytes);
    ed.msg = s_msg;
    return true;
}

/* Before the text goes: true to go on, having saved if asked. */
static bool may_discard(void) {
    if (!ed.modified) return true;
    int k = choose("The file is not saved. Save it now?", "< &Yes >  < &No >  < &Cancel >");
    if (k == 'y') return save_file(false);
    return k == 'n';
}

/* ---- the commands ------------------------------------------------------- */

static void find_again(void) {
    size_t at, lo, hi;
    size_t from = selection(&lo, &hi) ? hi : ed.cursor;
    if (!find_from(from, true, &at)) {
        say("Match not found");
        return;
    }
    ed.sel = true;
    ed.anchor = at;
    ed.cursor = at + strlen(s_find);
}

static bool ask_find(void) {
    strcpy(s_name, s_find);
    if (!ask("Find", "Find What:", s_name, sizeof s_find, false)) return false;
    strcpy(s_find, s_name);
    return true;
}

static void change(void) {
    if (!ask_find()) return;
    strcpy(s_name, s_change);
    if (!ask("Change", "Change To:", s_name, sizeof s_change, true)) return;
    strcpy(s_change, s_name);
    size_t n = strlen(s_find), m = strlen(s_change), at, from = 0;
    unsigned count = 0;
    while (find_from(from, false, &at) && splice(at, n, s_change, m)) {
        count++;
        from = at + m;
    }
    ed.sel = false;
    if (ed.cursor > ed.len) ed.cursor = ed.len;
    if (count == 0) {
        say("Match not found");
        return;
    }
    snprintf(s_msg, sizeof s_msg, "%u changed", count);
    ed.msg = s_msg;
}

static void keys_help(void) {
    unsigned r, c;
    message_box("Keys", s_keys_help, KEYS_HELP_LINES, "< OK >", &r, &c);
    show(TERM_ROWS, 0);
    key();
}

static void run(int cmd) {
    switch (cmd) {
    case C_NEW:
        if (may_discard()) untitled();
        break;
    case C_OPEN:
        if (!may_discard()) break;
        s_name[0] = '\0';
        if (ask("Open", "File Name:", s_name, sizeof s_name, false)) {
            const char *why = open_file(s_name);
            if (why) say(why);
        }
        break;
    case C_SAVE:
    case C_SAVE_AS:
        save_file(cmd == C_SAVE_AS);
        break;
    case C_EXIT:
        if (may_discard()) ed.done = true;
        break;
    case C_CUT:
    case C_COPY:
        cut(cmd == C_COPY);
        break;
    case C_PASTE:
        paste();
        break;
    case C_CLEAR:
        delete_selection();
        break;
    case C_FIND:
        if (ask_find()) find_again();
        break;
    case C_AGAIN:
        if (s_find[0] || ask_find()) find_again();
        break;
    case C_CHANGE:
        change();
        break;
    case C_DISPLAY: {
        int k = choose("Colours: Blue, or Phosphor on black?", "< &Blue >  < &Phosphor >");
        if (k == 'b' || k == 'p') {
            ed.phosphor = k == 'p';
            redraw_all();
        }
        break;
    }
    case C_HELP:
        keys_help();
        break;
    default:
        break;
    }
}

/* A key typed into the text. Up and Down keep to the goal column. */
static void text_key(int k) {
    bool vertical = false, alt = (k & KEYS_ALT) != 0;
    switch (k) {
    case KEY_LEFT:
    case KEYS_ALT | KEY_LEFT:
        move_to(ed.cursor > 0 ? ed.cursor - 1 : 0, alt);
        break;
    case KEY_RIGHT:
    case KEYS_ALT | KEY_RIGHT:
        move_to(ed.cursor < ed.len ? ed.cursor + 1 : ed.len, alt);
        break;
    case KEY_UP:
    case KEYS_ALT | KEY_UP:
    case KEY_DOWN:
    case KEYS_ALT | KEY_DOWN:
        move_to(lines_away((k & 0xFF) == KEY_UP ? -1 : 1), alt);
        vertical = true;
        break;
    case KEY_PAGE_UP:
    case KEY_PAGE_DOWN: {
        int d = k == KEY_PAGE_UP ? -EDIT_TEXT_ROWS : EDIT_TEXT_ROWS;
        move_to(lines_away(d), false);
        ed.top = ed.top + d < 0 ? 0 : ed.top + d;
        vertical = true;
        break;
    }
    case KEY_HOME:
        move_to(line_start_at(ed.cursor), false);
        break;
    case KEY_END:
        move_to(line_end_at(ed.cursor), false);
        break;
    case KEYS_CTRL | KEY_HOME:
        move_to(0, false);
        break;
    case KEYS_CTRL | KEY_END:
        move_to(ed.len, false);
        break;
    case KEYS_CTRL | KEY_LEFT:
        move_to(editor_word_left(s_buf, ed.cursor), false);
        break;
    case KEYS_CTRL | KEY_RIGHT:
        move_to(editor_word_right(s_buf, ed.len, ed.cursor), false);
        break;
    case KEY_INSERT:
        ed.overtype = !ed.overtype;
        break;
    case '\r':
        new_line();
        break;
    case KEY_BACKSPACE:
        if (!delete_selection() && ed.cursor > 0 && splice(ed.cursor - 1, 1, NULL, 0)) ed.cursor--;
        break;
    case KEY_DEL:
        if (!delete_selection() && ed.cursor < ed.len) splice(ed.cursor, 1, NULL, 0);
        break;
    case KEY_TAB:
        type('\t');
        break;
    case CTRL('Y'):
        cut_line();
        break;
    case ESC:
        ed.sel = false;
        break;
    default:
        if (k >= 0x20 && k < 0x7F) type((char)k);
        break;
    }
    if (!vertical) ed.goal = col_of(ed.cursor);
}

/* ---- the command -------------------------------------------------------- */

int cmd_edit(shell_t *sh, int argc, char **argv) {
    if (dos_too_many(sh, argc, argv, 2)) return 1;
    if (!sh->hooks.getkey) {
        shell_error(sh, "EDIT needs a keyboard\n");
        return 1;
    }
    memset(&ed, 0, sizeof ed);
    ed.sh = sh;
    untitled();
    if (argc == 2) {
        const char *why = open_file(argv[1]);
        if (why) {
            flavour_file_error(sh, "EDIT", argv[1], why);
            return 1;
        }
    }

    keys_report_chords(true);
    out_puts(&sh->con, "\033[0m\033[2J");
    redraw_all();
    s_cursor_on = false;

    while (!ed.done) {
        unsigned crow, ccol;
        compose(&crow, &ccol);
        show(crow, ccol);
        int k = key();
        if (k < 0) break;   /* the end of input: leave, as vi's :q! */
        ed.msg = NULL;
        int cmd = C_NONE;
        if (k == KEYS_ALT_TAP) {
            cmd = menu(0);
        } else if ((k & KEYS_ALT) && (k & 0xFF) >= 'a' && (k & 0xFF) <= 'z') {
            for (int m = 0; m < MENUS; m++)
                if (lower(s_menus[m].name[0]) == (char)(k & 0xFF)) cmd = menu(m);
        } else if (k == KEY_BREAK) {
            cmd = C_EXIT;
        } else if (k == KEY_F1) {
            cmd = C_HELP;
        } else if (k == KEY_F3) {
            cmd = C_AGAIN;
        } else if (k == CTRL('X') || k == CTRL('C') || k == CTRL('V')) {
            cmd = k == CTRL('X') ? C_CUT : k == CTRL('C') ? C_COPY : C_PASTE;
        } else {
            text_key(k);
        }
        run(cmd);
        if (ed.eof) break;
    }

    keys_report_chords(false);
    out_puts(&sh->con, "\033[0m\033(B\033[2J\033[H\033[?25h");
    return 0;
}
