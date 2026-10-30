/* main.c — web.uf2: a web file manager for the card, over Wi-Fi
 * (design.md §3.9).
 *
 * A RAM program for the Pico 2 W, run by the W build's `web` through
 * stage3's BOOT_RAM, as ntp.uf2 is (src/ntp/main.c, whose layout this
 * follows). It reads /ntp/ntp.cfg, joins the network it names, answers
 * on port 80 and at picocalc.local, and goes back to the shell when the
 * user presses Esc, Break or Ctrl-C.
 *
 * The protocol and the card are src/core/httpd.c's and web.c's, tested on
 * the host; this is the socket side: lwIP's raw TCP API, polled from the
 * main loop with the panel and the keyboard, as ntp.uf2 is.
 *
 * It links the CYW43 driver under the additional permission in
 * THIRD-PARTY.md (GPL-3.0 section 7), which covers only the owner's
 * code: it must link no code from UF2 Loader or another GPL-3.0 project
 * whose authors have not given the same permission. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "fat.h"
#include "httpd.h"
#include "kbd.h"
#include "keycodes.h"
#include "keys.h"
#include "lcd.h"
#include "ntp.h"
#include "picocalc_shell.h"
#include "screen.h"
#include "sd.h"
#include "southbridge.h"
#include "term.h"
#include "web_page.h"

#define KBD_POLL_US 33333u   /* about 30 Hz (hardware-notes.md §6.1) */
#define HTTP_PORT   80u
#define SHOW_MS     1500u

static term_t  s_term;
static keys_t  s_keys;
static FIL     s_file;
static char    s_text[NTP_CFG_BYTES];
static ntp_config_t s_cfg;
static uint64_t s_next_poll;

/* To the panel and, in the development build, UART0. */
static void say(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
#if PICO_SHELL_UART
    fputs(buf, stdout);
#endif
    /* The terminal's LF only moves down. */
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') term_write(&s_term, "\r", 1);
        term_write(&s_term, &buf[i], 1);
    }
    screen_flush(&s_term, time_us_64());
}

/* Whether the user stopped it: polled at the keyboard's rate. */
static bool stopped(void) {
    uint64_t now = time_us_64();
    screen_tick(&s_term, now);
    if (now < s_next_poll) return false;
    s_next_poll = now + KBD_POLL_US;
    bool hit = false;
    kbd_poll();
    uint8_t state, code;
    while (kbd_pop(&state, &code)) {
        int k = keys_event(&s_keys, state, code);
        if (k == KEY_BREAK || k == 0x1B || k == 0x03) hit = true;
    }
#if PICO_SHELL_UART
    int c;
    while ((c = getchar_timeout_us(0)) >= 0)
        if (c == 0x1B || c == 0x03) hit = true;
#endif
    return hit;
}

/* The network, from /ntp/ntp.cfg (§3.9). Its own function so that a
 * file for the network alone can replace it. */
static bool read_network(void) {
    say("Reading %s\n", NTP_CFG_PATH);
    files_mount();   /* the same volume the server serves; FatFs mounts it on first use */
    if (!sd_present()) {
        say("  The card cannot be read.\n");
        return false;
    }
    UINT n = 0;
    FRESULT fr = f_open(&s_file, NTP_CFG_PATH, FA_READ);
    if (fr == FR_OK) {
        fr = f_read(&s_file, s_text, sizeof s_text, &n);
        if (f_size(&s_file) > sizeof s_text) fr = FR_INVALID_PARAMETER;
        f_close(&s_file);
    }
    if (fr != FR_OK) {
        say("  It cannot be read.\n");
        return false;
    }
    unsigned line;
    ntp_cfg_status_t st = ntp_config_parse(&s_cfg, s_text, n, &line);
    if (st != NTP_CFG_OK) {
        say("  Line %u: %s.\n", line, ntp_cfg_status_str(st));
        return false;
    }
    if (!s_cfg.ssid[0]) {
        say("  It names no network.\n");
        return false;
    }
    return true;
}

