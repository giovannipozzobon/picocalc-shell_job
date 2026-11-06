/* args_test.c — a program that shows what the shell gave it, to check
 * program arguments on the board (design.md §3.5, M13). One source, built
 * twice: args-test.uf2 runs from flash and args-ram.uf2 from RAM, and both
 * declare the one name `args-test`.
 *
 * Copy them to /pico2-apps/ and type `args-test -t game.tap -d ../d.dsk`
 * (`ARGS-TEST /T GAME.TAP` in the DOS flavour). It reads /.shell/args,
 * removes it as a program must, and shows the flavour, the directory, its
 * own path, each word and the full paths of -t and -d; then any key returns
 * to the shell. Started from the loader's menu or at power-on, it says there
 * is no file. Each line goes to UART0 (GP2/GP3, 115200) as well, where a
 * printable character or CR counts as a key.
 *
 * It uses the shell's own panel, terminal, keyboard and card code. */

#include <stdio.h>
#include <string.h>

#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "kbd.h"
#include "keys.h"
#include "lcd.h"
#include "picocalc_shell.h"
#include "screen.h"
#include "sd.h"
#include "southbridge.h"
#include "term.h"

#define KBD_POLL_US 33333u   /* about 30 Hz (hardware-notes.md §6.1) */

PICOCALC_SHELL_TAKES_ARGS("args-test");

static term_t s_term;
static keys_t s_keys;
static FATFS  s_fs;
static FIL    s_file;
static char   s_text[PICOCALC_SHELL_ARGS_SIZE];

/* FatFs's clock, for writes; removing a file takes no time stamp. */
DWORD get_fattime(void) {
    return 0;
}

/* To the panel and the UART alike. */
static void say(const char *s) {
    fputs(s, stdout);
    for (; *s; s++) {
        if (*s == '\n') term_write(&s_term, "\r", 1);
        term_write(&s_term, s, 1);
    }
    screen_flush(&s_term, time_us_64());
}

static void line(const char *label, const char *value) {
    char buf[320];
    snprintf(buf, sizeof buf, "%s %s\n", label, value);
    say(buf);
}

static void show_option(const picocalc_shell_args_t *args, char letter) {
    char path[256], label[8];
    snprintf(label, sizeof label, "%c:", letter);
    line(label, picocalc_shell_args_option(args, letter, path, sizeof path) ? path : "(not given)");
}

/* What the shell left, read and removed: its length, or -1 with none. */
static long take_file(void) {
    if (!sd_present() || f_mount(&s_fs, "", 1) != FR_OK) {
        say("The card cannot be read.\n");
        return -1;
    }
    long len = -1;
    if (f_open(&s_file, PICOCALC_SHELL_ARGS_FILE, FA_READ) == FR_OK) {
        UINT n = 0;
        if (f_read(&s_file, s_text, sizeof s_text, &n) == FR_OK) len = (long)n;
        f_close(&s_file);
        if (f_unlink(PICOCALC_SHELL_ARGS_FILE) != FR_OK) say("The file was not removed.\n");
    }
    f_unmount("");
    return len;
}

static void wait_key(void) {
    for (;;) {
        kbd_poll();
        uint8_t state, code;
        while (kbd_pop(&state, &code))
            if (keys_event(&s_keys, state, code) != KEYS_NONE) return;
        int c;
        while ((c = getchar_timeout_us(0)) >= 0)
            if ((c >= 0x20 && c < 0x7F) || c == '\r') return;
        sleep_us(KBD_POLL_US);
    }
}

int main(void) {
    stdio_init_all();
    /* A floating RX reads as noise on a PicoCalc with no probe. */
    gpio_pull_up(PICO_DEFAULT_UART_RX_PIN);
    sb_init();
    lcd_init();
    screen_init();
    term_init(&s_term);
    keys_init(&s_keys);
#if PICO_NO_FLASH
    say("\033[1margs-test\033[0m from RAM\n\n");
#else
    say("\033[1margs-test\033[0m from flash\n\n");
#endif

    long len = take_file();
    picocalc_shell_args_t args;
    if (len < 0) {
        say("No arguments file: not started\nby the shell.\n");
    } else if (!picocalc_shell_args_parse(s_text, (size_t)len, "args-test", &args)) {
        say("A file, but not one for this\nprogram: ignored.\n");
    } else {
        line("flavour:", args.dos ? "dos" : "unix");
        line("dir:", args.dir);
        line("program:", args.program);
        line("name:", args.name);
        char buf[32];
        snprintf(buf, sizeof buf, "%d", args.argc);
        line("words:", buf);
        for (int i = 0; i < args.argc; i++) line(" ", picocalc_shell_args_word(&args, i));
        show_option(&args, 't');
        show_option(&args, 'd');
    }

    say("\nAny key returns to the shell.\n");
    while (getchar_timeout_us(0) >= 0) {}
    wait_key();
    stdio_flush();
    /* As picocalc_shell_exit(), less its check for a program in flash,
     * which a RAM program is not. */
    if (!sd_present()) say("Put the card back.\n");
    while (!sd_present()) sleep_ms(100);
    watchdog_hw->scratch[1] = PICOCALC_SHELL_BOOT_SD;
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[0] = PICOCALC_SHELL_LOADER_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}
