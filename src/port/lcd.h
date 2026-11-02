/* lcd.h — ST7789P/ST7365P-class 320x320 panel on spi1 (hardware-notes.md §4).
 *
 * From pico-oric's src/port/lcd.h (GPL-3.0, the owner's), with hardware
 * vertical scroll added for the terminal (design.md §4.1; §4.8).
 *
 * Only ever touched from thread context, never from an interrupt handler.
 * Nothing here masks interrupts: the controller tolerates SPI clock
 * pauses mid-window.
 *
 * Every y here is a panel row, 0-319. The window setup alone maps it
 * through the scroll offset into frame memory, wrapping at 480 rows
 * (hardware-notes.md §4.8), so no caller sees the offset.
 */
#ifndef SHELL_LCD_H
#define SHELL_LCD_H

#include <stdbool.h>
#include <stdint.h>

/* Reset, initialise, set the whole of frame memory as one scroll band at
 * offset 0, clear the panel, then DISPON (hardware-notes.md §4.4, §10).
 * Call after board_init_clocks(): the baud rate is derived from clk_peri.
 * Returns the SPI rate actually configured, which is a configured figure,
 * not one measured on SCK (§4.2). */
uint32_t lcd_init(void);

/* Re-apply the SPI baud rate. Required after any clk_sys change, because
 * the divider is computed from clk_peri at the time it is set (§3). */
uint32_t lcd_reapply_baud(void);

/* Move the panel's view of frame memory down by `lines` pixel rows (up if
 * negative), modulo 480: what was at panel row y is then at y - lines.
 * One command, no pixels sent (§4.8). */
void lcd_scroll(int lines);

/* Fill a rectangle with one colour, by DMA with the read address held.
 * Blocks until the wire is idle. */
void lcd_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t rgb565);

/* The ping-pong pattern of hardware-notes.md §4.6(b):
 *
 *     lcd_blit_begin(x, y, w, h);
 *     for each row: fill a line buffer, lcd_blit_row(buf, w);
 *     lcd_blit_end();
 *
 * lcd_blit_row waits for the previous row's DMA to finish before starting
 * this one, so with two line buffers the caller always fills the one that
 * is not on the wire. Sending the same buffer twice in a row is allowed:
 * a blit may repeat a row without refilling it.
 *
 * A blit is one window, so it must not cross the wrap in frame memory.
 * The terminal's text rows never do: the offset moves in whole 10-pixel
 * rows and 480 is a whole number of them. */
void lcd_blit_begin(unsigned x, unsigned y, unsigned w, unsigned h);
void lcd_blit_row(const uint16_t *px, unsigned n);
void lcd_blit_end(void);

/* Send the FPGA that mirrors the panel (mirror.h) the panel's state again:
 * a reset, the orientation, inversion, scroll and display on. For a link
 * the FPGA joined late. It sends no picture, so redraw afterwards.
 * Nothing in a build without PICO_SHELL_HDMI. */
void lcd_mirror_resync(void);

#endif /* SHELL_LCD_H */
