/* test_ntp.c — NTP (design.md §3.8, M12 step 4): /ntp/ntp.cfg's parser,
 * and the W build's `ntp` (`NTP` in the DOS flavour, which this file is
 * also built for, as test_ntp_dos): ntp.uf2 run once per power-on, so a
 * startup file with `ntp` in it does not loop, whatever ntp.uf2 did or
 * failed to do; -f (/F); and its mistakes.
 *
 * The port's record is a fake, and so is the loader: its boot stands
 * for ntp.uf2 running and rebooting into the shell, which the test then
 * starts again, as the board would. */

#include <stdio.h>
#include <string.h>

#include "clock.h"
#include "files.h"
#include "host_disk.h"
#include "ntp.h"
#include "run.h"
#include "shell.h"
#include "test_util.h"

/* ---- the parser ------------------------------------------------------- */

static ntp_cfg_status_t parse(ntp_config_t *cfg, const char *text, unsigned *line) {
    return ntp_config_parse(cfg, text, strlen(text), line);
}

static int test_parse(void) {
    ntp_config_t c;
    unsigned line;

    CHECK(parse(&c, "", &line) == NTP_CFG_NO_SSID && line == 0, "empty: %d", line);
    CHECK(strcmp(c.server, "pool.ntp.org") == 0 && c.timeout_s == 20 && !c.country[0] &&
              !c.password[0],
          "defaults");

    CHECK(parse(&c,
                "# /ntp/ntp.cfg\r\n"
                "server   = time.nrc.ca          # Ottawa\r\n"
                "country  = ca\r\n"
                "\r\n"
                "  TIMEOUT=30\r\n"
                "ssid     = My Home\r\n"
                "password = \"my # password \"  # quoted\r\n",
                &line) == NTP_CFG_OK && line == 0,
          "whole file: line %u", line);
    CHECK(strcmp(c.server, "time.nrc.ca") == 0, "server '%s'", c.server);
    CHECK(strcmp(c.country, "CA") == 0, "country '%s'", c.country);
    CHECK(c.timeout_s == 30, "timeout %u", c.timeout_s);
    CHECK(strcmp(c.ssid, "My Home") == 0, "ssid '%s'", c.ssid);
    CHECK(strcmp(c.password, "my # password ") == 0, "password '%s'", c.password);

    /* A # with no space before it is part of the value. */
    CHECK(parse(&c, "ssid = Cafe#2\npassword = pass#word\n", &line) == NTP_CFG_OK &&
              strcmp(c.ssid, "Cafe#2") == 0 && strcmp(c.password, "pass#word") == 0,
          "# in a value: '%s' '%s'", c.ssid, c.password);
    /* Open, and the key as 64 hex digits. */
    CHECK(parse(&c, "ssid = Open\npassword =\n", &line) == NTP_CFG_OK && !c.password[0], "open");
    CHECK(parse(&c, "ssid=\"\"", &line) == NTP_CFG_BAD_VALUE && line == 1, "ssid \"\"");
    const char *hex = "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef";
    char buf[256];
    snprintf(buf, sizeof buf, "ssid = x\npassword = %s\n", hex);
    CHECK(parse(&c, buf, &line) == NTP_CFG_OK && strcmp(c.password, hex) == 0, "hex key");

    /* Each mistake, on its line; the lines after it still apply. */
    static const struct {
        const char      *text;
        ntp_cfg_status_t want;
        unsigned         line;
    } bad[] = {
        { "ssid = x\nserver\n", NTP_CFG_SYNTAX, 2 },
        { "ssid = x\n= pool\n", NTP_CFG_SYNTAX, 2 },
        { "ssid = \"x\ny\n", NTP_CFG_SYNTAX, 1 },
        { "ssid = \"x\" y\n", NTP_CFG_SYNTAX, 1 },
        { "ssid = x\nwifi = y\n", NTP_CFG_UNKNOWN, 2 },
        { "ssid = x\nssid = y\n", NTP_CFG_DUPLICATE, 2 },
        { "ssid = x\ncountry = CAN\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = x\ncountry = 1A\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = x\ntimeout = 4\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = x\ntimeout = 61\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = x\ntimeout = 2x\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = x\ntimeout = 99999999999\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = x\npassword = short\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = x\nserver = a b\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = x\nserver =\n", NTP_CFG_BAD_VALUE, 2 },
        { "ssid = 123456789012345678901234567890123\n", NTP_CFG_TOO_LONG, 1 },
        { "server = ok\n", NTP_CFG_NO_SSID, 0 },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ntp_cfg_status_t st = parse(&c, bad[i].text, &line);
        CHECK(st == bad[i].want && line == bad[i].line, "'%s': %s line %u", bad[i].text,
              ntp_cfg_status_str(st), line);
    }
    CHECK(parse(&c, "country = CAN\nssid = x\ntimeout = 9\n", &line) == NTP_CFG_BAD_VALUE &&
              line == 1 && c.timeout_s == 9 && strcmp(c.ssid, "x") == 0 && !c.country[0],
          "the rest applies");
    /* 64 characters that are not hex. */
    snprintf(buf, sizeof buf, "ssid = x\npassword = %.63sg\n", hex);
    CHECK(parse(&c, buf, &line) == NTP_CFG_BAD_VALUE && line == 2, "64, not hex");
    /* A line too long, and one with a NUL. */
    memset(buf, 'a', sizeof buf);
    memcpy(buf, "ssid = x\n#", 10);
    buf[sizeof buf - 1] = 0;
    CHECK(parse(&c, buf, &line) == NTP_CFG_TOO_LONG && line == 2, "long line");
    CHECK(ntp_config_parse(&c, "ssid = x\0y\n", 11, &line) == NTP_CFG_SYNTAX && line == 1, "NUL");

    for (int r = 0; r < NTP_RESULTS; r++)
        CHECK(strcmp(ntp_result_str((ntp_result_t)r), "?") != 0 &&
                  strlen(ntp_result_str((ntp_result_t)r)) <= 34,
              "result %d: '%s'", r, ntp_result_str((ntp_result_t)r));
    return 0;
}

