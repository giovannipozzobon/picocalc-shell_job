/* test_unix.c — what UNIX users' habits expect of the UNIX flavour
 * (design.md §4.4): the error stream, scripts by name, the options of rm,
 * cp and mv, ~, cd -, $PWD, exit, and test, which, tee, find, uniq and
 * sort. The commands run on a FAT32 image, and what they leave is read
 * back through FatFs. */

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "ff.h"
#include "files.h"
#include "host_disk.h"
#include "shell.h"
#include "test_util.h"
#include "vars.h"

static shell_t sh;
static char    con[1 << 15];
static size_t  con_len;

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

static void clear(void) {
    con_len = 0;
    con[0] = '\0';
}

static int run(const char *line) {
    clear();
    return shell_run(&sh, line);
}

static bool put(const char *path, const char *text) {
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    f_write(&f, text, (UINT)strlen(text), &n);
    return f_close(&f) == FR_OK;
}

static const char *slurp(const char *path) {
    static char buf[1024];
    FIL f;
    UINT n = 0;
    buf[0] = '\0';
    if (f_open(&f, path, FA_READ) != FR_OK) return "(none)";
    f_read(&f, buf, sizeof buf - 1, &n);
    buf[n] = '\0';
    f_close(&f);
    return buf;
}

static bool exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

#define IS(line, status, text) \
    CHECK(run(line) == (status) && strcmp(con, text) == 0, "%s: '%s'", line, con)

