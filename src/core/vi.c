/* vi.c — the editor (vi.h).
 *
 * The key layer (editor_vi.c) turns each key into an action over byte
 * offsets; carrying the actions out is adapted from Pico Logo's editor.c
 * (its editor_vi_apply and the helpers under it, MIT, the owner's;
 * THIRD-PARTY.md), on this file's buffer in place of the Logo editor's.
 * Every change to the buffer is told to the undo journal before it is
 * made and to the line memo after, as there.
 *
 * Everything large is static, as in files.c: the shell's stack is small
 * (design.md §7). */

#include "vi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "editor_file.h"
#include "editor_lines.h"
#include "editor_pattern.h"
#include "editor_undo.h"
#include "editor_vi.h"
#include "keycodes.h"
#include "path.h"

#define STATUS_ROW VI_TEXT_ROWS
_Static_assert(VI_TEXT_ROWS + 1 == TERM_ROWS, "the text rows and the status row fill the screen");

/* A cell as the screen shows it: a glyph, 0x20-0x7E or NO_GLYPH, and
 * REVERSE for the visual selection. */
#define NO_GLYPH 0x7Fu
#define REVERSE  0x80u

typedef struct {
    shell_t    *sh;
    const char *name;                       /* as typed */
    char        path[SHELL_PATH_MAX + 1];   /* absolute */
    size_t      len, cursor;
    size_t      copy_len;
    EditorLineIndex lines;
    EditorUndo  undo;
    ViState     vi;
    editor_file_t file;     /* its line breaks, and read-only */
    int         top;        /* the first line on screen */
    unsigned    left;       /* the first column on screen */
    const char *msg;        /* the status row's message, until the next key */
    bool        wq;         /* the key being carried out ends a :w command */
} vi_t;

static vi_t    ed;
static char    s_buf[VI_BUF_SIZE];
static char    s_copy[VI_COPY_SIZE];
static char    s_undo[VI_UNDO_SIZE];

char *vi_scratch(size_t *size) {
    *size = sizeof s_buf;
    return s_buf;
}
static uint8_t s_shown[TERM_ROWS][TERM_COLS];   /* what the screen shows */
static uint8_t s_row[TERM_COLS];
static char    s_out[TERM_COLS * 6 + 32];       /* one row's escape sequences */
static char    s_msg[TERM_COLS + 1];
static char    s_text[512];                          /* a line's indentation */

/* ---- the buffer --------------------------------------------------------- */

static size_t line_start_at(size_t pos) {
    while (pos > 0 && s_buf[pos - 1] != '\n') pos--;
    return pos;
}

static size_t line_end_at(size_t pos) {
    while (pos < ed.len && s_buf[pos] != '\n') pos++;
    return pos;
}

static bool blank(char c) {
    return c == ' ' || c == '\t';
}

/* The first character after the indent of the line holding pos. */
static size_t first_non_blank(size_t pos) {
    size_t p = line_start_at(pos), end = line_end_at(pos);
    while (p < end && blank(s_buf[p])) p++;
    return p;
}

/* Tell the journal and the memo, then change: `del` bytes at pos give way
 * to `n` bytes of text. False, having changed nothing, if they would not
 * fit. `text` must not point into the buffer. */
static bool splice(size_t pos, size_t del, const char *text, size_t n) {
    if (pos > ed.len) pos = ed.len;
    if (del > ed.len - pos) del = ed.len - pos;
    if (del == 0 && n == 0) return true;
    if (ed.len - del + n >= VI_BUF_SIZE) {
        ed.msg = "E: the file would be too large";
        return false;
    }
    editor_undo_record(&ed.undo, pos, &s_buf[pos], del, text, n);
    editor_lines_edit(&ed.lines, pos);
    memmove(&s_buf[pos + n], &s_buf[pos + del], ed.len - pos - del);
    if (n) memcpy(&s_buf[pos], text, n);
    ed.len = ed.len - del + n;
    s_buf[ed.len] = '\0';
    ed.vi.modified = true;
    return true;
}

