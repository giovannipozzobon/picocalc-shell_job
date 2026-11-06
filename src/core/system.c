/* system.c — the PicoCalc's own commands (system.h). */

#include "system.h"

#include <ctype.h>
#include <string.h>

#include "clock.h"
#include "files.h"
#include "flavour.h"
#include "ntp.h"
#include "run.h"
#include "term.h"

static int usage(shell_t *sh, const char *text) {
    shell_error(sh, "usage: %s\n", text);
    return 1;
}

static int not_here(shell_t *sh, const char *cmd) {
    shell_error(sh, "%s: not on this machine\n", cmd);
    return 1;
}

/* ---- help (man) ------------------------------------------------------ */

int cmd_help(shell_t *sh, int argc, char **argv) {
    if (argc > 2) return usage(sh, SHELL_DOS ? "help [command]" : "man [command]");
    const char *name;
    if (argc == 2) {
        int i = shell_builtin_index(argv[1]);
        if (i >= 0) {
            out_printf(&sh->out, "usage: %s\n%s\n", shell_builtin_usage((unsigned)i),
                       shell_builtin_about((unsigned)i));
            return 0;
        }
        shell_error(sh, "%s: %s: not a builtin; a program, if one has that name\n",
                    SHELL_DOS ? "help" : "man", argv[1]);
        return 1;
    }
    /* The names in columns as wide as the longest, to the screen's width. */
    size_t width = 0;
    for (unsigned i = 0; (name = shell_builtin_at(i)) != NULL; i++)
        if (strlen(name) > width) width = strlen(name);
    width += 2;
    unsigned per_row = (unsigned)(TERM_COLS / width);
    if (per_row == 0) per_row = 1;
    unsigned col = 0;
    for (unsigned i = 0; (name = shell_builtin_at(i)) != NULL; i++) {
        bool last = ++col == per_row || shell_builtin_at(i + 1) == NULL;
        if (last) {
            out_printf(&sh->out, "%s\n", name);
            col = 0;
        } else {
            out_printf(&sh->out, "%-*s", (int)width, name);
        }
    }
    out_puts(&sh->out, SHELL_DOS ? "\nHELP name for one. Any other name runs\n"
                                   "a program here or on PATH: NAME.UF2.\n"
                                 : "\n`man name` for one. Any other name runs\n"
                                   "a program from $PATH, as `name.uf2`.\n");
    return 0;
}

/* ---- the screen ------------------------------------------------------- */

int cmd_clear(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc != 1) return usage(sh, "clear");
    out_puts(&sh->out, "\033[H\033[2J");
    return 0;
}

static const struct {
    const char *name;
    uint16_t    rgb;
} k_phosphors[] = {
    { "default", TERM_FOREGROUND },
    { "white",   TERM_WHITE_PHOSPHOR },
    { "green",   TERM_GREEN_PHOSPHOR },
    { "amber",   TERM_AMBER_PHOSPHOR },
};

#define NPHOSPHORS (sizeof k_phosphors / sizeof k_phosphors[0])

int cmd_phosphor(shell_t *sh, int argc, char **argv) {
    static const char use[] = "phosphor [white|green|amber|default]";
    if (argc > 2) return usage(sh, use);
    if (argc == 1) {
        out_printf(&sh->out, "%s\n", k_phosphors[sh->phosphor].name);
        return 0;
    }
    for (unsigned i = 0; i < NPHOSPHORS; i++) {
        if (strcmp(argv[1], k_phosphors[i].name) != 0) continue;
        if (!sh->sys.phosphor) return not_here(sh, "phosphor");
        sh->sys.phosphor(sh->sys.ctx, k_phosphors[i].rgb);
        sh->phosphor = (uint8_t)i;
        /* The new ink from here on, not only after the next reset. */
        out_puts(&sh->con, "\033[39m");
        return 0;
    }
    shell_error(sh, "phosphor: %s: white, green, amber or default\n", argv[1]);
    return 1;
}

