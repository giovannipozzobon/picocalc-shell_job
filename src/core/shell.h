/* shell.h — the command line (design.md §4.4, §4.5, §4.7).
 *
 * What both flavours share: the prompt and its line editor (line.h),
 * running a command with its redirections, pipes (pipe.h), and the
 * builtins' table, which is the flavour's (flavour.h). The UNIX
 * flavour's parser, expansion and scripts are in unix.c, the DOS
 * flavour's in dos.c. Any command that is not a builtin is a program
 * (run.h). */
#ifndef SHELL_SHELL_H
#define SHELL_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "ff.h"
#include "input.h"
#include "out.h"

/* What the shell needs from the port beyond its output. */
typedef struct {
    uint64_t (*now_us)(void *ctx);
    /* Break, Esc or Ctrl-C pressed since the last call; for long commands
     * to poll. The port keeps the keyboard polled while they run. */
    bool (*interrupted)(void *ctx);
    /* Wait for the next input byte (keys.h), for a command that reads the
     * keyboard itself, such as vi. -1 when there will be no more: the end
     * of a script or of the host's input. NULL: no keyboard. */
    int (*getkey)(void *ctx);
    /* The next input byte if one is waiting, otherwise -1 at once, for a
     * command that waits for a key with a time limit, such as DOS's
     * CHOICE /T. NULL: none, and such a command waits without one, as
     * it does when now_us is NULL. */
    int (*pollkey)(void *ctx);
    /* Offer the card over USB until the computer ejects it or a key
     * stops it (design.md §4.2): NULL, or why not. FatFs's volume is
     * unmounted around the call. NULL: no USB here. */
    const char *(*usbdisk)(void *ctx);
    void *ctx;
} shell_hooks_t;

/* The PicoCalc itself, for M8's commands (system.h): its southbridge,
 * the terminal's ink, and leaving the shell. Any hook may be NULL, and
 * the command that needs it says it cannot be done here. */
typedef enum { SHELL_LIGHT_LCD, SHELL_LIGHT_KBD } shell_light_t;
typedef enum { SHELL_POWER_OFF, SHELL_POWER_REBOOT, SHELL_POWER_BOOTSEL } shell_power_t;

typedef struct {
    /* The charge in percent, and whether the charger says it is
     * charging: NULL, or why it cannot be read. */
    const char *(*battery)(void *ctx, unsigned *percent, bool *charging);
    /* A backlight's level, 0-255: set it to `level` unless that is
     * negative, then read it into *now. NULL, or why not. */
    const char *(*backlight)(void *ctx, shell_light_t which, int level, unsigned *now);
    /* The terminal's default ink, RGB565 (term_set_phosphor). */
    void (*phosphor)(void *ctx, uint16_t rgb565);
    /* The port's lines for `info`, "name     value\n" each, to `o`. */
    void (*info)(void *ctx, const out_t *o);
    /* Switch off, reboot into the shell, or reboot into BOOTSEL. Does
     * not return if it worked; otherwise why not. */
    const char *(*power)(void *ctx, shell_power_t what);
    void *ctx;
    /* A line saying what Bluetooth is doing, for `bt` and `info`
     * (design.md §3.10); NULL where there is none: the Pico 2 build. */
    const char *(*bluetooth)(void *ctx);
    /* Send the HDMI mirror the screen's state and picture again, and say
     * in a line what the link is (design.md §3.11); NULL where the build
     * has no mirror. */
    const char *(*hdmi)(void *ctx);
} shell_system_t;

/* The port's clock (clock.h; design.md §3.8): UTC in milliseconds since
 * 1970. get is false while the clock is not set; set gives NULL, or why
 * not. Either may be NULL: no clock, or one that cannot be set. note,
 * if not NULL, is said once when the shell starts: "the DS3231 lost its
 * time". ntp is what NTP did since power-on (ntp.h's ntp_result_t), or
 * -1 if it was not tried, and ntp_keep keeps a result, -1 to forget
 * it; both NULL where there is no NTP, as in the Pico 2 build. offset,
 * if not NULL, keeps the local offset from UTC in minutes for the next
 * program, before the shell is left. */
typedef struct {
    bool (*get)(void *ctx, int64_t *utc_ms);
    const char *(*set)(void *ctx, int64_t utc_ms);
    void *ctx;
    const char *note;
    int (*ntp)(void *ctx);
    void (*ntp_keep)(void *ctx, int result);
    void (*offset)(void *ctx, int off_min);
} shell_clock_t;

/* The line being edited at the prompt (line.c). */
typedef struct {
    char   text[SHELL_LINE_MAX + 1];
    size_t len, pos;            /* its length, and the cursor in it */
    size_t shown;               /* characters drawn after the prompt */
    size_t prompt;              /* the prompt's width */
    size_t cur;                 /* the terminal's cursor, from the prompt's start */
    bool   tabbed;              /* the last key was a Tab that added nothing */
    int    hist;                /* the history entry shown, -1: the line typed */
    char   typed[SHELL_LINE_MAX + 1];   /* the line typed, while history is shown */
} line_t;

