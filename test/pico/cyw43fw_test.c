/* cyw43fw_test.c — a program that starts the CYW43439 with its firmware
 * read from the card, to check design.md §3.10 (M14 step 2) on the board.
 *
 * Copy build/pico-w/cyw43fw-test.uf2 to /pico2-apps/ and
 * build/pico-w/cyw43fw/cyw43-fw.bin to /bluetooth/ on the card, and type
 * `cyw43fw-test`. It opens the file, starts the radio (the driver reads
 * the firmware from the file, a block at a time, instead of from RAM),
 * says how long that took and the chip's address, lights the board's LED,
 * and scans for networks, which also needs the CLM blob the file holds.
 * Any key then returns to the shell. A printable character on UART0
 * counts as a key too, and each step is logged there.
 *
 * It uses the shell's own panel, terminal and keyboard code. */

#include <stdarg.h>
#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "cyw43_fw.h"
#include "ff.h"
#include "kbd.h"
#include "keys.h"
#include "lcd.h"
#include "picocalc_shell.h"
#include "screen.h"
#include "southbridge.h"
#include "term.h"

#define KBD_POLL_US 33333u   /* about 30 Hz (hardware-notes.md §6.1) */
#define SCAN_MS     6000u

static term_t s_term;
static keys_t s_keys;

/* The driver was built for Bluetooth, so the arch asks for BTstack's
 * hooks; this program has no Bluetooth, only the firmware's loading. */
bool btstack_cyw43_init(void *context) {
    (void)context;
    return true;
}

void btstack_cyw43_deinit(void *context) {
    (void)context;
}

/* The driver's poll calls this for Bluetooth: nothing here. */
void cyw43_bluetooth_hci_process(void) {
}

/* FatFs's, for files it writes; this only reads. */
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

/* A key from the PicoCalc or UART0, or 0 after `ms`. */
static int wait_key(unsigned ms) {
    uint64_t until = time_us_64() + (uint64_t)ms * 1000u;
    uint64_t next = 0;
    for (;;) {
        uint64_t now = time_us_64();
        screen_tick(&s_term, now);
        if (now >= next) {
            next = now + KBD_POLL_US;
            kbd_poll();
            uint8_t state, code;
            while (kbd_pop(&state, &code)) {
                int k = keys_event(&s_keys, state, code);
                if (k > 0) return k;
            }
            int c = getchar_timeout_us(0);
            if (c >= 0x20 || c == '\r') return c;
        }
        if (ms && now >= until) return 0;
    }
}

static unsigned s_found;

static int on_scan(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (r && r->ssid_len) {
        if (s_found < 6) say("  %-20.*s %4d dBm\n", r->ssid_len, r->ssid, (int)r->rssi);
        s_found++;
    }
    return 0;
}

static void leave(void) {
    say("\nAny key returns to the shell.\n");
    wait_key(0);
    /* Without a card, stage3 would drop to BOOTSEL (design.md §3.1). */
    if (!picocalc_shell_card_present()) {
        say("Put the card back to return to\nthe shell.\n");
        while (!picocalc_shell_card_present()) wait_key(100);
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

int main(void) {
    stdio_init_all();
    gpio_pull_up(PICO_DEFAULT_UART_RX_PIN);
    sb_init();
    lcd_init();
    screen_init();
    term_init(&s_term);
    keys_init(&s_keys);
    say("\033[1mcyw43fw-test\033[0m: the radio's firmware\nfrom the card, not from RAM\n\n");

    say("Opening " CYW43_FW_PATH "\n");
    const char *why = cyw43_fw_open();
    if (why) {
        say("  %s.\n", why);
        leave();
    }
    say("  Its length is this program's.\n");

    say("Starting the radio...\n");
    uint64_t t0 = time_us_64();
    int err = cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE);
    if (err) {
        cyw43_fw_close();
        say("  It did not start (%d).\n", err);
        leave();
    }
    /* The firmware is loaded by the first thing done to the chip, not by
     * the init: this, and the file stays open until it returns. */
    cyw43_arch_enable_sta_mode();
    uint8_t mac[6] = { 0 };
    int mac_err = cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000u);
    cyw43_fw_close();
    unsigned reads, fills;
    long failed;
    cyw43_fw_stats(&reads, &fills, &failed);
    say("  %u reads of the file, %u from\n  the card", reads, fills);
    if (failed >= 0) say(", one failed at %ld", failed);
    say(".\n");
    bool up = mac_err == 0 && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
    if (!up) {
        say("  The chip did not come up (%d)\n  after %lu ms.\n", mac_err, (unsigned long)ms);
        cyw43_arch_deinit();
        leave();
    }
    say("  Up in %lu ms.\n", (unsigned long)ms);
    say("  Address %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    say("  The LED is on.\n");

    say("Scanning for networks...\n");
    cyw43_wifi_scan_options_t opts = { 0 };
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, on_scan) != 0) {
        say("  The scan did not start.\n");
    } else {
        uint64_t until = time_us_64() + (uint64_t)SCAN_MS * 1000u;
        while (cyw43_wifi_scan_active(&cyw43_state) && time_us_64() < until) {
            screen_tick(&s_term, time_us_64());
        }
        say("  %u networks.\n", s_found);
    }
    cyw43_arch_deinit();
    leave();
    return 0;
}
