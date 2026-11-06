/* test_args.c — program arguments (design.md §3.5, M13), built once for
 * each flavour and run over FAT16 and FAT32: the record that says a program
 * takes them, found across a UF2 and absent from an older one; each refusal;
 * /.shell/args written and removed around a fake loader, with a failed write
 * stopping the start; and include/picocalc_shell.h's parser, options and path
 * rules on what the shell wrote. */

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "flavour.h"
#include "history.h"
#include "host_disk.h"
#include "path.h"
#include "picocalc_shell.h"
#include "run.h"
#include "shell.h"
#include "test_util.h"
#include "uf2.h"

#ifndef SHELL_SUITES_DIR
#define SHELL_SUITES_DIR "test/suites"
#endif

/* The shell's idea and the header's, which cannot include each other's. */
_Static_assert(PICOCALC_SHELL_ARGS_MAGIC0 == UF2_ARGS_MAGIC0 &&
                   PICOCALC_SHELL_ARGS_MAGIC1 == UF2_ARGS_MAGIC1 &&
                   PICOCALC_SHELL_ARGS_RECORD == UF2_ARGS_RECORD &&
                   PICOCALC_SHELL_ARGS_NAME == UF2_ARGS_NAME,
               "the record");
/* The header's buffer holds the most the shell can write: its version and
 * flavour lines, two paths, a name, and the words, whose NULs become LFs. */
_Static_assert(16u + 5u + 2u * (SHELL_PATH_MAX + 1u) + UF2_ARGS_NAME + SHELL_ARGS_TEXT <=
                   PICOCALC_SHELL_ARGS_SIZE,
               "the file's size");

/* ---- a fake app partition ---------------------------------------------- */

#define FLASH_BASE 0x10000000u
#define FLASH_SIZE (4u << 20)
#define FLASH_END  (FLASH_BASE + FLASH_SIZE - 8192u)

static uint8_t flash_mem[FLASH_SIZE];
static long    flash_ops;

static void fl_read(void *ctx, uint32_t addr, void *buf, uint32_t n) {
    (void)ctx;
    memcpy(buf, flash_mem + (addr - FLASH_BASE), n);
}

static bool fl_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    flash_ops++;
    memset(flash_mem + (addr - FLASH_BASE), 0xFF, UF2_SECTOR);
    return true;
}

static bool fl_program(void *ctx, uint32_t addr, const void *page) {
    (void)ctx;
    flash_ops++;
    const uint8_t *p = page;
    for (uint32_t i = 0; i < UF2_PAGE; i++) flash_mem[addr - FLASH_BASE + i] &= p[i];
    return true;
}

/* ---- making UF2s ------------------------------------------------------- */

#define FAM_ARM_S  0xe48bff59u
#define FAM_RP2040 0xe48bff56u

static uint8_t ubuf[1u << 20];
static size_t  ulen;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void block(uint32_t addr, uint32_t no, uint32_t num, uint32_t family) {
    uint8_t *b = ubuf + ulen;
    memset(b, 0, UF2_BLOCK);
    put32(b, 0x0A324655u);
    put32(b + 4, 0x9E5D5157u);
    put32(b + 8, 0x2000u);
    put32(b + 12, addr);
    put32(b + 16, UF2_PAGE);
    put32(b + 20, no);
    put32(b + 24, num);
    put32(b + 28, family);
    for (uint32_t i = 0; i < UF2_PAGE; i++) b[32 + i] = (uint8_t)(addr / UF2_PAGE + i);
    put32(b + 508, 0x0AB16F30u);
    ulen += UF2_BLOCK;
}

static void make(uint32_t base, uint32_t pages) {
    ulen = 0;
    for (uint32_t p = 0; p < pages; p++) block(base + p * UF2_PAGE, p, pages, FAM_ARM_S);
}

/* Put the record for `name` in page `page` of what make made, at `off`. */
static void declare(uint32_t page, uint32_t off, const char *name) {
    uint8_t *r = ubuf + page * UF2_BLOCK + 32 + off;
    put32(r, UF2_ARGS_MAGIC0);
    put32(r + 4, UF2_ARGS_MAGIC1);
    memset(r + 8, 0, UF2_ARGS_NAME);
    memcpy(r + 8, name, strlen(name));
}

static bool put(const char *path, const void *data, size_t n) {
    FIL f;
    UINT w;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT r = f_write(&f, data, (UINT)n, &w);
    return f_close(&f) == FR_OK && r == FR_OK && w == n;
}

static bool puts_file(const char *path, const char *text) {
    return put(path, text, strlen(text));
}

