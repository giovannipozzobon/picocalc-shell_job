/* test_dos.c — the DOS flavour's command line (design.md §4.7, M9 step
 * 1): its paths, wildcards and parser on their own, then its commands
 * run as typed over FAT16 and FAT32 images, as test_files does for the
 * UNIX flavour's. Linked with shell_core_dos.
 *
 * Output and errors are read as text from the terminal's sink, the
 * card's files back through FatFs, and the keys a command waits for
 * (DEL's question, DIR /P's pause) come from a script. */

#include <stdio.h>
#include <string.h>

#include "dos.h"
#include "files.h"
#include "flavour.h"
#include "history.h"
#include "host_disk.h"
#include "keycodes.h"
#include "line.h"
#include "run.h"
#include "shell.h"
#include "test_util.h"
#include "vars.h"

static shell_t sh;
static char    con[1 << 16];   /* the terminal's text, CR removed */
static size_t  con_len;
static const char *keys = "";  /* what getkey types; -1 after it */

static void sink(void *ctx, const char *buf, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n; i++)
        if (buf[i] != '\r' && con_len + 1 < sizeof con) con[con_len++] = buf[i];
    con[con_len] = '\0';
}

/* The clock, which moves only while pollkey finds no key. */
static uint64_t s_clock;

static uint64_t now(void *ctx) {
    (void)ctx;
    return s_clock;
}

/* Break after this many more polls; never if negative. */
static int break_after = -1;

static bool interrupted(void *ctx) {
    (void)ctx;
    return break_after >= 0 && break_after-- == 0;
}

static int getkey(void *ctx) {
    (void)ctx;
    return *keys ? (unsigned char)*keys++ : -1;
}

/* The next key typed, or a quarter of a second with none. */
static int pollkey(void *ctx) {
    (void)ctx;
    if (*keys) return (unsigned char)*keys++;
    s_clock += 250000u;
    return -1;
}

static void clear(void) {
    con_len = 0;
    con[0] = '\0';
}

/* Run a line; its status, with the terminal's text in con. */
static int run(const char *line) {
    clear();
    return shell_run(&sh, line);
}

static bool has(const char *text) {
    return strstr(con, text) != NULL;
}

static const char *slurp(const char *path) {
    static char buf[1 << 15];
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_READ) != FR_OK) return NULL;
    FRESULT r = f_read(&f, buf, sizeof buf - 1, &n);
    f_close(&f);
    if (r != FR_OK) return NULL;
    buf[n] = '\0';
    return buf;
}

static bool exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

static BYTE attrs(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK ? fno.fattrib : 0xFF;
}

static bool put(const char *path, const char *text) {
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT r = f_write(&f, text, (UINT)strlen(text), &n);
    return f_close(&f) == FR_OK && r == FR_OK && n == strlen(text);
}

/* A line of DIR as it is in 40 columns: the name, then the size at the
 * right, short of the last column. */
/* With the clock not set, every entry has FatFs's fixed date. */
#define FIXED_DATE "01-01-26 12:00a"

/* DIR's row for an 8.3 name, in 39 columns (design.md §3.8): the name,
 * the size or <DIR> in 11, the date and the time. */
static const char *dir_line(const char *name, const char *size) {
    static char buf[128];
    snprintf(buf, sizeof buf, "\n%-12s%11s " FIXED_DATE "\n", name, size);
    return buf;
}

/* ...and for any other name, the rest on the next row in its columns. */
static const char *dir_next(const char *name, const char *size) {
    static char buf[160];
    snprintf(buf, sizeof buf, "\n%s\n%12s%11s " FIXED_DATE "\n", name, "", size);
    return buf;
}

/* The column of `word` in the line of con that holds `line`, or -1. */
static int column_of(const char *line, const char *word) {
    const char *l = strstr(con, line);
    if (!l) return -1;
    while (l > con && l[-1] != '\n') l--;
    const char *w = strstr(l, word);
    const char *end = strchr(l, '\n');
    return w && (!end || w < end) ? (int)(w - l) : -1;
}

/* ---- on their own ----------------------------------------------------- */

static int unit_tests(void) {
    static const struct {
        const char *pat, *name;
        bool hit;
    } m[] = {
        { "*.*", "README", true },      { "*.*", "a.b.c", true },
        { "*.", "README", true },       { "*.", "A.TXT", false },
        { "*.TXT", "notes.txt", true }, { "*.TXT", "TXT", false },
        { "A?.TXT", "A.TXT", true },    { "A?.TXT", "AB.TXT", true },
        { "A?.TXT", "ABC.TXT", false }, { "*", "x.y", true },
        { "R*E", "readme", true },      { "R*E", "readmes", false },
        { "pico-*.cfg", "pico-atom.cfg", true },
        { "*.gz", "archive.tar.gz", true },
        { "FOO", "foo", true },         { "FOO", "foo.txt", false },
        { "FOO.*", "foo", true },
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++)
        CHECK(dos_match(m[i].pat, m[i].name) == m[i].hit, "match %s %s", m[i].pat, m[i].name);

    static const struct {
        const char *mask, *name, *want;
    } rn[] = {
        { "*.BAK", "README.TXT", "README.BAK" }, { "X*.*", "foo.c", "Xoo.c" },
        { "?A*", "abc", "aAc" },                 { "*.", "a.txt", "a" },
        { "NEW.TXT", "old.txt", "NEW.TXT" },     { "*.OLD", "noext", "noext.OLD" },
        { "??.*", "abcd.e", "ab.e" },
    };
    for (unsigned i = 0; i < sizeof rn / sizeof rn[0]; i++) {
        char out[64];
        CHECK(dos_rename(out, sizeof out, rn[i].mask, rn[i].name) && strcmp(out, rn[i].want) == 0,
              "rename %s %s: '%s'", rn[i].mask, rn[i].name, out);
    }

    static const struct {
        const char *arg, *want;
    } rs[] = {
        { "C:\\X", "/X" },   { "..\\Y", "/Y" },       { "C:Z", "/HOME/Z" },
        { "c:", "/HOME" },        { "C:\\", "/" },
        { "a\\.\\b\\..", "/HOME/a" }, { "\\", "/" },
    };
    for (unsigned i = 0; i < sizeof rs / sizeof rs[0]; i++) {
        char out[SHELL_PATH_MAX + 1];
        CHECK(dos_resolve(out, "/HOME", rs[i].arg) && strcmp(out, rs[i].want) == 0,
              "resolve %s: '%s'", rs[i].arg, out);
    }
    char out[SHELL_PATH_MAX + 1];
    CHECK(!dos_resolve(out, "/", "A:\\X") && dos_other_drive("b:") && !dos_other_drive("C:"),
          "other drives");
    CHECK(!dos_resolve(out, "/HOME", "/A/B") && !dos_resolve(out, "/HOME", "A/B") &&
              !dos_resolve(out, "/HOME", "C:/A"),
          "'/' is no separator");
    char shown[64];
    dos_show(shown, sizeof shown, "/");
    CHECK(strcmp(shown, "C:\\") == 0, "root shown '%s'", shown);
    dos_show(shown, sizeof shown, "/A/b");
    CHECK(strcmp(shown, "C:\\A\\b") == 0, "path shown '%s'", shown);

    vars_set("FOO", 3, "bar");
    char line[64];
    CHECK(dos_expand(line, sizeof line, "%foo%-%%-%NOPE%-50% %", NULL) &&
              strcmp(line, "bar-%--50% %") == 0,
          "expand '%s'", line);
    CHECK(!dos_expand(line, 8, "%FOO%%FOO%%FOO%", NULL), "expansion too long");

    static dos_parse_t p;
    CHECK(dos_parse(&p, "DIR/W/P") && p.ncmds == 1 && p.cmds[0].argc == 2 &&
              strcmp(p.cmds[0].argv[0], "DIR") == 0 && strcmp(p.cmds[0].argv[1], "/W/P") == 0,
          "DIR/W/P");
    CHECK(dos_parse(&p, "cd..") && p.cmds[0].argc == 2 && strcmp(p.cmds[0].argv[0], "cd") == 0 &&
              strcmp(p.cmds[0].argv[1], "..") == 0,
          "cd..");
    CHECK(dos_parse(&p, "CD\\") && p.cmds[0].argc == 2 && strcmp(p.cmds[0].argv[1], "\\") == 0,
          "CD\\");
    CHECK(dos_parse(&p, "ECHO.") && p.cmds[0].argc == 2 && strcmp(p.cmds[0].tail, ".") == 0,
          "ECHO.");
    CHECK(dos_parse(&p, "pico-atom") && strcmp(p.cmds[0].argv[0], "pico-atom") == 0, "a program");
    CHECK(dos_parse(&p, "FOO.BAT x") && strcmp(p.cmds[0].argv[0], "FOO.BAT") == 0,
          "not a builtin, not cut");
    CHECK(dos_parse(&p, "ECHO  a   b > f.txt") && strcmp(p.cmds[0].tail, "  a   b") == 0 &&
              strcmp(p.cmds[0].out, "f.txt") == 0 && !p.cmds[0].append,
          "tail '%s'", p.cmds[0].tail);
    CHECK(dos_parse(&p, "TYPE \"my file.txt\">>\"o u t\"") && p.cmds[0].argc == 2 &&
              strcmp(p.cmds[0].argv[1], "my file.txt") == 0 &&
              strcmp(p.cmds[0].out, "o u t") == 0 && p.cmds[0].append,
          "quotes");
    CHECK(dos_parse(&p, "SORT<in.txt|FIND \"a|b\" | MORE") && p.ncmds == 3 &&
              strcmp(p.cmds[0].in, "in.txt") == 0 && strcmp(p.cmds[1].argv[1], "a|b") == 0,
          "pipes");
    CHECK(dos_parse(&p, "   ") && p.ncmds == 0, "blank");
    CHECK(!dos_parse(&p, "| X") && !dos_parse(&p, "X |") && !dos_parse(&p, "X >") &&
              !dos_parse(&p, "> f"),
          "syntax errors");
    return 0;
}

