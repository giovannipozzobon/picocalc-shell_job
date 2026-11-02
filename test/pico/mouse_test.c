/* mouse_test.c — a program that starts Bluetooth and shows what the mouse
 * paired with btpair does, in three pages, to check src/port/btkbd.c's
 * mouse on the board before the shell uses it (design.md §3.10).
 *
 * Copy build/pico-w/mouse-test.uf2 to /pico2-apps/, and
 * build/pico-w/cyw43fw/cyw43-fw.bin to /bluetooth/ on the card, which
 * must hold /bluetooth/pairing.dat made by btpair 2.1 with a mouse paired
 * (a keyboard's trackpad comes as a mouse too). Type `mouse-test`; press a
 * button on the mouse to wake it.
 *
 *   A  the numbers: the radio's state for the mouse and the keyboard, what
 *      each poll gave (dx, dy, wheel, buttons), the totals, how many
 *      reports come a second, and the most it moved in one poll.
 *   B  a pointer on the panel in pixels: an arrow that follows the mouse,
 *      coloured by the buttons held; a click leaves a mark; the wheel
 *      moves the bar at the right edge; a grid every 40 pixels to judge
 *      how far it goes.
 *   C  the pointer as the shell would have it, a cell of the 40x32 text
 *      grid in reverse video: a list of items, a click chooses one, the
 *      wheel scrolls the list, and the status line says the cell.
 *
 * Tab goes to the next page, + and - change the gain, a turns the
 * acceleration on and off, c clears (the totals, the marks, the wheel), and
 * Esc, on either keyboard, returns to the shell. A printable character on
 * UART0 is a key too.
 *
 * It uses the shell's own panel, terminal and keyboard code, and the one
 * pointer position serves B and C. Movement comes from btkbd_mouse() as
 * counts, a number that depends on the mouse's resolution, so the gain
 * turns them into pixels: position += counts * gain / 16, with the
 * acceleration on, a faster move in the last 16 ms counting up to 4 times. */

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

#define KBD_POLL_US  33333u    /* about 30 Hz (hardware-notes.md §6.1) */
#define MOVE_US      16000u    /* the window the acceleration looks at */
#define TEXT_US      100000u   /* how often the text pages are drawn */

#define PANEL_W 320
#define PANEL_H 320
#define COLS    40
#define ROWS    32
#define GLYPH_W 8
#define GLYPH_H 10
#define B_TOP   GLYPH_H        /* page B keeps the top text row for its status */

static term_t s_term;
static keys_t s_pc, s_bt;

/* FatFs's, for the pairing file's saves: a fixed date. */
DWORD get_fattime(void) {
    return (DWORD)(46u << 25 | 1u << 21 | 1u << 16);   /* 1 January 2026 */
}

/* ---- the text ------------------------------------------------------------ */

static void flush(void) {
    screen_flush(&s_term, time_us_64());
}

/* A line of text at `row`, padded to 39 columns: the last column is never
 * written, so nothing wraps. */
static void put(int row, const char *fmt, ...) {
    char buf[96], at[16];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    for (; n < COLS - 1; n++) buf[n] = ' ';
    buf[COLS - 1] = '\0';
    snprintf(at, sizeof at, "\033[%d;1H", row + 1);
    term_write(&s_term, at, strlen(at));
    term_write(&s_term, buf, COLS - 1);
}

static void tw(const char *s) {
    term_write(&s_term, s, strlen(s));
}

static void say(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') term_write(&s_term, "\r", 1);
        term_write(&s_term, s, 1);
    }
    flush();
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

/* ---- the mouse, as the pages share it ------------------------------------- */

static const int s_gains[] = { 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };   /* sixteenths of a pixel a count */
#define N_GAINS ((int)(sizeof s_gains / sizeof s_gains[0]))
static int  s_gain_i = 4;           /* 8: half a pixel a count */
static bool s_accel = true;

static int s_x16 = PANEL_W / 2 * 16, s_y16 = PANEL_H / 2 * 16;   /* the pointer, in sixteenths of a pixel */
static int s_acc_x, s_acc_y;        /* counts since the last window */
static uint64_t s_move_at;