static bool insert_text(size_t pos, const char *text, size_t n) {
    return splice(pos, 0, text, n);
}

static void delete_range(size_t start, size_t end) {
    if (end > start) splice(start, end - start, NULL, 0);
}

/* One byte overwritten in place, as `r` and `~` do. */
static void overwrite(size_t pos, char c) {
    if (s_buf[pos] == c) return;
    editor_undo_record(&ed.undo, pos, &s_buf[pos], 1, &c, 1);
    s_buf[pos] = c;
    ed.vi.modified = true;
}

/* ---- the copy buffer -----------------------------------------------------
 * The one register. Linewise text is marked by a trailing newline. */

static void yank_range(size_t start, size_t end, bool linewise) {
    if (end > ed.len) end = ed.len;
    if (start > end) start = end;
    size_t n = end - start;
    if (n + 2 > VI_COPY_SIZE) {
        ed.msg = "E: too much to yank; register unchanged";
        return;
    }
    memcpy(s_copy, &s_buf[start], n);
    if (linewise) {
        /* `dd` on the last line takes the newline before it, since there
         * is none after; the register wants it the other way round. */
        if (n > 0 && s_copy[0] == '\n' && s_copy[n - 1] != '\n') memmove(s_copy, s_copy + 1, --n);
        if (n == 0 || s_copy[n - 1] != '\n') s_copy[n++] = '\n';
    }
    ed.copy_len = n;
}

static void paste(int count, bool after) {
    if (ed.copy_len == 0) {
        ed.msg = "E353: nothing in register";
        return;
    }
    bool linewise = s_copy[ed.copy_len - 1] == '\n';
    size_t at;
    if (linewise) {
        if (after) {
            at = line_end_at(ed.cursor);
            if (at < ed.len) {
                at++;   /* past the newline, onto the next line */
            } else {
                /* The last line has no newline of its own to paste after. */
                if (!insert_text(at, "\n", 1)) return;
                at = ed.len;
            }
        } else {
            at = line_start_at(ed.cursor);
        }
    } else {
        at = ed.cursor;
        if (after && at < line_end_at(ed.cursor)) at++;
    }

    size_t inserted = 0;
    for (int i = 0; i < count; i++) {
        if (!insert_text(at + inserted, s_copy, ed.copy_len)) break;
        inserted += ed.copy_len;
    }
    if (inserted == 0) return;
    ed.cursor = linewise ? first_non_blank(at) : at + inserted - 1;
}

/* ---- the other actions -------------------------------------------------- */

/* Join `count` lines onto the cursor's: one space where each line break
 * was, and the indentation after it swallowed. */
static void join(int count) {
    for (int i = 1; i < count; i++) {
        size_t start = line_start_at(ed.cursor), end = line_end_at(ed.cursor);
        if (end >= ed.len) break;
        size_t next = end + 1;
        while (next < ed.len && blank(s_buf[next])) next++;
        delete_range(end, next);
        ed.cursor = end;
        /* No space onto an empty line, nor where the line ends in one. */
        if (end > start && s_buf[end - 1] != ' ' && end < ed.len && s_buf[end] != '\n')
            insert_text(end, " ", 1);
    }
}

/* Open a line below or above the cursor's, with its indentation, and put
 * the cursor on it. */
static void open_line(bool below) {
    size_t start = line_start_at(ed.cursor), end = line_end_at(ed.cursor);
    size_t indent = 0;
    while (start + indent < end && blank(s_buf[start + indent]) && indent + 1 < sizeof s_text) indent++;
    if (below) {
        s_text[0] = '\n';
        memcpy(s_text + 1, &s_buf[start], indent);
        if (insert_text(end, s_text, indent + 1)) ed.cursor = end + 1 + indent;
    } else {
        memcpy(s_text, &s_buf[start], indent);
        s_text[indent] = '\n';
        if (insert_text(start, s_text, indent + 1)) ed.cursor = start + indent;
    }
}

