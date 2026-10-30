/* btkbd_test.c — a program that starts Bluetooth and shows the keys of the
 * keyboard paired with btpair, to check src/port/btkbd.c on the board
 * (design.md §3.10, M14 step 3).
 *
 * Copy build/pico-w/btkbd-test.uf2 to /pico2-apps/, and
 * build/pico-w/cyw43fw/cyw43-fw.bin to /bluetooth/ on the card, which
 * must hold /bluetooth/pairing.dat made by btpair. Type `btkbd-test`. It
 * reads the firmware from the card, starts the radio and says what
 * Bluetooth is doing as it changes; press a key on the Bluetooth keyboard
 * to wake it. Each key that comes, from it ("bt") or from the PicoCalc
 * ("pc"), is shown with the memory BTstack has taken from malloc. Esc on
 * either keyboard returns to the shell. A printable character on UART0
 * counts as a key too, and each step is logged there.
 *
 * It uses the shell's own panel, terminal and keyboard code. */

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "btkbd.h"
#include "ff.h"
#include "kbd.h"
#include "keys.h"
#include "lcd.h"
#include "picocalc_shell.h"
#include "screen.h"
#include "southbridge.h"
#include "term.h"

#define KBD_POLL_US 33333u   /* about 30 Hz (hardware-notes.md §6.1) */

static term_t s_term;
static keys_t s_pc, s_bt;

/* FatFs's, for the pairing file's saves: a fixed date. */
DWORD get_fattime(void) {
    return (DWORD)(46u << 25 | 1u << 21 | 1u << 16);   /* 1 January 2026 */
}

static void say(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    fputs(buf, stdout);
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') term_write(&s_term, "\r", 1);
        term_write(&s_term, &buf[i], 1);
    }
    screen_flush(&s_term, time_us_64());
}

static void leave(void) {
    say("\nReturning to the shell.\n");
    sleep_ms(300);
    if (!picocalc_shell_card_present()) {
        say("Put the card back to return to\nthe shell.\n");
        while (!picocalc_shell_card_present()) tight_loop_contents();
        sleep_ms(500);
    }
    /* As picocalc_shell_exit(), less its check for a program in flash:
     * this one is in SRAM, started by stage3's BOOT_RAM, as ntp.uf2 is. */
    watchdog_hw->scratch[1] = PICOCALC_SHELL_BOOT_SD;
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[0] = PICOCALC_SHELL_LOADER_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

static void show_key(const char *who, int k) {
    struct mallinfo mi = mallinfo();
    if (k >= 0x20 && k < 0x7f) say("%s '%c'  heap %d\n", who, k, mi.uordblks);
    else say("%s 0x%02x  heap %d\n", who, k, mi.uordblks);
}

int main(void) {
    stdio_init_all();
    gpio_pull_up(PICO_DEFAULT_UART_RX_PIN);
    sb_init();
    lcd_init();
    screen_init();
    term_init(&s_term);
    keys_init(&s_pc);
    keys_init(&s_bt);
    say("\033[1mbtkbd-test\033[0m: a Bluetooth keyboard\n\n");

    uint64_t t0 = time_us_64();
    say("Starting Bluetooth...\n");
    if (!btkbd_init()) {
        say("  %s.\n", btkbd_status());
        leave();
    }
    say("  Radio up in %lu ms.\n", (unsigned long)((time_us_64() - t0) / 1000u));
    struct mallinfo mi = mallinfo();
    say("  heap %d bytes taken.\n", mi.uordblks);
    say("Paired: '%s'. Press a key on it.\nEsc returns to the shell.\n\n", btkbd_name()[0] ? btkbd_name() : "(none)");

    char last[48] = "";
    btkbd_state_t last_state = (btkbd_state_t)-1;
    uint64_t next = 0;
    for (;;) {
        uint64_t now = time_us_64();
        screen_tick(&s_term, now);
        btkbd_poll();
        if (btkbd_state() != last_state || strcmp(btkbd_status(), last) != 0) {
            last_state = btkbd_state();
            snprintf(last, sizeof last, "%s", btkbd_status());
            say("[%u] %s\n", (unsigned)last_state, last);
        }
        uint16_t e;
        while ((e = btkbd_read()) != 0) {
            int k = keys_event(&s_bt, (uint8_t)(e >> 8), (uint8_t)e);
            if (k > 0) {
                show_key("bt", k);
                if (k == 0x1B) leave();
            }
        }
        if (now >= next) {
            next = now + KBD_POLL_US;
            kbd_poll();
            uint8_t state, code;
            while (kbd_pop(&state, &code)) {
                int k = keys_event(&s_pc, state, code);
                if (k > 0) {
                    show_key("pc", k);
                    if (k == 0x1B) leave();
                }
            }
            int c = getchar_timeout_us(0);
            if (c == 0x1B) leave();
        }
    }
}