/* ---- the builtin ------------------------------------------------------ */

static shell_t sh;
static char    con[1 << 14];
static size_t  con_len;

static void sink(void *ctx, const char *buf, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n; i++)
        if (buf[i] != '\r' && con_len + 1 < sizeof con) con[con_len++] = buf[i];
    con[con_len] = '\0';
}

static uint64_t now_us(void *ctx) {
    (void)ctx;
    return 0;
}

static bool interrupted(void *ctx) {
    (void)ctx;
    return false;
}

static int boots;

static int run(const char *line) {
    con_len = 0;
    con[0] = '\0';
    boots = 0;
    return shell_run(&sh, line);
}

/* The record's NTP bits, as the port keeps them, and the clock. */
static int  record_ntp = -1;
static bool clock_is_set;

static bool fake_get(void *ctx, int64_t *utc_ms) {
    (void)ctx;
    if (clock_is_set) *utc_ms = 1792679405000;
    return clock_is_set;
}

static int fake_ntp(void *ctx) {
    (void)ctx;
    return record_ntp;
}

static void fake_ntp_keep(void *ctx, int result) {
    (void)ctx;
    record_ntp = result;
}

/* The loader: a boot is ntp.uf2 running, which leaves `ntp_does` in the
 * record (-1: nothing, as if it never got as far), and the reboot. */
static char boot_ram[SHELL_PATH_MAX + 1];
static int  ntp_does = NTP_SET;

static void boot(void *ctx, const char *ram, const uf2_info_t *info) {
    (void)ctx;
    (void)info;
    boots++;
    snprintf(boot_ram, sizeof boot_ram, "%s", ram ? ram : "");
    if (ntp_does >= 0) record_ntp = ntp_does;
    if (ntp_does == NTP_SET) clock_is_set = true;
}