/* Shift each line the range touches by `stops` shift widths: in by
 * spaces, out by spaces or tabs, a tab counting as a whole width. An
 * empty line is not shifted in. */
static void indent(size_t start, size_t end, int stops) {
    static const char spaces[] = "                                ";
    size_t first = line_start_at(start);
    unsigned width = (unsigned)(stops < 0 ? -stops : stops) * VI_SHIFT_WIDTH;
    for (size_t p = first;;) {
        size_t before = ed.len;
        if (stops > 0) {
            if (p < ed.len && s_buf[p] != '\n') {
                for (unsigned left = width; left > 0;) {
                    unsigned n = left < sizeof spaces - 1 ? left : (unsigned)(sizeof spaces - 1);
                    if (!insert_text(p, spaces, n)) break;
                    left -= n;
                }
            }
        } else {
            size_t q = p;
            unsigned cols = 0;
            while (q < ed.len && cols < width && blank(s_buf[q]))
                cols += s_buf[q++] == '\t' ? VI_SHIFT_WIDTH : 1;
            delete_range(p, q);
        }
        end = end + ed.len - before;   /* the range moved with the change */
        size_t nl = line_end_at(p);
        if (nl >= ed.len || nl + 1 >= end) break;
        p = nl + 1;
    }
    ed.cursor = first_non_blank(first);
}

static void toggle_case(size_t start, size_t end) {
    if (end > ed.len) end = ed.len;
    for (size_t i = start; i < end; i++) {
        char c = s_buf[i];
        if (c >= 'a' && c <= 'z') overwrite(i, (char)(c - 'a' + 'A'));
        else if (c >= 'A' && c <= 'Z') overwrite(i, (char)(c - 'A' + 'a'));
    }
}

/* `/`, `?`, `n`, `N`, `*` and `#`: the key layer's pattern, from
 * `origin`, wrapping, leaving the cursor on the match. */
static void search(char direction, size_t origin) {
    bool forward = direction == '/';
    size_t from = forward ? origin + 1 : origin;
    if (from > ed.len) from = ed.len;
    size_t match;
    bool too_complex = false;
    if (editor_pattern_find(ed.vi.pattern, ed.vi.pattern_len, s_buf, ed.len, from, forward, &match,
                            &too_complex))
        ed.cursor = match;
    else
        ed.msg = too_complex ? "E486: pattern too complex" : "E486: pattern not found";
}

/* `zz`, `zt`, `zb`: the view moves, the cursor does not. */
static void scroll_view(char how) {
    int line = editor_lines_at_pos(&ed.lines, s_buf, ed.len, ed.cursor);
    int last = editor_lines_at_pos(&ed.lines, s_buf, ed.len, ed.len);
    int top = line;
    if (how == 'z') top = line - (VI_TEXT_ROWS - 1) / 2;
    else if (how == 'b') top = line - VI_TEXT_ROWS + 1;
    if (top > last - VI_TEXT_ROWS + 1) top = last - VI_TEXT_ROWS + 1;
    if (top < 0) top = 0;
    ed.top = top;
}

/* ---- the file ----------------------------------------------------------- */

static int count_lines(void) {
    if (ed.len == 0) return ed.file.was_empty ? 0 : 1;
    int n = 1;
    for (size_t i = 0; i < ed.len; i++) n += s_buf[i] == '\n';
    return n;
}

/* `"name" [flags] 3L, 52B` and what follows, on the status row. */
static void report(unsigned long bytes, const char *then) {
    snprintf(s_msg, sizeof s_msg, "\"%s\" %s%s%s%dL, %luB%s", ed.name,
             ed.file.readonly ? "[readonly] " : "", ed.file.crlf ? "[CRLF] " : "",
             ed.file.eol || ed.len == 0 ? "" : "[noeol] ", count_lines(), bytes, then);
    ed.msg = s_msg;
}

