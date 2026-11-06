/* test_select.c — the UNIX flavour's select and break (design.md §4.4).
 *
 * Replies are typed from a script of keys; the menu and the prompts are
 * read from the text the terminal was sent. */

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "host_disk.h"
#include "keycodes.h"
#include "shell.h"
#include "test_util.h"
#include "vars.h"

static shell_t sh;
static char    con[1 << 16];
static size_t  con_len;
static const char *keys;

static void sink(void *ctx, const char *buf, size_t n) {
    (void)ctx;
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
    if (!keys || !*keys) return -1;
    return (unsigned char)*keys++;
}

static int run_keys(const char *line, const char *script) {
    con_len = 0;
    con[0] = '\0';
    keys = script;
    return shell_run(&sh, line);
}

static bool has(const char *text) {
    return strstr(con, text) != NULL;
}

/* How many times `text` was sent. */
static unsigned count(const char *text) {
    unsigned n = 0;
    for (const char *s = con; (s = strstr(s, text)) != NULL; s += strlen(text)) n++;
    return n;
}

static const char *var(const char *name) {
    const char *v = vars_get(name, strlen(name));
    return v ? v : "(unset)";
}

static bool put(const char *path, const char *text) {
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT r = f_write(&f, text, (UINT)strlen(text), &n);
    return f_close(&f) == FR_OK && r == FR_OK && n == strlen(text);
}