static bool exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

static char   slurped[PICOCALC_SHELL_ARGS_SIZE + 1];
static size_t slurped_len;

/* The file's text, or "" with none. */
static const char *slurp(const char *path) {
    FIL f;
    UINT n = 0;
    slurped[0] = '\0';
    slurped_len = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) return slurped;
    f_read(&f, slurped, sizeof slurped - 1, &n);
    f_close(&f);
    slurped[n] = '\0';
    slurped_len = n;
    return slurped;
}

/* ---- the record ---------------------------------------------------------- */

static const uf2_flash_t flash = { FLASH_BASE, FLASH_END, fl_read, fl_erase, fl_program, NULL };
static FIL f;

/* What uf2_check finds in what make made: the name declared, or "". */
static const char *found(uf2_result_t want) {
    static uf2_info_t info;
    memset(&info, 0, sizeof info);
    if (!put("/r.uf2", ubuf, ulen) || f_open(&f, "/r.uf2", FA_READ) != FR_OK) return "(no file)";
    uf2_result_t r = uf2_check(&f, &flash, &info, NULL, NULL);
    f_close(&f);
    return r == want ? info.args_name : "(wrong result)";
}

static int test_record(void) {
    make(FLASH_BASE, 8);
    CHECK(strcmp(found(UF2_FLASH), "") == 0, "none: '%s'", found(UF2_FLASH));
    /* Wherever it was linked, on its alignment, in any block. */
    for (uint32_t page = 0; page < 8; page++) {
        for (uint32_t off = 0; off < UF2_PAGE; off += UF2_ARGS_RECORD) {
            make(FLASH_BASE, 8);
            declare(page, off, "pico-oric");
            CHECK(strcmp(found(UF2_FLASH), "pico-oric") == 0, "page %u off %u", page, off);
        }
    }
    /* A RAM program too. */
    make(UF2_SRAM_BASE, 8);
    declare(7, 192, "ram-prog");
    CHECK(strcmp(found(UF2_RAM), "ram-prog") == 0, "RAM: '%s'", found(UF2_RAM));
    /* The longest name. */
    make(FLASH_BASE, 8);
    char longest[UF2_ARGS_NAME];
    memset(longest, 'n', sizeof longest - 1);
    longest[sizeof longest - 1] = '\0';
    declare(3, 64, longest);
    CHECK(strcmp(found(UF2_FLASH), longest) == 0, "longest");
    /* Off its alignment it is not the record, and not looked for. */
    make(FLASH_BASE, 8);
    declare(3, 32, "pico-oric");
    CHECK(strcmp(found(UF2_FLASH), "") == 0, "unaligned");
    /* Half the magic. */
    make(FLASH_BASE, 8);
    declare(3, 64, "pico-oric");
    ubuf[3 * UF2_BLOCK + 32 + 64 + 4] ^= 1;
    CHECK(strcmp(found(UF2_FLASH), "") == 0, "half the magic");
    /* A name that is empty, not ended or not text. */
    make(FLASH_BASE, 8);
    declare(3, 64, "");
    CHECK(strcmp(found(UF2_FLASH), "") == 0, "empty");
    make(FLASH_BASE, 8);
    declare(3, 64, "");
    memset(ubuf + 3 * UF2_BLOCK + 32 + 64 + 8, 'n', UF2_ARGS_NAME);
    CHECK(strcmp(found(UF2_FLASH), "") == 0, "not ended");
    make(FLASH_BASE, 8);
    declare(3, 64, "two\nlines");
    CHECK(strcmp(found(UF2_FLASH), "") == 0, "a line break");
    /* The first of two. */
    make(FLASH_BASE, 8);
    declare(2, 0, "first");
    declare(5, 0, "second");
    CHECK(strcmp(found(UF2_FLASH), "first") == 0, "two");
    /* In a block for another chip, which is skipped, it does not count. */
    ulen = 0;
    block(FLASH_BASE, 0, 1, FAM_RP2040);
    declare(0, 0, "other-chip");
    block(FLASH_BASE, 0, 1, FAM_ARM_S);
    CHECK(strcmp(found(UF2_FLASH), "") == 0, "another chip's block");
    return 0;
}

/* A real UF2, if tools/fetch-suites.sh fetched it, as a file on the card:
 * false to skip. */