int main(void) {
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, NULL, NULL, NULL, NULL };
    shell_init(&sh, &out, &hooks, "test");
    CHECK(host_disk_create(NULL, 64u << 20, true), "image");
    files_mount();
    CHECK(f_mkdir("/home") == FR_OK && f_mkdir("/home/d") == FR_OK && f_mkdir("/home/d/deep") == FR_OK &&
              f_mkdir("/pico2-apps") == FR_OK && put("/home/d/x.txt", "x\n") && put("/home/a.txt", "a\n"),
          "files");
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/home") == 0, "home: %s", sh.cwd);
    static const char nosuch[] = "ls: nosuch: no such file or directory\n";

    /* ---- the error stream ---- */
    IS("ls nosuch 2> /dev/null", 1, "");
    CHECK(run("ls nosuch 2> e.txt") == 1 && !con[0] && strcmp(slurp("/home/e.txt"), nosuch) == 0,
          "2> file: '%s'", con);
    CHECK(run("ls nosuch 2>> e.txt") == 1 && strlen(slurp("/home/e.txt")) == 2 * strlen(nosuch),
          "2>> file: '%s'", slurp("/home/e.txt"));
    CHECK(run("ls nosuch > o.txt 2>&1") == 1 && !con[0] && strcmp(slurp("/home/o.txt"), nosuch) == 0,
          "> file 2>&1: '%s'", con);
    IS("ls nosuch > /dev/null 2>&1", 1, "");
    CHECK(run("ls nosuch 2>&1 > o.txt") == 1 && strcmp(con, nosuch) == 0 && !slurp("/home/o.txt")[0],
          "2>&1 > file, in sh's order: '%s'", con);
    IS("ls nosuch 2>&1 | wc -l", 0, "1\n");
    IS("ls nosuch | wc -l", 0, "ls: nosuch: no such file or directory\n0\n");
    IS("echo oops >&2 | wc -l", 2, "shell: output both redirected and piped\n");
    IS("echo oops >&2", 0, "oops\n");
    CHECK(run("echo oops 2> f.txt >&2") == 0 && !con[0] && strcmp(slurp("/home/f.txt"), "oops\n") == 0,
          ">&2 into 2>'s file: '%s'", con);
    CHECK(run("ls nosuch > s.txt 2> S.TXT") == 1 && strstr(con, "both > and 2>"), "'%s'", con);
    CHECK(!exists("/home/e.txt.new") && !exists("/home/o.txt.new"), "no name.new left");
    /* Back to the screen after each command, and for the next on a line. */
    IS("ls nosuch 2> /dev/null; ls nosuch", 1, nosuch);
    /* Words that only look like it. */
    CHECK(run("echo 2 > two.txt") == 0 && strcmp(slurp("/home/two.txt"), "2\n") == 0, "echo 2");
    /* What leaves the shell. */
    CHECK(run("menu 2> m.txt") == 1 && strstr(con, "not with a redirection") && !exists("/home/m.txt"),
          "'%s'", con);
    /* A script's errors go with the command that ran it. */
    CHECK(put("/home/bad.sh", "ls nosuch\n"), "bad.sh");
    CHECK(run("sh bad.sh 2> b.txt") == 1 && !con[0] && strcmp(slurp("/home/b.txt"), nosuch) == 0,
          "'%s'", con);

    /* ---- /dev/null as input ---- */
    IS("cat /dev/null", 0, "");
    CHECK(run("cat /dev/null > t.txt") == 0 && exists("/home/t.txt") && !slurp("/home/t.txt")[0], "empties");
    IS("wc -l < /dev/null", 0, "0\n");
    IS("cat < /dev/null", 0, "");

    /* ---- scripts and programs by name ---- */
    CHECK(put("/home/s.sh", "echo in s\n") && put("/home/bare", "#!/bin/sh\necho in bare\n") &&
              put("/home/plain", "not a script\n") && put("/home/p.uf2", "x") && put("/pico2-apps/q.uf2", "x"),
          "scripts");
    IS("./s.sh", 0, "in s\n");
    IS("./bare", 0, "in bare\n");
    IS("/home/s.sh", 0, "in s\n");
    CHECK(run("./s.sh > o.txt") == 0 && strcmp(slurp("/home/o.txt"), "in s\n") == 0, "redirected");
    IS("./s.sh extra", 1, "./s.sh: a script takes no arguments\n");
    IS("s.sh", 127, "s.sh: not found\n");   /* the current directory is not searched */
    CHECK(run("export PATH=/pico2-apps:/home") == 0, "PATH");
    IS("s.sh", 0, "in s\n");
    CHECK(run("./p") == 1 && strstr(con, "cannot run programs"), "./p is p.uf2: '%s'", con);
    CHECK(run("./p.uf2") == 1 && strstr(con, "cannot run programs"), "'%s'", con);
    CHECK(run("p.uf2") == 1 && strstr(con, "cannot run programs"), "on PATH with .uf2: '%s'", con);
    CHECK(run("./plain") != 0 && !strstr(con, "not a script") , "not run as a script: '%s'", con);
    CHECK(run("./nosuch") != 0 && strstr(con, "no such file"), "'%s'", con);
    CHECK(run("export PATH=/pico2-apps") == 0, "PATH back");
    IS("p", 127, "p: not found\n");

    /* ---- options ---- */
    CHECK(put("/home/m.txt", "m\n") && run("mv -f m.txt n.txt") == 0 && exists("/home/n.txt"), "mv -f");
    IS("mv -z a b", 1, "mv: -z: unknown option\nusage: mv [-f] from... to\n");
    IS("rm -f nosuch", 0, "");
    IS("rm -f", 0, "");
    CHECK(run("rm nosuch") == 1 && strstr(con, "no such file"), "still says, without -f");
    IS("rm -f n.txt", 0, "");
    CHECK(!exists("/home/n.txt"), "rm -f removes");
    CHECK(run("rm -f d") == 1 && strstr(con, "is a directory"), "a directory wants -r: '%s'", con);
    CHECK(run("mkdir r") == 0 && run("rm -rf r") == 0 && !exists("/home/r"), "rm -rf");
    CHECK(run("mkdir r") == 0 && run("rm -R r") == 0 && !exists("/home/r"), "rm -R");
    IS("rm -rf nosuch", 0, "");
    CHECK(run("rm -rf /") == 1 && strstr(con, "refusing"), "rm -rf /");
    CHECK(run("cp -R d e") == 0 && exists("/home/e/x.txt"), "cp -R");
    CHECK(run("cp -f a.txt b.txt") == 0 && exists("/home/b.txt"), "cp -f");
    CHECK(run("cp -f d z") == 1 && strstr(con, "is a directory"), "cp -f is not -r: '%s'", con);
    CHECK(run("rm -r e") == 0 && run("rm b.txt") == 0, "clean up");

    /* ---- ~, cd -, $PWD ---- */
    IS("echo ~ ~/x a~ '~' \"~\" ~nobody \\~", 0, "/home /home/x a~ ~ ~ ~nobody ~\n");
    CHECK(run("X=~/k") == 0 && strcmp(vars_get("X", 1), "/home/k") == 0, "in a value");
    CHECK(run("cd /") == 0 && run("cat ~/a.txt") == 0 && strcmp(con, "a\n") == 0, "~/ from /: '%s'", con);
    CHECK(run("echo y > ~/y.txt") == 0 && exists("/home/y.txt"), "in a redirection");
    IS("echo $PWD ${PWD}", 0, "/ /\n");
    CHECK(run("cd ~/d") == 0 && strcmp(sh.cwd, "/home/d") == 0, "cd ~/d");
    IS("echo $PWD $OLDPWD", 0, "/home/d /\n");
    IS("cd -", 0, "/\n");
    IS("cd -", 0, "/home/d\n");
    CHECK(run("cd nosuch") == 1 && strcmp(vars_get("OLDPWD", 6), "/") == 0, "a failed cd moves nothing");
    CHECK(run("unset OLDPWD") == 0, "unset");
    IS("cd -", 1, "cd: OLDPWD is not set\n");
    CHECK(run("cd /home") == 0, "home");

    /* ---- exit ---- */
    CHECK(put("/home/i.sh", "echo in\nexit 3\necho never\n") &&
              put("/home/o.sh", "sh i.sh\necho outer $?\nexit\necho never\n") &&
              put("/home/j.sh", "false; exit; echo never\necho never\n") &&
              put("/home/x.sh", "exit x\necho never\n"),
          "exit's scripts");
    IS("sh i.sh; echo $?", 0, "in\n3\n");
    IS("sh o.sh; echo top $?", 0, "in\nouter 3\ntop 0\n");
    IS("sh j.sh; echo $?", 0, "1\n");
    IS("sh x.sh; echo $?", 0, "exit: x: not a number\n2\n");
    IS("exit 1 2", 2, "usage: exit [N]\n");
    /* At the prompt: the header, the rest of the line dropped, and all else kept. */
    CHECK(run("Y=kept") == 0 && run("cd d") == 0, "state");
    CHECK(run("exit 4; echo dropped") == 4 && strstr(con, "PicoCalc shell") && strstr(con, "test\n\n") &&
              !strstr(con, "dropped"),
          "'%s'", con);
    IS("echo $? $Y $PWD", 0, "4 kept /home/d\n");
    CHECK(run("exit 4") == 4 && run("exit") == 4, "plain exit keeps $?");
    CHECK(run("cd ..") == 0, "cd ..");
    IS("echo next line runs", 0, "next line runs\n");

    /* ---- true, false, test ---- */
    IS("true && echo y", 0, "y\n");
    IS("false || echo n", 0, "n\n");
    static const struct { const char *line; int status; } tests[] = {
        { "test -f a.txt", 0 }, { "test -f d", 1 }, { "test -f nosuch", 1 },
        { "[ -d d ]", 0 }, { "[ -d a.txt ]", 1 }, { "[ -d / ]", 0 }, { "[ -f / ]", 1 },
        { "[ -e a.txt ]", 0 }, { "[ -e nosuch ]", 1 }, { "[ -s a.txt ]", 0 }, { "[ -s t.txt ]", 1 },
        { "[ a = a ]", 0 }, { "[ a = b ]", 1 }, { "[ a != b ]", 0 }, { "[ a == a ]", 0 },
        { "[ 2 -lt 10 ]", 0 }, { "[ 10 -lt 2 ]", 1 }, { "[ 2 -eq 2 ]", 0 }, { "[ 2 -ne 2 ]", 1 },
        { "[ 2 -le 2 ]", 0 }, { "[ 3 -gt 2 ]", 0 }, { "[ 2 -ge 3 ]", 1 },
        { "[ ! -e nosuch ]", 0 }, { "[ ! a = a ]", 1 }, { "[ -z \"\" ]", 0 }, { "[ -n \"\" ]", 1 },
        { "[ -n x ]", 0 }, { "[ x ]", 0 }, { "[ \"\" ]", 1 }, { "[ ]", 1 }, { "test", 1 },
        { "[ x -lt 1 ]", 2 }, { "[ a", 2 }, { "[ a b c d ]", 2 }, { "[ a -zz b ]", 2 },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        CHECK(run(tests[i].line) == tests[i].status, "%s: %d '%s'", tests[i].line, sh.status, con);
    IS("[ -e a.txt ] && echo there", 0, "there\n");

    /* ---- which, less ---- */
    IS("which ls", 0, "ls: builtin\n");
    IS("which q", 0, "/pico2-apps/q.uf2\n");
    IS("which nosuch", 1, "");
    IS("which ls nosuch", 1, "ls: builtin\n");
    IS("which ./s.sh", 0, "/home/s.sh\n");
    IS("which", 1, "usage: which name...\n");
    IS("less a.txt", 0, "a\n");

    /* ---- tee ---- */
    CHECK(run("cat a.txt | tee t.txt") == 0 && strcmp(con, "a\n") == 0 && strcmp(slurp("/home/t.txt"), "a\n") == 0,
          "tee: '%s'", con);
    CHECK(run("cat a.txt | tee -a t.txt") == 0 && strcmp(slurp("/home/t.txt"), "a\na\n") == 0, "tee -a");
    IS("tee < a.txt", 0, "a\n");
    IS("cat a.txt | tee > /dev/null", 0, "");
    CHECK(run("tee x.out") == 1 && strstr(con, "usage") && !exists("/home/x.out"), "no input");
    CHECK(run("cat a.txt | tee p1 p2") == 1 && strstr(con, "usage"), "one file");
    CHECK(run("cat a.txt | tee d") == 1 && !exists("/home/d.new"), "a directory: '%s'", con);

    /* ---- find ---- */
    IS("find d", 0, "d\nd/deep\nd/x.txt\n");
    IS("find d/", 0, "d\nd/deep\nd/x.txt\n");
    IS("find /home/d -type f", 0, "/home/d/x.txt\n");
    IS("find d -type d", 0, "d\nd/deep\n");
    IS("find d -name '*.TXT'", 0, "d/x.txt\n");
    IS("find ~/d -name x.txt", 0, "/home/d/x.txt\n");
    IS("find a.txt", 0, "a.txt\n");
    CHECK(run("find nosuch") == 1 && strstr(con, "no such file"), "'%s'", con);
    CHECK(run("find d -x y") == 1 && strstr(con, "usage: find"), "'%s'", con);
    CHECK(run("find d -name") == 1 && strstr(con, "usage: find"), "'%s'", con);
    CHECK(run("find / -type d") == 0 && strncmp(con, "/\n", 2) == 0 && strstr(con, "/home/d/deep\n"), "'%s'", con);
    CHECK(run("cd d") == 0, "cd d");
    IS("find", 0, ".\n./deep\n./x.txt\n");
    CHECK(run("cd ..") == 0, "cd ..");
    CHECK(put("/home/d/.hid", "h") && run("find d") == 0 && !strstr(con, ".hid"), "hidden left out: '%s'", con);

    /* ---- uniq ---- */
    CHECK(put("/home/u.txt", "a\na\nb\na\n"), "u.txt");
    IS("uniq u.txt", 0, "a\nb\na\n");
    IS("uniq -c u.txt", 0, "      2 a\n      1 b\n      1 a\n");
    IS("uniq -d u.txt", 0, "a\n");
    IS("cat u.txt | uniq", 0, "a\nb\na\n");
    IS("uniq < /dev/null", 0, "");
    CHECK(run("uniq") == 1 && strstr(con, "usage: uniq"), "no input");
    CHECK(run("uniq nosuch") == 1 && strstr(con, "no such file"), "'%s'", con);

    /* ---- sort ---- */
    CHECK(put("/home/n.txt", "10\n9\n9\nb\n"), "n.txt");
    IS("sort n.txt", 0, "10\n9\n9\nb\n");
    IS("sort -r n.txt", 0, "b\n9\n9\n10\n");
    IS("sort -n n.txt", 0, "b\n9\n9\n10\n");
    IS("sort -rn n.txt", 0, "10\n9\n9\nb\n");
    IS("sort -u n.txt", 0, "10\n9\nb\n");
    IS("cat n.txt | sort -nu", 0, "b\n9\n10\n");
    IS("sort a.txt u.txt", 0, "a\na\na\na\nb\n");
    IS("sort < /dev/null", 0, "");
    CHECK(run("sort") == 1 && strstr(con, "usage: sort"), "no input");
    CHECK(run("sort nosuch") == 1 && strstr(con, "no such file"), "'%s'", con);
    /* Too much is refused, with nothing shown. */
    {
        FIL f;
        UINT n;
        static char line[1001];
        memset(line, 'x', 1000);
        line[1000] = '\n';
        CHECK(f_open(&f, "/home/big.txt", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK, "big");
        for (unsigned i = 0; i < VI_BUF_SIZE / 1000u + 1u; i++) f_write(&f, line, sizeof line, &n);
        f_close(&f);
    }
    CHECK(run("sort big.txt") == 1 && strncmp(con, "sort: more than", 15) == 0, "'%.40s'", con);
    /* As much as fits, with an index of it. */
    {
        FIL f;
        UINT n;
        CHECK(f_open(&f, "/home/full.txt", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK, "full");
        for (unsigned i = 0; i < (VI_BUF_SIZE - 64u) / 4u; i++) f_write(&f, i % 2 ? "b\n" : "a\n", 2, &n);
        f_close(&f);
    }
    CHECK(run("sort full.txt | wc -l") == 0 && !strstr(con, "more than"), "fits: '%.40s'", con);

    host_disk_close();
    TEST_DONE();
}
