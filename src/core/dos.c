/* dos.c — the DOS flavour (flavour.h, dos.h; design.md §4.7): its
 * builtins, the prompt, running a parsed line, and CLS, VER, MEM, SET,
 * PATH, PROMPT, ECHO and PAUSE. CHOICE is with the batch files'
 * commands, in dos_batch.c. */

#include <stdio.h>
#include <string.h>

#include "clock.h"
#include "dos.h"
#include "edit.h"
#include "files.h"
#include "flavour.h"
#include "keycodes.h"
#include "ntp.h"
#include "path.h"
#include "pipe.h"
#include "run.h"
#include "system.h"
#include "text.h"
#include "vars.h"

bool dos_echo_on = true;
const char *dos_tail = "";

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static const char *skip_blanks(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ---- for the commands ------------------------------------------------- */

void dos_error(shell_t *sh, const char *why, const char *name) {
    if (name) shell_error(sh, "%s - %s\n", why, name);
    else shell_error(sh, "%s\n", why);
}

bool dos_too_many(shell_t *sh, int argc, char **argv, int max) {
    if (argc <= max) return false;
    dos_error(sh, "Too many parameters", argv[max]);
    return true;
}

bool dos_switches(shell_t *sh, int *argc, char **argv, const char *allowed, bool within,
                  unsigned *flags) {
    *flags = 0;
    int kept = 1;
    for (int i = 1; i < *argc; i++) {
        char *a = argv[i];
        char *slash = within ? strchr(a, '/') : (a[0] == '/' ? a : NULL);
        if (!slash) {
            argv[kept++] = a;
            continue;
        }
        if (slash != a) argv[kept++] = a;   /* the name before them */
        for (const char *s = slash; *s;) {
            const char *hit = s[1] ? strchr(allowed, upper(s[1])) : NULL;
            if (!hit || (s[2] && s[2] != '/')) {
                size_t n = 1;
                while (s[n] && s[n] != '/') n++;
                shell_error(sh, "Invalid switch - %.*s\n", (int)n, s);
                return false;
            }
            *flags |= 1u << (hit - allowed);
            s += 2;
        }
        *slash = '\0';
    }
    argv[kept] = NULL;
    *argc = kept;
    return true;
}

static bool stop_key(int key) {
    return key < 0 || key == 0x03 || key == 0x1B || key == KEY_BREAK;
}

bool dos_paging(const shell_t *sh) {
    return sh->out_is_con && sh->hooks.getkey;
}

bool dos_pause(shell_t *sh) {
    if (!sh->hooks.getkey) return false;
    out_puts(&sh->con, "Press any key to continue . . .");
    int key = sh->hooks.getkey(sh->hooks.ctx);
    out_puts(&sh->con, "\n");
    if (stop_key(key)) {
        sh->stop = true;
        return false;
    }
    return true;
}

bool dos_ask(shell_t *sh, const char *question) {
    for (;;) {
        out_puts(&sh->con, question);
        int key = sh->hooks.getkey ? sh->hooks.getkey(sh->hooks.ctx) : -1;
        if (stop_key(key)) {
            out_puts(&sh->con, "\n");
            return false;
        }
        char c = upper((char)key);
        if (c == 'Y' || c == 'N') {
            out_printf(&sh->con, "%c\n", c);
            return c == 'Y';
        }
        out_puts(&sh->con, "\n");
    }
}

/* ---- commands --------------------------------------------------------- */

static int dos_cls(shell_t *sh, int argc, char **argv) {
    if (dos_too_many(sh, argc, argv, 1)) return 1;
    out_puts(&sh->out, "\033[H\033[2J");
    return 0;
}

static int dos_ver(shell_t *sh, int argc, char **argv) {
    if (dos_too_many(sh, argc, argv, 1)) return 1;
    out_printf(&sh->out, "\nPicoCalc shell, DOS flavour\nVersion %s\n\n", sh->version);
    return 0;
}

static int dos_mem(shell_t *sh, int argc, char **argv) {
    if (dos_too_many(sh, argc, argv, 1)) return 1;
    const shell_loader_t *ld = sh->loader;
    bool programs = ld && !ld->problem;
    if (!sh->mem_total && !programs) {
        shell_error(sh, "MEM: not on this machine\n");
        return 1;
    }
    if (sh->mem_total) {
        out_printf(&sh->out, "\n%10lu bytes total memory\n", sh->mem_total);
        out_printf(&sh->out, "%10lu bytes used by the shell\n", sh->mem_used);
        out_printf(&sh->out, "%10lu bytes free\n", sh->mem_total - sh->mem_used);
        out_printf(&sh->out, "%10lu bytes PSRAM\n", sh->mem_psram);
    }
    if (programs)
        out_printf(&sh->out, "%s%10lu bytes largest program\n", sh->mem_total ? "" : "\n",
                   (unsigned long)(ld->flash.end - ld->flash.base));
    out_puts(&sh->out, "\n");
    return 0;
}

/* A variable's name from s[0, n), upper case, into buf: false if it is
 * not a name. */
static bool name_of(char *buf, const char *s, size_t n) {
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n > SHELL_LINE_MAX || !vars_is_name(s, n)) return false;
    for (size_t i = 0; i < n; i++) buf[i] = upper(s[i]);
    buf[n] = '\0';
    return true;
}