/* ---- batch files (step 2) --------------------------------------------- */

/* In \HOME, with HERE.UF2 there and the prompt $P$G. */
static int batch_tests(void) {
    CHECK(run("CD \\HOME") == 0 && run("PROMPT $P$G") == 0 && run("ECHO ON") == 0, "set up");

    /* Parameters, @, ECHO OFF for the file alone, REM. */
    CHECK(put("/HOME/T1.BAT", "@ECHO OFF\r\nECHO one %1 [%2] %3.\r\nREM nothing > X.TXT\r\n"
                              "ECHO %0 %%\r\n"),
          "T1");
    CHECK(run("t1 a,b") == 0 && strcmp(con, "one a [b] .\nt1 %\n") == 0, "'%s'", con);
    CHECK(!exists("/HOME/X.TXT"), "REM makes no file");
    CHECK(run("ECHO") == 0 && strcmp(con, "ECHO is on\n") == 0, "echo back on: '%s'", con);
    CHECK(run("T1.BAT \"a b\" c") == 0 && has("one \"a b\" [c] ."), "quotes kept: '%s'", con);
    CHECK(run("ECHO %1 %0") == 0 && strcmp(con, "%1 %0\n") == 0, "at the prompt: '%s'", con);

    /* ECHO ON shows each line after the prompt. */
    CHECK(put("/HOME/T2.BAT", "ECHO hi\r\n:LABEL\r\n@ECHO quiet\r\n"), "T2");
    CHECK(run("T2") == 0 && strcmp(con, "\nC:\\HOME>ECHO hi\nhi\nquiet\n") == 0, "'%s'", con);

    /* GOTO, labels, SHIFT and IF a==b. */
    CHECK(put("/HOME/LOOP.BAT", "@ECHO OFF\r\n:LOOP\r\nIF \"%1\"==\"\" GOTO END\r\n"
                                "ECHO [%1]\r\nSHIFT\r\nGOTO :loop\r\n:END extra\r\nECHO done\r\n"),
          "LOOP");
    CHECK(run("LOOP a b c d e f g h i j k") == 0 &&
              strcmp(con, "[a]\n[b]\n[c]\n[d]\n[e]\n[f]\n[g]\n[h]\n[i]\n[j]\n[k]\ndone\n") == 0,
          "'%s'", con);
    CHECK(put("/HOME/NOLABEL.BAT", "@ECHO OFF\r\nGOTO NOWHERE\r\nECHO never\r\n"), "NOLABEL");
    CHECK(run("NOLABEL") == 1 && strcmp(con, "Label not found\n") == 0, "'%s'", con);
    CHECK(run("GOTO X") == 0 && strcmp(con, "") == 0, "GOTO at the prompt");

    /* IF's three tests, NOT, and IF after IF. */
    CHECK(put("/HOME/IFS.BAT", "@ECHO OFF\r\nIF EXIST t1.bat ECHO e1\r\n"
                               "IF NOT EXIST NOPE.TXT ECHO e2\r\nIF EXIST *.BAT ECHO e3\r\n"
                               "IF EXIST *.ZZZ ECHO bad1\r\nIF EXIST \\PICO2-APPS ECHO e4\r\n"
                               "TYPE NOPE.TXT\r\nIF NOT ERRORLEVEL 1 ECHO bad2\r\n"
                               "IF ERRORLEVEL 2 ECHO bad3\r\nIF ERRORLEVEL 1 ECHO e5\r\n"
                               "IF a==a IF NOT b == c ECHO e6\r\nIF \"a b\"==\"a b\" ECHO e7\r\n"
                               "IF A==a ECHO bad4\r\n"),
          "IFS");
    run("IFS");
    CHECK(has("e1\ne2\ne3\ne4\nFile not found - NOPE.TXT\ne5\ne6\ne7\n") && !has("bad"), "'%s'",
          con);
    CHECK(run("IF ==x ECHO y") == 2 && strcmp(con, "Syntax error\n") == 0, "'%s'", con);
    CHECK(run("IF a==a") == 2 && has("Syntax error"), "no command: '%s'", con);
    CHECK(run("IF ERRORLEVEL x ECHO y") == 2 && has("Syntax error"), "'%s'", con);
    CHECK(run("ECHO x | IF a==a ECHO y") == 2 && has("Syntax error"), "after a pipe: '%s'", con);

    /* CHOICE: the keys' place, the prompt, /N, /S and /T. */
    keys = "n";
    CHECK(run("CHOICE") == 2 && strcmp(con, "[Y,N]?N\n") == 0, "default keys: '%s'", con);
    keys = "q";
    CHECK(run("CHOICE /C:12Q Which one") == 3 && strcmp(con, "Which one[1,2,Q]?Q\n") == 0,
          "/C: '%s'", con);
    keys = "2";
    CHECK(run("choice /c12q \"Pick \"") == 2 && strcmp(con, "Pick [1,2,Q]?2\n") == 0,
          "no colon, quoted text: '%s'", con);
    keys = "b";
    CHECK(run("CHOICE /N /C:ab") == 2 && strcmp(con, "B\n") == 0, "/N: '%s'", con);
    keys = "Aa";
    CHECK(run("CHOICE /S /C:aA") == 2 && strcmp(con, "[a,A]?A\n") == 0, "/S: '%s'", con);
    keys = "xy";
    CHECK(run("CHOICE") == 1 && strcmp(con, "[Y,N]?\aY\n") == 0, "a key not among them: '%s'", con);
    keys = "";
    s_clock = 0;
    CHECK(run("CHOICE /T:N,5") == 2 && strcmp(con, "[Y,N]?N\n") == 0 && s_clock >= 5000000u &&
              s_clock < 5500000u,
          "/T runs out: '%s' at %llu", con, (unsigned long long)s_clock);
    keys = "z";
    s_clock = 0;
    CHECK(run("CHOICE /TN,2 Go") == 2 && strcmp(con, "Go[Y,N]?\aN\n") == 0 && s_clock >= 2000000u &&
              s_clock < 2500000u,
          "/T not restarted: '%s'", con);
    keys = "y";
    CHECK(run("CHOICE /T:N,10") == 1 && strcmp(con, "[Y,N]?Y\n") == 0, "/T, a key: '%s'", con);
    keys = "";
    s_clock = 0;
    CHECK(run("CHOICE /T:n,0") == 2 && s_clock == 250000u, "/T at once: '%s'", con);
    /* With no clock, /T waits for a key as if it had no time limit. */
    sh.hooks.now_us = NULL;
    keys = "";
    CHECK(run("CHOICE /T:N,5") == 130, "/T with no clock: '%s'", con);
    keys = "y";
    CHECK(run("CHOICE /T:N,5") == 1 && strcmp(con, "[Y,N]?Y\n") == 0, "/T with no clock, a key: '%s'", con);
    sh.hooks.now_us = now;
    struct { const char *line, *why; } bad[] = {
        { "CHOICE /X", "CHOICE: invalid switch - /X\n" },
        { "CHOICE /NX", "CHOICE: invalid switch - /NX\n" },
        { "CHOICE /C", "CHOICE: invalid choice switch syntax. Expected form: /C[:]choices\n" },
        { "CHOICE /C:", "CHOICE: invalid choice switch syntax. Expected form: /C[:]choices\n" },
        { "CHOICE /C:a/b", "CHOICE: invalid choice switch syntax. Expected form: /C[:]choices\n" },
        { "CHOICE /T:N", "CHOICE: Incorrect timeout syntax. Expected form Tc,nn or T:c,nn\n" },
        { "CHOICE /T:N,100", "CHOICE: Incorrect timeout syntax. Expected form Tc,nn or T:c,nn\n" },
        { "CHOICE /T:N,x", "CHOICE: Incorrect timeout syntax. Expected form Tc,nn or T:c,nn\n" },
        { "CHOICE /T:Q,5", "CHOICE: Timeout default not in specified (or default) choices.\n" },
        { "CHOICE /S /T:n,5", "CHOICE: Timeout default not in specified (or default) choices.\n" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        keys = "y";
        CHECK(run(bad[i].line) == 255 && strcmp(con, bad[i].why) == 0, "%s: '%s'", bad[i].line, con);
    }
    /* Esc, and the end of the input, stop the line. */
    keys = "\x1b";
    CHECK(run("CHOICE") == 130 && strcmp(con, "[Y,N]?\n^C\n") == 0, "Esc: '%s'", con);
    keys = "";
    CHECK(run("CHOICE") == 130, "end of input: '%s'", con);
    /* A menu in a batch file: IF ERRORLEVEL, highest first, with GOTO. */
    CHECK(put("/HOME/MENU.BAT", "@ECHO OFF\r\n:TOP\r\nECHO 1 one\r\nECHO 2 two\r\n"
                                "CHOICE /C:12Q /N Pick:\r\n"
                                "IF ERRORLEVEL 3 GOTO END\r\nIF ERRORLEVEL 2 GOTO TWO\r\n"
                                "ECHO chose one\r\nGOTO TOP\r\n:TWO\r\nECHO chose two\r\nGOTO TOP\r\n"
                                ":END\r\nECHO bye\r\n"),
          "MENU");
    keys = "21q";
    CHECK(run("MENU") == 0 &&
              strcmp(con, "1 one\n2 two\nPick:2\nchose two\n1 one\n2 two\nPick:1\nchose one\n"
                          "1 one\n2 two\nPick:Q\nbye\n") == 0,
          "menu: '%s'", con);
    keys = "";

    /* FOR, at the prompt with %x and in a file with %%x; a wildcard as
     * typed before each name; echoed with ECHO ON, not after @. */
    CHECK(run("FOR %x IN (a b,c) DO ECHO [%x]") == 0 &&
              strcmp(con, "\nC:\\HOME>ECHO [a]\n[a]\n\nC:\\HOME>ECHO [b]\n[b]\n"
                          "\nC:\\HOME>ECHO [c]\n[c]\n") == 0,
          "'%s'", con);
    CHECK(f_mkdir("/HOME/FT") == FR_OK && put("/HOME/FT/A.TXT", "alpha\r\n") &&
              put("/HOME/FT/B.TXT", "beta\r\n") && put("/HOME/FT/C.DAT", "gamma\r\n") &&
              put("/HOME/FT/D.TXT", "hidden\r\n") && f_chmod("/HOME/FT/D.TXT", AM_HID, AM_HID) == FR_OK,
          "FT");
    CHECK(run("@FOR %f IN (FT\\*.TXT) DO ECHO %f") == 0 && strcmp(con, "FT\\A.TXT\nFT\\B.TXT\n") == 0,
          "'%s'", con);
    CHECK(put("/HOME/FORT.BAT", "@ECHO OFF\r\nFOR %%f IN (FT\\*.TXT FT\\C.DAT) DO TYPE %%f\r\n"), "FORT");
    CHECK(run("FORT") == 0 && strcmp(con, "alpha\nbeta\ngamma\n") == 0, "'%s'", con);
    CHECK(run("@FOR %x IN (a) DO FOR %y IN (b) DO ECHO x") == 2 && has("FOR cannot be nested"),
          "'%s'", con);
    CHECK(run("FOR x IN (a) DO ECHO") == 2 && has("Syntax error") &&
              run("FOR %x IN a DO ECHO") == 2 && run("FOR %x IN (a) ECHO") == 2 &&
              run("FOR %x IN (a) DO") == 2,
          "FOR's syntax");
    run("@FOR %x IN () DO ECHO x");
    CHECK(strcmp(con, "") == 0, "an empty set");

    /* CALL comes back; another batch file without it does not. */
    CHECK(put("/HOME/INNER.BAT", "@ECHO inner %1\r\n") &&
              put("/HOME/OUTER.BAT", "@ECHO OFF\r\nCALL INNER x\r\nECHO back\r\nINNER y\r\n"
                                     "ECHO never\r\n"),
          "INNER, OUTER");
    CHECK(run("OUTER") == 0 && strcmp(con, "inner x\nback\ninner y\n") == 0, "'%s'", con);
    CHECK(run("CALL ECHO  hi") == 0 && strcmp(con, "hi\n") == 0, "CALL a builtin: '%s'", con);
    CHECK(run("CALL INNER z > OUT.TXT") == 0 && strcmp(con, "") == 0 &&
              slurp("/HOME/OUT.TXT") && strstr(slurp("/HOME/OUT.TXT"), "inner z"),
          "CALL with >: '%s'", con);
    CHECK(run("INNER > O.TXT") == 1 && has("Cannot redirect a batch file") &&
              !exists("/HOME/O.TXT"),
          "'%s'", con);
    CHECK(run("ECHO x | INNER") == 1 && has("Cannot redirect a batch file"), "'%s'", con);
    CHECK(put("/HOME/FORCHAIN.BAT", "@ECHO OFF\r\nFOR %%x IN (1 2 3) DO INNER %%x\r\n"
                                    "ECHO never\r\n"),
          "FORCHAIN");
    CHECK(run("FORCHAIN") == 0 && strcmp(con, "inner 1\n") == 0, "chained from FOR: '%s'", con);

    /* Break stops an endless loop, and a FOR, though nothing in them
     * looks for it. */
    CHECK(put("/HOME/ENDLESS.BAT", "@ECHO OFF\r\n:A\r\nECHO x\r\nGOTO A\r\n"), "ENDLESS");
    break_after = 20;
    CHECK(run("ENDLESS") == 130 && has("x\nx\n") && has("^C\n") && break_after < 0, "'%s'", con);
    break_after = 2;
    CHECK(run("@FOR %x IN (1 2 3 4 5) DO ECHO %x") == 130 && !has("4"), "'%s'", con);
    break_after = -1;

    /* A batch file taking its own place runs on with no limit; CALLs
     * nest three deep. */
    CHECK(put("/HOME/COUNT.BAT", "@ECHO OFF\r\nSET N=%N%x\r\nIF NOT \"%N%\"==\"xxxxxx\" COUNT\r\n"
                                 "ECHO %N%\r\n"),
          "COUNT");
    CHECK(run("COUNT") == 0 && strcmp(con, "xxxxxx\n") == 0 && run("SET N=") == 0, "'%s'", con);
    CHECK(put("/HOME/SELF.BAT", "@CALL SELF\r\n"), "SELF");
    CHECK(run("SELF") == 1 && strcmp(con, "Batch files nested more than 3 deep\n") == 0, "'%s'", con);
    CHECK(run("CALL NOPE") == 127 && has("Bad command or file name"), "'%s'", con);

    /* A batch file here or on PATH, or by its path; not under another
     * extension. */
    CHECK(put("/HOME/ALONE.BAT", "@ECHO the batch\r\n") &&
              put("/pico2-apps/ONPATH.BAT", "@ECHO on the path\r\n"),
          "ALONE, ONPATH");
    CHECK(run("alone") == 0 && strcmp(con, "the batch\n") == 0, "'%s'", con);
    CHECK(run("ONPATH") == 0 && strcmp(con, "on the path\n") == 0, "'%s'", con);
    CHECK(run("\\pico2-apps\\onpath") == 0 && strcmp(con, "on the path\n") == 0, "'%s'", con);
    CHECK(run("C:\\PICO2-APPS\\ONPATH.BAT") == 0 && has("on the path"), "'%s'", con);
    CHECK(run("ONPATH.TXT") == 127 && has("Bad command or file name"), "'%s'", con);

    /* A line too long is said, and the next one runs. */
    static char longer[600];
    strcpy(longer, "ECHO ");
    memset(longer + 5, 'y', 300);
    strcpy(longer + 305, "\r\n@ECHO after\r\n");
    CHECK(put("/HOME/LONG.BAT", longer), "LONG");
    CHECK(run("LONG") == 0 && strcmp(con, "Line too long\nafter\n") == 0, "'%s'", con);

    /* AUTOEXEC.BAT at start, its ECHO OFF its own. */
    CHECK(put("/AUTOEXEC.BAT", "@ECHO OFF\r\nSET FROM=autoexec\r\nECHO started\r\n"), "AUTOEXEC");
    clear();
    flavour_start(&sh);
    CHECK(has("(DOS)\n\nstarted\n") && vars_get("FROM", 4) &&
              strcmp(vars_get("FROM", 4), "autoexec") == 0 && dos_echo_on,
          "'%s'", con);
    CHECK(f_unlink("/AUTOEXEC.BAT") == FR_OK, "unlink");
    clear();
    flavour_start(&sh);
    CHECK(!has("started"), "no AUTOEXEC.BAT: '%s'", con);

    CHECK(run("HELP") == 0 && has("CALL") && has("SHIFT") && has("GOTO"), "'%s'", con);
    return 0;
}

/* ---- run as typed ----------------------------------------------------- */

static int dos_tests(bool fat32) {
    const char *fs = fat32 ? "FAT32" : "FAT16";
    CHECK(host_disk_create(NULL, fat32 ? 64u << 20 : 32u << 20, fat32), "%s image", fs);
    files_mount();
    CHECK(f_mkdir("/HOME") == FR_OK && f_mkdir("/pico2-apps") == FR_OK, "dirs");
    CHECK(f_setlabel("PICOCALC") == FR_OK, "label");

    /* The banner and the prompt, $P$G, at the root even with a \HOME. */
    clear();
    shell_start(&sh);
    CHECK(has("PicoCalc shell\033[0m test (DOS)\n\nC:\\>"), "%s: start '%s'", fs, con);
    CHECK(strcmp(sh.cwd, "/") == 0, "cwd %s", sh.cwd);
    CHECK(run("cd \\home") == 0 && strcmp(sh.cwd, "/HOME") == 0, "as the card spells HOME");

    /* Names and switches in either case; CD alone shows where. */
    CHECK(run("cd") == 0 && strcmp(con, "C:\\HOME\n") == 0, "cd: '%s'", con);
    CHECK(run("Md DOCS") == 0 && run("cD docs") == 0 && strcmp(sh.cwd, "/HOME/DOCS") == 0,
          "md, cd: %s", sh.cwd);
    CHECK(run("CD..") == 0 && strcmp(sh.cwd, "/HOME") == 0, "cd..");
    CHECK(run("CD\\") == 0 && strcmp(sh.cwd, "/") == 0, "cd\\");
    CHECK(run("CD C:\\HOME\\DOCS") == 0 && strcmp(sh.cwd, "/HOME/DOCS") == 0, "cd C:");
    CHECK(run("CD \\HOME") == 0 && strcmp(sh.cwd, "/HOME") == 0, "back: '%s'", con);
    /* '/' is a switch, never a separator. */
    CHECK(run("CD /HOME") == 1 && strcmp(con, "Invalid switch - /HOME\n") == 0, "cd with /: '%s'", con);
    CHECK(run("CD DOCS/X") == 1 && strcmp(con, "Invalid switch - /X\n") == 0, "cd a/b: '%s'", con);
    CHECK(run("TYPE DOCS/A.TXT") == 1 && strcmp(con, "Invalid switch - /A.TXT\n") == 0, "type a/b: '%s'", con);
    CHECK(run("MD /NEWDIR") == 1 && strcmp(con, "Invalid switch - /NEWDIR\n") == 0 && !exists("/NEWDIR"),
          "md /x: '%s'", con);
    CHECK(run("ECHO hi > DOCS/OUT.TXT") != 0 && !exists("/HOME/DOCS/OUT.TXT"), "> a/b: '%s'", con);
    CHECK(run("IF EXIST \\HOME ECHO yes") == 0 && strcmp(con, "yes\n") == 0, "IF EXIST \\: '%s'", con);
    CHECK(run("IF EXIST /HOME ECHO yes") == 0 && strcmp(con, "") == 0, "IF EXIST /: '%s'", con);
    CHECK(run("CD NOPE") == 1 && strcmp(con, "Invalid directory\n") == 0, "'%s'", con);
    CHECK(run("MD DOCS") == 1 && strcmp(con, "Unable to create directory\n") == 0, "'%s'", con);
    CHECK(run("A:") == 1 && strcmp(con, "Invalid drive specification\n") == 0, "'%s'", con);
    CHECK(run("C:") == 0 && strcmp(con, "") == 0, "C:");
    CHECK(run("NOPE") == 127 && strcmp(con, "Bad command or file name\n") == 0, "'%s'", con);

    /* ECHO, redirection and TYPE. */
    CHECK(run("ECHO hello  world>A.TXT") == 0 && run("ECHO.>>a.txt") == 0 &&
              run("echo second line >> A.TXT") == 0,
          "echo");
    CHECK(strcmp(slurp("/HOME/A.TXT"), "hello  world\n\nsecond line\n") == 0, "'%s'",
          slurp("/HOME/A.TXT"));
    CHECK(run("TYPE a.txt") == 0 && strcmp(con, "hello  world\n\nsecond line\n") == 0, "'%s'",
          con);
    CHECK(run("TYPE NOPE.TXT") == 1 && strcmp(con, "File not found - NOPE.TXT\n") == 0, "'%s'",
          con);
    CHECK(run("TYPE DOCS") == 1 && strcmp(con, "Access denied - DOCS\n") == 0, "'%s'", con);
    CHECK(run("TYPE A.TXT > NUL") == 0 && strcmp(con, "") == 0 && !exists("/HOME/NUL"), "> NUL");
    CHECK(run("EXIT > NUL") != 0 && !has("redirection") && has("no UF2 Loader") && !exists("/HOME/NUL"),
          "EXIT > NUL is EXIT, not refused: '%s'", con);
    CHECK(run("REBOOT > NUL") != 0 && !has("redirection") && !exists("/HOME/NUL"),
          "REBOOT > NUL is REBOOT: '%s'", con);
    CHECK(run("EXIT > OUT.TXT") == 1 && has("not with a redirection") && !exists("/HOME/OUT.TXT"),
          "EXIT > a file is still refused: '%s'", con);
    CHECK(run("ECHO") == 0 && strcmp(con, "ECHO is on\n") == 0, "'%s'", con);

    /* COPY. */
    CHECK(run("COPY A.TXT B.TXT") == 0 && strcmp(con, "        1 file(s) copied\n") == 0, "'%s'",
          con);
    CHECK(strcmp(slurp("/HOME/B.TXT"), "hello  world\n\nsecond line\n") == 0, "copied");
    CHECK(run("COPY *.TXT *.BAK") == 0 && has("A.TXT\nB.TXT\n        2 file(s) copied"), "'%s'",
          con);
    CHECK(exists("/HOME/A.BAK") && exists("/HOME/B.BAK"), "masked copies");
    CHECK(run("COPY *.BAK DOCS") == 0 && exists("/HOME/DOCS/A.BAK") && exists("/HOME/DOCS/B.BAK"),
          "into a directory: '%s'", con);
    CHECK(run("COPY A.TXT") == 1 && has("File cannot be copied onto itself") &&
              has("0 file(s) copied"),
          "onto itself: '%s'", con);
    CHECK(run("COPY *.TXT ALL.TXT") == 1 && has("Cannot copy several") && !exists("/HOME/ALL.TXT"),
          "several into one: '%s'", con);
    CHECK(run("COPY NOPE.* DOCS") == 1 && has("File not found - NOPE.*"), "'%s'", con);
    CHECK(run("COPY DOCS C:\\") == 0 && exists("/A.BAK") && exists("/B.BAK"), "a dir's files");

    /* DIR. */
    CHECK(run("DIR") == 0, "dir: '%s'", con);
    CHECK(has(" Volume in drive C is PICOCALC\n Volume Serial Number is ") &&
              has(" Directory of C:\\HOME\n\n"),
          "header: '%s'", con);
    CHECK(has(dir_line("A        TXT", "26")) && has(dir_line("DOCS", " <DIR>     ")) &&
              has("\n    4 file(s)            104 bytes\n") && has(" bytes free\n") &&
              column_of("file(s)", " bytes") == column_of("bytes free", " bytes free") &&
              column_of("file(s)", " bytes") == 28,
          "lines: '%s'", con);
    CHECK(run("dir *.bak /b") == 0 && strcmp(con, "A.BAK\nB.BAK\n") == 0, "/B: '%s'", con);
    CHECK(run("DIR A /B") == 0 && strcmp(con, "A.TXT\nA.BAK\n") == 0, "NAME is NAME.*: '%s'", con);
    /* In the card's order, as DOS listed them. */
    CHECK(run("DIR/W") == 0 && has("[DOCS]       A.TXT        B.TXT\nA.BAK        B.BAK"),
          "/W: '%s'", con);
    CHECK(run("DIR NOPE") == 1 && has("File not found\n"), "'%s'", con);
    CHECK(run("DIR /X") == 1 && strcmp(con, "Invalid switch - /X\n") == 0, "'%s'", con);
    /* A '/' inside a word starts its switches, as in DOS. */
    CHECK(run("dir *.bak/b") == 0 && strcmp(con, "A.BAK\nB.BAK\n") == 0, "NAME/B: '%s'", con);
    CHECK(run("DIR DOCS/X") == 1 && strcmp(con, "Invalid switch - /X\n") == 0, "'%s'", con);
    /* But not FIND's string. */
    CHECK(run("ECHO a/b c | FIND \"a/b\"") == 0 && strcmp(con, "a/b c\n") == 0, "FIND a/b: '%s'",
          con);
    CHECK(put("/HOME/.hidden", "h") && put("/HOME/._A.TXT", "x"), "hidden");
    CHECK(run("DIR /B") == 0 && !has(".hidden") && run("DIR /B /A") == 0 && has(".hidden"),
          "/A: '%s'", con);
    CHECK(run("DIR \\ /S /B") == 0 && has("C:\\HOME\\DOCS\\A.BAK\n") && has("C:\\A.BAK\n") &&
              !has("C:\\\\"),
          "/S /B: '%s'", con);
    char whole[sizeof con];
    strcpy(whole, con);
    CHECK(run("DIR \\/S/B") == 0 && strcmp(con, whole) == 0, "\\/S/B: '%s'", con);
    CHECK(run("DIR /S") == 0 && has(" Directory of C:\\HOME\\DOCS\n") &&
              has("Total files listed:\n    6 file(s) "),
          "/S: '%s'", con);
    CHECK(run("DIR | FIND /C \"TXT\"") == 0 && strcmp(con, "2\n") == 0, "a pipe: '%s'", con);

    /* Long names: the size at the right when the name leaves room for
     * it, on the next line when it does not; no line reaches the last
     * column. */
    static char huge[1234567];
    memset(huge, 'x', sizeof huge - 1);
    CHECK(put("/HOME/BIG.DAT", huge) && f_mkdir("/HOME/a directory with a long name") == FR_OK, "big");
    CHECK(run("DIR") == 0 && has(dir_line("BIG      DAT", "1,234,566")) &&
              has(dir_next("a directory with a long name", " <DIR>     ")) && has("1,234,670 bytes\n") && strstr(con, "file(s)      1,234,670 bytes\n") &&
              has(",") && !has(" ,"),
          "separators, a long directory: '%s'", con);
    CHECK(run("DEL BIG.DAT") == 0 && run("RD \"a directory with a long name\"") == 0, "gone");
    CHECK(f_mkdir("/HOME/a directory with a name much, much too long") == FR_OK, "longer");
    CHECK(run("DIR") == 0 && has(dir_next("a directory with a name much, much too long", " <DIR>     ")),
          "<DIR> on the next line: '%s'", con);
    CHECK(run("RD \"a directory with a name much, much too long\"") == 0, "gone");
    CHECK(put("/HOME/asteroids-deluxe.lua", "123456") &&
              put("/HOME/4D1F8172 - MacPlus v3 with a long name.ROM", "1234567"),
          "long names");
    CHECK(run("DIR") == 0 && has(dir_next("asteroids-deluxe.lua", "6")) &&
              has(dir_next("4D1F8172 - MacPlus v3 with a long name.ROM", "7")),
          "long names: '%s'", con);
    CHECK(run("DEL asteroids-deluxe.lua") == 0 && run("DEL \"4D1F8172 - MacPlus v3 with a long name.ROM\"") == 0,
          "long names gone");

    /* /P pauses a full screen; Esc stops it. */
    CHECK(f_mkdir("/MANY") == FR_OK, "many");
    for (int i = 0; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof name, "/MANY/F%02d.TXT", i);
        CHECK(put(name, "x"), "%s", name);
    }
    keys = " ";
    char f26[128];
    snprintf(f26, sizeof f26, "F26      TXT%11s " FIXED_DATE "\nPress any key to continue . . .\nF27", "1");
    CHECK(run("DIR \\MANY /P") == 0 && has(f26) &&
              has("bytes free"),
          "pauses: '%s'", con);
    keys = "\x1b";
    CHECK(run("DIR \\MANY /P") == 130 && has("Press any key") && !has("F27") && !has("bytes free"),
          "Esc stops: '%s'", con);
    CHECK(run("DIR \\MANY | FIND /C \"TXT\"") == 0 && strcmp(con, "40\n") == 0,
          "no pause into a pipe: '%s'", con);
    keys = "";

    /* REN. */
    CHECK(run("REN *.BAK *.OLD") == 0 && exists("/HOME/A.OLD") && !exists("/HOME/A.BAK"),
          "ren mask: '%s'", con);
    CHECK(run("REN A.OLD B.OLD") == 1 && strcmp(con, "Duplicate file name or file not found\n") == 0,
          "'%s'", con);
    CHECK(run("REN a.old a.old") == 0 && run("REN a.old a.Old") == 0, "case: '%s'", con);
    CHECK(run("REN A.OLD DOCS\\X") == 1 && strcmp(con, "Invalid parameter\n") == 0, "'%s'", con);
    CHECK(run("REN DOCS PAPERS") == 0 && exists("/HOME/PAPERS"), "a directory");

    /* ATTRIB, and DEL against a read-only file. */
    CHECK(run("ATTRIB +R A.TXT") == 0 && (attrs("/HOME/A.TXT") & AM_RDO), "+R");
    CHECK(run("ATTRIB A.TXT") == 0 && strcmp(con, "A    R     C:\\HOME\\A.TXT\n") == 0, "'%s'",
          con);
    CHECK(run("ATTRIB") == 0 && has("C:\\HOME\\.hidden"), "all, hidden too: '%s'", con);
    CHECK(run("DEL A.TXT") == 1 && strcmp(con, "Access denied - C:\\HOME\\A.TXT\n") == 0 &&
              exists("/HOME/A.TXT"),
          "'%s'", con);
    CHECK(run("ATTRIB -R +H A.TXT") == 0 && attrs("/HOME/A.TXT") == (AM_ARC | AM_HID), "-R +H");
    CHECK(run("ATTRIB +X A.TXT") == 1 && has("Parameter format not correct - +X"), "'%s'", con);
    CHECK(run("ATTRIB -H A.TXT") == 0, "-H");

    /* DEL: *.* asks first. */
    CHECK(run("COPY *.OLD PAPERS") == 0, "more to delete");
    keys = "xn";
    CHECK(run("DEL PAPERS") == 0 && has("All files in directory will be deleted!\nAre you sure (Y/N)?") &&
              exists("/HOME/PAPERS/A.OLD"),
          "no: '%s'", con);
    keys = "y";
    CHECK(run("DEL PAPERS\\*.*") == 0 && !exists("/HOME/PAPERS/A.OLD") &&
              !exists("/HOME/PAPERS/A.BAK"),
          "yes: '%s'", con);
    CHECK(run("COPY *.OLD PAPERS") == 0, "again");
    /* One kept, one deleted, in the card's order. */
    keys = "ny";
    CHECK(run("DEL /P PAPERS\\*.OLD") == 0 && has("Delete (Y/N)?N\n") && has("Delete (Y/N)?Y\n") &&
              exists("/HOME/PAPERS/A.OLD") != exists("/HOME/PAPERS/B.OLD"),
          "/P: '%s'", con);
    keys = "";
    CHECK(run("DEL NOPE.*") == 1 && strcmp(con, "File not found\n") == 0, "'%s'", con);
    CHECK(run("DEL ._A.TXT") == 0 && !exists("/HOME/._A.TXT"), "a hidden file by name");

    /* RD. */
    CHECK(run("RD PAPERS") == 1 && has("Invalid path, not directory,\nor directory not empty"),
          "'%s'", con);
    CHECK(run("MD EMPTY") == 0 && run("RD EMPTY") == 0 && !exists("/HOME/EMPTY"), "rd");
    CHECK(run("RD \\HOME") == 1 && strcmp(con, "Attempt to remove current directory - C:\\HOME\n") == 0,
          "'%s'", con);

    /* XCOPY /S, its hidden files left out, and a cyclic copy refused. */
    CHECK(f_mkdir("/HOME/PAPERS/DEEP") == FR_OK && put("/HOME/PAPERS/DEEP/D.TXT", "deep") &&
              put("/HOME/PAPERS/.secret", "s"),
          "a tree");
    CHECK(run("XCOPY PAPERS \\BACKUP /S") == 0 && has("Reading source file(s)...") &&
              has("DEEP\\D.TXT\n") && has("2 File(s) copied"),
          "xcopy: '%s'", con);
    CHECK(strcmp(slurp("/BACKUP/DEEP/D.TXT"), "deep") == 0 &&
              (exists("/BACKUP/A.OLD") || exists("/BACKUP/B.OLD")) && !exists("/BACKUP/.secret"),
          "the copy");
    CHECK(run("XCOPY \\HOME \\HOME\\X /S") == 4 && has("Cannot perform a cyclic copy"), "'%s'",
          con);
    CHECK(run("XCOPY \\BACKUP\\*.TXT \\FLAT") == 1 && !exists("/FLAT") &&
              has("File not found - \\BACKUP\\*.TXT") && has("0 File(s) copied"),
          "no TXT at the top: '%s'", con);
    CHECK(run("XCOPY \\BACKUP\\DEEP\\D.TXT \\D2.TXT") == 0 && exists("/D2.TXT"), "one file: '%s'",
          con);

    /* MOVE. */
    CHECK(run("MOVE B.TXT PAPERS") == 0 && has("C:\\HOME\\B.TXT => C:\\HOME\\PAPERS\\B.TXT [ok]") &&
              exists("/HOME/PAPERS/B.TXT") && !exists("/HOME/B.TXT"),
          "move: '%s'", con);
    CHECK(run("MOVE \\BACKUP \\SAVED") == 0 && exists("/SAVED/DEEP/D.TXT"), "rename a dir: '%s'",
          con);
    CHECK(run("MOVE *.OLD A.TXT") == 1 && has("Cannot move multiple files to a single file"),
          "'%s'", con);
    CHECK(run("MOVE \\SAVED \\SAVED\\DEEP") == 1 && has("[Cannot move a directory into itself]"),
          "'%s'", con);
    CHECK(put("/HOME/C.TXT", "new") && put("/HOME/PAPERS/C.TXT", "old") &&
              run("MOVE C.TXT,A.OLD PAPERS") == 0 &&
              strcmp(slurp("/HOME/PAPERS/C.TXT"), "new") == 0 && exists("/HOME/PAPERS/A.OLD"),
          "a list, a file replaced: '%s'", con);

    /* DELTREE: hidden and read-only files go too; the current directory
     * cannot. */
    CHECK(run("ATTRIB +R \\SAVED\\DEEP\\D.TXT") == 0, "read-only inside");
    keys = "n";
    CHECK(run("DELTREE \\SAVED") == 0 &&
              has("Delete directory \"SAVED\" and all its subdirectories? [yn] N") &&
              exists("/SAVED"),
          "no: '%s'", con);
    keys = "y";
    CHECK(run("DELTREE \\SAVED") == 0 && has("Deleting SAVED...") && !exists("/SAVED"),
          "yes: '%s'", con);
    keys = "";
    CHECK(run("DELTREE /Y \\D2.TXT") == 0 && !exists("/D2.TXT"), "a file");
    CHECK(run("DELTREE /Y \\HOME") == 1 && has("Attempt to remove current directory"), "'%s'",
          con);

    /* FIND. */
    CHECK(put("/HOME/F.TXT", "Apple\nbanana\nAPPLE pie\ncherry\n"), "f.txt");
    CHECK(run("FIND \"pple\" F.TXT") == 0 && strcmp(con, "\n---------- F.TXT\nApple\n") == 0,
          "'%s'", con);
    CHECK(run("FIND /I /N \"apple\" f.txt") == 0 &&
              strcmp(con, "\n---------- F.TXT\n[1]Apple\n[3]APPLE pie\n") == 0,
          "'%s'", con);
    /* Case counts without /I: only banana has an a. */
    CHECK(run("FIND /V /C \"a\" F.TXT") == 0 && strcmp(con, "---------- F.TXT: 3\n") == 0,
          "'%s'", con);
    CHECK(run("TYPE F.TXT | FIND \"an\"") == 0 && strcmp(con, "banana\n") == 0, "'%s'", con);
    CHECK(run("FIND \"kiwi\" < F.TXT") == 1 && strcmp(con, "") == 0, "none");
    CHECK(run("FIND \"x\" NOPE.TXT") == 2 && has("File not found - NOPE.TXT"), "'%s'", con);
    CHECK(run("FIND") == 2 && has("FIND: Required parameter missing"), "'%s'", con);

    /* MORE copies when its output is not the screen. */
    CHECK(run("MORE F.TXT > G.TXT") == 0 &&
              strcmp(slurp("/HOME/G.TXT"), "Apple\nbanana\nAPPLE pie\ncherry\n") == 0,
          "more");
    CHECK(run("MORE") == 1 && has("Required parameter missing"), "'%s'", con);

    /* SET, %NAME%, PATH and PROMPT. */
    CHECK(run("SET greeting=hi there") == 0 && run("ECHO [%GREETING%] [%greeting%] 100%") == 0 &&
              strcmp(con, "[hi there] [hi there] 100%\n") == 0,
          "'%s'", con);
    CHECK(run("SET") == 0 && has("PATH=C:\\PICO2-APPS\nPROMPT=$P$G\n") &&
              has("GREETING=hi there\n"),
          "'%s'", con);
    CHECK(run("SET GREETING=") == 0 && !vars_get("GREETING", 8), "cleared");
    CHECK(run("SET NOEQUALS") == 1 && strcmp(con, "Syntax error\n") == 0, "'%s'", con);
    CHECK(run("PATH") == 0 && strcmp(con, "PATH=C:\\PICO2-APPS\n") == 0, "'%s'", con);
    CHECK(run("PATH ;") == 0 && run("PATH") == 0 && strcmp(con, "No Path\n") == 0, "'%s'", con);
    CHECK(run("path c:\\pico2-apps;\\HOME") == 0 &&
              strcmp(vars_get("PATH", 4), "C:\\PICO2-APPS;\\HOME") == 0,
          "%s", vars_get("PATH", 4));
    CHECK(run("PROMPT [$p]$_$n$g") == 0, "prompt");
    clear();
    sh.at_bol = true;
    shell_prompt(&sh);
    CHECK(strcmp(con, "[C:\\HOME]\nC>") == 0 && sh.line.prompt == 2, "'%s' %zu", con,
          sh.line.prompt);
    CHECK(run("PROMPT") == 0, "default");
    clear();
    sh.at_bol = true;
    shell_prompt(&sh);
    CHECK(strcmp(con, "C:\\HOME>") == 0 && sh.line.prompt == 8, "'%s'", con);
    CHECK(run("ECHO OFF") == 0, "echo off");
    clear();
    sh.at_bol = true;
    shell_prompt(&sh);
    CHECK(strcmp(con, "") == 0 && sh.line.prompt == 0, "no prompt: '%s'", con);
    CHECK(run("ECHO") == 0 && strcmp(con, "ECHO is off\n") == 0 && run("ECHO ON") == 0, "'%s'",
          con);

    /* Programs: in the current directory first, then PATH. */
    CHECK(put("/HOME/HERE.UF2", "not a uf2") && put("/pico2-apps/THERE.UF2", "nor this"),
          "programs");
    CHECK(run("here") == 1 && has("cannot run programs"), "found here: '%s'", con);
    CHECK(run("THERE") == 1 && has("cannot run programs"), "found on PATH: '%s'", con);
    CHECK(run("\\pico2-apps\\there.uf2") == 1 && has("cannot run programs"), "by path: '%s'",
          con);
    CHECK(run("HERE.UF2") == 1 && has("cannot run programs"), "with .UF2: '%s'", con);
    CHECK(run("THERE.uf2") == 1 && has("cannot run programs"), "on PATH, .uf2: '%s'", con);
    CHECK(run("\\HOME\\HERE") == 1 && has("cannot run programs"), "path, no .UF2: '%s'", con);
    CHECK(run("..\\HOME\\here") == 1 && has("cannot run programs"), "relative: '%s'", con);
    CHECK(run("\\HOME\\NOPE") != 0 && !has("cannot run programs"), "no such path: '%s'", con);

    /* DOS's order: one directory at a time, a program before a batch file. */
    CHECK(put("/HOME/BOTH.UF2", "x") && put("/HOME/BOTH.BAT", "@ECHO the batch\r\n"), "BOTH");
    CHECK(run("BOTH") == 1 && has("cannot run programs"), "program first: '%s'", con);
    CHECK(run("\\HOME\\BOTH") == 1 && has("cannot run programs"), "by path too: '%s'", con);
    CHECK(run("CALL BOTH") == 1 && has("cannot run programs"), "CALL too: '%s'", con);
    CHECK(run("BOTH.BAT") == 0 && strcmp(con, "the batch\n") == 0, "typed: '%s'", con);
    CHECK(run("\\HOME\\BOTH.BAT") == 0 && strcmp(con, "the batch\n") == 0, "typed: '%s'", con);
    CHECK(put("/HOME/NEAR.BAT", "@ECHO near\r\n") && put("/pico2-apps/NEAR.UF2", "x"), "NEAR");
    CHECK(run("NEAR") == 0 && strcmp(con, "near\n") == 0, "batch here, program on PATH: '%s'", con);
    CHECK(put("/HOME/FAR.UF2", "x") && put("/pico2-apps/FAR.BAT", "@ECHO far\r\n"), "FAR");
    CHECK(run("FAR") == 1 && has("cannot run programs"), "program here, batch on PATH: '%s'", con);

    /* COPY's devices: NUL and CON, either way, in any case, with ':'. */
    CHECK(put("/HOME/DEV.TXT", "dev text\r\n"), "DEV");
    CHECK(run("COPY DEV.TXT NUL") == 0 && strcmp(con, "        1 file(s) copied\n") == 0, "'%s'", con);
    CHECK(!exists("/HOME/NUL"), "no file NUL");
    CHECK(run("COPY DEV.TXT nul:") == 0 && has("1 file(s) copied"), "'%s'", con);
    CHECK(run("COPY MISSING.TXT NUL") == 1 && has("File not found"), "'%s'", con);
    CHECK(run("COPY DEV.TXT CON") == 0 && strcmp(con, "dev text\n        1 file(s) copied\n") == 0,
          "'%s'", con);
    CHECK(!exists("/HOME/CON"), "no file CON");
    CHECK(put("/HOME/NOEOL.TXT", "no end"), "NOEOL");
    CHECK(run("COPY NOEOL.TXT CON:") == 0 && strcmp(con, "no end\n        1 file(s) copied\n") == 0,
          "'%s'", con);
    CHECK(run("COPY D*.TXT CON") == 0 && has("DEV.TXT\ndev text\n") , "wildcard: '%s'", con);
    CHECK(run("COPY NUL EMPTY.TXT") == 0 && has("1 file(s) copied") &&
              strcmp(slurp("/HOME/EMPTY.TXT"), "") == 0,
          "NUL makes it empty: '%s'", con);
    CHECK(run("COPY CON") == 1 && has("onto itself"), "no destination: '%s'", con);

    keys = "one\rtwo\r\x1a";
    CHECK(run("COPY CON TYPED.TXT") == 0 && has("^Z\n        1 file(s) copied\n"), "'%s'", con);
    CHECK(strcmp(slurp("/HOME/TYPED.TXT"), "one\r\ntwo\r\n") == 0, "'%s'", slurp("/HOME/TYPED.TXT"));
    keys = "last\x1a";
    CHECK(run("copy con: TYPED.TXT") == 0 && strcmp(slurp("/HOME/TYPED.TXT"), "last") == 0,
          "Ctrl+Z after text: '%s'", slurp("/HOME/TYPED.TXT"));
    static const char f6[] = { 'x', '\r', (char)KEY_F6, 0 };
    keys = f6;
    CHECK(run("COPY CON TYPED.TXT") == 0 && strcmp(slurp("/HOME/TYPED.TXT"), "x\r\n") == 0, "F6");
    keys = "gone\rgone too\x03";
    CHECK(run("COPY CON TYPED.TXT") != 0 && has("gone too") && has("^C") && !has("file(s) copied"),
          "'%s'", con);
    CHECK(strcmp(slurp("/HOME/TYPED.TXT"), "x\r\n") == 0 && !exists("/HOME/TYPED.TXT.new"),
          "stopped: the old file kept");
    keys = "wrong\x1bright\r\x1a";
    CHECK(run("COPY CON TYPED.TXT") == 0 && strcmp(slurp("/HOME/TYPED.TXT"), "right\r\n") == 0,
          "Esc drops the line: '%s'", slurp("/HOME/TYPED.TXT"));
    keys = "seen\r\x1a";
    CHECK(run("COPY CON CON") == 0 && has("seen\nseen\n"), "CON to CON: '%s'", con);
    keys = "unseen\r\x1a";
    CHECK(run("COPY CON NUL") == 0 && has("1 file(s) copied") && !exists("/HOME/NUL"), "'%s'", con);
    keys = "x\x1a";
    CHECK(run("COPY CON \\HOME") == 1, "a directory: '%s'", con);
    keys = "";

    /* Tab finds a program in the current directory. */
    clear();
    line_start(&sh, 0);
    for (const char *s = "FA"; *s; s++) line_key(&sh, *s);
    line_key(&sh, KEY_TAB);
    CHECK(strcmp(sh.line.text, "FAR ") == 0, "Tab, here: '%s'", sh.line.text);
    line_start(&sh, 0);

    /* VER, VOL, MEM, HELP. */
    CHECK(run("VER") == 0 && strcmp(con, "\nPicoCalc shell, DOS flavour\nVersion test\n\n") == 0, "'%s'",
          con);
    CHECK(run("VOL") == 0 && has(" Volume in drive C is PICOCALC\n Volume Serial Number is "),
          "'%s'", con);
    CHECK(run("VOL D:") == 1 && has("Invalid drive specification"), "'%s'", con);
    CHECK(run("MEM") == 1 && has("MEM: not on this machine"), "'%s'", con);
    shell_set_memory(&sh, 400000, 532480, 0);
    CHECK(run("MEM") == 0 && has("    532480 bytes total memory\n") &&
              has("    132480 bytes free\n         0 bytes PSRAM\n"),
          "'%s'", con);
    shell_set_memory(&sh, 400000, 532480, 8388608);
    CHECK(run("MEM") == 0 && has("    132480 bytes free\n   8388608 bytes PSRAM\n"), "'%s'", con);
    CHECK(run("HELP") == 0 && has("ATTRIB") && has("XCOPY") && has("HELP name for one"), "'%s'",
          con);
    CHECK(run("help copy") == 0 && strcmp(con, "usage: COPY [/V] [/B] source [destination]\nCopies files; CON, NUL are devices\n") == 0,
          "'%s'", con);
    /* Each about line fits the 40-column screen. */
    for (unsigned i = 0; shell_builtin_at(i); i++)
        CHECK(strlen(shell_builtin_about(i)) < TERM_COLS, "%s: too wide", shell_builtin_at(i));
    CHECK(run("PHOSPHOR GREEN") == 1 && has("phosphor: not on this machine"), "lower: '%s'", con);

    /* The prompt's keys: F3 the rest of the last line, Esc drops the line,
     * Tab completes with '\'. */
    sh.status = 0;
    history_add("ECHO from history");
    clear();
    sh.at_bol = true;
    shell_prompt(&sh);
    shell_input(&sh, 'E');
    shell_input(&sh, 'C');
    shell_input(&sh, KEY_F3);
    CHECK(strcmp(sh.line.text, "ECHO from history") == 0, "F3: '%s'", sh.line.text);
    shell_input(&sh, 0x1B);
    CHECK(sh.line.len == 0, "Esc: '%s'", sh.line.text);
    for (const char *s = "CD PAP\t"; *s; s++) shell_input(&sh, (unsigned char)*s);
    CHECK(strcmp(sh.line.text, "CD PAPERS\\") == 0, "Tab: '%s'", sh.line.text);
    for (const char *s = "DE\t"; *s; s++) shell_input(&sh, (unsigned char)*s);
    CHECK(strcmp(sh.line.text, "CD PAPERS\\DEEP\\") == 0, "Tab again: '%s'", sh.line.text);
    shell_input(&sh, 0x1B);
    /* A '/' separates nothing, so nothing is completed after it. */
    for (const char *s = "CD PAPERS/DE\t"; *s; s++) shell_input(&sh, (unsigned char)*s);
    CHECK(strcmp(sh.line.text, "CD PAPERS/DE") == 0, "Tab after /: '%s'", sh.line.text);
    shell_input(&sh, 0x1B);
    for (const char *s = "xc\t"; *s; s++) shell_input(&sh, (unsigned char)*s);
    CHECK(strcmp(sh.line.text, "XCOPY ") == 0, "a command: '%s'", sh.line.text);
    shell_input(&sh, 0x1B);

    if (batch_tests()) return 1;

    /* A copy that fails part way leaves the old file. */
    CHECK(put("/HOME/OLD.TXT", "the old text"), "old");
    static char big[20000];
    memset(big, 'x', sizeof big - 1);
    CHECK(put("/HOME/BIG.TXT", big), "big");
    host_disk_fail_writes_after(3);
    CHECK(run("COPY BIG.TXT OLD.TXT") == 1 && has("0 file(s) copied"), "fails: '%s'", con);
    host_disk_fail_writes_after(-1);
    files_mount();   /* as after the card was taken out and put back */
    CHECK(slurp("/HOME/OLD.TXT") && strcmp(slurp("/HOME/OLD.TXT"), "the old text") == 0,
          "old file kept");

    /* Where the shell starts: the root, or AUTOEXEC.BAT's CD; after a
     * restart, the directory left, once, whatever AUTOEXEC.BAT did. */
    CHECK(f_mkdir("/HOME/LEFT") == FR_OK, "LEFT");
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/") == 0, "no restart, the root: %s", sh.cwd);
    CHECK(run("CD \\home\\left") == 0, "cd");
    shell_keep_cwd(&sh);
    CHECK(exists("/.shell/cwd"), "kept");
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/HOME/LEFT") == 0, "back: %s", sh.cwd);
    CHECK(!exists("/.shell/cwd"), "taken");
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/") == 0, "only once: %s", sh.cwd);
    CHECK(put("/AUTOEXEC.BAT", "@CD \\HOME\r\n"), "AUTOEXEC");
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/HOME") == 0, "AUTOEXEC's CD: %s", sh.cwd);
    CHECK(run("CD LEFT") == 0, "cd");
    shell_keep_cwd(&sh);
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/HOME/LEFT") == 0, "back, past AUTOEXEC's CD: %s", sh.cwd);
    CHECK(f_unlink("/AUTOEXEC.BAT") == FR_OK, "no AUTOEXEC");
    shell_keep_cwd(&sh);
    CHECK(run("CD \\") == 0 && run("RD \\HOME\\LEFT") == 0, "the directory goes");
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/") == 0, "gone, the root: %s", sh.cwd);

    files_unmount();
    host_disk_close();
    return 0;
}

int main(void) {
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, getkey, pollkey, NULL, NULL };
    shell_init(&sh, &out, &hooks, "test");
    if (unit_tests()) return 1;
    if (dos_tests(false)) return 1;
    shell_init(&sh, &out, &hooks, "test");
    if (dos_tests(true)) return 1;
    TEST_DONE();
}
