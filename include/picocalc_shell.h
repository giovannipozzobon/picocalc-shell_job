/* picocalc_shell.h — return from a program to the PicoCalc shell, the
 * time it keeps, and the arguments it gives.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later OR MIT
 *
 * This one header is offered under the MIT licence as well as the GPL,
 * so that a program under any licence can include it (design.md §9).
 * Copyright (c) 2026 Blair Leduc.
 *
 * For a program built with the Pico SDK for an RP2350 PicoCalc under UF2
 * Loader 2.3 or later, with the shell installed as /BOOT2350.UF2. The
 * shell started the program by flashing it and rebooting; this reboots
 * the other way, telling the loader's stage3 to load the shell from the
 * card instead of starting the program again.
 *
 *     #include "picocalc_shell.h"
 *     ...
 *     save_settings();              // finish every card write first
 *     if (!picocalc_shell_exit())
 *         show_message("Cannot return to the shell");
 *
 * The program must close its files first: the reboot does not wait for
 * them. It need do nothing to return the user to their directory: the
 * shell comes back to where it was. /.shell/ on the card is private to
 * the shell: do not read, change or delete its files, which may change
 * between versions, but for the one file a program that takes arguments
 * is given (below). Holding Up, F1 or F5 at power-on also returns to the shell, for
 * any program. With the shell's patched loader (design.md M7), power-on
 * gives the shell with no key held, and Up, F1 or F5 the program; a
 * program's own watchdog_reboot(0, 0, 0) starts it again under either
 * loader, and holding Up, F1 or F5 through that reboot gives the shell.
 *
 * A program can also be installed without the loader, straight from
 * BOOTSEL; the call returns false there too, and the program can do
 * something else instead, such as power off through its own southbridge
 * driver. picocalc_shell_loader_present() and
 * picocalc_shell_card_present() tell it beforehand, to name its menu
 * item.
 *
 * The time (design.md §3.8): the shell keeps UTC in the RP2350's
 * always-on timer, which survives the reboot into the program, and a
 * record of it in POWMAN's scratch registers 0-2:
 *
 *     int64_t utc_ms;
 *     int offset_min;
 *     if (picocalc_shell_time(&utc_ms, &offset_min))
 *         show_clock(utc_ms / 1000 + offset_min * 60);   // local time
 *
 * These two functions are here when the program links hardware_powman
 * (target_link_libraries(... hardware_powman)), so a program that only
 * returns to the shell needs nothing more. A program may read the time
 * at any moment; it must not stop the timer, change its tick source,
 * set it except through picocalc_shell_set_time(), or write POWMAN
 * scratch 0-2.
 *
 * The arguments (design.md §3.5): with `pico-oric -t game.tap`, or `PICO-ORIC
 * /T GAME.TAP` in the shell's DOS flavour, the shell gives the words to a
 * program that says it takes them, in one line of a source file that is
 * sure to be linked (the one with main):
 *
 *     PICOCALC_SHELL_TAKES_ARGS("pico-oric");
 *
 * Any other program is refused, so a word is never silently dropped. At its
 * start the program reads PICOCALC_SHELL_ARGS_FILE with its own FatFs,
 * removes it, and hands the text to the functions below, which need no
 * FatFs:
 *
 *     static char text[PICOCALC_SHELL_ARGS_SIZE];
 *     ... read the file into text, len its length, and f_unlink it ...
 *     picocalc_shell_args_t args;
 *     if (picocalc_shell_args_parse(text, len, "pico-oric", &args)) {
 *         char path[256];
 *         if (picocalc_shell_args_option(&args, 't', path, sizeof path)) load_tape(path);
 *         if (picocalc_shell_args_option(&args, 'd', path, sizeof path)) load_disc(path);
 *     }
 *
 * There is a file at every start by the shell, with or without arguments,
 * so args.dir always tells the program the user's directory. There is none
 * when the loader's menu or a power-on starts the program, which then starts
 * as it would with no arguments.
 *
 * The loader's values below are UF2 Loader's
 * (common/bootloader/proginfo.c); the shell's src/port/loader.c checks
 * it agrees with them, and src/port/clock.c with the clock's.
 */
#ifndef PICOCALC_SHELL_H
#define PICOCALC_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/time.h"