/* ---- the server --------------------------------------------------------- */

/* A connection is an httpd_t (src/core/httpd.c), which does the
 * protocol and the card, and the socket it is moved to and from. */
#define MAX_CONNS   3
#define POLL_HALFS  4      /* tcp_poll's interval, in half seconds */
#define IDLE_POLLS  15     /* of them, without a byte either way, and it is dropped */

typedef struct {
    httpd_t         h;
    struct tcp_pcb *pcb;
    int             idle;
    bool            used;
} conn_t;

static conn_t s_conns[MAX_CONNS];

static bool env_now(void *ctx, int64_t *utc_s, int *off_min) {
    (void)ctx;
    int64_t ms;
    if (!picocalc_shell_time(&ms, off_min)) return false;
    *utc_s = ms / 1000;
    return true;
}

static void env_note(void *ctx, const char *text) {
    (void)ctx;
    say("%.39s\n", text);
}

static const httpd_env_t s_env = { web_page, sizeof web_page, env_now, env_note, NULL };

static void release(conn_t *c) {
    httpd_close(&c->h);
    c->used = false;
}

/* Let go of the socket and the connection: politely, or with a reset. */
static err_t drop(conn_t *c, bool abort) {
    struct tcp_pcb *pcb = c->pcb;
    c->pcb = NULL;
    release(c);
    if (!pcb) return ERR_OK;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (abort || tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

/* Hand the socket what the response has, as much as it takes now. */
static err_t pump(conn_t *c) {
    const uint8_t *p;
    size_t n;
    for (;;) {
        if (httpd_failed(&c->h)) return drop(c, true);
        if (httpd_finished(&c->h)) {
            tcp_output(c->pcb);
            return drop(c, false);
        }
        n = httpd_peek(&c->h, &p);
        if (n == 0) break;
        u16_t room = tcp_sndbuf(c->pcb);
        if (room == 0 || tcp_sndqueuelen(c->pcb) + 2 >= TCP_SND_QUEUELEN) break;
        if (n > room) n = room;
        err_t err = tcp_write(c->pcb, p, (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) break;   /* lwIP's heap is full: sent() calls again */
        if (err != ERR_OK) return drop(c, true);
        httpd_advance(&c->h, n);
        c->idle = 0;
    }
    tcp_output(c->pcb);
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    conn_t *c = arg;
    (void)err;
    if (!c) {
        if (p) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (!p) return drop(c, true);   /* the browser went away */
    c->idle = 0;
    for (struct pbuf *q = p; q; q = q->next) httpd_feed(&c->h, q->payload, q->len);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return pump(c);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb;
    (void)len;
    conn_t *c = arg;
    if (!c) return ERR_OK;
    c->idle = 0;
    return pump(c);
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    conn_t *c = arg;
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (++c->idle > IDLE_POLLS) return drop(c, true);
    return pump(c);
}

static void on_err(void *arg, err_t err) {
    (void)err;
    conn_t *c = arg;
    if (!c) return;
    c->pcb = NULL;   /* lwIP has freed it already */
    release(c);
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    conn_t *c = NULL;
    for (int i = 0; i < MAX_CONNS && !c; i++)
        if (!s_conns[i].used) c = &s_conns[i];
    if (!c) {
        tcp_abort(pcb);   /* all are busy: the browser asks again */
        return ERR_ABRT;
    }
    c->used = true;
    c->pcb = pcb;
    c->idle = 0;
    httpd_init(&c->h, &s_env);
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, POLL_HALFS);
    return ERR_OK;
}

static bool serve(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb || tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) return false;
    pcb = tcp_listen_with_backlog(pcb, 2);
    if (!pcb) return false;
    tcp_accept(pcb, on_accept);
    return true;
}

/* ---- the network -------------------------------------------------------- */

/* Joins the network and waits for an address, with the panel and the
 * keyboard kept alive. As ntp.uf2's join(): the SDK's blocking call
 * would freeze the panel until the timeout. */
static bool join(void) {
    say("Joining %s\n", s_cfg.ssid);
    cyw43_arch_enable_sta_mode();
    uint32_t auth = s_cfg.password[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    const char *pw = s_cfg.password[0] ? s_cfg.password : NULL;
    if (cyw43_arch_wifi_connect_async(s_cfg.ssid, pw, auth)) {
        say("  The radio refused.\n");
        return false;
    }
    uint64_t deadline = time_us_64() + (uint64_t)s_cfg.timeout_s * 1000000u;
    int link = CYW43_LINK_DOWN;
    while (time_us_64() < deadline) {
        cyw43_arch_poll();
        link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) return true;
        if (link == CYW43_LINK_BADAUTH) {
            say("  The password was refused.\n");
            return false;
        }
        if (link == CYW43_LINK_FAIL) break;
        /* A scan that missed the network: ask again (ntp.uf2's note). */
        if (link == CYW43_LINK_NONET && cyw43_arch_wifi_connect_async(s_cfg.ssid, pw, auth)) break;
        if (stopped()) {
            say("  Stopped.\n");
            return false;
        }
    }
    say(link == CYW43_LINK_JOIN || link == CYW43_LINK_NOIP ? "  No address from the network.\n"
                                                           : "  The network was not joined.\n");
    return false;
}

static bool run(void) {
    if (!read_network()) return false;
    uint32_t country = s_cfg.country[0] ? CYW43_COUNTRY(s_cfg.country[0], s_cfg.country[1], 0)
                                        : CYW43_COUNTRY_WORLDWIDE;
    if (cyw43_arch_init_with_country(country)) {
        say("No radio.\n");
        return false;
    }
    if (!join()) {
        cyw43_arch_deinit();
        return false;
    }
    struct netif *nif = netif_default;
    mdns_resp_init();
    mdns_resp_add_netif(nif, "picocalc");
    mdns_resp_add_service(nif, "picocalc", "_http", DNSSD_PROTO_TCP, HTTP_PORT, NULL, NULL);
    if (!serve()) {
        say("No port 80.\n");
        cyw43_arch_deinit();
        return false;
    }
    say("http://%s/\nhttp://picocalc.local/\n\nAnyone on the network can use the\ncard. Esc stops.\n\n",
        ip4addr_ntoa(netif_ip4_addr(nif)));
    while (!stopped()) cyw43_arch_poll();
    for (int i = 0; i < MAX_CONNS; i++)
        if (s_conns[i].used) drop(&s_conns[i], true);
    files_unmount();
    cyw43_arch_deinit();
    return true;
}

int main(void) {
    stdio_init_all();
#if PICO_SHELL_UART
    gpio_pull_up(PICO_DEFAULT_UART_RX_PIN);
#endif
    sb_init();
    lcd_init();
    screen_init();
    term_init(&s_term);
    keys_init(&s_keys);
    say("\033[1mweb\033[0m: the card on the network\n\n");

    bool ok = run();

    uint64_t until = time_us_64() + (ok ? SHOW_MS : 2 * SHOW_MS) * 1000u;
    while (time_us_64() < until) stopped();
    /* Without a card, stage3 would drop to BOOTSEL (design.md §3.1). */
    if (!sd_present()) {
        say("Put the card back to return to\nthe shell.\n");
        while (!sd_present()) stopped();
        sleep_ms(500);
    }
    say("Back to the shell.\n");
#if PICO_SHELL_UART
    uart_tx_wait_blocking(uart_default);
#endif
    /* As picocalc_shell_exit(), less its check for a program in flash:
     * this one is in SRAM, started by stage3's BOOT_RAM. */
    watchdog_hw->scratch[1] = PICOCALC_SHELL_BOOT_SD;
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[0] = PICOCALC_SHELL_LOADER_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}