static bool fetch(const char *file, const char *path) {
    static uint8_t real[3u << 20];
    char host[256];
    snprintf(host, sizeof host, "%s/uf2/%s", SHELL_SUITES_DIR, file);
    FILE *in = fopen(host, "rb");
    if (!in) {
        printf("(%s not fetched: skipped)\n", file);
        return false;
    }
    size_t n = fread(real, 1, sizeof real, in);
    fclose(in);
    return put(path, real, n);
}

static int test_real(void) {
    /* Built by this tree with the macro, through the SDK's --gc-sections. */
    static const struct { const char *file; uf2_result_t want; } declared[] = {
        { "args-test.uf2", UF2_FLASH },
        { "args-ram.uf2", UF2_RAM },
    };
    /* Older programs. Should one of the emulators come to declare, name
     * another. */
    static const struct { const char *file; uf2_result_t want; } older[] = {
        { "pico-ace.uf2", UF2_FLASH },
        { "loader-menu.uf2", UF2_RAM },
    };
    static uf2_info_t info;
    for (size_t i = 0; i < sizeof declared / sizeof declared[0]; i++) {
        if (!fetch(declared[i].file, "/real.uf2")) continue;
        CHECK(f_open(&f, "/real.uf2", FA_READ) == FR_OK, "open");
        uf2_result_t r = uf2_check(&f, &flash, &info, NULL, NULL);
        f_close(&f);
        CHECK(r == declared[i].want && strcmp(info.args_name, "args-test") == 0, "%s: %d '%s'",
              declared[i].file, r, info.args_name);
    }
    for (size_t i = 0; i < sizeof older / sizeof older[0]; i++) {
        if (!fetch(older[i].file, "/real.uf2")) continue;
        CHECK(f_open(&f, "/real.uf2", FA_READ) == FR_OK, "open");
        uf2_result_t r = uf2_check(&f, &flash, &info, NULL, NULL);
        f_close(&f);
        CHECK(r == older[i].want && info.args_name[0] == '\0', "%s: %d '%s'", older[i].file, r,
              info.args_name);
    }
    f_unlink("/real.uf2");
    return 0;
}

/* ---- the header, on text as the shell writes it ------------------------- */

static char   text[PICOCALC_SHELL_ARGS_SIZE + 1];
static picocalc_shell_args_t args;

static bool parse(const char *file, const char *name) {
    size_t n = strlen(file);
    memcpy(text, file, n + 1);
    return picocalc_shell_args_parse(text, n, name, &args);
}

static const char *path_of(const char *word) {
    static char out[256];
    return picocalc_shell_args_path(&args, word, out, sizeof out) ? out : "(refused)";
}

static const char *option(char letter) {
    static char out[256];
    return picocalc_shell_args_option(&args, letter, out, sizeof out) ? out : "(none)";
}

#define HEAD "picocalc-args 1\n"
#define UNIX_FILE HEAD "unix\n/home/docs\n/games/p.uf2\np\n"
#define DOS_FILE  HEAD "dos\n/HOME/DOCS\n/GAMES/P.UF2\np\n"