/* The card's detect switch: low with a card in, so it needs a pull-up. */
#define PICOCALC_SHELL_CARD_DETECT_PIN 22u

/* A command to stage3 in the watchdog's scratch registers: the magic in
 * scratch[0], the mode in [1], an argument in [2]. They survive the
 * watchdog's reboot, and stage3 clears the magic once it has read them. */
#define PICOCALC_SHELL_LOADER_MAGIC 0xe98cc638u
#define PICOCALC_SHELL_BOOT_SD      1u    /* load /BOOT2350.UF2 into SRAM */

/* Whether a card is in the slot. */
static inline bool picocalc_shell_card_present(void) {
    gpio_init(PICOCALC_SHELL_CARD_DETECT_PIN);
    gpio_set_dir(PICOCALC_SHELL_CARD_DETECT_PIN, GPIO_IN);
    gpio_pull_up(PICOCALC_SHELL_CARD_DETECT_PIN);
    busy_wait_us(100);   /* the pull-up's settling */
    return !gpio_get(PICOCALC_SHELL_CARD_DETECT_PIN);
}

/* Whether the program was started by UF2 Loader. stage3 starts it from
 * the app partition, after the loader in flash, so the boot ROM maps a
 * flash offset other than 0 at 0x10000000; a program copied on in
 * BOOTSEL runs from offset 0. (It shows a partition boot, not stage3 by
 * name; without stage3, the reboot below would only start the program
 * again.) */
static inline bool picocalc_shell_loader_present(void) {
    return (qmi_hw->atrans[0] & QMI_ATRANS0_BASE_BITS) != 0;
}

/* Return to the shell: stage3 loads /BOOT2350.UF2 instead of the app.
 * Returns false, and does nothing, if UF2 Loader did not start the
 * program, or there is no card to load the shell from: without one,
 * stage3 would drop to BOOTSEL and the PicoCalc would look dead.
 * Otherwise it does not return. */