/* Read the file into the buffer: NULL, or why it cannot be edited. */
static const char *load(void) {
    unsigned long bytes;
    const char *why = editor_file_load(ed.path, s_buf, sizeof s_buf, &ed.len, &ed.file, &bytes,
                                       s_msg, sizeof s_msg);
    if (why) return why;
    if (ed.file.is_new) {
        snprintf(s_msg, sizeof s_msg, "\"%s\" [New]", ed.name);
        ed.msg = s_msg;
    } else {
        report(bytes, "");
    }
    return NULL;
}

/* Write the buffer back, through name.new. */
static bool save(void) {
    if (ed.file.readonly) {
        ed.msg = "E45: read-only; not written";
        return false;
    }
    unsigned long bytes;
    const char *why = editor_file_save(ed.path, s_buf, ed.len, &ed.file, &bytes);
    if (why) {
        snprintf(s_msg, sizeof s_msg, "E212: not written: %s", why);
        ed.msg = s_msg;
        return false;
    }
    ed.vi.modified = false;
    report(bytes, " written");
    return true;
}

/* ---- carrying out an action --------------------------------------------- */

enum { GO_ON, LEAVE };

static int apply(const ViAction *act) {
    switch (act->kind) {
    case VI_ACT_NONE:
    case VI_ACT_REDRAW:
        break;

    case VI_ACT_BEEP:
    case VI_ACT_MESSAGE:
        ed.msg = act->msg;
        break;

    case VI_ACT_MOVE:
        ed.cursor = act->start;
        break;

    case VI_ACT_YANK:
        yank_range(act->start, act->end, act->linewise);
        ed.cursor = act->start;
        break;

    case VI_ACT_DELETE:
    case VI_ACT_CHANGE:
        yank_range(act->start, act->end, act->linewise);
        delete_range(act->start, act->end);
        ed.cursor = act->start;
        break;

    case VI_ACT_PASTE_AFTER:
    case VI_ACT_PASTE_BEFORE:
        paste(act->count > 0 ? act->count : 1, act->kind == VI_ACT_PASTE_AFTER);
        break;

    case VI_ACT_PASTE_OVER:
        if (ed.copy_len == 0) {
            ed.msg = "E353: nothing in register";
            break;
        }
        delete_range(act->start, act->end);
        ed.cursor = act->start;
        paste(1, false);
        break;

    case VI_ACT_INDENT:
        indent(act->start, act->end, act->count);
        break;

    case VI_ACT_REPLACE_CHAR:
        if (act->ch == '\n') {
            /* A split, not an overwrite: the range goes and one line break
             * takes its place. */
            delete_range(act->start, act->end);
            if (insert_text(act->start, "\n", 1)) ed.cursor = act->start + 1;
            break;
        }
        for (size_t i = act->start; i < act->end && i < ed.len; i++) overwrite(i, act->ch);
        ed.cursor = act->end > act->start ? act->end - 1 : act->start;
        break;

    case VI_ACT_OPEN_BELOW:
    case VI_ACT_OPEN_ABOVE:
        open_line(act->kind == VI_ACT_OPEN_BELOW);
        break;

    case VI_ACT_JOIN:
        ed.cursor = act->start;
        join(act->count);
        break;

    case VI_ACT_TOGGLE_CASE:
        toggle_case(act->start, act->end);
        ed.cursor = act->end < ed.len ? act->end : act->start;
        break;

    case VI_ACT_INCREMENT: {
        size_t landed = ed.cursor;
        ViIncrement done = editor_vi_increment(s_buf, &ed.len, VI_BUF_SIZE, act->start, act->count,
                                               &ed.undo, &landed);
        if (done != VI_INC_OK) {
            ed.msg = done == VI_INC_NO_ROOM ? "Not enough room" : "No number under the cursor";
            break;
        }
        editor_lines_reset(&ed.lines);
        ed.cursor = landed;
        ed.vi.modified = true;
        break;
    }

    case VI_ACT_SEARCH:
        search(act->ch, act->start);
        break;

    case VI_ACT_SCROLL:
        scroll_view(act->ch);
        break;

    case VI_ACT_SUBSTITUTE: {
        size_t landed = ed.cursor;
        size_t n = editor_vi_substitute(s_buf, &ed.len, VI_BUF_SIZE, act->start, act->end,
                                        ed.vi.pattern, ed.vi.pattern_len, ed.vi.replacement,
                                        ed.vi.replacement_len, ed.vi.sub_global, &ed.undo,
                                        &landed);
        if (n == SIZE_MAX) {
            ed.msg = "E486: pattern too complex";
        } else if (n == 0) {
            ed.msg = "No substitution made";
        } else {
            editor_lines_reset(&ed.lines);
            ed.cursor = landed;
            ed.vi.modified = true;
        }
        break;
    }

    case VI_ACT_MOVE_LINES: {
        size_t landed = ed.cursor;
        if (!editor_vi_move_lines(s_buf, &ed.len, VI_BUF_SIZE, act->start, act->end, act->dest,
                                  act->ch == 't', &ed.undo, &landed)) {
            ed.msg = "Not enough room";
            break;
        }
        editor_lines_reset(&ed.lines);
        ed.cursor = landed;
        ed.vi.modified = true;
        break;
    }

    case VI_ACT_GLOBAL: {
        size_t landed = ed.cursor;
        size_t n = editor_vi_global(s_buf, &ed.len, VI_BUF_SIZE, act->start, act->end,
                                    ed.vi.pattern, ed.vi.pattern_len, act->invert, act->ch,
                                    act->sub_pattern, act->sub_pattern_len, ed.vi.replacement,
                                    ed.vi.replacement_len, ed.vi.sub_global, &ed.undo, &landed);
        if (n == SIZE_MAX) {
            ed.msg = "E486: pattern too complex";
            break;
        }
        if (n == 0) {
            ed.msg = "No lines matched";
            break;
        }
        snprintf(ed.vi.msg, sizeof ed.vi.msg, act->ch == 'd' ? "%u fewer lines" : "%u lines changed",
                 (unsigned)n);
        ed.msg = ed.vi.msg;
        editor_lines_reset(&ed.lines);
        ed.cursor = landed;
        ed.vi.modified = true;
        break;
    }

    case VI_ACT_UNDO:
    case VI_ACT_REDO: {
        bool forward = act->kind == VI_ACT_REDO;
        int steps = act->count > 0 ? act->count : 1;
        size_t at = ed.cursor;
        bool any = false;
        for (int i = 0; i < steps; i++) {
            size_t pos;
            bool moved = forward
                ? editor_undo_redo(&ed.undo, s_buf, &ed.len, VI_BUF_SIZE, &pos)
                : editor_undo_undo(&ed.undo, s_buf, &ed.len, VI_BUF_SIZE, &pos);
            if (!moved) break;
            any = true;
            at = pos;
        }
        if (!any) {
            ed.msg = forward ? "Already at newest change" : "Already at oldest change";
            break;
        }
        editor_lines_reset(&ed.lines);
        ed.cursor = at;
        ed.vi.modified = true;
        break;
    }

    case VI_ACT_WRITE:
        save();
        break;

    case VI_ACT_ACCEPT:
        /* :wq writes, as vi's does; ZZ and :x only when there is a change. */
        if ((ed.vi.modified || ed.wq) && !save()) break;
        return LEAVE;

    case VI_ACT_CANCEL:
        return LEAVE;
    }

    /* `.` repeating a change that ended in insert mode types the text
     * again, then steps back off its last character as Esc does. */
    if (act->insert != NULL && insert_text(ed.cursor, act->insert, act->insert_len)) {
        ed.cursor += act->insert_len;
        if (ed.cursor > line_start_at(ed.cursor)) ed.cursor--;
    }
    if (ed.cursor > ed.len) ed.cursor = ed.len;
    return GO_ON;
}

