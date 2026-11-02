/* test_edit.c — EDIT, the DOS flavour's editor, through the shell over
 * FAT16 and FAT32 images (design.md §4.7, M9 step 3). Linked with
 * shell_core_dos.
 *
 * Each case types a command line and then a script of keys; the screen
 * is read from the terminal's grid as EDIT left it when the script ran
 * out, and the file back through FatFs. In a script, US (\x1f) puts Alt
 * on the next byte and US US is a tap of Alt, as over the UART
 * (keys_uart); RS (\x1e) puts Ctrl on the next byte, here only. */

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "host_disk.h"
#include "keycodes.h"
#include "keys.h"
#include "shell.h"
#include "term.h"
#include "test_util.h"

#define ALT   "\x1f"
#define TAP   "\x1f\x1f"
#define CTL   "\x1e"
#define ESC   "\x1b"
#define LEFT  "\xb4"
#define UP    "\xb5"
#define DOWN  "\xb6"
#define RIGHT "\xb7"
#define HOME  "\xd2"
#define END   "\xd5"
#define PGDN  "\xd7"
#define DEL   "\xd4"
#define INS   "\xd1"
#define BRK   "\xd0"
#define BS    "\x08"
#define F1    "\x81"
#define F3    "\x83"
#define CUT   "\x18"
#define COPY  "\x03"
#define PASTE "\x16"
#define CUTLN "\x19"

static term_t  t, shot;
static shell_t sh;
static keys_t  kb;
static const char *keys;
static size_t  keys_left;

static void sink(void *ctx, const char *buf, size_t n) {
    (void)ctx;
    term_write(&t, buf, n);
}

static uint64_t now(void *ctx) {
    (void)ctx;
    return 0;
}

static bool interrupted(void *ctx) {
    (void)ctx;
    return false;
}

static int getkey(void *ctx) {
    (void)ctx;
    for (;;) {
        if (keys_left == 0) {
            shot = t;
            return -1;
        }
        keys_left--;
        uint8_t b = (uint8_t)*keys++;
        if (b == 0x1E && keys_left > 0) {
            keys_left--;
            return KEYS_CTRL | (uint8_t)*keys++;
        }
        int k = keys_uart(&kb, b);
        if (k != KEYS_NONE) return k;
    }
}

static int edit(const char *line, const char *script) {
    term_init(&shot);
    keys = script;
    keys_left = strlen(script);
    keys_init(&kb);
    int status = shell_run(&sh, line);
    keys_left = 0;
    return status;
}

static const char *row(unsigned r) {
    static char buf[TERM_COLS + 1];
    term_row_text(&shot, r, buf);
    return buf;
}

static bool on_screen(const char *text) {
    for (unsigned r = 0; r < TERM_ROWS; r++)
        if (strstr(row(r), text)) return true;
    return false;
}

static size_t got_len;
static const char *slurp(const char *path) {
    static char buf[1 << 17];
    FIL f;
    UINT n;
    got_len = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) return NULL;
    FRESULT r = f_read(&f, buf, sizeof buf - 1, &n);
    f_close(&f);
    if (r != FR_OK) return NULL;
    buf[n] = '\0';
    got_len = n;
    return buf;
}

static bool is(const char *path, const char *text) {
    const char *s = slurp(path);
    return s && got_len == strlen(text) && memcmp(s, text, got_len) == 0;
}

static bool put(const char *path, const char *text) {
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT r = f_write(&f, text, (UINT)strlen(text), &n);
    return f_close(&f) == FR_OK && r == FR_OK && n == strlen(text);
}

static bool exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

