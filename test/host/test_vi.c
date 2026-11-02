/* test_vi.c — vi through the shell, over a FAT image (design.md §4.6, §10).
 *
 * Each case types a command line and then a script of keys, as the
 * keyboard would; the screen is read from the terminal's grid and the
 * file back through FatFs. The key layer itself has Pico Logo's tests
 * (test_editor_vi.c); these are for what this project added: the file,
 * its line endings and size, read-only, safe saving, and the screen. */

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "host_disk.h"
#include "keycodes.h"
#include "shell.h"
#include "term.h"
#include "test_util.h"

#define ESC "\x1b"
#define BRK "\xd0"

static term_t  t;
static shell_t sh;
static char    log_text[1 << 18];   /* everything sent to the terminal */
static size_t  log_len;
static const char *keys;            /* the script getkey types */
static size_t  keys_left;

static void sink(void *ctx, const char *buf, size_t n) {
    (void)ctx;
    term_write(&t, buf, n);
    for (size_t i = 0; i < n && log_len + 1 < sizeof log_text; i++) log_text[log_len++] = buf[i];
    log_text[log_len] = '\0';
}

static uint64_t now(void *ctx) {
    (void)ctx;
    return 0;
}

static bool interrupted(void *ctx) {
    (void)ctx;
    return false;
}

/* The screen as vi left it when the script ran out, before it cleared it
 * on leaving; row() and reversed() read this. */
static term_t shot;

static int getkey(void *ctx) {
    (void)ctx;
    if (keys_left == 0) {
        shot = t;
        return -1;
    }
    keys_left--;
    return (unsigned char)*keys++;
}

/* `line` at the prompt, then `script` typed into it; the status. The
 * script is counted, so it may hold NUL. */
static int edit_n(const char *line, const char *script, size_t n) {
    term_init(&shot);
    log_len = 0;
    log_text[0] = '\0';
    keys = script;
    keys_left = n;
    int status = shell_run(&sh, line);
    keys_left = 0;
    return status;
}

static int edit(const char *line, const char *script) {
    return edit_n(line, script, strlen(script));
}

/* A command with no keys, around the edits. */
static bool run_ok(const char *line) {
    return edit(line, "") == 0;
}

static const char *row(unsigned r) {
    static char buf[TERM_COLS + 1];
    term_row_text(&shot, r, buf);
    return buf;
}

/* The screen now, after vi has left. */
static const char *live_row(unsigned r) {
    static char buf[TERM_COLS + 1];
    term_row_text(&t, r, buf);
    return buf;
}

/* A file's bytes, through FatFs, and how many; NULL if unreadable. */
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

static bool exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

static bool put_n(const char *path, const char *text, size_t n) {
    FIL f;
    UINT done;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    bool ok = f_write(&f, text, (UINT)n, &done) == FR_OK && done == n;
    return f_close(&f) == FR_OK && ok;
}

static bool put(const char *path, const char *text) {
    return put_n(path, text, strlen(text));
}

/* What vi drew last on the status row, from the log: the text after the
 * last move to row 32, up to the next escape. */
static const char *last_status(void) {
    static char buf[TERM_COLS + 1];
    const char *p = NULL;
    for (const char *q = log_text; (q = strstr(q, "\033[32;1H")) != NULL; q++) p = q;
    buf[0] = '\0';
    if (!p) return buf;
    p += 7;
    size_t n = 0;
    while (p[n] && p[n] != '\033' && n < TERM_COLS) n++;
    memcpy(buf, p, n);
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '\0';
    return buf;
}

/* Whether the cell is drawn reversed. */
static bool reversed(unsigned r, unsigned c) {
    return shot.cells[r][c].bg != TERM_BACKGROUND;
}