/* ---- insert mode -------------------------------------------------------- */

/* The column of pos within its line, in bytes. */
static size_t byte_col(size_t pos) {
    return pos - line_start_at(pos);
}

/* The line `delta` lines from the cursor's, at the same byte column or
 * its end. */
static size_t up_down(int delta) {
    size_t col = byte_col(ed.cursor), p = line_start_at(ed.cursor);
    for (; delta < 0 && p > 0; delta++) p = line_start_at(p - 1);
    for (; delta > 0; delta--) {
        size_t e = line_end_at(p);
        if (e >= ed.len) break;
        p = e + 1;
    }
    size_t e = line_end_at(p);
    return p + col < e ? p + col : e;
}

/* A key the key layer handed back while inserting. */
static void insert_key(int key) {
    char c = (char)key;
    switch (key) {
    case '\r': {
        /* A line break, with the line's indentation carried down. */
        size_t start = line_start_at(ed.cursor), n = 1;
        s_text[0] = '\n';
        while (start + n - 1 < ed.cursor && blank(s_buf[start + n - 1]) && n < sizeof s_text) {
            s_text[n] = s_buf[start + n - 1];
            n++;
        }
        if (insert_text(ed.cursor, s_text, n)) ed.cursor += n;
        break;
    }
    case KEY_BACKSPACE:
        if (ed.cursor > 0) {
            ed.cursor--;
            delete_range(ed.cursor, ed.cursor + 1);
        }
        break;
    case KEY_DEL:
        delete_range(ed.cursor, ed.cursor + 1);
        break;
    case KEY_TAB:
        if (insert_text(ed.cursor, "\t", 1)) ed.cursor++;
        break;
    case KEY_LEFT:
        if (ed.cursor > 0) ed.cursor--;
        break;
    case KEY_RIGHT:
        if (ed.cursor < ed.len) ed.cursor++;
        break;
    case KEY_UP:
        ed.cursor = up_down(-1);
        break;
    case KEY_DOWN:
        ed.cursor = up_down(1);
        break;
    case KEY_PAGE_UP:
        ed.cursor = up_down(-VI_TEXT_ROWS);
        break;
    case KEY_PAGE_DOWN:
        ed.cursor = up_down(VI_TEXT_ROWS);
        break;
    case KEY_HOME:
        ed.cursor = line_start_at(ed.cursor);
        break;
    case KEY_END:
        ed.cursor = line_end_at(ed.cursor);
        break;
    default:
        if (key >= 0x20 && key < 0x7F && insert_text(ed.cursor, &c, 1)) ed.cursor++;
        break;
    }
}