/* ---- the southbridge -------------------------------------------------- */

int cmd_battery(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc != 1) return usage(sh, "battery");
    if (!sh->sys.battery) return not_here(sh, "battery");
    unsigned pct;
    bool charging;
    const char *why = sh->sys.battery(sh->sys.ctx, &pct, &charging);
    if (why) {
        shell_error(sh, "battery: %s\n", why);
        return 1;
    }
    out_printf(&sh->out, "%u%%%s\n", pct, charging ? ", charging" : "");
    return 0;
}

static const char *const k_lights[] = { "lcd", "kbd" };

/* One backlight: set it unless `level` is negative, then show it. */
static int light(shell_t *sh, shell_light_t which, int level) {
    unsigned now;
    const char *why = sh->sys.backlight(sh->sys.ctx, which, level, &now);
    if (why) {
        shell_error(sh, "backlight: %s: %s\n", k_lights[which], why);
        return 1;
    }
    out_printf(&sh->out, "%s %u\n", k_lights[which], now);
    return 0;
}

int cmd_backlight(shell_t *sh, int argc, char **argv) {
    static const char use[] = "backlight [lcd|kbd] [0-255]";
    if (argc > 3) return usage(sh, use);
    int i = 1;
    int which = -1;
    for (unsigned k = 0; k < 2 && i < argc; k++) {
        if (strcmp(argv[i], k_lights[k]) == 0) {
            which = (int)k;
            i++;
            break;
        }
    }
    int level = -1;
    if (i < argc) {
        const char *p = argv[i++];
        level = 0;
        if (!*p) return usage(sh, use);
        for (; *p; p++) {
            if (*p < '0' || *p > '9' || (level = level * 10 + (*p - '0')) > 255) {
                shell_error(sh, "backlight: %s: a level is 0 to 255\n", argv[i - 1]);
                return 1;
            }
        }
        if (which < 0) which = SHELL_LIGHT_LCD;
    }
    if (i != argc) return usage(sh, use);
    if (!sh->sys.backlight) return not_here(sh, "backlight");
    if (which >= 0) return light(sh, (shell_light_t)which, level);
    return light(sh, SHELL_LIGHT_LCD, -1) | light(sh, SHELL_LIGHT_KBD, -1);
}

/* ---- info ------------------------------------------------------------- */

int cmd_info(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc != 1) return usage(sh, "info");
    const out_t *o = &sh->out;
    out_printf(o, "shell     %s\n", sh->version);
    if (sh->sys.info) sh->sys.info(sh->sys.ctx, o);
    if (sh->sys.bluetooth) out_printf(o, "bluetooth %.29s\n", sh->sys.bluetooth(sh->sys.ctx));
    char card[48];
    FRESULT r = files_card(card, sizeof card);
    out_printf(o, "card      %s\n", r == FR_OK ? card : files_why(r));
    if (sh->sys.battery) {
        unsigned pct;
        bool charging;
        const char *why = sh->sys.battery(sh->sys.ctx, &pct, &charging);
        if (why) out_printf(o, "battery   %s\n", why);
        else out_printf(o, "battery   %u%%%s\n", pct, charging ? ", charging" : "");
    }
    return 0;
}

/* ---- leaving ---------------------------------------------------------- */

/* Save the history, say what is happening, and go; back only if the
 * port could not. */
static int leave(shell_t *sh, int argc, const char *name, shell_power_t what, const char *msg) {
    if (argc != 1) return usage(sh, name);
    if (!sh->sys.power) return not_here(sh, name);
    if (what != SHELL_POWER_OFF) shell_keep_cwd(sh);
    shell_leaving(sh);
    out_puts(&sh->con, msg);
    const char *why = sh->sys.power(sh->sys.ctx, what);
    shell_error(sh, "%s: %s\n", name, why ? why : "it did not happen");
    return 1;
}