typedef struct shell {
    out_t         con;              /* the terminal: prompt, echo */
    out_t         out;              /* a command's output: con, a file or a pipe */
    bool          out_is_con;
    out_t         err;              /* shell_error's: con, unless UNIX's 2> says otherwise */
    bool          err_is_con;
    bool          err_to_out;       /* the next shell_command: `> file 2>&1` */
    input_t       in;               /* a command's input: a file, a pipe, or none */
    bool          stop;             /* interrupted: the rest of the line is dropped */
    int           wheel_left;       /* Up (+) or Down (-) keys left of a wheel notch (shell_getkey) */
    int           status;           /* the last command's, kept across lines */
    unsigned      depth;            /* 0 at the prompt, one more in each `sh` */
    bool          at_bol;           /* the terminal's cursor is at a line's start */
    out_t         term;             /* the port's sink, under con */
    shell_hooks_t hooks;
    shell_system_t sys;
    const struct shell_loader *loader;   /* run.h; NULL: programs cannot run */
    const char   *version;
    char          cwd[SHELL_PATH_MAX + 1];   /* absolute and normal (path.h) */
    uint8_t       phosphor;         /* system.h: the ink chosen, 0 for the default */
    unsigned long mem_used, mem_total;   /* SRAM, for DOS's MEM; 0: not known */
    unsigned long mem_psram;             /* PSRAM, for DOS's MEM; 0 is shown */
    line_t        line;
} shell_t;

/* A builtin: its exit status, 0 for success. */
typedef int (*shell_cmd_fn)(shell_t *sh, int argc, char **argv);

void shell_init(shell_t *sh, const out_t *out, const shell_hooks_t *hooks, const char *version);

/* How programs are run (run.h): the port's link to UF2 Loader. */
void shell_set_loader(shell_t *sh, const struct shell_loader *loader);

/* The port's link to the PicoCalc's southbridge, ink and power. */
void shell_set_system(shell_t *sh, const shell_system_t *sys);

/* The port's clock; without one, the clock is never set. */
void shell_set_clock(shell_t *sh, const shell_clock_t *clock);

/* The SRAM the shell uses, the SRAM there is and the PSRAM there is (0
 * for none), in bytes, for MEM. */
void shell_set_memory(shell_t *sh, unsigned long used, unsigned long total, unsigned long psram);

/* Mount the card, reset the terminal, read the history, print the
 * banner, run the flavour's startup script (/etc/profile), and show the
 * first prompt. The starting directory is $HOME, or /home, if the card
 * has it. */
void shell_start(shell_t *sh);

/* One input byte (keys.h): a character, or a key code of 0x80 and above. */
void shell_input(shell_t *sh, int key);

/* Run one line as if typed, without echo or prompt; the last command's
 * status. For tests, and for scripts at the level below. */
int shell_run(shell_t *sh, const char *line);

/* Before the shell is left for a program, the menu or USB: give the
 * port the clock's local offset, and save the history. */
void shell_leaving(shell_t *sh);

/* The shell is restarting, not being switched off: its next start comes
 * back to the current directory. */
void shell_keep_cwd(shell_t *sh);

/* Output thrown away: /dev/null's and DOS's NUL. */
extern const out_t shell_null_out;

/* Whether `name` is a program or a builtin that leaves the shell, so
 * can leave no redirection's name.new behind. */
bool shell_leaves(const char *name);

/* For builtins: an error to the terminal, never to a redirection. */
void shell_error(shell_t *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* For builtins: true once Break, Esc or Ctrl-C is pressed, and from then
 * on for the rest of the line. */
bool shell_interrupted(shell_t *sh);

/* The next key for an editor: hooks.getkey, with a notch of the mouse's
 * wheel (KEY_WHEEL_UP, KEY_WHEEL_DOWN) turned into SHELL_WHEEL_LINES of
 * Up or Down. */
int shell_getkey(shell_t *sh);

/* Whether `name` is a builtin, and which, -1 if none; and the i-th
 * builtin's name, NULL past the last, for Tab completion and `help`. */
bool shell_is_builtin(const char *name);
int shell_builtin_index(const char *name);
const char *shell_builtin_at(unsigned i);
/* The i-th builtin's usage and what it does, for `help`. */
const char *shell_builtin_usage(unsigned i);
const char *shell_builtin_about(unsigned i);

/* Show the flavour's prompt: on a line of its own if the cursor is not
 * at the start of one. */
void shell_prompt(shell_t *sh);

#endif /* SHELL_SHELL_H */