/* ---- the screen --------------------------------------------------------- */

/* How many columns a byte takes at column col: a tab to the next stop, a
 * control character two (^X), any other one. */
static unsigned width_at(char c, unsigned col) {
    unsigned char u = (unsigned char)c;
    if (c == '\t') return VI_TAB_STOP - col % VI_TAB_STOP;
    if (u < 0x20 || u == 0x7F) return 2;
    return 1;
}

/* The screen column of pos within its line. */
static unsigned screen_col(size_t pos) {
    unsigned col = 0;
    for (size_t p = line_start_at(pos); p < pos; p++) col += width_at(s_buf[p], col);
    return col;
}

/* One cell of the row being built, if it is in view. */
static void put_cell(unsigned col, uint8_t glyph, bool rev) {
    if (col < ed.left || col - ed.left >= TERM_COLS) return;
    s_row[col - ed.left] = (uint8_t)(glyph | (rev ? REVERSE : 0));
}

/* Line `line` into s_row; [sel0, sel1) is shown reversed. */
static void build_line(int line, int last, size_t sel0, size_t sel1) {
    memset(s_row, ' ', sizeof s_row);
    if (line > last) {
        s_row[0] = '~';
        return;
    }
    size_t p = editor_lines_start(&ed.lines, s_buf, ed.len, line);
    size_t end = line_end_at(p);
    if (p == end && sel0 <= p && p < sel1 && ed.left == 0) s_row[0] |= REVERSE;   /* an empty line selected */
    unsigned col = 0;
    for (; p < end && col < ed.left + TERM_COLS; p++) {
        unsigned char u = (unsigned char)s_buf[p];
        bool rev = sel0 <= p && p < sel1;
        unsigned w = width_at((char)u, col);
        if (u == '\t') {
            for (unsigned i = 0; i < w; i++) put_cell(col + i, ' ', rev);
        } else if (w == 2) {
            put_cell(col, '^', rev);
            put_cell(col + 1, u == 0x7F ? '?' : (uint8_t)(u ^ 0x40), rev);
        } else {
            put_cell(col, u >= 0x80 ? NO_GLYPH : u, rev);
        }
        col += w;
    }
}

