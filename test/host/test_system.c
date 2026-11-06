/* test_system.c — the PicoCalc's own commands (system.h, design.md
 * §4.5): help, clear, phosphor, battery, backlight, info, poweroff,
 * reboot and bootsel, against a fake of the port's southbridge, ink and
 * power. */

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "history.h"
#include "host_disk.h"
#include "shell.h"
#include "term.h"
#include "test_util.h"

static term_t  t;
static shell_t sh;
static char    con[1 << 15];
static size_t  con_len;

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

static int run(const char *line) {
    con_len = 0;
    con[0] = '\0';
    return shell_run(&sh, line);
}

static const char *row(unsigned r) {
    static char buf[TERM_COLS + 1];
    term_row_text(&t, r, buf);
    return buf;
}

static const char *slurp(const char *path) {
    static char buf[4096];
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

/* ---- the fake port ----------------------------------------------------- */

static const char *sb_says;          /* every southbridge call's failure, or NULL */
static unsigned    battery_pct = 87;
static bool        battery_charging;
static unsigned    lights[2] = { 128, 64 };
static unsigned    light_writes;
static uint16_t    ink;
static unsigned    power_calls;
static shell_power_t power_what;
static const char *power_says = "the fake does not leave";
static const char *history_then;     /* the history file when power was called */

static const char *battery(void *ctx, unsigned *percent, bool *charging) {
    (void)ctx;
    if (sb_says) return sb_says;
    *percent = battery_pct;
    *charging = battery_charging;
    return NULL;
}

/* Stepped as the MCU does (hardware-notes.md §4.11, §6). */
static const char *backlight(void *ctx, shell_light_t which, int level, unsigned *now_) {
    (void)ctx;
    if (sb_says) return sb_says;
    if (level >= 0) {
        light_writes++;
        unsigned step = which == SHELL_LIGHT_LCD ? 16u : 32u;
        unsigned v = (unsigned)level / step * step;
        if (which == SHELL_LIGHT_LCD && v < 16u) v = 16u;
        if (which == SHELL_LIGHT_LCD && v > 240u) v = 240u;
        lights[which] = v;
    }
    *now_ = lights[which];
    return NULL;
}

static void phosphor(void *ctx, uint16_t rgb565) {
    (void)ctx;
    ink = rgb565;
    term_set_phosphor(&t, rgb565);
}

static void info(void *ctx, const out_t *o) {
    (void)ctx;
    out_puts(o, "chip      fake\n");
}

static const char *power(void *ctx, shell_power_t what) {
    (void)ctx;
    power_calls++;
    power_what = what;
    history_then = slurp(HISTORY_FILE);
    return power_says;
}

int main(void) {
    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, NULL, NULL, NULL, NULL };
    term_init(&t);
    shell_init(&sh, &out, &hooks, "v-test");
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    shell_start(&sh);

    /* ---- with no port: each says so ---- */
    const char *needs[] = { "battery", "backlight", "backlight lcd 100", "phosphor green",
                            "poweroff", "reboot", "bootsel" };
    for (size_t i = 0; i < sizeof needs / sizeof needs[0]; i++)
        CHECK(run(needs[i]) == 1 && strstr(con, ": not on this machine\n"), "%s: '%s'", needs[i],
              con);
    CHECK(run("info") == 0 && strstr(con, "shell     v-test\n") && strstr(con, "card      FAT") &&
              !strstr(con, "battery"),
          "info, no port: '%s'", con);
    CHECK(run("phosphor") == 0 && strcmp(con, "default\n") == 0, "phosphor: '%s'", con);

    const shell_system_t sys = { battery, backlight, phosphor, info, power, NULL, NULL, NULL };
    shell_set_system(&sh, &sys);

    /* ---- man ---- */
    CHECK(run("man") == 0, "man");
    CHECK(strncmp(con, "backlight  battery    bt\nbootsel    break      cat", 44) == 0, "columns: '%.60s'", con);
    CHECK(strstr(con, "\n`man name` for one.") && strstr(con, "wc") && strstr(con, "web") && strstr(con, "which") && strstr(con, "tail"),
          "man: '%s'", con);
    for (unsigned i = 0; shell_builtin_at(i); i++) {
        char line[32];
        snprintf(line, sizeof line, "man %s", shell_builtin_at(i));
        CHECK(run(line) == 0 && strncmp(con, "usage: ", 7) == 0 &&
                  strncmp(con + 7, shell_builtin_at(i), strlen(shell_builtin_at(i))) == 0,
              "%s: '%s'", line, con);
        /* The about line fits the narrow screen. */
        CHECK(strlen(shell_builtin_about(i)) < TERM_COLS, "%s: too wide", line);
    }
    CHECK(run("man head") == 0 &&
              strcmp(con, "usage: head [-n N] [file...]\nDisplays the first lines of a file\n") == 0,
          "man head: '%s'", con);
    CHECK(run("man pico-oric") == 1 && strstr(con, "man: pico-oric: not a builtin"), "a program: '%s'", con);
    CHECK(run("man a b") == 1 && strstr(con, "usage: man"), "two");
    CHECK(run("man | grep -c tail") == 0 && strcmp(con, "1\n") == 0, "man |: '%s'", con);

    /* ---- clear ---- */
    run("echo something");
    CHECK(strcmp(row(t.row - 1), "something") == 0 || strstr(con, "something"), "before");
    CHECK(run("clear") == 0 && t.row == 0 && t.col == 0 && strcmp(row(0), "") == 0 &&
              strcmp(row(5), "") == 0,
          "clear: at %u,%u '%s'", t.row, t.col, row(0));
    CHECK(run("clear x") == 1 && strstr(con, "usage: clear"), "clear x");
    CHECK(run("clear > /c.txt") == 0 && strcmp(slurp("/c.txt"), "\033[H\033[2J") == 0, "clear > f");

    /* ---- phosphor ---- */
    CHECK(run("phosphor green") == 0 && ink == TERM_GREEN_PHOSPHOR, "green");
    CHECK(run("echo x") == 0 && t.cells[t.row - 1][0].fg == TERM_GREEN_PHOSPHOR, "drawn green");
    CHECK(run("phosphor") == 0 && strcmp(con, "green\n") == 0, "shown: '%s'", con);
    CHECK(run("phosphor amber") == 0 && ink == TERM_AMBER_PHOSPHOR, "amber");
    CHECK(run("phosphor white") == 0 && ink == TERM_WHITE_PHOSPHOR, "white");
    CHECK(run("phosphor default") == 0 && ink == TERM_FOREGROUND, "default");
    CHECK(run("phosphor pink") == 1 && strstr(con, "pink: white, green, amber or default") &&
              ink == TERM_FOREGROUND,
          "pink: '%s'", con);
    CHECK(run("phosphor a b") == 1 && strstr(con, "usage: phosphor"), "two");

    /* ---- battery ---- */
    CHECK(run("battery") == 0 && strcmp(con, "87%\n") == 0, "battery: '%s'", con);
    battery_charging = true;
    battery_pct = 100;
    CHECK(run("battery") == 0 && strcmp(con, "100%, charging\n") == 0, "charging: '%s'", con);
    CHECK(run("battery > /b.txt") == 0 && strcmp(slurp("/b.txt"), "100%, charging\n") == 0, "> f");
    sb_says = "the southbridge did not answer";
    CHECK(run("battery") == 1 && strcmp(con, "battery: the southbridge did not answer\n") == 0,
          "failed: '%s'", con);
    sb_says = NULL;
    CHECK(run("battery x") == 1 && strstr(con, "usage: battery"), "battery x");

    /* ---- backlight ---- */
    CHECK(run("backlight") == 0 && strcmp(con, "lcd 128\nkbd 64\n") == 0, "both: '%s'", con);
    CHECK(run("backlight kbd") == 0 && strcmp(con, "kbd 64\n") == 0, "kbd: '%s'", con);
    CHECK(run("backlight 200") == 0 && strcmp(con, "lcd 192\n") == 0, "set lcd, stepped: '%s'",
          con);
    CHECK(run("backlight kbd 0") == 0 && strcmp(con, "kbd 0\n") == 0 && lights[1] == 0,
          "kbd off: '%s'", con);
    CHECK(run("backlight lcd 255") == 0 && strcmp(con, "lcd 240\n") == 0, "clamped: '%s'", con);
    light_writes = 0;
    CHECK(run("backlight 256") == 1 && strstr(con, "256: a level is 0 to 255") && !light_writes,
          "256: '%s'", con);
    CHECK(run("backlight -1") == 1 && strstr(con, "a level is 0 to 255") && !light_writes, "-1");
    CHECK(run("backlight lcd x") == 1 && !light_writes, "x");
    CHECK(run("backlight lcd 1 2") == 1 && strstr(con, "usage: backlight") && !light_writes, "three");
    CHECK(run("backlight tv 1") == 1 && !light_writes, "tv: '%s'", con);
    sb_says = "the southbridge timed out";
    CHECK(run("backlight") == 1 && strstr(con, "backlight: lcd: the southbridge timed out") &&
              strstr(con, "backlight: kbd: the southbridge timed out"),
          "failed: '%s'", con);
    sb_says = NULL;

    /* ---- info ---- */
    battery_charging = false;
    CHECK(run("info") == 0, "info");
    CHECK(strncmp(con, "shell     v-test\nchip      fake\ncard      FAT16, ", 47) == 0 &&
              strstr(con, " free\nbattery   100%\n"),
          "info: '%s'", con);
    sb_says = "the southbridge did not answer";
    CHECK(run("info") == 0 && strstr(con, "battery   the southbridge did not answer\n"),
          "info, no battery: '%s'", con);
    sb_says = NULL;
    CHECK(run("info x") == 1 && strstr(con, "usage: info"), "info x");

    /* ---- leaving ---- */
    const struct {
        const char   *name;
        shell_power_t what;
        const char   *says;
    } leaves[] = {
        { "poweroff", SHELL_POWER_OFF,     "Switching off" },
        { "reboot",   SHELL_POWER_REBOOT,  "Restarting the shell" },
        { "bootsel",  SHELL_POWER_BOOTSEL, "BOOTSEL" },
    };
    for (size_t i = 0; i < 3; i++) {
        char typed[32];
        snprintf(typed, sizeof typed, "echo before %s", leaves[i].name);
        history_add(typed);
        power_calls = 0;
        CHECK(run(leaves[i].name) == 1 && power_calls == 1 && power_what == leaves[i].what,
              "%s: %u calls", leaves[i].name, power_calls);
        CHECK(strstr(con, leaves[i].says) && strstr(con, "the fake does not leave"), "%s: '%s'",
              leaves[i].name, con);
        /* The history was on the card before the port was asked. */
        CHECK(history_then && strstr(history_then, typed), "%s: history '%s'", leaves[i].name,
              history_then ? history_then : "(none)");
        /* The directory is kept for a restart, not for switching off. */
        CHECK(exists(CWD_FILE) == (leaves[i].what != SHELL_POWER_OFF), "%s: kept", leaves[i].name);
        f_unlink(CWD_FILE);
        power_calls = 0;
        char line[48];
        snprintf(line, sizeof line, "%s > /p.txt", leaves[i].name);
        CHECK(run(line) == 1 && power_calls == 0 && strstr(con, "not with a redirection") &&
                  !exists("/p.txt.new"),
              "%s: '%s'", line, con);
        snprintf(line, sizeof line, "echo | %s", leaves[i].name);
        CHECK(run(line) == 1 && power_calls == 0, "%s", line);
        snprintf(line, sizeof line, "%s now", leaves[i].name);
        CHECK(run(line) == 1 && power_calls == 0 && strstr(con, "usage: "), "%s", line);
    }
    power_says = NULL;
    CHECK(run("reboot") == 1 && strstr(con, "reboot: it did not happen"), "no reason: '%s'", con);
    f_unlink(CWD_FILE);

    /* The start: $HOME; after a restart, the directory left, once. */
    CHECK(f_mkdir("/home") == FR_OK && f_mkdir("/left") == FR_OK, "dirs");
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/home") == 0, "home: %s", sh.cwd);
    CHECK(run("cd /left") == 0, "cd");
    shell_keep_cwd(&sh);
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/left") == 0, "back: %s", sh.cwd);
    shell_start(&sh);
    CHECK(strcmp(sh.cwd, "/home") == 0, "only once: %s", sh.cwd);

    host_disk_close();
    TEST_DONE();
}