static uint8_t flash_mem[64u << 10];

static void fl_read(void *ctx, uint32_t addr, void *buf, uint32_t n) {
    (void)ctx;
    memcpy(buf, flash_mem + (addr - 0x10000000u), n);
}

static bool fl_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    (void)addr;
    return false;
}

static bool fl_program(void *ctx, uint32_t addr, const void *page) {
    (void)ctx;
    (void)addr;
    (void)page;
    return false;
}

static shell_loader_t loader = {
    NULL, { 0x10000000u, 0x10000000u + sizeof flash_mem, fl_read, fl_erase, fl_program, NULL },
    boot, NULL
};

/* A RAM program of a few pages, as stage3 loads one. */
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static bool put(const char *path, const void *data, size_t n) {
    FIL f;
    UINT w;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT r = f_write(&f, data, (UINT)n, &w);
    return f_close(&f) == FR_OK && r == FR_OK && w == n;
}

static bool put_ram_uf2(const char *path) {
    static uint8_t u[4 * UF2_BLOCK];
    for (uint32_t k = 0; k < 4; k++) {
        uint8_t *b = u + k * UF2_BLOCK;
        memset(b, 0, UF2_BLOCK);
        put32(b, 0x0A324655u);
        put32(b + 4, 0x9E5D5157u);
        put32(b + 8, 0x2000u);   /* the family is given */
        put32(b + 12, UF2_SRAM_BASE + k * UF2_PAGE);
        put32(b + 16, UF2_PAGE);
        put32(b + 20, k);
        put32(b + 24, 4);
        put32(b + 28, 0xe48bff59u);   /* RP2350, Arm, secure */
        for (uint32_t i = 0; i < UF2_PAGE; i++) b[32 + i] = (uint8_t)(k * 7u + i);
        if (k == 1) put32(b + 32 + 0x3c, 0xffffded3u);   /* a picobin block's head */
        put32(b + 508, 0x0AB16F30u);
    }
    return put(path, u, sizeof u);
}

/* As the board starts the shell: the banner, then the startup file. */
static void start(void) {
    con_len = 0;
    con[0] = '\0';
    boots = 0;
    shell_start(&sh);
}

#define NTP      (SHELL_DOS ? "NTP" : "ntp")
#define FORCE    (SHELL_DOS ? "NTP /F" : "ntp -f")
#define PROFILE  (SHELL_DOS ? "/AUTOEXEC.BAT" : "/etc/profile")
#define RAM_PATH "/PICO2-~1/NTP.UF2"