/* The status row: the command line, a message, or the mode, and on the
 * right the cursor's line and column. Never in the last column, where
 * the terminal would wrap and scroll. */
static void build_status(int line, unsigned col) {
    memset(s_row, ' ', sizeof s_row);
    const char *text = ed.vi.mode == VI_CMDLINE ? ed.vi.cmdline
                     : ed.msg                   ? ed.msg
                     : ed.vi.mode != VI_NORMAL  ? editor_vi_status(&ed.vi)
                                                : "";
    size_t n = strlen(text);
    if (n > TERM_COLS - 1) n = TERM_COLS - 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char u = (unsigned char)text[i];
        s_row[i] = u >= 0x20 && u < 0x7F ? u : NO_GLYPH;
    }
    if (ed.vi.mode == VI_CMDLINE) return;
    char ruler[24];
    int r = snprintf(ruler, sizeof ruler, "%d,%u", line + 1, col + 1);
    unsigned at = TERM_COLS - 1 - 10;
    if (r > 0 && (size_t)r < 10 && n < at) memcpy(&s_row[at], ruler, (size_t)r);
}

/* Send row `r` if it differs from what the screen shows. */
static void send_row(unsigned r) {
    if (memcmp(s_shown[r], s_row, sizeof s_row) == 0) return;
    memcpy(s_shown[r], s_row, sizeof s_row);
    unsigned last = TERM_COLS;
    while (last > 0 && s_row[last - 1] == ' ') last--;
    size_t n = (size_t)snprintf(s_out, sizeof s_out, "\033[%u;1H", r + 1);
    bool rev = false;
    for (unsigned c = 0; c < last; c++) {
        bool want = s_row[c] & REVERSE;
        if (want != rev) {
            memcpy(s_out + n, want ? "\033[7m" : "\033[0m", 4);
            n += 4;
            rev = want;
        }
        uint8_t g = s_row[c] & ~REVERSE;
        s_out[n++] = g == NO_GLYPH ? (char)0x80 : (char)g;   /* the terminal's checkerboard */
    }
    if (rev) {
        memcpy(s_out + n, "\033[0m", 4);
        n += 4;
    }
    if (last < TERM_COLS) {
        memcpy(s_out + n, "\033[K", 3);
        n += 3;
    }
    out_write(&ed.sh->con, s_out, n);
}

