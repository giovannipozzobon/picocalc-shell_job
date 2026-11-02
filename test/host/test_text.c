/* test_text.c — `more`, `grep`, `head`, `tail` and `wc` (text.h,
 * design.md §4.5).
 *
 * more is read on the terminal's grid, with keys typed from a script;
 * grep through the text the terminal was sent. */

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "host_disk.h"
#include "keycodes.h"
#include "shell.h"
#include "term.h"
#include "test_util.h"

static term_t  t;
static shell_t sh;
static char    con[1 << 17];
static size_t  con_len;
static const char *keys;
static unsigned waits;      /* how many times more asked for a key */
static term_t   shots[8];   /* the screen at each of the first waits */

static void sink(void *ctx, const char *buf, size_t n) {
    (void)ctx;
    term_write(&t, buf, n);
    for (size_t i = 0; i < n; i++)
        if (buf[i] != '\r' && con_len + 1 < sizeof con) con[con_len++] = buf[i];
    con[con_len] = '\0';
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
    if (waits < sizeof shots / sizeof shots[0]) shots[waits] = t;
    waits++;
    if (!keys || !*keys) return -1;
    return (unsigned char)*keys++;
}

static int run_keys(const char *line, const char *script) {
    con_len = 0;
    con[0] = '\0';
    keys = script;
    waits = 0;
    term_init(&t);
    return shell_run(&sh, line);
}

static int run(const char *line) {
    return run_keys(line, NULL);
}

static const char *row(const term_t *s, unsigned r) {
    static char buf[TERM_COLS + 1];
    term_row_text(s, r, buf);
    return buf;
}

static bool put(const char *path, const char *text) {
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT r = f_write(&f, text, (UINT)strlen(text), &n);
    return f_close(&f) == FR_OK && r == FR_OK && n == strlen(text);
}

static const char *numbered(unsigned lines) {
    static char buf[1 << 16];
    size_t n = 0;
    for (unsigned i = 1; i <= lines; i++) n += (size_t)snprintf(buf + n, sizeof buf - n, "line %u\n", i);
    return buf;
}

/* "line a\n" to "line b\n". */
static const char *span(unsigned a, unsigned b) {
    static char buf[1 << 16];
    size_t n = 0;
    buf[0] = '\0';
    for (unsigned i = a; i <= b; i++) n += (size_t)snprintf(buf + n, sizeof buf - n, "line %u\n", i);
    return buf;
}