static int set_var(shell_t *sh, const char *name, const char *value) {
    if (!*value) {
        vars_unset(name, strlen(name));
        return 0;
    }
    if (vars_set(name, strlen(name), value)) return 0;
    shell_error(sh, "Out of environment space\n");
    return 1;
}

/* SET lists; SET NAME=value sets, the value as typed; SET NAME= clears. */
static int dos_set(shell_t *sh, int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *t = skip_blanks(dos_tail);
    if (!*t) {
        const char *v;
        for (unsigned i = 0; (v = vars_at(i)) != NULL; i++) out_printf(&sh->out, "%s\n", v);
        return 0;
    }
    const char *eq = strchr(t, '=');
    char name[SHELL_LINE_MAX + 1];
    if (!eq || !name_of(name, t, (size_t)(eq - t))) {
        shell_error(sh, "Syntax error\n");
        return 1;
    }
    return set_var(sh, name, eq + 1);
}

/* PATH shows it; PATH dir;dir sets it, in capitals as DOS kept it;
 * PATH ; clears it. */
static int dos_path(shell_t *sh, int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *t = skip_blanks(dos_tail);
    if (*t == '=') t = skip_blanks(t + 1);
    if (!*t) {
        const char *v = vars_get("PATH", 4);
        if (v && *v) out_printf(&sh->out, "PATH=%s\n", v);
        else out_puts(&sh->out, "No Path\n");
        return 0;
    }
    if (strcmp(t, ";") == 0) {
        vars_unset("PATH", 4);
        return 0;
    }
    char value[SHELL_LINE_MAX + 1];
    size_t n = 0;
    for (; t[n] && n < SHELL_LINE_MAX; n++) value[n] = upper(t[n]);
    value[n] = '\0';
    return set_var(sh, "PATH", value);
}

#define DEFAULT_PROMPT "$P$G"

/* PROMPT text sets it; PROMPT alone puts back $P$G. */
static int dos_prompt(shell_t *sh, int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *t = skip_blanks(dos_tail);
    if (*t == '=') t++;
    return set_var(sh, "PROMPT", *t ? t : DEFAULT_PROMPT);
}

/* ECHO message prints it as typed; ECHO. a blank line; ECHO ON and
 * ECHO OFF show and hide the prompt; ECHO alone says which. */
static int dos_echo(shell_t *sh, int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (*dos_tail == '.') {
        out_printf(&sh->out, "%s\n", dos_tail + 1);
        return 0;
    }
    const char *t = skip_blanks(dos_tail);
    if (!*t) {
        out_printf(&sh->out, "ECHO is %s\n", dos_echo_on ? "on" : "off");
    } else if (path_same(t, "ON") || path_same(t, "OFF")) {
        dos_echo_on = path_same(t, "ON");
    } else {
        out_printf(&sh->out, "%s\n", t);
    }
    return 0;
}