static int test_header(void) {
    CHECK(strcmp(PICOCALC_SHELL_ARGS_FILE, ARGS_FILE) == 0, "the file's name");
    /* What is refused. */
    CHECK(!parse("", "p"), "empty");
    CHECK(!parse(UNIX_FILE "cut", "p"), "no last LF");
    CHECK(!parse("picocalc-args 2\nunix\n/\n/p.uf2\np\n", "p"), "a later version");
    CHECK(!parse("picocalc-args 1\nvms\n/\n/p.uf2\np\n", "p"), "another flavour");
    CHECK(!parse("picocalc-args 1\nunix\n/\n/p.uf2\n", "p"), "too few lines");
    CHECK(!parse("picocalc-args 1\nunix\nhome\n/p.uf2\np\n", "p"), "a relative directory");
    CHECK(!parse("picocalc-args 1\nunix\n/\np.uf2\np\n", "p"), "a relative program");
    CHECK(!parse(UNIX_FILE, "other"), "meant for another program");
    CHECK(!parse(UNIX_FILE, "P"), "the name is exact");
    char nul[] = "picocalc-args 1\nunix\n/\n/p.uf2\np\n";
    nul[3] = '\0';
    CHECK(!picocalc_shell_args_parse(nul, sizeof nul - 1, "p", &args), "a NUL");

    /* No arguments still gives the directory. */
    CHECK(parse(UNIX_FILE, "p") && !args.dos && args.argc == 0, "none");
    CHECK(strcmp(args.dir, "/home/docs") == 0 && strcmp(args.program, "/games/p.uf2") == 0 &&
              strcmp(args.name, "p") == 0,
          "fields: %s %s %s", args.dir, args.program, args.name);
    CHECK(picocalc_shell_args_word(&args, 0) == NULL, "no word");
    CHECK(strcmp(option('t'), "(none)") == 0, "no option");

    /* UNIX: -t exactly, its file from the directory. */
    CHECK(parse(UNIX_FILE "-t\ngame.tap\n-d\n../d.dsk\nrest\n", "p") && args.argc == 5, "words");
    CHECK(strcmp(picocalc_shell_args_word(&args, 4), "rest") == 0, "word 4");
    CHECK(picocalc_shell_args_word(&args, 5) == NULL && picocalc_shell_args_word(&args, -1) == NULL,
          "past the ends");
    CHECK(strcmp(option('t'), "/home/docs/game.tap") == 0, "-t: %s", option('t'));
    CHECK(strcmp(option('d'), "/home/d.dsk") == 0, "-d: %s", option('d'));
    CHECK(strcmp(option('T'), "(none)") == 0, "-T is not -t");
    CHECK(strcmp(option('x'), "(none)") == 0, "-x");
    CHECK(parse(UNIX_FILE "/t\ngame.tap\n", "p") && strcmp(option('t'), "(none)") == 0, "/t");
    CHECK(parse(UNIX_FILE "-t\n", "p") && strcmp(option('t'), "(none)") == 0, "nothing after -t");
    CHECK(parse(UNIX_FILE "-tx\ny\n", "p") && strcmp(option('t'), "(none)") == 0, "-tx");
    CHECK(parse(UNIX_FILE "-t\n\n", "p") && args.argc == 2 && strcmp(option('t'), "(none)") == 0,
          "an empty word");
    /* The first, as -t *.tap gives each match. */
    CHECK(parse(UNIX_FILE "-t\na.tap\nb.tap\n", "p") && strcmp(option('t'), "/home/docs/a.tap") == 0,
          "first: %s", option('t'));

    /* UNIX's paths. */
    CHECK(parse(UNIX_FILE, "p"), "unix");
    static const char *const unix_paths[][2] = {
        { "a", "/home/docs/a" },       { "/a/b", "/a/b" },      { "./a", "/home/docs/a" },
        { "../a", "/home/a" },         { "../../../a", "/a" },  { "a//b/", "/home/docs/a/b" },
        { "/", "/" },                  { "..", "/home" },       { "../..", "/" },
        { "a\\b", "/home/docs/a\\b" }, { "C:a", "/home/docs/C:a" },
    };
    for (size_t i = 0; i < sizeof unix_paths / sizeof unix_paths[0]; i++)
        CHECK(strcmp(path_of(unix_paths[i][0]), unix_paths[i][1]) == 0, "%s: %s", unix_paths[i][0],
              path_of(unix_paths[i][0]));

    /* DOS: /T or /t, whichever the program asks for. */
    CHECK(parse(DOS_FILE "/T\nGAME.TAP\n/d\nD.DSK\n", "p") && args.dos, "dos");
    CHECK(strcmp(option('t'), "/HOME/DOCS/GAME.TAP") == 0, "/T: %s", option('t'));
    CHECK(strcmp(option('T'), "/HOME/DOCS/GAME.TAP") == 0, "/T, asked as T: %s", option('T'));
    CHECK(strcmp(option('d'), "/HOME/DOCS/D.DSK") == 0, "/d: %s", option('d'));
    CHECK(parse(DOS_FILE "-t\nGAME.TAP\n", "p") && strcmp(option('t'), "(none)") == 0, "-t in DOS");

    /* DOS's paths, and another drive refused. */
    CHECK(parse(DOS_FILE, "p"), "dos");
    static const char *const dos_paths[][2] = {
        { "A.TAP", "/HOME/DOCS/A.TAP" }, { "..\\A.TAP", "/HOME/A.TAP" }, { "\\A\\B", "/A/B" },
        { "C:\\A\\B", "/A/B" },          { "c:\\A", "/A" },              { "C:A", "/HOME/DOCS/A" },
        { "C:", "/HOME/DOCS" },          { "C:\\", "/" },                { "/A/B", "(refused)" },
        { "A/B", "(refused)" },          { "C:/A", "(refused)" },
        { "A\\.\\B\\..", "/HOME/DOCS/A" }, { "*.TAP", "/HOME/DOCS/*.TAP" },
        { "D:\\A", "(refused)" },        { "A:B", "(refused)" },         { "", "(refused)" },
    };
    for (size_t i = 0; i < sizeof dos_paths / sizeof dos_paths[0]; i++)
        CHECK(strcmp(path_of(dos_paths[i][0]), dos_paths[i][1]) == 0, "%s: %s", dos_paths[i][0],
              path_of(dos_paths[i][0]));
    CHECK(parse(DOS_FILE "/T\nD:\\A\n", "p") && strcmp(option('t'), "(none)") == 0, "/T D:");

    /* What does not fit is refused, and what just fits is whole. */
    CHECK(parse(UNIX_FILE, "p"), "unix");
    char small[20];
    CHECK(picocalc_shell_args_path(&args, "abcdefgh", small, 20) &&
              strcmp(small, "/home/docs/abcdefgh") == 0,
          "fits: %s", small);
    CHECK(!picocalc_shell_args_path(&args, "abcdefghi", small, 20), "one too many");
    CHECK(!picocalc_shell_args_path(&args, "a", small, 1), "no room");
    return 0;
}