static void draw(void) {
    int line = editor_lines_at_pos(&ed.lines, s_buf, ed.len, ed.cursor);
    int last = editor_lines_at_pos(&ed.lines, s_buf, ed.len, ed.len);
    if (line < ed.top) ed.top = line;
    if (line >= ed.top + VI_TEXT_ROWS) ed.top = line - VI_TEXT_ROWS + 1;
    unsigned col = screen_col(ed.cursor);
    if (col < ed.left) ed.left = col;
    if (col >= ed.left + TERM_COLS) ed.left = col - TERM_COLS + 1;

    size_t sel0 = 0, sel1 = 0;
    if (ed.vi.mode == VI_VISUAL || ed.vi.mode == VI_VISUAL_LINE) {
        size_t a = ed.vi.anchor < ed.len ? ed.vi.anchor : ed.len;
        sel0 = a < ed.cursor ? a : ed.cursor;
        sel1 = (a < ed.cursor ? ed.cursor : a) + 1;
        if (ed.vi.mode == VI_VISUAL_LINE) {
            sel0 = line_start_at(sel0);
            sel1 = line_end_at(sel1 - 1) + 1;
        }
    }

    for (int r = 0; r < VI_TEXT_ROWS; r++) {
        build_line(ed.top + r, last, sel0, sel1);
        send_row((unsigned)r);
    }
    build_status(line, col);
    send_row(STATUS_ROW);

    unsigned crow = (unsigned)(line - ed.top), ccol = col - ed.left;
    if (ed.vi.mode == VI_CMDLINE) {
        crow = STATUS_ROW;
        ccol = ed.vi.cmdline_len < TERM_COLS - 1 ? (unsigned)ed.vi.cmdline_len : TERM_COLS - 1;
    }
    out_printf(&ed.sh->con, "\033[%u;%uH", crow + 1, ccol + 1);
}

/* ---- the command -------------------------------------------------------- */

static int edit(shell_t *sh, const char *arg, bool readonly) {
    if (!sh->hooks.getkey) {
        shell_error(sh, "vi: no keyboard\n");
        return 1;
    }
    memset(&ed, 0, sizeof ed);
    ed.sh = sh;
    ed.name = arg;
    ed.file.readonly = readonly;
    if (!path_resolve(ed.path, sh->cwd, arg)) {
        shell_error(sh, "vi: %s: name too long\n", arg);
        return 1;
    }
    const char *why = load();
    if (why) {
        shell_error(sh, "vi: %s: %s\n", arg, why);
        return 1;
    }
    editor_lines_reset(&ed.lines);
    editor_undo_init(&ed.undo, s_undo, sizeof s_undo);
    editor_vi_reset(&ed.vi);

    out_puts(&sh->con, "\033[0m\033[2J\033[?25h");
    memset(s_shown, ' ', sizeof s_shown);

    for (;;) {
        draw();
        int key = shell_getkey(sh);
        if (key < 0 || key == KEY_BREAK) break;   /* the end of input, or Brk: leave as :q! */
        if (key == 0x1B) key = KEY_ESC;
        ed.msg = NULL;

        /* One key is one undo step, except while inserting: everything
         * typed between `i` and Esc is undone with the command that
         * opened it. */
        if (ed.vi.mode != VI_INSERT) editor_undo_begin(&ed.undo);
        ViMode before = ed.vi.mode;
        ed.wq = before == VI_CMDLINE && strncmp(ed.vi.cmdline, ":w", 2) == 0;
        ViAction act;
        if (editor_vi_key(&ed.vi, s_buf, ed.len, ed.cursor, key, &act)) {
            if (apply(&act) == LEAVE) break;
            /* Insert mode has begun where the action put the cursor; the
             * key layer is told, so Esc can see what was typed. */
            if (before != VI_INSERT && ed.vi.mode == VI_INSERT)
                editor_vi_insert_began(&ed.vi, ed.cursor, ed.len);
        } else if (ed.vi.mode == VI_INSERT) {
            insert_key(key);
        }
    }

    /* A clean screen for the prompt, with what was written. */
    out_puts(&sh->con, "\033[0m\033[2J\033[H");
    if (ed.msg && ed.msg == s_msg && strstr(s_msg, " written")) out_printf(&sh->con, "%s\n", s_msg);
    return 0;
}

int cmd_vi(shell_t *sh, int argc, char **argv) {
    bool readonly = argc == 3 && strcmp(argv[1], "-R") == 0;
    if (argc != 2 + readonly || argv[argc - 1][0] == '-') {
        shell_error(sh, "usage: vi [-R] file\n");
        return 1;
    }
    return edit(sh, argv[argc - 1], readonly);
}

int cmd_view(shell_t *sh, int argc, char **argv) {
    if (argc != 2) {
        shell_error(sh, "usage: view file\n");
        return 1;
    }
    return edit(sh, argv[1], true);
}