static int dos_pause_cmd(shell_t *sh, int argc, char **argv) {
    if (dos_too_many(sh, argc, argv, 1)) return 1;
    dos_pause(sh);
    return 0;
}

/* The PicoCalc's own commands take their words in small letters. */
static void lower_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        for (char *s = argv[i]; *s; s++)
            if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static int dos_phosphor(shell_t *sh, int argc, char **argv) {
    lower_args(argc, argv);
    return cmd_phosphor(sh, argc, argv);
}

static int dos_backlight(shell_t *sh, int argc, char **argv) {
    lower_args(argc, argv);
    return cmd_backlight(sh, argc, argv);
}

const shell_builtin_t flavour_builtins[] = {
    { "ATTRIB",    dos_attrib,    "ATTRIB [+R|-R] [+A|-A] [+S|-S] [+H|-H] [path]", "Displays or changes attributes" },
    { "BACKLIGHT", dos_backlight, "BACKLIGHT [lcd|kbd] [0-255]", "Displays or sets the backlights" },
    { "BATTERY",   cmd_battery,   "BATTERY", "Displays the battery's charge" },
#if SHELL_NTP
    { "BT",        cmd_bt,        "BT", "Shows the Bluetooth keyboard's state" },
#endif
    { "BOOTSEL",   cmd_bootsel,   "BOOTSEL", "Reboot into bootloader mode (BOOTSEL)" },
    { "CALL",      dos_call,      "CALL file [parameters]", "Starts a batch file and returns" },
    { "CD",        dos_cd,        "CD [path]", "Displays or changes the directory" },
    { "CHDIR",     dos_cd,        "CHDIR [path]", "Displays or changes the directory" },
    { "CHOICE",    dos_choice,    "CHOICE [/C[:]keys] [/N] [/S] [/T[:]c,nn] [text]", "Waits for one of a set of keys" },
    { "CLS",       dos_cls,       "CLS", "Clears the screen" },
    { "COPY",      dos_copy,      "COPY [/V] [/B] source [destination]", "Copies files; CON, NUL are devices" },
    { "DATE",      dos_date,      "DATE [/T | date]", "Displays or sets the date" },
    { "DEL",       dos_del,       "DEL [/P] path", "Removes one or more files" },
    { "DELTREE",   dos_deltree,   "DELTREE [/Y] path...", "Deletes a directory and its contents" },
    { "DIR",       dos_dir,       "DIR [path] [/W] [/P] [/S] [/A] [/B]", "Displays the contents of a directory" },
    { "ECHO",      dos_echo,      "ECHO [ON|OFF|message]", "Displays a message, or echo status" },
    { "EDIT",      cmd_edit,      "EDIT [file]", "Edits a text file" },
    { "ERASE",     dos_del,       "ERASE [/P] path", "Removes one or more files" },
    { "EXIT",      cmd_menu,      "EXIT", "Exit to UF2 Loader's menu" },
    { "FIND",      dos_find,      "FIND [/V] [/C] [/N] [/I] \"text\" [file...]", "Print the lines that contain text" },
    { "FOR",       dos_control,   "FOR %x IN (set) DO command", "Runs a command for each file in a set" },
    { "GOTO",      dos_goto,      "GOTO label", "Jumps to a label in a batch file" },
    { "HDMI",      cmd_hdmi,      "HDMI", "Resends the screen to HDMI" },
    { "HELP",      cmd_help,      "HELP [command]", "Provides help information" },
    { "IF",        dos_control,   "IF [NOT] EXIST f|ERRORLEVEL n|a==b command", "Performs conditional processing" },
    { "INFO",      cmd_info,      "INFO", "Diagnostic information" },
    { "MD",        dos_md,        "MD path", "Creates a directory" },
    { "MEM",       dos_mem,       "MEM", "Displays the free and used memory" },
    { "MKDIR",     dos_md,        "MKDIR path", "Creates a directory" },
    { "MORE",      dos_more,      "MORE [file] (or < file, or | MORE)", "Displays text one screen at a time" },
    { "MOVE",      dos_move,      "MOVE source... destination", "Move files or rename a directory" },
#if SHELL_NTP
    { "NTP",       cmd_ntp,       "NTP [/F]", "Sets the clock from the network, once" },
#endif
    { "PATH",      dos_path,      "PATH [dir[;dir...]] (PATH ; clears)", "Displays or sets the search path" },
    { "PAUSE",     dos_pause_cmd, "PAUSE", "Waits for a key" },
    { "PHOSPHOR",  dos_phosphor,  "PHOSPHOR [white|green|amber|default]", "Sets the colour of the text" },
    { "POWEROFF",  cmd_poweroff,  "POWEROFF", "Switches the PicoCalc off" },
    { "PROMPT",    dos_prompt,    "PROMPT [text] ($P path, $G >, $_ line)", "Changes the prompt" },
    { "RD",        dos_rd,        "RD path", "Removes an empty directory" },
    { "REBOOT",    cmd_reboot,    "REBOOT", "Restarts the shell" },
    { "REM",       dos_rem,       "REM [comment]", "Adds a comment in a batch file" },
    { "REN",       dos_ren,       "REN path name", "Renames a file or files" },
    { "RENAME",    dos_ren,       "RENAME path name", "Renames a file or files" },
    { "RMDIR",     dos_rd,        "RMDIR path", "Removes an empty directory" },
    { "SET",       dos_set,       "SET [name=[value]]", "Displays, sets or clears variables" },
    { "SHIFT",     dos_shift,     "SHIFT", "Change the position of parameters" },
    { "TIME",      dos_time,      "TIME [/T | time]", "Displays or sets the time" },
    { "TYPE",      dos_type,      "TYPE file", "Displays the contents of a text file" },
    { "USBDISK",   cmd_usbdisk,   "USBDISK", "Exposes the SD Card to a computer" },
    { "VER",       dos_ver,       "VER", "Displays the shell's version" },
    { "VOL",       dos_vol,       "VOL", "Displays information about the SD Card" },
#if SHELL_NTP
    { "WEB",       cmd_web,       "WEB", "Manages the SD Card from a browser" },
#endif
    { "XCOPY",     dos_xcopy,     "XCOPY source [destination] [/S]", "Copies files and directories" },
};