static int test_builtin(void) {
    char want[128];

    /* No NTP in the port: as the Pico 2 build would be, had it the word. */
    CHECK(run(NTP) == 1 && strstr(con, "not on this machine"), "no NTP: '%s'", con);

    const shell_clock_t clock = { fake_get, NULL, NULL, NULL, fake_ntp, fake_ntp_keep, NULL };
    shell_set_clock(&sh, &clock);
    shell_set_loader(&sh, &loader);

    /* No ntp.uf2: said, and not counted as tried. */
    CHECK(run(NTP) == 127 && boots == 0 && record_ntp == -1, "no ntp.uf2: '%s' %d", con, record_ntp);
    CHECK(strcmp(con, SHELL_DOS ? "Bad command or file name\n" : "ntp: not found\n") == 0,
          "no ntp.uf2: '%s'", con);

    /* Found in $PATH, as a program is. */
    CHECK(f_mkdir("/bin2") == FR_OK && put_ram_uf2("/bin2/ntp.uf2"), "elsewhere");
    CHECK(run(SHELL_DOS ? "PATH C:\\BIN2" : "PATH=/bin2") == 0 && run(NTP) == 0 && boots == 1 &&
              strcmp(boot_ram, "/BIN2/NTP.UF2") == 0 && record_ntp == NTP_SET,
          "in $PATH: '%s' '%s'", con, boot_ram);
    record_ntp = -1;
    clock_is_set = false;
    CHECK(f_unlink("/bin2/ntp.uf2") == FR_OK &&
              run(SHELL_DOS ? "PATH C:\\PICO2-APPS" : "PATH=/pico2-apps") == 0,
          "PATH back");

    CHECK(f_mkdir("/pico2-apps") == FR_OK && put_ram_uf2("/pico2-apps/ntp.uf2"), "ntp.uf2");
    if (!SHELL_DOS) CHECK(f_mkdir("/etc") == FR_OK, "/etc");
    snprintf(want, sizeof want, SHELL_DOS ? "@ECHO OFF\r\n%s\r\n" : "%s\n", NTP);
    CHECK(put(PROFILE, want, strlen(want)), "startup file");

    /* Power-on: the startup file runs ntp.uf2, which sets the clock. */
    start();
    CHECK(boots == 1 && strcmp(boot_ram, RAM_PATH) == 0, "power-on: %d '%s' '%s'", boots,
          boot_ram, con);
    CHECK(record_ntp == NTP_SET && clock_is_set, "set: %d", record_ntp);
    /* Back in the shell: the startup file does not run it again, and
     * says nothing. */
    start();
    snprintf(want, sizeof want, "%s: ", NTP);
    CHECK(boots == 0 && !strstr(con, want), "back: %d '%s'", boots, con);
    CHECK(run(NTP) == 0 && boots == 0 && !con[0], "typed: '%s'", con);

    /* -f runs it again; a failure is kept, and the status says so. */
    ntp_does = NTP_NO_REPLY;
    CHECK(run(FORCE) == 0 && boots == 1, "force: '%s'", con);
    snprintf(want, sizeof want, "%s: no time from the server\n", NTP);
    start();
    CHECK(boots == 0 && strstr(con, want), "failed, back: '%s'", con);
    CHECK(run(NTP) == 1 && boots == 0 && strcmp(con, want) == 0, "failed: '%s'", con);
    if (SHELL_DOS) {
        CHECK(run("ntp /f") == 0 && boots == 1, "/f: '%s'", con);
        CHECK(run("NTP > NUL") == 1 && boots == 0, "> NUL: '%s'", con);
    }

    /* ntp.uf2 that never reports, as if the power failed in it: the next
     * start does not run it again. */
    record_ntp = -1;
    ntp_does = -1;
    start();
    CHECK(boots == 1 && record_ntp == NTP_UNFINISHED, "unfinished: %d", record_ntp);
    start();
    snprintf(want, sizeof want, "%s: ntp.uf2 did not finish\n", NTP);
    CHECK(boots == 0 && strstr(con, want), "no loop: '%s'", con);

    /* Its mistakes. */
    CHECK(run(SHELL_DOS ? "NTP /X" : "ntp -x") == 1 && boots == 0 &&
              strcmp(con, SHELL_DOS ? "usage: NTP [/F]\n" : "usage: ntp [-f]\n") == 0,
          "usage: '%s'", con);
    CHECK(run(SHELL_DOS ? "NTP /F /F" : "ntp -f -f") == 1 && boots == 0, "twice: '%s'", con);
    if (!SHELL_DOS)
        CHECK(run("ntp -f > /out") == 1 && boots == 0 &&
                  strcmp(con, "ntp: not with a redirection or a pipe\n") == 0,
              "redirected: '%s'", con);
    /* A failed run leaves the record as it was. */
    record_ntp = NTP_NO_NETWORK;
    CHECK(f_unlink("/pico2-apps/ntp.uf2") == FR_OK && run(FORCE) == 127 && record_ntp == NTP_NO_NETWORK,
          "-f, no ntp.uf2: %d", record_ntp);
    return 0;
}