/* ---- the shell ------------------------------------------------------------ */

static shell_t sh;
static char    con[1 << 14];
static size_t  con_len;
static int     boots;
static bool    boot_was_ram;
static char    boot_ram[SHELL_PATH_MAX + 1];

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

static void boot(void *ctx, const char *ram, const uf2_info_t *info) {
    (void)ctx;
    (void)info;
    boots++;
    boot_was_ram = ram != NULL;
    snprintf(boot_ram, sizeof boot_ram, "%s", ram ? ram : "");
}

static const shell_loader_t loader = {
    NULL, { FLASH_BASE, FLASH_END, fl_read, fl_erase, fl_program, NULL }, boot, NULL
};

static int run(const char *line) {
    con_len = 0;
    con[0] = '\0';
    boots = 0;
    return shell_run(&sh, line);
}

#if SHELL_DOS
#define FLAVOUR "dos"
#define CD_DOCS "CD \\HOME\\DOCS"
#define CD_ROOT "CD \\"
#define CD_TAPES "CD \\TAPES"
#define DECL     "DECL"
#define DEEP_RUN "DEEP"
#define OPT_T    "/T"
#define SHOW_DIR "CD"
#define MENU     "EXIT"
#else
#define FLAVOUR "unix"
#define CD_DOCS "cd /HOME/DOCS"
#define CD_ROOT "cd /"
#define CD_TAPES "cd /TAPES"
#define DECL     "decl"
#define DEEP_RUN "./deep"
#define OPT_T    "-t"
#define SHOW_DIR "pwd"
#define MENU     "menu"
#endif

#define GIVEN(dir, prog, name) "picocalc-args 1\n" FLAVOUR "\n" dir "\n" prog "\n" name "\n"