static inline bool picocalc_shell_exit(void) {
    if (!picocalc_shell_loader_present() || !picocalc_shell_card_present()) return false;
    watchdog_hw->scratch[1] = PICOCALC_SHELL_BOOT_SD;
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[0] = PICOCALC_SHELL_LOADER_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

#if defined(__has_include)
#if __has_include("hardware/powman.h")
#define PICOCALC_SHELL_HAS_TIME 1
#endif
#endif

#ifdef PICOCALC_SHELL_HAS_TIME
#include "hardware/powman.h"

/* The record: scratch[0] the magic, scratch[2] ~(scratch[0] ^
 * scratch[1]) as a check, and in scratch[1] these, among the shell's
 * own. */
#define PICOCALC_SHELL_CLOCK_MAGIC   0x434c4b31u   /* "CLK1" */
#define PICOCALC_SHELL_CLOCK_OFFSET  0x0000ffffu   /* minutes from UTC, signed */
#define PICOCALC_SHELL_CLOCK_SOURCE  0x00070000u   /* who set it */
#define PICOCALC_SHELL_CLOCK_PROGRAM 0x00040000u   /* a program */
#define PICOCALC_SHELL_CLOCK_SET     0x00080000u
#define PICOCALC_SHELL_CLOCK_RTC_DUE 0x00400000u   /* the shell writes the DS3231 */

/* scratch[1], or 0 if the record is not whole: power-on cleared it. */
static inline uint32_t picocalc_shell_clock_record(void) {
    uint32_t magic = powman_hw->scratch[0], bits = powman_hw->scratch[1];
    if (magic != PICOCALC_SHELL_CLOCK_MAGIC || powman_hw->scratch[2] != ~(magic ^ bits)) return 0;
    return bits;
}

/* The time the shell keeps: UTC milliseconds since 1970, and the local
 * offset in minutes when the shell started the program (its TZ then).
 * Returns false, and leaves both alone, if the clock is not set: as
 * after power-on with the stock loader, which starts the last program
 * without the shell. */
static inline bool picocalc_shell_time(int64_t *utc_ms, int *offset_min) {
    uint32_t bits = picocalc_shell_clock_record();
    if (!(bits & PICOCALC_SHELL_CLOCK_SET) || !powman_timer_is_running()) return false;
    *utc_ms = (int64_t)powman_timer_get_ms();
    *offset_min = (int16_t)(bits & PICOCALC_SHELL_CLOCK_OFFSET);
    return true;
}

/* Set the time, for a program that learns it itself (its own NTP or
 * RTC). The shell writes the DS3231, if there is one, when it comes
 * back. The offset is left as the shell gave it, 0 if it did not. */
static inline void picocalc_shell_set_time(int64_t utc_ms) {
    powman_timer_set_1khz_tick_source_xosc();
    powman_timer_set_ms((uint64_t)utc_ms);
    powman_timer_start();
    uint32_t bits = picocalc_shell_clock_record() & ~PICOCALC_SHELL_CLOCK_SOURCE;
    bits |= PICOCALC_SHELL_CLOCK_SET | PICOCALC_SHELL_CLOCK_PROGRAM | PICOCALC_SHELL_CLOCK_RTC_DUE;
    powman_hw->scratch[0] = PICOCALC_SHELL_CLOCK_MAGIC;
    powman_hw->scratch[1] = bits;
    powman_hw->scratch[2] = ~(PICOCALC_SHELL_CLOCK_MAGIC ^ bits);
}
#endif /* PICOCALC_SHELL_HAS_TIME */

/* ---- arguments ------------------------------------------------------- */

#define PICOCALC_SHELL_ARGS_FILE "/.shell/args"
/* The most the file can hold, so a buffer of this size always has it. */
#define PICOCALC_SHELL_ARGS_SIZE 4700u

/* The record, found by the shell in the program's UF2 while it checks the
 * file. Aligned to its size, it lies whole in one UF2 block. */
#define PICOCALC_SHELL_ARGS_RECORD 64u
#define PICOCALC_SHELL_ARGS_MAGIC0 0x9a3f6c1du
#define PICOCALC_SHELL_ARGS_MAGIC1 0x5e8b27f4u
#define PICOCALC_SHELL_ARGS_NAME   (PICOCALC_SHELL_ARGS_RECORD - 8u)

typedef struct {
    uint32_t magic[2];
    char     name[PICOCALC_SHELL_ARGS_NAME];
} picocalc_shell_args_record_t;

#ifdef __cplusplus
#define PICOCALC_SHELL_STATIC_ASSERT static_assert
#else
#define PICOCALC_SHELL_STATIC_ASSERT _Static_assert
#endif
PICOCALC_SHELL_STATIC_ASSERT(sizeof(picocalc_shell_args_record_t) == PICOCALC_SHELL_ARGS_RECORD,
                             "the record's size");

/* Nothing in the program reads the record, so the linker's --gc-sections
 * would drop it, and the Arm toolchain ignores the `retain` attribute. A
 * constructor that only names it keeps it, since every linker script keeps
 * .init_array; it does nothing else, in a few instructions before main. The
 * name is the one picocalc_shell_args_parse is given, so renaming the UF2
 * changes nothing. */
#define PICOCALC_SHELL_TAKES_ARGS(program_name)                                           \
    __attribute__((used, aligned(PICOCALC_SHELL_ARGS_RECORD),                             \
                   section(".rodata.picocalc_shell_args")))                               \
    static const picocalc_shell_args_record_t picocalc_shell_args_record = {              \
        { PICOCALC_SHELL_ARGS_MAGIC0, PICOCALC_SHELL_ARGS_MAGIC1 }, program_name          \
    };                                                                                    \
    __attribute__((constructor)) static void picocalc_shell_args_keep(void) {             \
        __asm__ volatile("" : : "r"(&picocalc_shell_args_record));                        \
    }                                                                                     \
    PICOCALC_SHELL_STATIC_ASSERT(sizeof(program_name) > 1 &&                              \
                                     sizeof(program_name) <= PICOCALC_SHELL_ARGS_NAME,    \
                                 "the name is 1 to 55 characters")

typedef struct {
    bool        dos;      /* the shell's flavour: words are in DOS's spelling */
    const char *dir;      /* the user's directory, always with '/' */
    const char *program;  /* the program's own path on the card */
    const char *name;     /* the name it declared */
    int         argc;     /* how many words follow its name */
    const char *words;    /* those words, each NUL-ended, one after another */
} picocalc_shell_args_t;

/* Split the file's `len` bytes of `text`, in place. False, with `text` no
 * longer to be used, unless it is a whole file of a version known here and
 * meant for the program called `name`; ignore such a file. The pointers in
 * `args` are into `text`. */
static inline bool picocalc_shell_args_parse(char *text, size_t len, const char *name,
                                             picocalc_shell_args_t *args) {
    if (len == 0 || text[len - 1] != '\n' || memchr(text, 0, len)) return false;
    const char *line[5] = { 0 };
    int n = 0;
    char *end = text + len;
    char *p = text;
    args->argc = 0;
    args->words = end;
    while (p < end) {
        char *lf = (char *)memchr(p, '\n', (size_t)(end - p));
        *lf = '\0';
        if (n < 5) {
            line[n++] = p;
        } else if (args->argc++ == 0) {
            args->words = p;
        }
        p = lf + 1;
    }
    if (n < 5 || strcmp(line[0], "picocalc-args 1") != 0) return false;
    if (strcmp(line[1], "unix") != 0 && strcmp(line[1], "dos") != 0) return false;
    if (line[2][0] != '/' || line[3][0] != '/' || strcmp(line[4], name) != 0) return false;
    args->dos = line[1][0] == 'd';
    args->dir = line[2];
    args->program = line[3];
    args->name = line[4];
    return true;
}

/* The i-th word as typed, from 0, or NULL. */
static inline const char *picocalc_shell_args_word(const picocalc_shell_args_t *args, int i) {
    if (i < 0 || i >= args->argc) return NULL;
    const char *w = args->words;
    while (i--) w += strlen(w) + 1;
    return w;
}

/* `word`, a file as typed, as a full path from the user's directory, by the
 * flavour's rules: DOS separates names with '\\' and refuses '/', which is its
 * switch character, and takes C: but not another drive. False if it cannot
 * be, or does not fit `size` bytes. Whether the
 * file exists is for the program to find out. */
static inline bool picocalc_shell_args_path(const picocalc_shell_args_t *args, const char *word,
                                            char *out, size_t size) {
    if (!word[0] || size < 2) return false;
    if (args->dos && strchr(word, '/')) return false;
    if (args->dos && word[1] == ':') {
        if (word[0] != 'C' && word[0] != 'c') return false;
        word += 2;
    }
    bool rooted = word[0] == (args->dos ? '\\' : '/');
    const char *part[2] = { rooted ? "" : args->dir, word };
    size_t len = 0;
    for (int i = 0; i < 2; i++) {
        const char *s = part[i];
        while (*s) {
            const char *e = s;
            while (*e && *e != '/' && !(args->dos && *e == '\\')) e++;
            size_t n = (size_t)(e - s);
            if (n == 2 && s[0] == '.' && s[1] == '.') {
                while (len > 0 && out[len - 1] != '/') len--;
                if (len > 0) len--;   /* the '/' before it */
            } else if (n > 0 && !(n == 1 && s[0] == '.')) {
                if (len + 1 + n >= size) return false;
                out[len++] = '/';
                memcpy(out + len, s, n);
                len += n;
            }
            s = *e ? e + 1 : e;
        }
    }
    if (len == 0) out[len++] = '/';
    out[len] = '\0';
    return true;
}

/* The file given with the option `letter`, as a full path: `-t name` exactly
 * in the UNIX flavour, `/t name` or `/T name` in DOS's. The first if it was
 * given more than once, as `-t *.tap` is, with each match a word. False if
 * it was not given, or has no word after it, or the path cannot be made. */
static inline bool picocalc_shell_args_option(const picocalc_shell_args_t *args, char letter,
                                              char *out, size_t size) {
    char mark = args->dos ? '/' : '-';
    char folded = letter >= 'a' && letter <= 'z' ? (char)(letter - 'a' + 'A') : letter;
    for (int i = 0; i + 1 < args->argc; i++) {
        const char *w = picocalc_shell_args_word(args, i);
        if (w[0] != mark || !w[1] || w[2]) continue;
        char c = w[1];
        if (args->dos && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == (args->dos ? folded : letter))
            return picocalc_shell_args_path(args, picocalc_shell_args_word(args, i + 1), out, size);
    }
    return false;
}

#endif /* PICOCALC_SHELL_H */