/* `web` (design.md §3.9): web.uf2, run each time it is typed. */
static int test_web(void) {
    const char *web = SHELL_DOS ? "WEB" : "web";
    char want[64];

    /* No web.uf2: said, and nothing booted. */
    CHECK(run(web) == 127 && boots == 0, "no web.uf2: '%s'", con);
    CHECK(strcmp(con, SHELL_DOS ? "Bad command or file name\n" : "web: not found\n") == 0, "said: '%s'", con);

    CHECK(f_mkdir("/pico2-apps") == FR_EXIST || f_stat("/pico2-apps", &(FILINFO){ 0 }) == FR_OK, "dir");
    CHECK(put_ram_uf2("/pico2-apps/web.uf2"), "web.uf2");
    record_ntp = -1;
    CHECK(run(web) == 0 && boots == 1 && strcmp(boot_ram, "/PICO2-~1/WEB.UF2") == 0, "runs: %d '%s' '%s'",
          boots, boot_ram, con);
    CHECK(record_ntp == NTP_SET || record_ntp == -1, "the NTP record is left alone");
    /* Every time: there is no once-per-power-on rule, a person typed it. */
    CHECK(run(web) == 0 && boots == 1, "again: %d", boots);

    /* Its mistakes. */
    snprintf(want, sizeof want, "usage: %s\n", web);
    CHECK(run(SHELL_DOS ? "WEB x" : "web x") == 1 && boots == 0 && strcmp(con, want) == 0, "usage: '%s'", con);
    if (!SHELL_DOS)
        CHECK(run("web > /out") == 1 && boots == 0 && strcmp(con, "web: not with a redirection or a pipe\n") == 0,
              "redirected: '%s'", con);
    CHECK(run(SHELL_DOS ? "HELP WEB" : "man web") == 0 && strstr(con, "browser"), "help: '%s'", con);
    CHECK(f_unlink("/pico2-apps/web.uf2") == FR_OK, "removed");
    return 0;
}

/* `bt` (design.md §3.10): what the Bluetooth keyboard is doing. */
static const char *bt_line = "connected: K380";

static const char *fake_bluetooth(void *ctx) {
    (void)ctx;
    return bt_line;
}

static int test_bt(void) {
    const char *bt = SHELL_DOS ? "BT" : "bt";
    char want[96];

    /* No Bluetooth in the port, as in the Pico 2 build. */
    CHECK(run(bt) == 1 && strstr(con, "not on this machine"), "no Bluetooth: '%s'", con);

    shell_system_t sys = { 0 };
    sys.bluetooth = fake_bluetooth;
    shell_set_system(&sh, &sys);
    CHECK(run(bt) == 0 && strcmp(con, "connected: K380\n") == 0 && boots == 0, "connected: '%s'", con);
    bt_line = "off: no firmware file on the card";
    CHECK(run(bt) == 0 && strcmp(con, "off: no firmware file on the card\n") == 0, "off: '%s'", con);

    /* `info` has it too, cut to fit 40 columns with its name. */
    CHECK(run(SHELL_DOS ? "INFO" : "info") == 0, "info");
    snprintf(want, sizeof want, "bluetooth %.29s\n", bt_line);
    CHECK(strstr(con, want), "info line: '%s'", con);
    char *nl = strstr(strstr(con, "bluetooth "), "\n");
    CHECK(nl && (size_t)(nl - strstr(con, "bluetooth ")) <= 39, "fits 40 columns");

    snprintf(want, sizeof want, "usage: %s\n", bt);
    CHECK(run(SHELL_DOS ? "BT x" : "bt x") == 1 && strcmp(con, want) == 0, "usage: '%s'", con);
    CHECK(run(SHELL_DOS ? "HELP BT" : "man bt") == 0 && strstr(con, "Bluetooth"), "help: '%s'", con);
    memset(&sys, 0, sizeof sys);
    shell_set_system(&sh, &sys);
    return 0;
}

int main(void) {
    if (test_parse()) return 1;
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now_us, interrupted, NULL, NULL, NULL, NULL };
    shell_init(&sh, &out, &hooks, "test");
    if (test_builtin()) return 1;
    if (test_web()) return 1;
    if (test_bt()) return 1;
    TEST_DONE();
}