const unsigned flavour_nbuiltins = sizeof flavour_builtins / sizeof flavour_builtins[0];

/* ---- running a line --------------------------------------------------- */

/* Each level's line, parsed. */
static dos_parse_t s_parse[SHELL_DEPTH];

static int stage(shell_t *sh, int k, pipe_t *from, pipe_t *to, void *ctx) {
    const dos_cmd_t *c = &((const dos_parse_t *)ctx)->cmds[k];
    const char *name = c->argv[0];
    /* /GAMES/ORIC has no name before its switch. */
    if (!name[0]) {
        flavour_not_found(sh, c->tail);
        return 127;
    }
    /* C: is the drive there is; any other is not. */
    if (name[1] == ':' && !name[2]) {
        if (dos_other_drive(name)) {
            shell_error(sh, "Invalid drive specification\n");
            return 1;
        }
        /* C:/GAMES/ORIC is not a path. */
        if (c->tail[0] == '/') {
            flavour_not_found(sh, c->tail);
            return 127;
        }
        return 0;
    }
    dos_tail = c->tail;
    /* A batch file, unless a program of the same name is found first. */
    const char *bat = shell_is_builtin(name) ? NULL : dos_batch_find(sh, name);
    if (bat) {
        if (c->in || c->out || from || to) {
            shell_error(sh, "Cannot redirect a batch file; CALL it\n");
            return 1;
        }
        return dos_batch_run(sh, bat, name, c->tail, false);
    }
    /* > NUL throws the output away, as DOS's NUL device did. */
    if (c->out && path_same(c->out, "NUL"))
        return shell_command_null(sh, c->argc, c->argv, c->in, from, to);
    return shell_command(sh, c->argc, c->argv, c->in, c->out, c->append, from, to);
}

int dos_commands(shell_t *sh, const char *text) {
    dos_parse_t *p = &s_parse[sh->depth];
    if (!dos_parse(p, text)) {
        shell_error(sh, "%s\n", p->error);
        return sh->status = 2;
    }
    if (p->ncmds > 0) sh->status = shell_pipeline(sh, p->ncmds, stage, p);
    return sh->status;
}