int cmd_poweroff(shell_t *sh, int argc, char **argv) {
    (void)argv;
    return leave(sh, argc, "poweroff", SHELL_POWER_OFF, "Switching off in a few seconds.\n");
}

int cmd_reboot(shell_t *sh, int argc, char **argv) {
    (void)argv;
    return leave(sh, argc, "reboot", SHELL_POWER_REBOOT, "Restarting the shell.\n");
}

int cmd_bootsel(shell_t *sh, int argc, char **argv) {
    (void)argv;
    return leave(sh, argc, "bootsel", SHELL_POWER_BOOTSEL,
                 "BOOTSEL: the RP2350 is now a USB drive.\n"
                 "Switch off and on to come back.\n");
}

bool system_leaves(shell_cmd_fn fn) {
    return fn == cmd_poweroff || fn == cmd_reboot || fn == cmd_bootsel ||
           (SHELL_NTP && (fn == cmd_ntp || fn == cmd_web));
}

#if SHELL_NTP
/* ---- ntp -------------------------------------------------------------- */

/* ntp.uf2, once per power-on (design.md §3.8). A startup file runs each
 * time the shell comes back, from ntp.uf2 as from any program, so once
 * NTP was tried `ntp` only says why it did not set the clock, with status
 * 1; when it did, it says nothing (the owner, 2026-10-23). -f (/F) tries
 * again. NTP_UNFINISHED is kept before ntp.uf2 starts, so one
 * that never reports cannot make a loop either. */
int cmd_ntp(shell_t *sh, int argc, char **argv) {
    const char *name = SHELL_DOS ? "NTP" : "ntp";
    bool force = argc == 2 && (SHELL_DOS ? argv[1][0] == '/' && toupper((unsigned char)argv[1][1]) == 'F' &&
                                               !argv[1][2]
                                         : strcmp(argv[1], "-f") == 0);
    if (argc != 1 && !(argc == 2 && force)) return usage(sh, SHELL_DOS ? "NTP [/F]" : "ntp [-f]");
    if (!clock_has_ntp()) return not_here(sh, name);
    int was = clock_ntp();
    if (was >= 0 && !force) {
        if (was == NTP_SET) return 0;
        shell_error(sh, "%s: %s\n", name, ntp_result_str((ntp_result_t)was));
        return 1;
    }
    clock_ntp_keep(NTP_UNFINISHED);
    /* ntp.uf2, found as a program is: in $PATH, /pico2-apps by default. */
    int status = run_ram_program(sh, name);
    /* Back without rebooting: ntp.uf2 did not run, so NTP was not tried. */
    if (status != 0) clock_ntp_keep(was);
    return status;
}

/* ---- web -------------------------------------------------------------- */

/* web.uf2, the card on the network (design.md §3.9): the W build's, as
 * `ntp` is, and unlike it every time it is typed: a person asked for it,
 * and it ends when the person says so, with Esc. The W build is the one
 * whose port has NTP. */
int cmd_web(shell_t *sh, int argc, char **argv) {
    (void)argv;
    const char *name = SHELL_DOS ? "WEB" : "web";
    if (argc != 1) return usage(sh, name);
    if (!clock_has_ntp()) return not_here(sh, name);
    return run_ram_program(sh, name);
}
#endif

/* ---- bt --------------------------------------------------------------- */

/* What the Bluetooth keyboard is doing: paired and connected, waiting
 * for it, or why the radio is not on (design.md §3.10). The pairing is
 * btpair's, and the radio starts with the shell, so there is nothing to
 * switch here. */
int cmd_bt(shell_t *sh, int argc, char **argv) {
    (void)argv;
    const char *name = SHELL_DOS ? "BT" : "bt";
    if (argc != 1) return usage(sh, name);
    if (!sh->sys.bluetooth) return not_here(sh, name);
    out_printf(&sh->out, "%s\n", sh->sys.bluetooth(sh->sys.ctx));
    return 0;
}