int main(void) {
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, getkey, NULL, NULL, NULL };
    shell_init(&sh, &out, &hooks, "test");
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    shell_start(&sh);

    /* ---- one line ---- */
    CHECK(run_keys("select p in red green blue; do echo got $p; done", "2\r") == 0,
          "select: %s", con);
    CHECK(has("1) red    2) green  3) blue\n#? 2\ngot green\n#? \n"), "menu and reply: '%s'", con);
    CHECK(strcmp(var("p"), "green") == 0 && strcmp(var("REPLY"), "2") == 0, "p=%s REPLY=%s",
          var("p"), var("REPLY"));
    /* A wrong number empties NAME and still runs the LIST; REPLY keeps it. */
    CHECK(run_keys("select p in a b; do echo x${p}x $REPLY; done", "7\rx\r") == 0, "wrong: %s", con);
    CHECK(has("xx 7\n") && has("xx x\n") && strcmp(var("p"), "") == 0, "wrong number: '%s'", con);
    /* An empty line shows the menu again, and runs nothing. */
    CHECK(run_keys("select p in a b; do echo ran; done", "\r1\r") == 0, "empty: %s", con);
    CHECK(count("1) a  2) b\n") == 2 && count("ran\n") == 1, "empty line: '%s'", con);
    /* The menu is shown only at the start and after an empty line. */
    CHECK(run_keys("select p in a b; do echo -n; done", "1\r2\r") == 0, "quiet");
    CHECK(count("1) a  2) b\n") == 1, "menu once: '%s'", con);
    /* PS3. */
    CHECK(run_keys("PS3='pick: '; select p in a; do echo $p; done", "1\r") == 0, "ps3: %s", con);
    CHECK(has("pick: 1\na\npick: "), "PS3: '%s'", con);
    run_keys("unset PS3", NULL);
    /* Commands before and after, on the line. */
    CHECK(run_keys("echo before; select p in a; do echo $p; done; echo after", "1\r") == 0,
          "around: %s", con);
    CHECK(has("before\n1) a\n") && has("\nafter\n"), "around: '%s'", con);
    /* The LIST may start on do's own command, and hold several. */
    CHECK(run_keys("select p in a; do echo one && echo two; echo three; done", "1\r") == 0,
          "list: %s", con);
    CHECK(has("one\ntwo\nthree\n"), "list: '%s'", con);

    /* ---- running the word chosen ---- */
    CHECK(run_keys("select c in pwd break; do $c; done; echo out", "1\r2\r") == 0, "run: %s", con);
    CHECK(has("#? 1\n/\n#? 2\nout\n"), "pwd then break: '%s'", con);
    /* A wrong number runs nothing, as $c is empty. */
    CHECK(run_keys("select c in pwd break; do $c; done", "9\r2\r") == 0 && !has("\n/\n"),
          "nothing run: '%s'", con);

    /* ---- break ---- */
    CHECK(run_keys("select p in a; do break; echo no; done; echo $?", "1\r") == 0, "break: %s", con);
    CHECK(!has("no\n") && has("\n0\n"), "break ends the LIST: '%s'", con);
    CHECK(run_keys("break", NULL) == 1 && has("break: not in a select"), "outside: '%s'", con);
    CHECK(run_keys("select p in a; do break 0; done", "1\r") == 1 && has("usage: break [N]"),
          "break 0: '%s'", con);
    /* The status is the LIST's last. */
    CHECK(run_keys("select p in a; do cat /nofile; done", "1\r") == 1, "status: %s", con);
    /* Nested, on one line: break 2 leaves both. */
    CHECK(run_keys("select a in x; do select b in y; do break 2; done; echo no; done; echo out",
                   "1\r1\r") == 0,
          "break 2: %s", con);
    CHECK(!has("no\n") && has("out\n"), "break 2: '%s'", con);
    /* break leaves only the inner one. */
    CHECK(run_keys("select a in x; do select b in y; do break; done; echo inner $b; done",
                   "1\r1\r") == 0,
          "break 1: %s", con);
    CHECK(has("inner y\n"), "break 1: '%s'", con);
    /* More than there are is all of them. */
    CHECK(run_keys("select a in x; do break 9; done; echo out", "1\r") == 0 && has("out\n"),
          "break 9: '%s'", con);

    /* ---- the words ---- */
    CHECK(f_mkdir("/g") == FR_OK && put("/g/one.txt", "") && put("/g/two.txt", "") && put("/g/x.bin", ""),
          "files");
    CHECK(run_keys("select f in /g/*.txt; do echo $f; done", "2\r") == 0, "glob: %s", con);
    CHECK(has("1) /g/one.txt  2) /g/two.txt\n") && has("\n/g/two.txt\n"), "glob: '%s'", con);
    CHECK(run_keys("w=hello; select f in $w world; do echo $f; done", "1\r") == 0 && has("\nhello\n"),
          "variable: '%s'", con);
    /* No words: nothing, status 0. */
    CHECK(run_keys("e=; select f in $e; do echo no; done", "1\r") == 0 && !has("#?"),
          "no words: '%s'", con);
    /* Twelve, down the columns, the numbers right-aligned, each row short
     * of the 40th column, where the terminal would wrap. */
    CHECK(run_keys("select f in a b c d e f g h i j k l; do echo -n; done", "") == 0, "twelve");
    CHECK(has(" 1) a   4) d   7) g  10) j\n 2) b   5) e   8) h  11) k\n 3) c   6) f   9) i  12) l\n"),
          "columns: '%s'", con);
    /* Three of 12 columns would fill the row: two. */
    CHECK(run_keys("select f in aaaaaaaaa bbbbbbbbb ccccccccc; do echo -n; done", "") == 0 &&
              has("1) aaaaaaaaa  3) ccccccccc\n2) bbbbbbbbb\n"),
          "not a full row: '%s'", con);
    /* A word too wide for two columns: one per row. */
    CHECK(run_keys("select f in abcdefghijklmnopqrstuvwxyz0123456789 b; do echo -n; done", "") == 0 &&
              has("1) abcdefghijklmnopqrstuvwxyz0123456789\n2) b\n"),
          "wide: '%s'", con);

    /* ---- leaving it ---- */
    /* The end of the input ends it. */
    CHECK(run_keys("select p in a; do echo $p; done; echo after", "1\r") == 0 && has("after\n"),
          "end of input: '%s'", con);
    /* Ctrl-C stops the line. */
    CHECK(run_keys("select p in a; do echo $p; done; echo after", "1\r\x03") == 130, "stop: %s", con);
    CHECK(has("^C\n") && !has("after"), "stopped: '%s'", con);
    char brk[] = { (char)KEY_BREAK, 0 };
    CHECK(run_keys("select p in a; do echo $p; done", brk) == 130, "break key");
    CHECK(run_keys("select p in a; do echo $p; done", "\x1b") == 130, "esc");

    /* ---- mistakes ---- */
    struct { const char *line, *why; } bad[] = {
        { "select p in a; do echo", "select with no done" },
        { "select p in a", "select with no done" },
        { "select p a; do echo; done", "usage: select NAME in WORD...; do LIST; done" },
        { "select p; do echo; done", "usage: select" },
        { "select 1p in a; do echo; done", "select: not a name" },
        { "select p in a; echo; do echo; done", "select: do expected" },
        { "select p in a; done", "select: do expected" },
        { "do echo", "do without select" },
        { "done", "done without select" },
        { "select p in a; do echo; done x", "done: a word after it" },
        { "select p in a; do echo; done > f", "a select cannot be redirected" },
        { "select p in a; do echo; done | more", "only ; or a line's end" },
        { "select p in a | more; do echo; done", "only ; or a line's end" },
        { "true && select p in a; do echo; done", "may follow only ;" },
        { "ls | select p in a; do echo; done", "may follow only ;" },
        { "select p in a; do echo; done && true", "only ; or a line's end" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(run_keys(bad[i].line, "1\r") == 2 && has(bad[i].why) && !has("#?"), "'%s': '%s'",
              bad[i].line, con);
    }
    /* Quoted, it is a word: not the keyword. */
    CHECK(run_keys("'select' p in a", NULL) == 2 && has("select: only as a command's first word"),
          "quoted: '%s'", con);
    CHECK(run_keys("echo select do done", NULL) == 0 && has("select do done\n"), "words: '%s'", con);
    /* The DO in a LIST's redirection belongs to its command. */
    CHECK(run_keys("select p in a; do echo $p > /g/out; done", "1\r") == 0, "do redirect: %s", con);
    FIL f;
    char buf[16] = { 0 };
    UINT n = 0;
    CHECK(f_open(&f, "/g/out", FA_READ) == FR_OK && f_read(&f, buf, sizeof buf - 1u, &n) == FR_OK &&
              f_close(&f) == FR_OK && strcmp(buf, "a\n") == 0,
          "out: '%s'", buf);

    /* ---- scripts ---- */
    CHECK(put("/s1",
              "# a menu\n"
              "PS3='> '\n"
              "select p in one two quit # the words\n"
              "do\n"
              "  echo chose $p\n"
              "  select q in x y\n"
              "  do\n"
              "    echo inner $q\n"
              "    break\n"
              "  done\n"
              "  echo back\n"
              "done\n"
              "echo end\n"),
          "s1");
    CHECK(run_keys("sh /s1", "1\r2\r\r") == 0, "s1: %s", con);
    CHECK(has("1) one   2) two   3) quit\n> 1\nchose one\n1) x  2) y\n> 2\ninner y\nback\n"),
          "script: '%s'", con);
    CHECK(has("\nend\n"), "script end: '%s'", con);
    /* A mistake in LIST is reported with its own line. */
    CHECK(put("/s2", "select p in a\ndo\necho 'x\ndone\necho after\n"), "s2");
    CHECK(run_keys("sh /s2", "1\r") == 2 && has("sh: /s2: line 3: unterminated '") && !has("after"),
          "s2: '%s'", con);
    /* No done: the select's own line. */
    CHECK(put("/s3", "echo first\nselect p in a\ndo\necho $p\n"), "s3");
    CHECK(run_keys("sh /s3", "1\r") == 2 && has("first\n") &&
              has("sh: /s3: line 2: select with no done") && !has("#?"),
          "s3: '%s'", con);
    /* A mistake in a select ends the script. */
    CHECK(put("/s4", "select p in a\necho no do\ndo\necho $p\ndone\necho after\n"), "s4");
    CHECK(run_keys("sh /s4", "1\r") == 2 && has("sh: /s4: line 2: select: do expected") &&
              !has("after") && !has("#?"),
          "s4: '%s'", con);
    /* break does not reach out of a script run from LIST. */
    CHECK(put("/s5", "break\necho in s5\n"), "s5");
    CHECK(run_keys("select p in a; do sh /s5; echo after s5; break; done", "1\r") == 0, "s5: %s", con);
    CHECK(has("break: not in a select") && has("in s5\nafter s5\n"), "s5: '%s'", con);
    /* At the prompt, a select fits on one line. */
    CHECK(run_keys("select p in a", NULL) == 2 && has("shell: select with no done"), "prompt: '%s'",
          con);
    /* After all that, an ordinary line still runs. */
    CHECK(run_keys("echo fine", NULL) == 0 && strcmp(con, "fine\n") == 0, "fine: '%s'", con);

    /* ---- exit leaves the selects with its script, and the next runs ---- */
    CHECK(put("/ex.sh", "select p in a b\ndo\nselect q in c\ndo\nexit 7\ndone\necho never\ndone\necho never\n"),
          "ex.sh");
    CHECK(run_keys("sh /ex.sh; echo got $?", "1\r1\r") == 0 && has("got 7\n") && !has("never"), "'%s'", con);
    CHECK(run_keys("select p in a; do echo $p; done", "1\r\033") != 0 && has("a\n"), "selects still work: '%s'", con);
    sh.stop = false;

    /* ---- no keyboard ---- */
    const shell_hooks_t none = { now, interrupted, NULL, NULL, NULL, NULL };
    shell_init(&sh, &out, &none, "test");
    CHECK(run_keys("select p in a; do echo $p; done", NULL) == 1 && has("select: no keyboard"),
          "no keyboard: '%s'", con);

    files_unmount();
    host_disk_close();
    TEST_DONE();
}