static int s_buttons, s_buttons_was;
static int s_wheel_total;           /* notches since the last clear */
static int s_total_x, s_total_y;    /* counts since the last clear */
static int s_last_dx, s_last_dy, s_last_wheel;   /* the last poll that moved */
static int s_max_dx, s_max_dy;      /* the most in one poll */

enum { PAGE_A, PAGE_B, PAGE_C, PAGES };
static int s_page = PAGE_A;
static int s_y_min;                 /* the pointer's top row, in pixels: B keeps row 0 for text */

static int px(void) { return s_x16 >> 4; }
static int py(void) { return s_y16 >> 4; }

static void clamp(void) {
    if (s_x16 < 0) s_x16 = 0;
    if (s_x16 > (PANEL_W - 1) * 16) s_x16 = (PANEL_W - 1) * 16;
    if (s_y16 < s_y_min * 16) s_y16 = s_y_min * 16;
    if (s_y16 > (PANEL_H - 1) * 16) s_y16 = (PANEL_H - 1) * 16;
}

/* The window's counts into pixels. */
static void move_window(void) {
    int ax = s_acc_x, ay = s_acc_y;
    s_acc_x = s_acc_y = 0;
    if (!ax && !ay) return;
    int f = 1;
    if (s_accel) {
        int v = (ax < 0 ? -ax : ax) > (ay < 0 ? -ay : ay) ? (ax < 0 ? -ax : ax) : (ay < 0 ? -ay : ay);
        f += v / 10;
        if (f > 4) f = 4;
    }
    s_x16 += ax * s_gains[s_gain_i] * f;
    s_y16 += ay * s_gains[s_gain_i] * f;
    clamp();
}

/* ---- page A: the numbers --------------------------------------------------- */

static const char *const s_state_names[] = {
    "off", "starting", "none paired", "waiting", "searching", "pairing", "connected",
};

static const char *state_name(btkbd_state_t s) {
    return (unsigned)s < sizeof s_state_names / sizeof s_state_names[0] ? s_state_names[s] : "?";
}

static void page_a(unsigned rate) {
    unsigned reports;
    int x, y, w, b;
    btkbd_pointer(&reports, &x, &y, &w, &b);
    struct mallinfo mi = mallinfo();
    put(0, "mouse-test A numbers  Tab page Esc exit");
    put(2, "Mouse:    %s", state_name(btkbd_device_state(BTKBD_MOUSE)));
    put(3, "          '%.28s'", btkbd_device_name(BTKBD_MOUSE));
    put(4, "Keyboard: %s", state_name(btkbd_device_state(BTKBD_KEYBOARD)));
    put(5, "          '%.28s'", btkbd_device_name(BTKBD_KEYBOARD));
    put(7, "last  dx %+5d  dy %+5d  wheel %+3d", s_last_dx, s_last_dy, s_last_wheel);
    put(8, "buttons  L:%c  M:%c  R:%c", s_buttons & 1 ? '#' : '.', s_buttons & 4 ? '#' : '.',
        s_buttons & 2 ? '#' : '.');
    put(10, "total  x %+7d  y %+7d", s_total_x, s_total_y);
    put(11, "wheel  %+d notches", s_wheel_total);
    put(13, "reports %u, %u a second", reports, rate);
    put(14, "most in a poll  |dx| %d  |dy| %d", s_max_dx, s_max_dy);
    put(15, "heap %d bytes taken", mi.uordblks);
    put(17, "The radio's status: %.18s", btkbd_status());
    put(30, "c clears the totals");
    flush();
}

/* ---- page B: a pointer in pixels ------------------------------------------- */