static int file_tests(bool fat32) {
    CHECK(host_disk_create(NULL, 64u << 20, fat32), "image");
    files_mount();
    shell_start(&sh);

    /* Open, show, edit, write and leave. */
    CHECK(put("/a.cfg", "hello\nworld\n"), "file");
    CHECK(edit("vi /a.cfg", "") == 0, "vi: %s", log_text);
    CHECK(strstr(log_text, "\"/a.cfg\" 2L, 12B"), "the opening report: '%s'", last_status());
    CHECK(edit("vi /a.cfg", "x" ":wq\r") == 0 && is("/a.cfg", "ello\nworld\n"), "x :wq: '%s'",
          slurp("/a.cfg"));
    CHECK(strcmp(live_row(0), "\"/a.cfg\" 2L, 11B written") == 0 && live_row(1)[0] == '\0',
          "the write said on a clean screen: '%s'", live_row(0));
    CHECK(!exists("/a.cfg.new"), "no .new left");
    CHECK(edit("vi /a.cfg", "jA!!" ESC "ZZ") == 0 && is("/a.cfg", "ello\nworld!!\n"), "A ZZ: '%s'",
          slurp("/a.cfg"));
    CHECK(edit("vi /a.cfg", "Ohi" ESC ":w\r" "u:wq\r") == 0 && is("/a.cfg", "ello\nworld!!\n"),
          ":w, undo, :wq: '%s'", slurp("/a.cfg"));

    /* A settings line changed as the done-when check does it. */
    CHECK(put("/atom.cfg", "# pico-atom\r\nmachine = atom\r\nclock = 1\r\n"), "cfg");
    CHECK(edit("vi /atom.cfg", "/clock\r" "f1r2" ":wq\r") == 0 &&
              is("/atom.cfg", "# pico-atom\r\nmachine = atom\r\nclock = 2\r\n"),
          "a CRLF settings file: '%s'", slurp("/atom.cfg"));
    CHECK(strstr(log_text, "[CRLF] 3L, 40B written"), "said as CRLF: '%s'", log_text);

    /* Line endings. A mixed file keeps its CRs as characters. */
    CHECK(put("/m", "a\r\nb\nc\r\n") && edit("vi /m", "Gx:wq\r") == 0 && is("/m", "a\r\nb\n\r\n"),
          "mixed: '%s'", slurp("/m"));
    CHECK(put("/m", "a\r\nb\n") && edit("vi /m", "") == 0 && strcmp(row(0), "a^M") == 0,
          "a lone CR is shown: '%s'", row(0));
    CHECK(put("/n", "one\ntwo") && edit("vi /n", "x:wq\r") == 0 && is("/n", "ne\ntwo"),
          "no final newline is kept: '%s'", slurp("/n"));
    CHECK(strstr(log_text, "[noeol]"), "and said");
    CHECK(put("/n", "one\r\ntwo") && edit("vi /n", "Gx:wq\r") == 0 && is("/n", "one\r\nwo"),
          "CRLF, no final newline: '%s'", slurp("/n"));
    CHECK(put("/e", "") && edit("vi /e", ":w\r:q\r") == 0 && is("/e", ""), "empty stays empty");
    CHECK(strstr(log_text, "\"/e\" 0L, 0B"), "an empty file is 0L: '%s'", log_text);
    CHECK(edit("vi /e", "ia" ESC ":wq\r") == 0 && is("/e", "a\n"), "then text has a newline");
    CHECK(put("/e", "\n") && edit("vi /e", ":w\r:q\r") == 0 && is("/e", "\n"), "one empty line");
    CHECK(put_n("/z", "a\0b\n", 4) && edit("vi /z", "") == 0 && strcmp(row(0), "a^@b") == 0,
          "NUL is shown: '%s'", row(0));
    CHECK(edit("vi /z", "A!" ESC ":wq\r") == 0 && slurp("/z") && got_len == 5 &&
              memcmp(slurp("/z"), "a\0b!\n", 5) == 0,
          "and kept");

    /* A new file: nothing is made unless written. */
    CHECK(edit("vi /new", "ZZ") == 0 && !exists("/new"), "ZZ on a new file makes nothing");
    CHECK(edit("vi /new", ":x\r") == 0 && !exists("/new"), "nor does :x");
    CHECK(strstr(log_text, "\"/new\" [New]"), "said new: '%s'", log_text);
    CHECK(edit("vi /new", ":wq\r") == 0 && is("/new", ""), ":wq makes it");
    CHECK(run_ok("mkdir /d"), "mkdir");
    CHECK(edit("cd /d; vi f", "ix" ESC ":wq\r") == 0 && is("/d/f", "x\n"),
          "a name against the current directory");
    CHECK(edit("cd /", "") == 0, "cd /");

    /* Leaving without writing. */
    CHECK(put("/q", "keep\n"), "q");
    CHECK(edit("vi /q", "dd:q\r" BRK) == 0 && is("/q", "keep\n"), ":q with changes refused");
    CHECK(strstr(log_text, "E37: no write since last change"), "and said");
    CHECK(edit("vi /q", "dd:q!\r") == 0 && is("/q", "keep\n"), ":q! discards");
    CHECK(edit("vi /q", "ddi" BRK) == 0 && is("/q", "keep\n"), "Brk discards, from insert mode");
    CHECK(edit("vi /q", "dd") == 0 && is("/q", "keep\n"), "the end of input discards");

    /* Read-only. */
    CHECK(edit("view /q", "dd:w\r:wq\rZZ:q!\r") == 0 && is("/q", "keep\n"), "view never writes");
    CHECK(strstr(log_text, "E45: read-only; not written") && strstr(log_text, "[readonly]"),
          "and says so: '%s'", last_status());
    CHECK(edit("view /q", "ZZ") == 0, "view: ZZ with no change leaves");
    CHECK(edit("vi -R /q", "x:wq\r:q!\r") == 0 && is("/q", "keep\n"), "vi -R");
    CHECK(f_chmod("/q", AM_RDO, AM_RDO) == FR_OK, "chmod");
    CHECK(edit("vi /q", "x:wq\r:q!\r") == 0 && is("/q", "keep\n") && strstr(log_text, "E45"),
          "a file FAT marks read-only");
    CHECK(f_chmod("/q", 0, AM_RDO) == FR_OK, "chmod");

    /* What cannot be edited. */
    CHECK(edit("vi /d", "") == 1 && strstr(log_text, "vi: /d: is a directory"), "a directory: '%s'",
          log_text);
    CHECK(edit("vi /nodir/f", "") == 1 && strstr(log_text, "no such file"), "no such directory: '%s'",
          log_text);
    CHECK(edit("vi", "") == 1 && strstr(log_text, "usage: vi [-R] file"), "usage");
    CHECK(edit("vi -x f", "") == 1 && strstr(log_text, "usage"), "an unknown option");
    CHECK(edit("view", "") == 1 && strstr(log_text, "usage: view file"), "view's usage");

    /* Size: VI_BUF_SIZE - 1 bytes open; one more is refused, not truncated. */
    static char big[VI_BUF_SIZE + 1];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (i % 64 == 63) ? '\n' : (char)('a' + i % 26);
    CHECK(put_n("/big", big, VI_BUF_SIZE), "big");
    char want[64];
    snprintf(want, sizeof want, "too large: %u bytes, %u at most", (unsigned)VI_BUF_SIZE, (unsigned)VI_BUF_SIZE - 1u);
    CHECK(edit("vi /big", "") == 1 && strstr(log_text, want), "too large: '%s'", log_text);
    CHECK(is("/big", "") == false && got_len == VI_BUF_SIZE, "and left alone");
    CHECK(put_n("/big", big, VI_BUF_SIZE - 1), "big - 1");
    CHECK(edit("vi /big", ":w\r:q\r") == 0 && slurp("/big") && got_len == VI_BUF_SIZE - 1 &&
              memcmp(slurp("/big"), big, VI_BUF_SIZE - 1) == 0,
          "the largest file goes round unchanged");
    CHECK(edit("vi /big", "ix" ESC ":q!\r") == 0 && strstr(log_text, "the file would be too large"),
          "and is full: '%s'", last_status());

    /* The copy buffer: a yank larger than it is refused, said, and the
     * register keeps what it had. */
    CHECK(edit("vi /big", "yy" "yG" "Gp" ":q!\r") == 0 &&
              strstr(log_text, "too much to yank; register unchanged"),
          "a large yank: '%s'", log_text);

    host_disk_close();
    return test_failures;
}