static int edit_tests(bool fat32) {
    const char *fs = fat32 ? "FAT32" : "FAT16";
    CHECK(host_disk_create(NULL, 64u << 20, fat32), "%s image", fs);
    files_mount();
    CHECK(f_mkdir("/HOME") == FR_OK, "dirs");
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/HOME") == 0, "in \\HOME");

    /* The screen: menu bar, title, text, status, in 40 columns. */
    CHECK(put("/HOME/A.TXT", "hello\nworld\n"), "a.txt");
    CHECK(edit("EDIT a.txt", "") == 0, "%s: edit", fs);
    CHECK(strcmp(row(0), "  File  Edit  Search  Options  Help") == 0, "menu bar '%s'", row(0));
    CHECK(shot.cells[0][2].bg == TERM_RGB(168, 168, 168) && shot.cells[0][3].fg == 0 &&
              shot.cells[0][2].fg == TERM_RGB(255, 255, 255),
          "the bar black on grey, its letters white: %04x %04x", shot.cells[0][3].bg, shot.cells[0][3].fg);
    CHECK(shot.cells[2][0].bg == TERM_RGB(0, 0, 238), "the text on blue");
    CHECK(strstr(row(1), " A.TXT "), "title '%s'", row(1));
    CHECK(strcmp(row(2), "hello") == 0 && strcmp(row(3), "world") == 0 && row(4)[0] == '\0',
          "text '%s' '%s'", row(2), row(3));
    CHECK(strstr(row(31), "F1=Help  Alt=Menu") && strstr(row(31), "Line:1 Col:1"), "status '%s'",
          row(31));
    CHECK(shot.row == 2 && shot.col == 0, "cursor at %u,%u", shot.row, shot.col);
    CHECK(strcmp(sh.cwd, "/HOME") == 0, "in \\HOME after");

    /* The mouse's wheel: three lines a notch. */
    CHECK(put("/HOME/V.TXT", "1\n2\n3\n4\n5\n6\n7\n8\n") && edit("EDIT V.TXT", "\xdb") == 0 && shot.row == 5,
          "wheel back: row %u", shot.row);
    CHECK(edit("EDIT V.TXT", "\xdb\xdb\xda") == 0 && shot.row == 5, "and forward: row %u", shot.row);

    /* Only 40 columns: no FONT. */
    CHECK(shell_run(&sh, "FONT 5x10") != 0, "no FONT");

    /* 40 columns: a long line scrolls sideways; the boxes fit. */
    CHECK(put("/HOME/W.TXT", "0123456789012345678901234567890123456789ABCDEF\n"), "w.txt");
    CHECK(edit("EDIT W.TXT", END) == 0 && strcmp(row(2), "789012345678901234567890123456789ABCDEF") == 0 &&
              shot.col == 39 && strstr(row(31), "Col:47"),
          "scrolled: '%s' col %u", row(2), shot.col);
    CHECK(edit("EDIT W.TXT", F1) == 0 && on_screen("Ctrl+X, C, V     cut, copy, paste") &&
              on_screen("Shift+Del        End: line's end"),
          "the keys in 40");
    CHECK(edit("EDIT W.TXT", ALT "o" "d") == 0 && on_screen(" Colours: Blue, or Phosphor on black? ") &&
              on_screen("< Blue >"),
          "Display's question");

    /* Type, save with Alt+F S, and leave with Brk. */
    CHECK(edit("EDIT A.TXT", "X" ALT "f" "s" BRK) == 0 && is("/HOME/A.TXT", "Xhello\nworld\n"),
          "type, save: '%s'", slurp("/HOME/A.TXT"));
    CHECK(!exists("/HOME/A.TXT.new"), "no .new left");

    /* The menus: tap Alt, across to Search, down to Repeat, back. */
    CHECK(edit("EDIT A.TXT", TAP) == 0 && on_screen("New") && on_screen("Save As...") &&
              shot.cursor_on == false,
          "File menu, cursor hidden");
    CHECK(edit("EDIT A.TXT", TAP RIGHT RIGHT DOWN) == 0 && on_screen("Repeat Last Find   F3") &&
              !on_screen("Save As..."),
          "Search menu");
    CHECK(edit("EDIT A.TXT", TAP RIGHT "\r") == 0 && !on_screen("Ctrl+X"),
          "Edit's first item chosen, the menu closed");
    CHECK(edit("EDIT A.TXT", TAP ESC "Z") == 0 && strcmp(row(2), "ZXhello") == 0,
          "Esc closes the menu: '%s'", row(2));
    CHECK(edit("EDIT A.TXT", ALT "h" "k") == 0 && on_screen("Keys") && on_screen("cut the line"),
          "Help, Keys");
    CHECK(edit("EDIT A.TXT", F1) == 0 && on_screen("Alt+arrows       select"), "F1");
    CHECK(edit("EDIT A.TXT", ALT "s" "f") == 0 && on_screen("Find What:"), "Find's box");
    /* Alt held: an item's letter picks it before another menu's. */
    CHECK(edit("EDIT A.TXT", ALT "s" ALT "f") == 0 && on_screen("Find What:"), "Alt+S, Alt+F");
    CHECK(edit("EDIT A.TXT", ALT "f" ALT "h") == 0 && on_screen("Keys") && !on_screen("Save As..."),
          "Alt+H from File opens Help");

    /* Exit with a change: No keeps the file, Esc stays, Yes saves. */
    CHECK(edit("EDIT A.TXT", "Q" ALT "f" "x") == 0 && on_screen("Save it now?"), "asked");
    {
        /* Each button's letter in red, the rest of it black on grey. */
        unsigned r = 0;
        while (r < TERM_ROWS && !strstr(row(r), "< Yes >")) r++;
        const char *line = row(r);
        unsigned y = (unsigned)(strstr(line, "Yes") - line), c = (unsigned)(strstr(line, "Cancel") - line);
        CHECK(r < TERM_ROWS && shot.cells[r][y].fg == TERM_RGB(205, 0, 0) &&
                  shot.cells[r][y + 1].fg == 0 && shot.cells[r][c].fg == TERM_RGB(205, 0, 0) &&
                  shot.cells[r][y].bg == TERM_RGB(168, 168, 168),
              "the buttons' letters in red: row %u", r);
    }
    CHECK(edit("EDIT A.TXT", "Q" ALT "f" "x" "c" "Z") == 0 && strcmp(row(2), "QZXhello") == 0,
          "C is Cancel: '%s'", row(2));
    CHECK(edit("EDIT A.TXT", "Q" ALT "f" "x" "n") == 0 && is("/HOME/A.TXT", "Xhello\nworld\n"),
          "No");
    CHECK(edit("EDIT A.TXT", "Q" ALT "f" ALT "x") == 0 && on_screen("Save it now?"),
          "Alt+F, Alt+X asks");
    CHECK(edit("EDIT A.TXT", "Q" BRK ESC) == 0 && strcmp(row(2), "QXhello") == 0, "Cancel stays");
    CHECK(edit("EDIT A.TXT", DEL BRK "\r") == 0 && is("/HOME/A.TXT", "hello\nworld\n"),
          "Brk, Enter is Yes: '%s'", slurp("/HOME/A.TXT"));
    CHECK(edit("EDIT A.TXT", "zz") == 0 && is("/HOME/A.TXT", "hello\nworld\n"),
          "the end of input leaves without saving");

    /* Selecting with Alt and the arrows; cut, copy, paste, clear. */
    CHECK(edit("EDIT A.TXT", ALT RIGHT ALT RIGHT ALT RIGHT ALT RIGHT ALT RIGHT) == 0 &&
              shot.cells[2][0].bg != shot.cells[2][5].bg && shot.cells[2][4].bg == shot.cells[2][0].bg,
          "five letters selected");
    CHECK(edit("EDIT A.TXT", ALT RIGHT ALT RIGHT ALT RIGHT ALT RIGHT ALT RIGHT CUT CTL END PASTE
                             ALT "f" "s") == 0 &&
              is("/HOME/A.TXT", "\nworldhello\n"),
          "cut, paste: '%s'", slurp("/HOME/A.TXT"));
    CHECK(edit("EDIT A.TXT", DOWN END ALT LEFT ALT LEFT ALT LEFT COPY CTL HOME PASTE ALT "f" "s") == 0 &&
              is("/HOME/A.TXT", "llo\nworldhello\n"),
          "copy, paste: '%s'", slurp("/HOME/A.TXT"));
    CHECK(edit("EDIT A.TXT", ALT DOWN DEL ALT "f" "s") == 0 && is("/HOME/A.TXT", "worldhello\n"),
          "Alt+Down, Del clears: '%s'", slurp("/HOME/A.TXT"));
    CHECK(edit("EDIT A.TXT", ALT RIGHT ALT RIGHT "ab" ALT "f" "s") == 0 &&
              is("/HOME/A.TXT", "abrldhello\n"),
          "typing replaces the selection: '%s'", slurp("/HOME/A.TXT"));
    CHECK(edit("EDIT A.TXT", ALT RIGHT ALT "e" "t" END ALT "e" "p" ALT "f" "s") == 0 &&
              is("/HOME/A.TXT", "brldhelloa\n"),
          "Edit's Cut and Paste: '%s'", slurp("/HOME/A.TXT"));

    /* Ctrl+Y cuts the line, the last taking the break before it. */
    CHECK(put("/HOME/L.TXT", "one\ntwo\nthree\n"), "l.txt");
    CHECK(edit("EDIT L.TXT", CUTLN DOWN PASTE ALT "f" "s") == 0 &&
              is("/HOME/L.TXT", "two\none\nthree\n"),
          "Ctrl+Y, paste: '%s'", slurp("/HOME/L.TXT"));
    CHECK(edit("EDIT L.TXT", CTL END CUTLN ALT "f" "s") == 0 && is("/HOME/L.TXT", "two\none\n"),
          "Ctrl+Y on the last line: '%s'", slurp("/HOME/L.TXT"));

    /* Moving: words, the goal column past a short line and a tab, pages. */
    CHECK(put("/HOME/M.TXT", "alpha beta  gamma\nab\n\tx = 1\n"), "m.txt");
    CHECK(edit("EDIT M.TXT", CTL RIGHT CTL RIGHT "!") == 0 && strcmp(row(2), "alpha beta  !gamma") == 0,
          "Ctrl+Right: '%s'", row(2));
    CHECK(edit("EDIT M.TXT", END CTL LEFT "!") == 0 && strcmp(row(2), "alpha beta  !gamma") == 0,
          "Ctrl+Left: '%s'", row(2));
    CHECK(edit("EDIT M.TXT", END LEFT DOWN DOWN "!") == 0 && strcmp(row(4), "        x = 1!") == 0,
          "goal column kept: '%s'", row(4));
    CHECK(edit("EDIT M.TXT", RIGHT RIGHT RIGHT DOWN DOWN "!") == 0 && strcmp(row(4), "!       x = 1") == 0,
          "a tab's columns: '%s'", row(4));
    CHECK(edit("EDIT M.TXT", "\tq\r") == 0 && strcmp(row(3), "        ") != 0 && shot.col == 8,
          "Enter carries the indentation: col %u", shot.col);

    /* Insert toggles overtype; the status says OVR. */
    CHECK(edit("EDIT M.TXT", INS "XY") == 0 && strcmp(row(2), "XYpha beta  gamma") == 0 &&
              strstr(row(31), "OVR"),
          "overtype: '%s'", row(2));
    CHECK(edit("EDIT M.TXT", INS END "XY") == 0 && strcmp(row(2), "alpha beta  gammaXY") == 0,
          "overtype at the end adds: '%s'", row(2));

    /* Find, F3 and Change, in either case; a CRLF file stays CRLF. */
    CHECK(put("/HOME/ATOM.CFG", "# pico-atom\r\nmachine = atom\r\nclock = 1\r\n"), "cfg");
    CHECK(edit("EDIT ATOM.CFG", ALT "s" "f" "CLOCK\r" END BS "2" ALT "f" "x" "\r") == 0 &&
              is("/HOME/ATOM.CFG", "# pico-atom\r\nmachine = atom\r\nclock = 2\r\n"),
          "the done-when edit: '%s'", slurp("/HOME/ATOM.CFG"));
    CHECK(edit("EDIT ATOM.CFG", ALT "s" "f" "atom\r" F3 "!") == 0 && strcmp(row(3), "machine = !") == 0,
          "F3 finds the next: '%s'", row(3));
    CHECK(edit("EDIT ATOM.CFG", ALT "s" "f" "nope\r") == 0 && on_screen("Match not found"),
          "not found");
    CHECK(edit("EDIT ATOM.CFG", ALT "s" "c" "a\r" "A\r") == 0 && strstr(row(31), "3 changed") &&
              strcmp(row(3), "mAchine = Atom") == 0,
          "Change: '%s' '%s'", row(3), row(31));
    CHECK(edit("EDIT ATOM.CFG", ALT "s" "c" "a\r" "A\r" ALT "f" "s") == 0 &&
              is("/HOME/ATOM.CFG", "# pico-Atom\r\nmAchine = Atom\r\nclock = 2\r\n"),
          "Change saved: '%s'", slurp("/HOME/ATOM.CFG"));
    CHECK(edit("EDIT ATOM.CFG", ALT "s" "c" "\r" BS "\r") == 0 && strcmp(row(3), "mchine = tom") == 0,
          "Change to nothing, the offered text kept: '%s'", row(3));
    CHECK(edit("EDIT ATOM.CFG", ALT "s" "c" "\r" ESC "z") == 0 && strcmp(row(2), "z# pico-Atom") == 0,
          "Esc cancels Change: '%s'", row(2));
    CHECK(edit("EDIT", CUTLN BRK) == 0 && !on_screen("Save it now?"), "Ctrl+Y on nothing changes nothing");

    /* New, Save As, Open, and Save asking for a name. */
    CHECK(edit("EDIT", "new text" ALT "f" "s" "NEW.TXT\r" BRK) == 0 &&
              is("/HOME/NEW.TXT", "new text\n"),
          "UNTITLED saved by name: '%s'", slurp("/HOME/NEW.TXT"));
    CHECK(edit("EDIT", "") == 0 && strstr(row(1), " UNTITLED "), "UNTITLED '%s'", row(1));
    CHECK(edit("EDIT NEW.TXT", ALT "f" "a" "C:\\COPY.TXT\r" "X" ALT "f" "s") == 0 &&
              is("/COPY.TXT", "Xnew text\n") && is("/HOME/NEW.TXT", "new text\n") &&
              strstr(row(1), " COPY.TXT "),
          "Save As, then Save to the new name: '%s'", slurp("/COPY.TXT"));
    CHECK(edit("EDIT NEW.TXT", ALT "f" "a" "A.TXT\r" "n") == 0 && is("/HOME/A.TXT", "brldhelloa\n"),
          "Save As over a file asks first");
    CHECK(edit("EDIT NEW.TXT", ALT "f" "o" "\\COPY.TXT\r") == 0 && strcmp(row(2), "Xnew text") == 0 &&
              strstr(row(1), " COPY.TXT "),
          "Open: '%s'", row(2));
    CHECK(edit("EDIT NEW.TXT", ALT "f" "o" "\\NOPE\\X\r") == 0 && on_screen("< OK >"),
          "a failed Open says why");
    CHECK(edit("EDIT NEW.TXT", ALT "f" "o" "\\NOPE\\X\r" "\r") == 0 &&
              strcmp(row(2), "new text") == 0 && strstr(row(1), " NEW.TXT "),
          "and keeps the text: '%s'", row(2));
    CHECK(edit("EDIT NEW.TXT", "Q" ALT "f" "n" "y") == 0 && is("/HOME/NEW.TXT", "Qnew text\n"),
          "New saves first if asked: '%s'", slurp("/HOME/NEW.TXT"));
    CHECK(edit("EDIT MADE.TXT", "made" ALT "f" "s") == 0 && is("/HOME/MADE.TXT", "made\n"),
          "a new file by name");

    /* Read-only: shown, not saved. */
    CHECK(f_chmod("/HOME/L.TXT", AM_RDO, AM_RDO) == FR_OK, "chmod");
    CHECK(edit("EDIT L.TXT", "") == 0 && strstr(row(31), "Read-only: changes can't be saved") &&
              !strstr(row(31), "Line:"),
          "read-only, the ruler hidden in 40: '%s'", row(31));
    CHECK(edit("EDIT L.TXT", "x" ALT "f" "s") == 0 && on_screen(" The file is read-only; save it under ") &&
              on_screen(" another name ") && is("/HOME/L.TXT", "two\none\n"),
          "not saved, the reason wrapped in 40");

    /* What EDIT refuses before it starts. */
    CHECK(edit("EDIT \\HOME", "") == 1, "a directory");
    CHECK(edit("EDIT A B", "") == 1, "two names");
    static char big[VI_BUF_SIZE + 10];
    memset(big, 'x', sizeof big - 1);
    CHECK(put("/HOME/BIG.TXT", big), "big");
    CHECK(edit("EDIT BIG.TXT", "") == 1, "too large");

    /* After EDIT, Alt chords are the MCU's keys again. */
    keys_t k;
    keys_init(&k);
    keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_ALT);
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'F') == 'F', "chords not reported after EDIT");

    files_unmount();
    host_disk_close();
    return 0;
}

int main(void) {
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, getkey, NULL, NULL, NULL };
    term_init(&t);
    shell_init(&sh, &out, &hooks, "test");
    if (edit_tests(false)) return 1;
    term_init(&t);
    shell_init(&sh, &out, &hooks, "test");
    if (edit_tests(true)) return 1;
    TEST_DONE();
}