/* ---- hdmi ------------------------------------------------------------- */

/* The FPGA that shows the screen on HDMI is told the panel's state again
 * and the whole screen is redrawn, for a link that was not there when
 * the shell drew it: the FPGA powered after the PicoCalc, or a cable
 * moved. The port does it; this says what it found (design.md §3.11). */
int cmd_hdmi(shell_t *sh, int argc, char **argv) {
    (void)argv;
    const char *name = SHELL_DOS ? "HDMI" : "hdmi";
    if (argc != 1) return usage(sh, name);
    if (!sh->sys.hdmi) return not_here(sh, name);
    out_printf(&sh->out, "%s\n", sh->sys.hdmi(sh->sys.ctx));
    return 0;
}

/* ---- date ------------------------------------------------------------- */

static void date_show(shell_t *sh, const clock_tm_t *tm) {
    out_printf(&sh->out, "%s %s %2d %02d:%02d:%02d %s %d\n", clock_wday_name(tm->wday),
               clock_mon_name(tm->mon), tm->mday, tm->hour, tm->min, tm->sec, tm->zone, tm->year);
}

/* Two digits from s, into *v. */
static bool two(const char *s, int *v) {
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
    *v = (s[0] - '0') * 10 + (s[1] - '0');
    return true;
}

/* mmddHHMM[[cc]yy], POSIX's form, in local time or UTC. Without a year,
 * this year, or the fixed date's with the clock not set. Seconds are 0.
 * The years are FAT's and DOS's, 1980-2099; yy of 69-99 is the 1900s,
 * as POSIX has it. */
static int date_set(shell_t *sh, const char *arg, bool utc) {
    clock_tm_t tm;
    int64_t t;
    if (clock_utc(&t)) {
        if (utc) clock_gmt(t, &tm);
        else clock_local(t, &tm);
    } else {
        clock_now(&tm);
    }
    size_t n = strlen(arg);
    int cc = 0, yy = 0;
    bool ok = (n == 8 || n == 10 || n == 12) && two(arg, &tm.mon) && two(arg + 2, &tm.mday) &&
              two(arg + 4, &tm.hour) && two(arg + 6, &tm.min);
    if (ok && n == 10) {
        ok = two(arg + 8, &yy);
        tm.year = yy + (yy >= 69 ? 1900 : 2000);
    } else if (ok && n == 12) {
        ok = two(arg + 8, &cc) && two(arg + 10, &yy);
        tm.year = cc * 100 + yy;
    }
    tm.sec = 0;
    if (!ok || tm.year < 1980 || tm.year > 2099 || !clock_valid(tm.year, tm.mon, tm.mday) ||
        tm.hour > 23 || tm.min > 59) {
        shell_error(sh, "date: %s: not a date: mmddHHMM[[cc]yy]\n", arg);
        return 1;
    }
    t = utc ? clock_seconds(&tm) : clock_from_local(&tm);
    const char *why = clock_set(t);
    if (why) {
        shell_error(sh, "date: %s\n", why);
        return 1;
    }
    if (utc) clock_gmt(t, &tm);
    else clock_local(t, &tm);
    date_show(sh, &tm);
    return 0;
}

int cmd_date(shell_t *sh, int argc, char **argv) {
    static const char use[] = "date [-u] [mmddHHMM[[cc]yy]]";
    int i = 1;
    bool utc = i < argc && strcmp(argv[i], "-u") == 0;
    if (utc) i++;
    if (argc - i > 1 || (i < argc && argv[i][0] == '-')) return usage(sh, use);
    if (i < argc) return date_set(sh, argv[i], utc);
    int64_t t;
    if (!clock_utc(&t)) {
        shell_error(sh, "date: clock not set\n");
        return 1;
    }
    clock_tm_t tm;
    if (utc) clock_gmt(t, &tm);
    else clock_local(t, &tm);
    date_show(sh, &tm);
    return 0;
}