static int test_shell(void) {
    CHECK(f_mkdir("/pico2-apps") == FR_OK || exists("/pico2-apps"), "apps");
    CHECK(f_mkdir("/HOME") == FR_OK && f_mkdir("/HOME/DOCS") == FR_OK && f_mkdir("/TAPES") == FR_OK,
          "dirs");
    CHECK(puts_file("/TAPES/a.tap", "a") && puts_file("/TAPES/b.tap", "b"), "tapes");

    /* A flash program that declared, and one that did not; and each as a
     * RAM program. */
    make(FLASH_BASE, 16);
    declare(9, 128, "decl-name");
    CHECK(put("/pico2-apps/decl.uf2", ubuf, ulen), "decl");
    make(FLASH_BASE, 16);
    CHECK(put("/pico2-apps/old.uf2", ubuf, ulen), "old");
    make(UF2_SRAM_BASE, 16);
    declare(4, 0, "ram-name");
    CHECK(put("/pico2-apps/longramprogram.uf2", ubuf, ulen), "ram");
    make(UF2_SRAM_BASE, 16);
    CHECK(put("/pico2-apps/oldram.uf2", ubuf, ulen), "oldram");

    /* Refused, with nothing written to the flash or the card. */
    flash_ops = 0;
    unsigned long writes = host_disk_writes();
    CHECK(run("old x") == 2 && boots == 0 && strcmp(con, "old: takes no arguments\n") == 0,
          "undeclared: '%s'", con);
    CHECK(run("oldram " OPT_T " x") == 2 && boots == 0 &&
              strcmp(con, "oldram: takes no arguments\n") == 0,
          "undeclared RAM: '%s'", con);
    CHECK(flash_ops == 0 && host_disk_writes() == writes && !exists(ARGS_FILE),
          "nothing written: %ld flash, %lu card", flash_ops, host_disk_writes() - writes);
    /* A word for a program that cannot run says why it cannot. */
    CHECK(puts_file("/pico2-apps/text.uf2", "hello\n") && run("text x") == 126 &&
              strcmp(con, "text: not a UF2 program\n") == 0,
          "not a UF2: '%s'", con);
    /* Still refused, declared or not: there is nothing to give them to. */
    CHECK(run(DECL " x > out") == 2 && boots == 0 &&
              strcmp(con, DECL ": a program takes no redirection or pipe\n") == 0,
          "redirection: '%s'", con);
    CHECK(run("echo x | " DECL " y") == 2 && boots == 0, "pipe: '%s'", con);
    CHECK(!exists(ARGS_FILE), "no file for a refusal");

    /* An older program costs no write of the file. */
    CHECK(run("old") == 0 && boots == 1 && !exists(ARGS_FILE), "old runs: '%s'", con);
    CHECK(run("oldram") == 0 && boots == 1 && boot_was_ram && !exists(ARGS_FILE), "oldram: '%s'", con);

    /* A declared program gets it with no arguments, for the directory. */
    CHECK(run(CD_DOCS) == 0, "cd: '%s'", con);
    CHECK(run(DECL) == 0 && boots == 1 && !boot_was_ram, "decl: '%s'", con);
    CHECK(strcmp(slurp(ARGS_FILE), GIVEN("/HOME/DOCS", "/pico2-apps/decl.uf2", "decl-name")) == 0,
          "file: '%s'", slurped);
    /* The directory to come back to is written too, as ever. */
    CHECK(strcmp(slurp(CWD_FILE), "/HOME/DOCS\n") == 0, "cwd: '%s'", slurped);

    /* With them; written again, not added to. */
    CHECK(run(DECL " " OPT_T " game.tap") == 0 && boots == 1, "decl -t: '%s'", con);
    CHECK(strcmp(slurp(ARGS_FILE),
                 GIVEN("/HOME/DOCS", "/pico2-apps/decl.uf2", "decl-name") OPT_T "\ngame.tap\n") == 0,
          "file: '%s'", slurped);
    /* What the header makes of it: the program's whole job. */
    CHECK(picocalc_shell_args_parse(slurped, slurped_len, "decl-name", &args), "parse");
    CHECK(args.dos == (bool)SHELL_DOS && args.argc == 2, "dos %d argc %d", args.dos, args.argc);
    CHECK(strcmp(option('t'), "/HOME/DOCS/game.tap") == 0, "option: %s", option('t'));
    /* A file for another program is not one for this. */
    slurp(ARGS_FILE);
    CHECK(!picocalc_shell_args_parse(slurped, slurped_len, "other", &args), "other");

    /* A RAM program: its long path in the file, its 8.3 path to stage3. */
    CHECK(run("longramprogram a") == 0 && boots == 1 && boot_was_ram &&
              strcmp(boot_ram, "/PICO2-~1/LONGRA~1.UF2") == 0,
          "ram: '%s' '%s'", con, boot_ram);
    CHECK(strcmp(slurp(ARGS_FILE), GIVEN("/HOME/DOCS", "/pico2-apps/longramprogram.uf2",
                                         "ram-name") "a\n") == 0,
          "file: '%s'", slurped);

    /* The program's path as the card spells it, however it was typed. */
    CHECK(run(SHELL_DOS ? "\\PICO2-APPS\\DECL.UF2 x" : "/PICO2-APPS/DECL.UF2 x") == 0 && boots == 1,
          "by path: '%s'", con);
    CHECK(strcmp(slurp(ARGS_FILE), GIVEN("/HOME/DOCS", "/pico2-apps/decl.uf2", "decl-name") "x\n") == 0,
          "spelt: '%s'", slurped);

    /* Renaming the UF2 changes nothing: the name is the declared one. */
    make(FLASH_BASE, 16);
    declare(9, 128, "decl-name");
    CHECK(put("/pico2-apps/renamed.uf2", ubuf, ulen) && run("renamed") == 0, "renamed: '%s'", con);
    CHECK(strcmp(slurp(ARGS_FILE), GIVEN("/HOME/DOCS", "/pico2-apps/renamed.uf2", "decl-name")) == 0,
          "renamed: '%s'", slurped);

    /* Each flavour's words. */
    CHECK(run(CD_TAPES) == 0, "cd: '%s'", con);
#if SHELL_DOS
    /* After %NAME%, with wildcards as typed, and a batch file's %1. */
    CHECK(run("SET T=WHO.TAP") == 0 && run("DECL /T %T% \"TWO WORDS\" *.TAP") == 0 && boots == 1,
          "words: '%s'", con);
    CHECK(strcmp(slurp(ARGS_FILE), GIVEN("/TAPES", "/pico2-apps/decl.uf2", "decl-name")
                 "/T\nWHO.TAP\nTWO WORDS\n*.TAP\n") == 0, "words: '%s'", slurped);
    CHECK(puts_file("/TAPES/GO.BAT", "@DECL /T %1\r\n") && run("GO X.TAP") == 0 && boots == 1,
          "batch: '%s'", con);
    CHECK(strcmp(slurp(ARGS_FILE), GIVEN("/TAPES", "/pico2-apps/decl.uf2", "decl-name")
                 "/T\nX.TAP\n") == 0, "batch: '%s'", slurped);
    /* With no space, as DIR/W. */
    CHECK(run("DECL/T X.TAP") == 0 && boots == 1, "DECL/T: '%s'", con);
    CHECK(strcmp(slurp(ARGS_FILE), GIVEN("/TAPES", "/pico2-apps/decl.uf2", "decl-name")
                 "/T\nX.TAP\n") == 0, "DECL/T: '%s'", slurped);
    CHECK(run("OLD/T X.TAP") == 2 && strcmp(con, "OLD: takes no arguments\n") == 0, "OLD/T: '%s'", con);
    CHECK(run("NOSUCH/T") == 127 && boots == 0, "NOSUCH/T: '%s'", con);
    /* The '/' after a path's name is a switch too, and is never a path's. */
    CHECK(run("\\PICO2-APPS\\DECL/T X.TAP") == 0 && boots == 1, "path/T: '%s'", con);
    CHECK(strstr(slurp(ARGS_FILE), "/T\nX.TAP\n") != NULL, "path/T: '%s'", slurped);
    CHECK(run("\\PICO2-APPS\\OLD") == 0 && boots == 1, "path: '%s'", con);
    CHECK(run("/PICO2-APPS/OLD") == 127 && boots == 0 &&
              strcmp(con, "Bad command or file name\n") == 0, "/path: '%s'", con);
    CHECK(run("C:/PICO2-APPS/OLD") == 127 && boots == 0 &&
              strcmp(con, "Bad command or file name\n") == 0, "C:/path: '%s'", con);
    CHECK(run("PICO2-APPS/OLD") == 127 && boots == 0, "a/b: '%s'", con);
    CHECK(run("CALL /PICO2-APPS/OLD") == 127 && boots == 0 &&
              strcmp(con, "Bad command or file name\n") == 0, "CALL /path: '%s'", con);
#else
    /* After variables, quotes and globs. */
    CHECK(run("T=who.tap") == 0 && run("decl -t $T 'two words' *.tap") == 0 && boots == 1,
          "words: '%s'", con);
    CHECK(strcmp(slurp(ARGS_FILE), GIVEN("/TAPES", "/pico2-apps/decl.uf2", "decl-name")
                 "-t\nwho.tap\ntwo words\na.tap\nb.tap\n") == 0, "words: '%s'", slurped);
    /* -t *.tap: each match a word, and the helper takes the first. */
    CHECK(run("decl -t *.tap") == 0 && boots == 1, "glob: '%s'", con);
    slurp(ARGS_FILE);
    CHECK(picocalc_shell_args_parse(slurped, slurped_len, "decl-name", &args) && args.argc == 3 &&
              strcmp(option('t'), "/TAPES/a.tap") == 0,
          "glob: %s", option('t'));
    /* The longest the shell can give, in the header's buffer. */
    CHECK(f_mkdir("/MANY") == FR_OK && run("cd /MANY") == 0, "many");
    for (int i = 0; i < 100; i++) {
        char name[64];
        snprintf(name, sizeof name, "/MANY/a-file-with-a-long-name-%03d.tap", i);
        CHECK(puts_file(name, ""), "%s", name);
    }
    CHECK(run("decl *") == 0 && boots == 1, "many: '%s'", con);
    CHECK(slurp(ARGS_FILE) && slurped_len > 3000 && slurped_len < PICOCALC_SHELL_ARGS_SIZE, "%zu bytes",
          slurped_len);
    CHECK(picocalc_shell_args_parse(slurped, slurped_len, "decl-name", &args) && args.argc == 100,
          "argc %d", args.argc);
    CHECK(strcmp(picocalc_shell_args_word(&args, 99), "a-file-with-a-long-name-099.tap") == 0,
          "last: %s", picocalc_shell_args_word(&args, 99));
#endif
    CHECK(run(CD_ROOT) == 0, "cd: '%s'", con);

    /* A failed write stops the start, at every write it takes, and leaves
     * flash, the shell and the next start well. */
    CHECK(run(DECL " x") == 0 && boots == 1, "whole: '%s'", con);
    f_unlink(ARGS_FILE);
    f_unlink(CWD_FILE);
    int stopped = 0;
    for (long n = 0; n < 64; n++) {
        host_disk_fail_writes_after(n);
        int status = run(DECL " x");
        host_disk_fail_writes_after(-1);
        files_mount();
        if (boots) {
            /* It got far enough: the file must be whole. */
            CHECK(status == 0 && strcmp(slurp(ARGS_FILE), GIVEN("/", "/pico2-apps/decl.uf2",
                                                               "decl-name") "x\n") == 0,
                  "after %ld writes: '%s'", n, slurped);
            CHECK(stopped > 0, "no write ever failed");
            break;
        }
        stopped++;
        CHECK(status == 1 && strstr(con, DECL ": " ARGS_FILE ": "), "%ld writes: %d '%s'", n, status, con);
        /* Nothing is left that a program could read, once the shell has
         * started again. */
        shell_start(&sh);
        CHECK(!exists(ARGS_FILE) && !exists(ARGS_FILE ".new"), "%ld writes: left over", n);
        CHECK(run(CD_ROOT) == 0, "cd: '%s'", con);
    }
    CHECK(stopped > 0, "no failure seen");
    CHECK(run(DECL " x") == 0 && boots == 1, "and again: '%s'", con);

    /* What a program did not take is gone at the shell's next start: a
     * power cut between the write and the program, or a .new from one
     * during the write. */
    CHECK(exists(ARGS_FILE), "left");
    CHECK(puts_file(ARGS_FILE ".new", "half"), ".new");
    shell_start(&sh);
    CHECK(!exists(ARGS_FILE) && !exists(ARGS_FILE ".new"), "the start removes it");
    /* And it does no harm with nothing there. */
    shell_start(&sh);
    CHECK(run(SHOW_DIR) == 0, "the shell runs: '%s'", con);

    /* A RAM program whose path stage3 could not open stops before anything
     * is written: each name is longer as its 8.3 entry, past the limit, so
     * nothing is left for the loader's menu to start the program with. */
    make(UF2_SRAM_BASE, 8);
    declare(3, 64, "decl-name");
    static char deep[SHELL_PATH_MAX + 1];
    deep[0] = '\0';
    size_t deep_len = 0;
    for (int i = 0; i < 60; i++) {
        deep_len += (size_t)snprintf(deep + deep_len, sizeof deep - deep_len, "/a+");
        CHECK(f_mkdir(deep) == FR_OK, "mkdir %d", i);
    }
    char deep_prog[SHELL_PATH_MAX + 1];
    CHECK(path_join(deep_prog, deep, "deep.uf2") && put(deep_prog, ubuf, ulen), "the program");
    f_unlink(CWD_FILE);
    snprintf(sh.cwd, sizeof sh.cwd, "%s", deep);
    int deep_status = run(DEEP_RUN " x");
    CHECK(deep_status == 1 && boots == 0 && strstr(con, DEEP_RUN), "too deep: %d '%s'", deep_status, con);
    CHECK(!exists(ARGS_FILE) && !exists(ARGS_FILE ".new"), "too deep: a file was left");
    CHECK(!exists(CWD_FILE), "too deep: the directory was kept");
    CHECK(run(CD_ROOT) == 0, "cd: '%s'", con);

    /* The menu is never given any. */
    make(UF2_SRAM_BASE, 16);
    CHECK(put(SHELL_MENU_UF2, ubuf, ulen) && run(MENU) == 0 && boots == 1 && !exists(ARGS_FILE),
          "menu: '%s'", con);
    f_unlink(SHELL_MENU_UF2);
    return 0;
}

static int all(bool fat32) {
    CHECK(host_disk_create(NULL, fat32 ? 64u << 20 : 32u << 20, fat32), "image");
    files_mount();
    memset(flash_mem, 0xFF, sizeof flash_mem);
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, NULL, NULL, NULL, NULL, NULL };
    shell_init(&sh, &out, &hooks, "test");
    shell_set_loader(&sh, &loader);
    shell_start(&sh);
    if (test_record() || test_real() || test_shell()) return 1;
    host_disk_close();
    return 0;
}

int main(void) {
    if (test_header() || all(false) || all(true)) return 1;
    TEST_DONE();
}