#define ARROW_W 12
#define ARROW_H 19
static const char *const s_arrow[ARROW_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
    "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};

#define MARKS 48
static struct { int16_t x, y; uint16_t colour; } s_marks[MARKS];
static int s_n_marks, s_next_mark;

#define BAR_X 308
static int bar_y(void) {
    int y = (B_TOP + PANEL_H) / 2 - s_wheel_total * 6;
    if (y < B_TOP + 2) y = B_TOP + 2;
    if (y > PANEL_H - 6) y = PANEL_H - 6;
    return y;
}

static uint16_t pointer_colour(void) {
    switch (s_buttons & 7) {
    case 0: return 0xC618;                 /* light grey */
    case 1: return 0xF800;                 /* left: red */
    case 2: return 0x051F;                 /* right: blue */
    case 4: return 0x07E0;                 /* middle: green */
    default: return 0xFFE0;                /* more than one: yellow */
    }
}

static uint16_t bg_pixel(int x, int y) {
    if (x >= BAR_X) {
        int by = bar_y();
        if (y >= by && y < by + 4) return 0xFFE0;
        if (x >= BAR_X + 5 && x <= BAR_X + 6) return 0x4208;
    }
    for (int i = 0; i < s_n_marks; i++) {
        int dx = x - s_marks[i].x, dy = y - s_marks[i].y;
        if (dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2) return s_marks[i].colour;
    }
    if (x % 40 == 0 || (y - B_TOP) % 40 == 0) return 0x2104;
    return 0x0000;
}

static uint16_t b_pixel(int x, int y) {
    int sx = x - px(), sy = y - py();
    if (sx >= 0 && sx < ARROW_W && sy >= 0 && sy < ARROW_H) {
        char c = s_arrow[sy][sx];
        if (c == 'X') return 0xFFFF;
        if (c == '.') return pointer_colour();
    }
    return bg_pixel(x, y);
}

/* Paint a rectangle of page B's picture. A blit must not cross the wrap
 * of the panel's frame memory, which the hardware scroll moves in whole
 * text rows: so it goes in pieces that stay inside one. */
static void paint(int x, int y, int w, int h) {
    static uint16_t line[2][PANEL_W];
    if (x < 0) { w += x; x = 0; }
    if (y < B_TOP) { h -= B_TOP - y; y = B_TOP; }
    if (x + w > PANEL_W) w = PANEL_W - x;
    if (y + h > PANEL_H) h = PANEL_H - y;
    if (w <= 0 || h <= 0) return;
    for (int y0 = y; y0 < y + h;) {
        int y1 = (y0 / GLYPH_H + 1) * GLYPH_H;
        if (y1 > y + h) y1 = y + h;
        lcd_blit_begin((unsigned)x, (unsigned)y0, (unsigned)w, (unsigned)(y1 - y0));
        for (int r = y0; r < y1; r++) {
            uint16_t *l = line[r & 1];
            for (int c = 0; c < w; c++) l[c] = b_pixel(x + c, r);
            lcd_blit_row(l, (unsigned)w);
        }
        lcd_blit_end();
        y0 = y1;
    }
}

static void paint_pointer(int x, int y) {
    paint(x, y, ARROW_W, ARROW_H);
}

static void page_b_status(void) {
    put(0, "B pixel %3d,%3d gain %d.%02d accel %s", px(), py(), s_gains[s_gain_i] / 16,
        s_gains[s_gain_i] % 16 * 100 / 16, s_accel ? "on" : "off");
    flush();
}

/* ---- page C: the pointer as a cell ----------------------------------------- */

#define LIST_FIRST 2
#define LIST_LAST  29
#define LIST_ITEMS 100
static int s_top, s_sel = -1;
static int s_cell_row = -1, s_cell_col = -1;   /* where the cell pointer is drawn */
static char s_c_status[COLS];

/* What the grid holds at `row`: the list, with a ruler so that a column is
 * easy to read off. */
static void c_line(int row, char out[COLS]) {
    memset(out, ' ', COLS);
    out[COLS - 1] = '\0';
    if (row < LIST_FIRST || row > LIST_LAST) return;
    int item = s_top + row - LIST_FIRST;
    if (item < 0 || item >= LIST_ITEMS) return;
    char text[COLS];
    snprintf(text, sizeof text, "%c Item %02d ", item == s_sel ? '>' : ' ', item);
    memcpy(out, text, strlen(text));
    for (int c = (int)strlen(text); c < COLS - 1; c++) out[c] = c % 10 == 0 ? '|' : '.';
}

static void c_draw_cell(int row, int col, bool pointer) {
    char line[COLS], at[16], ch[2] = { ' ', 0 };
    if (row < 0 || col < 0) return;
    if (row >= LIST_FIRST && row <= LIST_LAST) { c_line(row, line); ch[0] = line[col]; }
    snprintf(at, sizeof at, "\033[%d;%dH", row + 1, col + 1);
    tw(at);
    if (pointer) tw("\033[7m");
    term_write(&s_term, ch, 1);
    if (pointer) tw("\033[0m");
}

static void c_draw_list(void) {
    char line[COLS];
    for (int r = LIST_FIRST; r <= LIST_LAST; r++) {
        c_line(r, line);
        put(r, "%s", line);
    }
}

static void c_status(void) {
    put(0, "mouse-test C cells  gain %d.%02d accel %s", s_gains[s_gain_i] / 16,
        s_gains[s_gain_i] % 16 * 100 / 16, s_accel ? "on" : "off");
    put(1, "click an item, wheel scrolls, Tab page");
    put(30, "wheel %+d  first item %d", s_wheel_total, s_top);
    put(31, "%s", s_c_status);
}

static void c_move_pointer(void) {
    int row = py() / GLYPH_H, col = px() / GLYPH_W;
    if (row == s_cell_row && col == s_cell_col) return;
    c_draw_cell(s_cell_row, s_cell_col, false);
    s_cell_row = row;
    s_cell_col = col;
    c_draw_cell(row, col, true);
    flush();
}

static void c_click(int which) {
    static const char *const names[] = { "Left", "Right", "Middle" };
    int row = py() / GLYPH_H, col = px() / GLYPH_W;
    if (which == 0 && row >= LIST_FIRST && row <= LIST_LAST && s_top + row - LIST_FIRST < LIST_ITEMS) {
        s_sel = s_top + row - LIST_FIRST;
        snprintf(s_c_status, sizeof s_c_status, "Left click col %d row %d: item %d", col, row, s_sel);
        c_draw_list();
    } else {
        snprintf(s_c_status, sizeof s_c_status, "%s click col %d row %d", names[which], col, row);
    }
    c_status();
    s_cell_row = s_cell_col = -1;
    c_move_pointer();
}

static void c_wheel(int notches) {
    int top = s_top - notches;      /* the wheel forward (positive) scrolls the list up */
    if (top < 0) top = 0;
    if (top > LIST_ITEMS - (LIST_LAST - LIST_FIRST + 1)) top = LIST_ITEMS - (LIST_LAST - LIST_FIRST + 1);
    if (top != s_top) {
        s_top = top;
        c_draw_list();
    }
    c_status();
    s_cell_row = s_cell_col = -1;
    c_move_pointer();
}

/* ---- the pages ------------------------------------------------------------- */

static void clear_all(void) {
    s_total_x = s_total_y = s_wheel_total = 0;
    s_last_dx = s_last_dy = s_last_wheel = 0;
    s_max_dx = s_max_dy = 0;
    s_n_marks = s_next_mark = 0;
}

/* A new page: the grid cleared and every row drawn again, since the pixels
 * of page B are not what the shadow thinks the panel holds. */
static void enter(int page) {
    s_page = page;
    s_y_min = page == PAGE_B ? B_TOP : 0;
    clamp();
    tw("\033[?25l\033[0m\033[2J\033[H");
    screen_repaint(&s_term);
    flush();
    if (page == PAGE_A) {
        page_a(0);
    } else if (page == PAGE_B) {
        page_b_status();
        paint(0, B_TOP, PANEL_W, PANEL_H - B_TOP);
    } else {
        s_c_status[0] = '\0';
        s_cell_row = s_cell_col = -1;
        c_draw_list();
        c_status();
        c_move_pointer();
        flush();
    }
}

static void key(int k) {
    if (k == 0x1B) leave();
    if (k == 0x09) {
        enter((s_page + 1) % PAGES);
        return;
    }
    if (k == '+' || k == '=') {
        if (s_gain_i < N_GAINS - 1) s_gain_i++;
    } else if (k == '-' || k == '_') {
        if (s_gain_i > 0) s_gain_i--;
    } else if (k == 'a') {
        s_accel = !s_accel;
    } else if (k == 'c') {
        clear_all();
        if (s_page == PAGE_B) paint(0, B_TOP, PANEL_W, PANEL_H - B_TOP);
        else if (s_page == PAGE_C) { s_sel = -1; s_top = 0; c_draw_list(); c_status(); s_cell_row = s_cell_col = -1; c_move_pointer(); }
    } else {
        return;
    }
    if (s_page == PAGE_B) page_b_status();
    else if (s_page == PAGE_C) c_status();
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
    say("\033[1mmouse-test\033[0m: a Bluetooth mouse\n\n");

    uint64_t t0 = time_us_64();
    say("Starting Bluetooth...\n");
    if (!btkbd_init()) {
        say("  ");
        say(btkbd_status());
        say(".\n");
        leave();
    }
    char line[COLS];
    snprintf(line, sizeof line, "  Radio up in %lu ms.\n", (unsigned long)((time_us_64() - t0) / 1000u));
    say(line);
    say("Press a button on the mouse to wake\nit. Tab changes the page.\n");
    sleep_ms(1500);
    enter(PAGE_A);

    uint64_t next_kbd = 0, next_text = 0, next_rate = 0;
    unsigned rate = 0, rate_base = 0;
    s_move_at = time_us_64() + MOVE_US;
    for (;;) {
        uint64_t now = time_us_64();
        screen_tick(&s_term, now);
        btkbd_poll();

        /* the mouse */
        int dx, dy, wheel, buttons;
        if (btkbd_mouse(&dx, &dy, &wheel, &buttons)) {
            if (dx || dy || wheel) {
                s_last_dx = dx;
                s_last_dy = dy;
                s_last_wheel = wheel;
            }
            if ((dx < 0 ? -dx : dx) > s_max_dx) s_max_dx = dx < 0 ? -dx : dx;
            if ((dy < 0 ? -dy : dy) > s_max_dy) s_max_dy = dy < 0 ? -dy : dy;
            s_total_x += dx;
            s_total_y += dy;
            s_acc_x += dx;
            s_acc_y += dy;
            s_wheel_total += wheel;
            s_buttons_was = s_buttons;
            s_buttons = buttons;
            int pressed = buttons & ~s_buttons_was;
            if (pressed) printf("buttons %d at %d,%d\n", buttons, px(), py());

            if (s_page == PAGE_B) {
                if (pressed) {
                    s_marks[s_next_mark].x = (int16_t)px();
                    s_marks[s_next_mark].y = (int16_t)py();
                    s_marks[s_next_mark].colour = pointer_colour();
                    s_next_mark = (s_next_mark + 1) % MARKS;
                    if (s_n_marks < MARKS) s_n_marks++;
                    paint(px() - 3, py() - 3, 7, 7);
                }
                if (wheel) paint(BAR_X, B_TOP, PANEL_W - BAR_X, PANEL_H - B_TOP);
                if (pressed || (buttons != s_buttons_was)) paint_pointer(px(), py());
            } else if (s_page == PAGE_C) {
                if (wheel) c_wheel(wheel);
                for (int i = 0; i < 3; i++)
                    if (pressed & (1 << i)) c_click(i);
            }
        }
        if (now >= s_move_at) {
            s_move_at = now + MOVE_US;
            int ox = px(), oy = py();
            move_window();
            if (px() != ox || py() != oy) {
                if (s_page == PAGE_B) {
                    paint_pointer(ox, oy);
                    paint_pointer(px(), py());
                    page_b_status();
                } else if (s_page == PAGE_C) {
                    c_move_pointer();
                }
            }
        }

        if (now >= next_rate) {
            next_rate = now + 1000000u;
            unsigned reports;
            int x, y, w, b;
            btkbd_pointer(&reports, &x, &y, &w, &b);
            rate = reports - rate_base;
            rate_base = reports;
        }
        if (s_page == PAGE_A && now >= next_text) {
            next_text = now + TEXT_US;
            page_a(rate);
        }

        /* the keys: the Bluetooth keyboard's, the PicoCalc's, and UART0's */
        uint16_t e;
        while ((e = btkbd_read()) != 0) {
            int k = keys_event(&s_bt, (uint8_t)(e >> 8), (uint8_t)e);
            if (k > 0) key(k);
        }
        if (now >= next_kbd) {
            next_kbd = now + KBD_POLL_US;
            kbd_poll();
            uint8_t state, code;
            while (kbd_pop(&state, &code)) {
                int k = keys_event(&s_pc, state, code);
                if (k > 0) key(k);
            }
            int c = getchar_timeout_us(0);
            if (c > 0 && c < 0x80) key(c);
        }
    }
}