static int screen_tests(void) {
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    shell_start(&sh);

    /* 100 lines: the first 31 shown, then the end after G. */
    static char text[2000];
    size_t n = 0;
    for (int i = 1; i <= 100; i++) n += (size_t)snprintf(text + n, sizeof text - n, "line %d\n", i);
    CHECK(put("/l", text), "l");
    CHECK(edit("vi /l", "") == 0, "vi /l");
    CHECK(strcmp(row(0), "line 1") == 0 && strcmp(row(30), "line 31") == 0, "top: '%s' '%s'",
          row(0), row(30));
    CHECK(strstr(row(31), "\"/l\" 100L, 792B") && strstr(row(31), "1,1"), "status: '%s'", row(31));
    CHECK(edit("vi /l", "G") == 0 && strstr(log_text, "line 100") && strstr(last_status(), "100,1"),
          "G: '%s'", last_status());
    CHECK(edit("vi /l", "Ggg") == 0 && strstr(last_status(), "1,1"), "gg: '%s'", last_status());
    CHECK(edit("vi /l", "50G") == 0 && strstr(last_status(), "50,1"), "50G: '%s'", last_status());
    CHECK(edit("vi /l", "G7gg") == 0 && strstr(last_status(), "7,1"), "7gg: '%s'", last_status());
    CHECK(edit("vi /l", "Gzt") == 0 && strstr(log_text, "\033[1;1Hline 70"), "zt keeps a screenful");

    /* What the screen holds just before vi leaves: the end of input is
     * reached with the last key's drawing done, so stop with :q! last
     * and look at the log, or with nothing more and look at the grid. */
    CHECK(put("/s", "short\n\tx\na\x01z\n") && edit("vi /s", "") == 0, "vi /s");
    CHECK(strcmp(row(0), "short") == 0 && strcmp(row(1), "        x") == 0 &&
              strcmp(row(2), "a^Az") == 0 && strcmp(row(3), "~") == 0,
          "tabs and control characters: '%s' '%s' '%s'", row(1), row(2), row(3));

    /* A long line scrolls sideways to the cursor; the rest do too. */
    static char wide[200];
    memset(wide, 0, sizeof wide);
    for (int i = 0; i < 60; i++) wide[i] = (char)('0' + i % 10);
    strcat(wide, "\nnext\n");
    CHECK(put("/w", wide), "w");
    CHECK(edit("vi /w", "$") == 0, "$");
    /* $ is past the last character, in Pico Logo's key layer. */
    CHECK(strcmp(row(0), "123456789012345678901234567890123456789") == 0 && shot.row == 0 &&
              shot.col == 39,
          "the end in view, the cursor after it: '%s' at %u,%u", row(0), shot.row, shot.col);
    CHECK(edit("vi /w", "$0") == 0 && strcmp(row(0), "0123456789012345678901234567890123456789") == 0 &&
              shot.col == 0,
          "and back: '%s'", row(0));

    /* The mouse's wheel moves the cursor SHELL_WHEEL_LINES lines a notch,
     * forward (up) or back. */
    CHECK(put("/w8", "1\n2\n3\n4\n5\n6\n7\n8\n") && edit("vi /w8", "\xdb") == 0 && shot.row == 3,
          "wheel back: row %u", shot.row);
    CHECK(edit("vi /w8", "\xdb\xdb\xda") == 0 && shot.row == 3, "and forward: row %u", shot.row);
    CHECK(edit("vi /w8", "\xda") == 0 && shot.row == 0, "forward at the top stays: row %u", shot.row);

    /* Visual mode reverses the selection. */
    CHECK(edit("vi /s", "vl") == 0 && reversed(0, 0) && reversed(0, 1) && !reversed(0, 2),
          "v: two characters reversed");
    CHECK(strstr(last_status(), "-- VISUAL --"), "the mode: '%s'", last_status());
    CHECK(edit("vi /s", "Vj") == 0 && reversed(0, 4) && reversed(1, 0) && !reversed(2, 0),
          "V: two lines");
    CHECK(edit("vi /s", "i") == 0 && strstr(last_status(), "-- INSERT --"), "insert mode is said");
    CHECK(edit("vi /s", ":s/x") == 0 && strcmp(row(31), ":s/x") == 0 && shot.row == 31 && shot.col == 4,
          "the command line, with the cursor: '%s' at %u,%u", row(31), shot.row, shot.col);

    /* Only what changed is sent: moving the cursor along a line sends
     * the status row and the cursor, not the text. */
    CHECK(edit("vi /l", "l") == 0, "l");
    const char *after = strstr(log_text, "1,2");
    CHECK(after && !strstr(after, "line 1"), "no text redrawn for l");

    host_disk_close();
    return test_failures;
}