int main(void) {
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, getkey, NULL, NULL, NULL };
    shell_init(&sh, &out, &hooks, "test");
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    shell_start(&sh);

    /* ---- more ---- */
    CHECK(put("/n100", numbered(100)) && put("/n10", numbered(10)), "files");
    /* Short: no prompt at all. */
    CHECK(run_keys("more n10", "") == 0 && waits == 0 && strcmp(con, numbered(10)) == 0,
          "short: %u waits", waits);
    /* A page of TERM_ROWS - 1 rows, then --More-- on the last row. */
    CHECK(run_keys("more n100", " \rq") == 0, "more: %s", con);
    CHECK(waits == 3, "waits: %u", waits);
    CHECK(strcmp(row(&shots[0], 0), "line 1") == 0 && strcmp(row(&shots[0], 30), "line 31") == 0 &&
              strcmp(row(&shots[0], 31), "--More--") == 0,
          "page 1: '%s' '%s' '%s'", row(&shots[0], 0), row(&shots[0], 30), row(&shots[0], 31));
    CHECK(shots[0].cells[31][0].bg != shots[0].cells[0][0].bg, "--More-- in reverse");
    /* Space: the next 31 rows. */
    CHECK(strcmp(row(&shots[1], 0), "line 32") == 0 && strcmp(row(&shots[1], 30), "line 62") == 0,
          "page 2: '%s' '%s'", row(&shots[1], 0), row(&shots[1], 30));
    /* Enter: one row more. */
    CHECK(strcmp(row(&shots[2], 30), "line 63") == 0 && strcmp(row(&shots[2], 29), "line 62") == 0,
          "one more: '%s'", row(&shots[2], 30));
    /* q: stopped, the prompt row cleared, nothing more shown. */
    CHECK(!strstr(con, "line 64") && strcmp(row(&t, 31), "") == 0, "q: '%s'", row(&t, 31));
    /* The mouse's wheel: forward cannot go back and is ignored, and a notch
     * back is SHELL_WHEEL_LINES rows more. */
    CHECK(run_keys("more n100", "\xda\xdbq") == 0 && waits == 3, "wheel: %u waits", waits);
    CHECK(strcmp(row(&shots[1], 30), "line 31") == 0 && strcmp(row(&shots[2], 30), "line 34") == 0 &&
              strcmp(row(&shots[2], 27), "line 31") == 0,
          "wheel rows: '%s' '%s'", row(&shots[1], 30), row(&shots[2], 30));
    /* Each way to stop. */
    const char *stops[] = { "Q", "\x1b", "\x03", "\xd0" };
    for (size_t i = 0; i < 4; i++)
        CHECK(run_keys("more n100", stops[i]) == 0 && waits == 1 && !strstr(con, "line 32"),
              "stop %zu", i);
    /* The end of input stops it too. */
    CHECK(run_keys("more n100", "") == 0 && waits == 1, "no keys");
    /* To the end with spaces: no prompt after the last page. */
    CHECK(run_keys("more n100", "    ") == 0 && waits == 3 && strstr(con, "line 100\n"),
          "to the end: %u", waits);
    /* A long line counts as the rows it wraps to: in 40 columns each
     * line is two rows, so the page ends on the 16th line's first. */
    char wide[4096] = "";
    for (int i = 0; i < 40; i++) strcat(wide, "0123456789012345678901234567890123456789X\n");
    CHECK(put("/wide", wide), "wide");
    CHECK(run_keys("more wide", "q") == 0 && waits == 1 &&
              strcmp(row(&shots[0], 29), "X") == 0 &&
              strcmp(row(&shots[0], 30), "0123456789012345678901234567890123456789") == 0,
          "wrapped: '%s' '%s'", row(&shots[0], 29), row(&shots[0], 30));
    /* From a pipe or <, and two files with headers. */
    CHECK(run_keys("cat n100 | more", "q") == 0 && waits == 1, "| more");
    CHECK(run_keys("more < n10", "") == 0 && strcmp(con, numbered(10)) == 0, "more <");
    CHECK(run_keys("more n10 n10", "") == 0 &&
              strncmp(con, "::::::::::::::\nn10\n::::::::::::::\nline 1\n", 41) == 0,
          "headers: '%.60s'", con);
    /* Not to the terminal: copied, no prompt. */
    CHECK(run_keys("more n100 > copy", "") == 0 && waits == 0, "more > f");
    CHECK(run_keys("more n100 | grep -c line", "") == 0 && waits == 0 && strcmp(con, "100\n") == 0,
          "more |: '%s'", con);
    CHECK(run("more") == 1 && strstr(con, "usage: more"), "more alone");
    CHECK(run("more nope") == 1 && strstr(con, "more: nope: no such file"), "more nope");

    /* ---- grep ---- */
    CHECK(put("/g1", "Apple pie\nbanana\ncherry\r\nAPPLE juice\n") && put("/g2", "apple\nfig"),
          "grep files");
    CHECK(run("grep apple g1") == 0 && strcmp(con, "Apple pie\nAPPLE juice\n") == 0,
          "case: '%s'", con);
    CHECK(run("grep -i apple g1") == 0 && strcmp(con, "Apple pie\nAPPLE juice\n") == 0, "-i");
    CHECK(run("grep -v apple g1") == 0 && strcmp(con, "banana\ncherry\n") == 0, "-v: '%s'", con);
    CHECK(run("grep -n an g1") == 0 && strcmp(con, "2:banana\n") == 0, "-n: '%s'", con);
    CHECK(run("grep -c a g1") == 0 && strcmp(con, "3\n") == 0, "-c: '%s'", con);
    CHECK(run("grep -vn '^[ab]' g1") == 0 && strcmp(con, "3:cherry\n") == 0, "-vn: '%s'", con);
    CHECK(run("grep 'y$' g1") == 0 && strcmp(con, "cherry\n") == 0, "CR LF and $: '%s'", con);
    CHECK(run("grep '\\<juice' g1 g2") == 0 && strcmp(con, "g1:APPLE juice\n") == 0,
          "names: '%s'", con);
    CHECK(run("grep -c apple g1 g2") == 0 && strcmp(con, "g1:2\ng2:1\n") == 0, "-c two: '%s'",
          con);
    CHECK(run("grep fig g2") == 0 && strcmp(con, "fig\n") == 0, "no final LF: '%s'", con);
    CHECK(run("grep nothing g1") == 1 && con_len == 0, "none: 1");
    CHECK(run("grep x nope") == 2 && strstr(con, "grep: nope: no such file"), "nope: 2");
    CHECK(run("grep apple g1 nope") == 2 && strstr(con, "Apple pie"), "found, and an error: 2");
    CHECK(run("grep '\\(' g1") == 2 && strstr(con, "not a pattern"), "bad pattern: '%s'", con);
    CHECK(run("grep") == 2 && strstr(con, "usage: grep"), "grep alone");
    CHECK(run("grep x") == 2 && strstr(con, "usage: grep"), "no input");
    CHECK(run("grep -z x g1") == 2 && strstr(con, "-z: unknown option"), "-z");
    CHECK(run("grep pie < g1") == 0 && strcmp(con, "Apple pie\n") == 0, "grep <");
    CHECK(run("cat g1 | grep -c an") == 0 && strcmp(con, "1\n") == 0, "| grep");
    /* A line longer than SHELL_TEXT_LINE - 1 is matched in pieces. */
    static char longline[3 * SHELL_TEXT_LINE];
    memset(longline, 'a', sizeof longline - 2);
    memcpy(longline + SHELL_TEXT_LINE + 10, "needle", 6);
    longline[sizeof longline - 2] = '\n';
    CHECK(put("/long", longline), "long");
    CHECK(run("grep -n needle long") == 0 && strncmp(con, "1:", 2) == 0 &&
              strlen(con) == SHELL_TEXT_LINE - 1 + 3,
          "pieces: %zu", strlen(con));
    char want[16];
    snprintf(want, sizeof want, "%zu\n", (sizeof longline - 2 + SHELL_TEXT_LINE - 2) / (SHELL_TEXT_LINE - 1));
    CHECK(run("grep -c a long") == 0 && strcmp(con, want) == 0, "pieces: '%s'", con);

    /* ---- head ---- */
    CHECK(run("head n100") == 0 && strcmp(con, span(1, 10)) == 0, "head: '%s'", con);
    CHECK(run("head -n 3 n100") == 0 && strcmp(con, span(1, 3)) == 0, "-n 3: '%s'", con);
    CHECK(run("head -n3 n100") == 0 && strcmp(con, span(1, 3)) == 0, "-n3");
    CHECK(run("head -3 n100") == 0 && strcmp(con, span(1, 3)) == 0, "-3");
    CHECK(run("head -n 0 n100") == 0 && con_len == 0, "-n 0");
    CHECK(run("head -n 500 n10") == 0 && strcmp(con, span(1, 10)) == 0, "more than there are");
    CHECK(run("head -2 g2") == 0 && strcmp(con, "apple\nfig") == 0, "no final LF: '%s'", con);
    CHECK(run("head -1 n10 g2") == 0 && strcmp(con, "==> n10 <==\nline 1\n\n==> g2 <==\napple\n") == 0,
          "headers: '%s'", con);
    CHECK(run("head -2 < n100") == 0 && strcmp(con, span(1, 2)) == 0, "head <");
    CHECK(run("cat n100 | head -2") == 0 && strcmp(con, span(1, 2)) == 0, "| head");
    CHECK(run("head -2 n100 | wc -l") == 0 && strcmp(con, "2\n") == 0, "head |: '%s'", con);
    CHECK(run("head") == 1 && strstr(con, "usage: head"), "head alone");
    CHECK(run("head -n") == 1 && strstr(con, "usage: head"), "-n alone");
    CHECK(run("head -n x n10") == 1 && strstr(con, "usage: head"), "-n x");
    CHECK(run("head -x n10") == 1 && strstr(con, "usage: head"), "-x");
    CHECK(run("head nope n10") == 1 && strstr(con, "head: nope: no such file") && strstr(con, "line 10"),
          "nope, and a file: '%s'", con);
    CHECK(run("head -- n10") == 0 && strcmp(con, span(1, 10)) == 0, "--");
    /* Several options: the last count wins, as in other heads. */
    CHECK(run("head -n 5 -n 3 n100") == 0 && strcmp(con, span(1, 3)) == 0, "-n -n: '%s'", con);
    CHECK(run("head -5 -2 -- n100") == 0 && strcmp(con, span(1, 2)) == 0, "-5 -2 --: '%s'", con);
    CHECK(run("tail -n 5 -1 n100") == 0 && strcmp(con, "line 100\n") == 0, "tail -n -1: '%s'", con);
    CHECK(run("head -3 -x n100") == 1 && strstr(con, "usage: head"), "-3 -x");

    /* ---- tail ---- */
    CHECK(run("tail n100") == 0 && strcmp(con, span(91, 100)) == 0, "tail: '%s'", con);
    CHECK(run("tail -n 1 n100") == 0 && strcmp(con, "line 100\n") == 0, "-n 1: '%s'", con);
    CHECK(run("tail -3 n10") == 0 && strcmp(con, span(8, 10)) == 0, "-3");
    CHECK(run("tail -n 0 n10") == 0 && con_len == 0, "-n 0");
    CHECK(run("tail -n 500 n10") == 0 && strcmp(con, span(1, 10)) == 0, "more than there are");
    CHECK(run("tail -1 g2") == 0 && strcmp(con, "fig") == 0, "no final LF: '%s'", con);
    CHECK(run("tail -2 g2") == 0 && strcmp(con, "apple\nfig") == 0, "no final LF, 2: '%s'", con);
    CHECK(run("tail -1 n10 g2") == 0 && strcmp(con, "==> n10 <==\nline 10\n\n==> g2 <==\nfig") == 0,
          "headers: '%s'", con);
    CHECK(run("cat n100 | tail -2") == 0 && strcmp(con, span(99, 100)) == 0, "| tail");
    CHECK(run("tail -2 < n100") == 0 && strcmp(con, span(99, 100)) == 0, "tail <");
    CHECK(put("/empty", ""), "empty");
    CHECK(run("tail empty") == 0 && con_len == 0, "empty");
    CHECK(put("/blank", "\n\n\n"), "blank");
    CHECK(run("tail -2 blank") == 0 && strcmp(con, "\n\n") == 0, "blank lines");
    /* Larger than the ring: the last lines all fit, and a page of
     * 6000 lines (some 60 KB) gives the same end. */
    CHECK(put("/n6000", numbered(6000)), "n6000");
    CHECK(run("tail -n 3 n6000") == 0 && strcmp(con, span(5998, 6000)) == 0, "big: '%s'", con);
    /* More lines asked for than the ring holds: the whole ones kept, and
     * a note. Each is 10 bytes, so 4096 bytes keep 409 whole lines. */
    CHECK(run("tail -n 1000 n6000 > t.out") == 0 && strstr(con, "only the last 4096 bytes"),
          "cut: '%s'", con);
    CHECK(run("head -1 t.out") == 0 && strcmp(con, "line 5592\n") == 0, "first kept: '%s'", con);
    CHECK(run("wc -l t.out") == 0 && strcmp(con, "    409 t.out\n") == 0, "kept: '%s'", con);
    /* One line longer than the ring: its last 4096 bytes, and a note. */
    static char huge[5002];
    memset(huge, 'b', 5000);
    huge[5000] = '\n';
    CHECK(put("/huge", huge), "huge");
    CHECK(run("tail -1 huge | wc -c") == 0 &&
              strcmp(con, "tail: huge: only the last 4096 bytes are kept\n4096\n") == 0,
          "huge line: '%s'", con);
    CHECK(run("tail -1 long | wc -c") == 0 && strcmp(con, "1535\n") == 0, "long line: '%s'", con);
    CHECK(run("tail") == 1 && strstr(con, "usage: tail"), "tail alone");

    /* ---- wc ---- */
    CHECK(run("wc g1") == 0 && strcmp(con, "      4       6      37 g1\n") == 0, "wc: '%s'", con);
    CHECK(run("wc -l g1") == 0 && strcmp(con, "      4 g1\n") == 0, "-l: '%s'", con);
    CHECK(run("wc -w g1") == 0 && strcmp(con, "      6 g1\n") == 0, "-w: '%s'", con);
    CHECK(run("wc -c g1") == 0 && strcmp(con, "     37 g1\n") == 0, "-c: '%s'", con);
    CHECK(run("wc -lc g1") == 0 && strcmp(con, "      4      37 g1\n") == 0, "-lc: '%s'", con);
    CHECK(run("wc g1 g2") == 0 &&
              strcmp(con, "      4       6      37 g1\n      1       2       9 g2\n"
                          "      5       8      46 total\n") == 0,
          "total: '%s'", con);
    CHECK(run("wc -l < n100") == 0 && strcmp(con, "100\n") == 0, "-l <: '%s'", con);
    CHECK(run("cat g1 | wc") == 0 && strcmp(con, "      4       6      37\n") == 0, "| wc: '%s'", con);
    CHECK(run("wc empty") == 0 && strcmp(con, "      0       0       0 empty\n") == 0, "empty");
    CHECK(run("wc -c long") == 0 && strstr(con, "   1535 long\n"), "long: '%s'", con);
    /* A word across the reads of SHELL_TEXT_LINE bytes is one word. */
    CHECK(run("wc -w long") == 0 && strcmp(con, "      1 long\n") == 0, "one word: '%s'", con);
    CHECK(run("wc nope g1") == 1 && strstr(con, "wc: nope: no such file") && strstr(con, " g1\n") &&
              strstr(con, "37 total\n"),
          "nope: '%s'", con);
    CHECK(run("wc") == 1 && strstr(con, "usage: wc"), "wc alone");
    CHECK(run("wc -x g1") == 1 && strstr(con, "-x: unknown option"), "-x");

    host_disk_close();
    TEST_DONE();
}