int flavour_run_line(shell_t *sh, const char *line) {
    return dos_line(sh, line, false);
}

/* ---- the rest of flavour.h -------------------------------------------- */

void flavour_init(shell_t *sh) {
    (void)sh;
    char path[SHELL_PATH_MAX + 3];
    dos_show(path, sizeof path, SHELL_PROGRAM_DIR);
    for (char *s = path; *s; s++) *s = upper(*s);
    vars_set("PATH", 4, path);
    vars_set("PROMPT", 6, DEFAULT_PROMPT);
    dos_echo_on = true;
}

void flavour_start(shell_t *sh) {
    /* $P shows each name as the card spells it: C:\HOME, not C:\home. */
    files_chdir(sh, sh->cwd);
    out_printf(&sh->con, "\033[1mPicoCalc shell\033[0m %s (DOS)\n\n", sh->version);
    const char *note = clock_note();
    if (note) shell_error(sh, "Warning: %s\n", note);
    dos_config(sh);
    dos_autoexec(sh);
}

/* PROMPT's codes: $P the drive and directory, $G >, $L <, $B |, $Q =,
 * $$ $, $N the drive, $_ a new line, $E Esc, $H a backspace, $V the
 * version, $D the date and $T the time, as DATE and TIME show them, or
 * nothing with the clock not set. With no
 * PROMPT it is $N$G, C>, as in DOS; with ECHO OFF there is none. */
void flavour_prompt(shell_t *sh, char *buf, size_t size) {
    size_t n = 0;
    buf[0] = '\0';
    if (!dos_echo_on) return;
    const char *p = vars_get("PROMPT", 6);
    if (!p) p = "$N$G";
    char piece[SHELL_PATH_MAX + 48];
    for (; *p; p++) {
        piece[0] = *p;
        piece[1] = '\0';
        if (*p == '$' && p[1]) {
            p++;
            switch (upper(*p)) {
            case 'P': dos_show(piece, sizeof piece, sh->cwd); break;
            case 'G': strcpy(piece, ">"); break;
            case 'L': strcpy(piece, "<"); break;
            case 'B': strcpy(piece, "|"); break;
            case 'Q': strcpy(piece, "="); break;
            case '$': strcpy(piece, "$"); break;
            case 'N': strcpy(piece, "C"); break;
            case '_': strcpy(piece, "\n"); break;
            case 'E': strcpy(piece, "\033"); break;
            case 'H': strcpy(piece, "\b"); break;
            case 'V': snprintf(piece, sizeof piece, "PicoCalc shell %s", sh->version); break;
            case 'D':
            case 'T': {
                clock_tm_t tm;
                char when[24];
                piece[0] = '\0';
                if (!clock_now(&tm)) break;
                if (upper(*p) == 'T') {
                    dos_format_time(when, sizeof when, &tm, true);
                    strcpy(piece, when);
                } else {
                    dos_format_date(when, sizeof when, &tm, true);
                    snprintf(piece, sizeof piece, "%s %s", clock_wday_name(tm.wday), when);
                }
                break;
            }
            default:  piece[0] = '\0'; break;
            }
        }
        size_t len = strlen(piece);
        if (n + len >= size) break;
        memcpy(buf + n, piece, len + 1);
        n += len;
    }
}

bool flavour_resolve(char out[SHELL_PATH_MAX + 1], const char *cwd, const char *arg) {
    return dos_resolve(out, cwd, arg);
}

bool flavour_is_path(const char *name) {
    return strpbrk(name, "/\\:") != NULL;
}

void flavour_file_error(shell_t *sh, const char *cmd, const char *name, const char *why) {
    (void)cmd;
    char shown[SHELL_PATH_MAX + 3];
    if (name[0] == '/') {
        dos_show(shown, sizeof shown, name);
        name = shown;
    }
    dos_error(sh, dos_say(why), name);
}

void flavour_not_found(shell_t *sh, const char *name) {
    (void)name;
    shell_error(sh, "Bad command or file name\n");
}