/* A card that fails part way through the save, at every sector of the
 * way: the old file is whole, or the new one is, or the old is gone and
 * a whole name.new is there; and a failure is said, with vi still open. */
static int fault_tests(void) {
    static char old_text[5001], new_text[5001];
    for (int i = 0; i < 5000; i++) old_text[i] = (i % 50 == 49) ? '\n' : (char)('a' + i % 26);
    memcpy(new_text, old_text, sizeof new_text);
    new_text[0] = 'X';
    unsigned long writes = 0;
    for (long fail = 0; fail < 1000; fail++) {
        CHECK(host_disk_create(NULL, 32u << 20, false), "image");
        files_mount();
        shell_start(&sh);
        CHECK(put("/a", old_text), "a");
        unsigned long before = host_disk_writes();
        host_disk_fail_writes_after(fail);
        edit("vi /a", "rX:wq\r");
        host_disk_fail_writes_after(-1);
        writes = host_disk_writes() - before;
        bool said = strstr(log_text, "E212: not written") != NULL;
        files_mount();
        const char *a = slurp("/a");
        bool old_whole = a && strcmp(a, old_text) == 0;
        bool new_whole = a && strcmp(a, new_text) == 0;
        const char *nw = a ? NULL : slurp("/a.new");
        bool recoverable = !a && nw && strcmp(nw, new_text) == 0;
        CHECK(old_whole || new_whole || recoverable, "failing after %ld writes: a is %s", fail,
              a ? "neither" : "missing, and a.new not whole");
        CHECK(said || new_whole, "after %ld: a failure not said", fail);
        host_disk_close();
        if (!said) break;   /* every write got through */
    }
    CHECK(writes > 10, "the save took %lu writes", writes);

    /* No card: vi says so and does not start. */
    files_mount();
    CHECK(edit("vi /a", "") == 1 && strstr(log_text, "no card"), "no card: '%s'", log_text);
    return test_failures;
}

int main(void) {
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, getkey, NULL, NULL, NULL };
    term_init(&t);
    shell_init(&sh, &out, &hooks, "test");

    if (file_tests(false)) return 1;
    if (file_tests(true)) return 1;
    if (screen_tests()) return 1;
    if (fault_tests()) return 1;

    /* With no keyboard, vi says so. */
    shell_t bare;
    const shell_hooks_t none = { now, interrupted, NULL, NULL, NULL, NULL };
    shell_init(&bare, &out, &none, "test");
    log_len = 0;
    CHECK(shell_run(&bare, "vi /a") == 1 && strstr(log_text, "vi: no keyboard"), "no keyboard");
    TEST_DONE();
}
