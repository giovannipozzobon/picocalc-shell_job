/* test_line.c — the line editor at the prompt: editing, wrapped lines,
 * history and Tab completion (line.h, design.md §4.4).
 *
 * Keys are typed through shell_input; the screen and the cursor are read
 * from the terminal's grid, which wraps as the panel does. */

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "history.h"
#include "host_disk.h"
#include "keycodes.h"
#include "path.h"
#include "shell.h"
#include "term.h"
#include "test_util.h"
#include "vars.h"

static term_t  t;
static shell_t sh;

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

static void type(const char *s) {
    while (*s) shell_input(&sh, (unsigned char)*s++);
}

static void key(int k) {
    shell_input(&sh, k);
}

static const char *row(unsigned r) {
    static char buf[TERM_COLS + 1];
    term_row_text(&t, r, buf);
    return buf;
}

/* A clean screen with the prompt on row 0. */
static void fresh(void) {
    if (sh.line.len) key(0x03);
    term_init(&t);
    sh.at_bol = true;
    shell_prompt(&sh);
}

static bool touch(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    return f_close(&f) == FR_OK;
}

#define AT(r, c) (t.row == (r) && t.col == (c))

int main(void) {
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, NULL, NULL, NULL, NULL };
    term_init(&t);
    shell_init(&sh, &out, &hooks, "test");
    CHECK(host_disk_create(NULL, 64u << 20, true), "image");
    files_mount();
    CHECK(f_mkdir("/home") == FR_OK && f_mkdir("/home/Docs") == FR_OK &&
              f_mkdir("/home/data") == FR_OK && touch("/home/notes.txt") &&
              touch("/home/new notes.txt") && touch("/home/.secret") &&
              f_mkdir("/pico2-apps") == FR_OK && touch("/pico2-apps/pico-oric.uf2") &&
              touch("/pico2-apps/pico-atom.uf2") && touch("/pico2-apps/readme.txt") &&
              touch("/home/Docs/a.txt"),
          "files");
    shell_start(&sh);

    /* ---- editing ---- */
    fresh();
    type("echo wrld");
    CHECK(strcmp(row(0), "/home>echo wrld") == 0 && AT(0, 15), "typed: '%s' %u,%u", row(0), t.row,
          t.col);
    key(KEY_LEFT), key(KEY_LEFT), key(KEY_LEFT);
    CHECK(AT(0, 12), "left: %u", t.col);
    type("o");
    CHECK(strcmp(row(0), "/home>echo world") == 0 && AT(0, 13), "insert: '%s' %u", row(0), t.col);
    key(KEY_HOME);
    CHECK(AT(0, 6), "Home: %u", t.col);
    key(KEY_DEL);
    CHECK(strcmp(row(0), "/home>cho world") == 0 && AT(0, 6), "Del: '%s'", row(0));
    type("e");
    key(KEY_END);
    CHECK(AT(0, 16), "End: %u", t.col);
    key(0x01);   /* Ctrl-A */
    CHECK(AT(0, 6), "Ctrl-A");
    key(0x05);   /* Ctrl-E */
    CHECK(AT(0, 16), "Ctrl-E");
    key(0x02), key(0x02);   /* Ctrl-B */
    key(KEY_BACKSPACE);
    CHECK(strcmp(row(0), "/home>echo wold") == 0 && AT(0, 13), "Backspace: '%s' %u", row(0), t.col);
    key(0x06);   /* Ctrl-F */
    CHECK(AT(0, 14), "Ctrl-F");
    key(KEY_RIGHT), key(KEY_RIGHT), key(KEY_RIGHT);
    CHECK(AT(0, 15), "Right stops at the end: %u", t.col);
    key(KEY_HOME);
    key(KEY_LEFT);
    CHECK(AT(0, 6), "Left stops at the start");
    key(KEY_END);
    key(0x02), key(0x02);
    key(0x0B);   /* Ctrl-K */
    CHECK(strcmp(row(0), "/home>echo wo") == 0 && AT(0, 13), "Ctrl-K: '%s'", row(0));
    key(0x02);
    key(0x15);   /* Ctrl-U */
    CHECK(strcmp(row(0), "/home>o") == 0 && AT(0, 6), "Ctrl-U: '%s'", row(0));
    key(0x03);
    CHECK(strcmp(row(0), "/home>o^C") == 0 && strcmp(row(1), "/home>") == 0 && AT(1, 6),
          "Ctrl-C: '%s' '%s'", row(0), row(1));
    type("echo hi\r");
    CHECK(strcmp(row(2), "hi") == 0 && strcmp(row(3), "/home>") == 0, "Enter runs it: '%s'", row(2));
    /* Enter in the middle runs the whole line. */
    type("echo abc");
    key(KEY_LEFT), key(KEY_LEFT);
    type("\r");
    CHECK(strcmp(row(4), "abc") == 0, "Enter mid-line: '%s'", row(4));

    /* ---- a line over several rows ---- */
    fresh();
    type("echo ");
    for (int i = 0; i < 60; i++) type(i % 10 == 9 ? "|" : "x");   /* 6 + 5 + 60 = 71 */
    CHECK(AT(1, 31), "wrapped: %u,%u", t.row, t.col);
    key(KEY_HOME);
    CHECK(AT(0, 6), "Home across rows: %u,%u", t.row, t.col);
    type("@");
    CHECK(row(0)[6] == '@' && row(1)[0] == 'x' && row(0)[39] == 'x' && AT(0, 7),
          "insert pushes text to the next row: '%s' / '%s'", row(0), row(1));
    key(KEY_END);
    CHECK(AT(1, 32), "End across rows: %u,%u", t.row, t.col);
    for (int i = 0; i < 33; i++) key(KEY_LEFT);
    CHECK(AT(0, 39), "Left across rows: %u,%u", t.row, t.col);
    key(KEY_RIGHT);
    CHECK(AT(1, 0), "Right across rows: %u,%u", t.row, t.col);
    key(KEY_HOME);
    for (int i = 0; i < 40; i++) key(KEY_DEL);
    CHECK(strlen(row(1)) == 0 && strlen(row(0)) == 6u + 66u - 40u,
          "the second row is blanked when the line shrinks: '%s' / '%s'", row(0), row(1));
    CHECK(AT(0, 6), "the cursor stays: %u,%u", t.row, t.col);
    /* Exactly a row: the cursor wraps to the next, as the panel does. */
    fresh();
    for (int i = 0; i < 34; i++) type("y");
    CHECK(AT(1, 0), "a full row: %u,%u", t.row, t.col);
    key(KEY_BACKSPACE);
    CHECK(AT(0, 39) && t.cells[0][39].ch == ' ', "back over the wrap: %u,%u '%s'", t.row, t.col,
          row(0));
    type("z");
    CHECK(AT(1, 0) && row(0)[39] == 'z', "and forward again: %u,%u", t.row, t.col);
    /* The line holds SHELL_LINE_MAX characters and drops the rest. */
    fresh();
    for (unsigned i = 0; i < SHELL_LINE_MAX + 10; i++) key('q');
    CHECK(sh.line.len == SHELL_LINE_MAX, "full: %zu", sh.line.len);
    key(0x03);
    /* At the bottom of the screen the rows scroll, and moves still land. */
    fresh();
    for (int i = 0; i < 31; i++) type("echo\r");
    CHECK(t.row == 31, "at the bottom: %u", t.row);
    for (int i = 0; i < 50; i++) type("w");
    CHECK(AT(31, 16), "scrolled: %u,%u", t.row, t.col);
    key(KEY_HOME);
    CHECK(AT(30, 6) && strncmp(row(30), "/home>www", 9) == 0, "Home after a scroll: %u,%u '%s'",
          t.row, t.col, row(30));
    key(0x03);

    /* ---- the prompt after output with no line break ---- */
    fresh();
    type("echo -n x\r");
    CHECK(strcmp(row(1), "x") == 0 && strcmp(row(2), "/home>") == 0, "own line: '%s' '%s'", row(1),
          row(2));

    /* ---- history ---- */
    history_clear();
    fresh();
    type("echo first\r");
    type("echo second\r");
    fresh();
    type("ec");
    key(KEY_UP);
    CHECK(strcmp(row(0), "/home>echo second") == 0 && AT(0, 17), "Up: '%s'", row(0));
    key(KEY_UP);
    CHECK(strcmp(row(0), "/home>echo first") == 0 && AT(0, 16), "Up again: '%s'", row(0));
    key(KEY_UP);
    CHECK(strcmp(row(0), "/home>echo first") == 0, "the oldest stays");
    key(KEY_DOWN);
    CHECK(strcmp(row(0), "/home>echo second") == 0, "Down: '%s'", row(0));
    key(KEY_DOWN);
    CHECK(strcmp(row(0), "/home>ec") == 0 && AT(0, 8), "the typed line back: '%s'", row(0));
    key(KEY_DOWN);
    CHECK(strcmp(row(0), "/home>ec") == 0, "Down past it does nothing");
    /* The mouse's wheel is a step of the history a notch. */
    key(KEY_WHEEL_UP);
    CHECK(strcmp(row(0), "/home>echo second") == 0, "wheel forward: '%s'", row(0));
    key(KEY_WHEEL_DOWN);
    CHECK(strcmp(row(0), "/home>ec") == 0, "wheel back: '%s'", row(0));
    key(0x10);   /* Ctrl-P */
    type(" again\r");
    CHECK(strcmp(row(1), "second again") == 0, "an edited recall runs: '%s'", row(1));
    CHECK(history_count() == 3 && strcmp(history_get(2), "echo second again") == 0,
          "and is kept: %u", history_count());
    fresh();
    key(0x10);
    key(0x0E);   /* Ctrl-N */
    CHECK(strcmp(row(0), "/home>") == 0, "Ctrl-P, Ctrl-N: '%s'", row(0));

    /* ---- Tab completion ---- */
    /* A command: a builtin, or a program from $PATH without .uf2. */
    fresh();
    type("hist\t");
    CHECK(strcmp(sh.line.text, "history ") == 0 && sh.line.pos == sh.line.len,
          "builtin: '%s'", row(0));
    fresh();
    type("pico-o\t");
    CHECK(strcmp(sh.line.text, "pico-oric ") == 0 && sh.line.pos == sh.line.len,
          "program: '%s'", row(0));
    fresh();
    type("pico\t");
    CHECK(strcmp(row(0), "/home>pico-") == 0, "shared part: '%s'", row(0));
    type("\t");
    CHECK(strcmp(row(0), "/home>pico-") == 0, "one Tab with nothing to add lists nothing");
    type("\t");
    CHECK(strcmp(row(1), "pico-atom  pico-oric") == 0 && strcmp(row(2), "/home>pico-") == 0 &&
              AT(2, 11),
          "two Tabs list them: '%s' '%s'", row(1), row(2));
    fresh();
    type("readme\t");
    CHECK(strcmp(row(0), "/home>readme") == 0, "only programs, not every file: '%s'", row(0));
    /* An empty $PATH entry is the current directory, as when it runs. */
    CHECK(path_resolve(sh.cwd, "/", "/pico2-apps") && vars_set("PATH", 4, "/nope:"),
          "PATH=/nope: in /pico2-apps");
    fresh();
    type("pico-o\t");
    CHECK(strcmp(sh.line.text, "pico-oric ") == 0, "trailing ':': '%s'", sh.line.text);
    CHECK(vars_set("PATH", 4, ""), "PATH=");
    fresh();
    type("pico-a\t");
    CHECK(strcmp(sh.line.text, "pico-atom ") == 0, "empty: '%s'", sh.line.text);
    CHECK(path_resolve(sh.cwd, "/", "/home") && vars_set("PATH", 4, "/pico2-apps"), "put back");
    fresh();
    type("PICO-A\t");
    CHECK(strcmp(sh.line.text, "pico-atom ") == 0 && sh.line.pos == sh.line.len,
          "case: '%s'", row(0));
    /* A path: after the command, after a separator's command, after < >. */
    fresh();
    type("cat no\t");
    CHECK(strcmp(sh.line.text, "cat notes.txt ") == 0 && sh.line.pos == sh.line.len,
          "file: '%s'", row(0));
    fresh();
    type("cd d\t");
    CHECK(strcmp(row(0), "/home>cd d") == 0, "Docs and data share 'd': '%s'", row(0));
    type("o\t");
    CHECK(strcmp(row(0), "/home>cd Docs/") == 0, "a directory gets '/', the card's case: '%s'",
          row(0));
    type("\t");
    CHECK(strcmp(sh.line.text, "cd Docs/a.txt ") == 0 && sh.line.pos == sh.line.len,
          "into it: '%s'", row(0));
    fresh();
    type("cat new\t");
    CHECK(strcmp(sh.line.text, "cat new\\ notes.txt ") == 0 && sh.line.pos == sh.line.len,
          "escaped: '%s'", row(0));
    fresh();
    type("ls; ca\t");
    CHECK(strcmp(sh.line.text, "ls; cat ") == 0 && sh.line.pos == sh.line.len,
          "a command after ';': '%s'", row(0));
    fresh();
    type("echo x > not\t");
    CHECK(strcmp(sh.line.text, "echo x > notes.txt ") == 0 && sh.line.pos == sh.line.len,
          "after >: '%s'", row(0));
    fresh();
    type("cat /pico2-apps/r\t");
    CHECK(strcmp(sh.line.text, "cat /pico2-apps/readme.txt ") == 0 && sh.line.pos == sh.line.len,
          "absolute: '%s'", row(0));
    fresh();
    type("ls .s\t");
    CHECK(strcmp(sh.line.text, "ls .secret ") == 0 && sh.line.pos == sh.line.len,
          "a hidden name, asked for: '%s'", row(0));
    fresh();
    type("ls zz\t");
    CHECK(strcmp(row(0), "/home>ls zz") == 0, "no match: '%s'", row(0));
    /* In the middle of a line. */
    fresh();
    type("cat no > x");
    for (int i = 0; i < 4; i++) key(KEY_LEFT);
    type("\t");
    CHECK(strcmp(row(0), "/home>cat notes.txt  > x") == 0 && AT(0, 20), "mid-line: '%s' %u", row(0),
          t.col);
    key(0x03);
    /* Listing a directory's names. */
    fresh();
    type("ls \t\t");
    CHECK(strcmp(row(1), "data/          new notes.txt") == 0 &&
              strcmp(row(2), "Docs/          notes.txt") == 0,
          "list: '%s' '%s'", row(1), row(2));
    /* Three columns of 12 would fill the row, and the terminal would wrap
     * it into a blank row: two columns, for the list and for ls. */
    key(0x03);
    CHECK(f_mkdir("/home/w") == FR_OK, "mkdir");
    const char *wide[] = { "aaaaaaaaaaaa", "bbbbbbbbbbbb", "cccccccccccc",
                           "dddddddddddd", "eeeeeeeeeeee", "ffffffffffff" };
    for (int i = 0; i < 6; i++) {
        char p[32];
        snprintf(p, sizeof p, "/home/w/%s", wide[i]);
        CHECK(touch(p), "%s", p);
    }
    fresh();
    type("ls w/\t\t");
    CHECK(strcmp(row(1), "aaaaaaaaaaaa  dddddddddddd") == 0 &&
              strcmp(row(2), "bbbbbbbbbbbb  eeeeeeeeeeee") == 0 &&
              strcmp(row(3), "cccccccccccc  ffffffffffff") == 0,
          "list, wide: '%s' '%s' '%s'", row(1), row(2), row(3));
    key(0x03);
    fresh();
    type("ls w\r");
    CHECK(strcmp(row(1), "aaaaaaaaaaaa  dddddddddddd") == 0 &&
              strcmp(row(2), "bbbbbbbbbbbb  eeeeeeeeeeee") == 0 &&
              strcmp(row(3), "cccccccccccc  ffffffffffff") == 0 && strcmp(row(4), "/home>") == 0,
          "ls, wide: '%s' '%s' '%s' '%s'", row(1), row(2), row(3), row(4));

    host_disk_close();
    TEST_DONE();
}
