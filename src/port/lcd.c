/* lcd.c — ST7789P/ST7365P-class 320x320 panel on spi1 (hardware-notes.md §4).
 *
 * From pico-oric's src/port/lcd.c (GPL-3.0, the owner's). Added for the
 * terminal: hardware vertical scroll (VSCRDEF, VSCSAD) with the offset
 * applied in lcd_window alone (§4.8), and a fill split at the wrap. The
 * scroll band is the whole of frame memory, 0/480/0, as Pico Logo's
 * lcd.c sets it; the starter set a 320-row band and wrapped at 480.
 *
 * The init sequence's gamma, power, VCOM, frame-rate and manufacturer
 * commands are ClockworkPi's, from Code/picocalc_helloworld/lcdspi/lcdspi.c
 * at clockworkpi/PicoCalc f91519806d4b2e0a62c4638a9f695cd5162c5479
 * (hardware-notes.md §4.4, §11: record the revision used). That driver
 * runs the panel at 18 bpp; the pixel format and entry mode here are the
 * RGB565 pair hardware-notes.md §4.4 records as known-working instead.
 *
 * With PICO_SHELL_HDMI every command, parameter and pixel sent here is also
 * sent to mirror.c, which copies it to an FPGA for HDMI (design.md §3.11).
 * The calls are where pico_tang20k_hdmi's lcd.c has them; without it they
 * are nothing.
 */

#include "lcd.h"

#include <stddef.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "config.h"
#include "mirror.h"

/* hardware-notes.md §1.1. CS, D/CX and RST are plain GPIOs: the SPI
 * peripheral does not manage chip select here, and that matters (§4.3). */
#define LCD_SPI      spi1
#define LCD_PIN_SCK  10
#define LCD_PIN_TX   11
#define LCD_PIN_RX   12
#define LCD_PIN_CS   13
#define LCD_PIN_DCX  14
#define LCD_PIN_RST  15

/* 75 MHz, the full rate the divider delivers from a 150 MHz clk_peri: the
 * owner's decision after M0 (design.md §4.1). It is above the cited
 * 62.5 MHz ST7789P limit and was exercised on hardware rather than
 * guaranteed (hardware-notes.md §4.2), so CMake's PICO_SHELL_LCD_SPI_HZ
 * can set the 25 MHz fallback. */
#ifndef PICO_SHELL_LCD_SPI_HZ
#define PICO_SHELL_LCD_SPI_HZ (75u * 1000u * 1000u)
#endif

/* Claimed explicitly, not left to allocation order (hardware-notes.md §10). */
#define LCD_DMA_CH 0

#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_INVON   0x21
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_VSCRDEF 0x33
#define CMD_MADCTL  0x36
#define CMD_VSCSAD  0x37
#define CMD_COLMOD  0x3A

#define MADCTL_VALUE 0x48   /* MX | BGR (§4.4) */

static dma_channel_config s_dma_cfg;
static bool s_dma_busy;
static unsigned s_scroll;   /* frame memory row at panel row 0 */

/* ---- low level --------------------------------------------------------- */

/* One command and its parameters, 8-bit, CS framed. spi_write_blocking
 * waits for the shifter to go idle and drains RX before it returns, so
 * D/CX may change between the command and its data. */
static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t n) {
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_put(LCD_PIN_DCX, 0);
    gpio_put(LCD_PIN_CS, 0);
    spi_write_blocking(LCD_SPI, &cmd, 1);
    if (n) {
        gpio_put(LCD_PIN_DCX, 1);
        spi_write_blocking(LCD_SPI, data, n);
    }
    gpio_put(LCD_PIN_CS, 1);
    mirror_cmd(cmd);
    if (n) mirror_data(data, n);
}

#define LCD_CMD(c, ...)                                                   \
    do {                                                                  \
        static const uint8_t d_[] = { __VA_ARGS__ };                      \
        lcd_cmd((c), d_, sizeof(d_));                                     \
    } while (0)

/* The only place a panel row becomes a frame memory row (§4.8). */
static void lcd_window(unsigned x, unsigned y, unsigned w, unsigned h) {
    y = (y + s_scroll) % SHELL_PANEL_MEM_H;
    unsigned x1 = x + w - 1u, y1 = y + h - 1u;
    uint8_t col[4] = { (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t row[4] = { (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    lcd_cmd(CMD_CASET, col, 4);
    lcd_cmd(CMD_RASET, row, 4);
    lcd_cmd(CMD_RAMWR, NULL, 0);
}

/* Open the RAM write for 16-bit pixel data. */
static void lcd_pixels_begin(void) {
    /* DO NOT MOVE. These two lines must precede the CS low, not follow
     * it: they are what creates the required 40 ns CS-high interval
     * (hardware-notes.md §4.3). */
    spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_put(LCD_PIN_DCX, 1);
    gpio_put(LCD_PIN_CS, 0);
}

static void lcd_dma_wait(void) {
    /* The mirror's copy of the row is slower than the panel's, and the
     * caller refills the buffer the moment this returns: wait for both. */
    mirror_wait();
    if (s_dma_busy) {
        /* Polled, not an interrupt: the next row is useful work to do
         * while waiting, and the audio engine owns its DMA IRQ
         * uncontended (hardware-notes.md §4.6). */
        dma_channel_wait_for_finish_blocking(LCD_DMA_CH);
        s_dma_busy = false;
    }
}

/* Close the RAM write. The DMA only fills the TX FIFO; the last pixels
 * are still on the wire when it finishes (hardware-notes.md §4.6). */
static void lcd_pixels_end(void) {
    lcd_dma_wait();
    spi_hw_t *hw = spi_get_hw(LCD_SPI);
    while (hw->sr & SPI_SSPSR_BSY_BITS) tight_loop_contents();
    while (spi_is_readable(LCD_SPI)) (void)hw->dr;
    hw->icr = SPI_SSPICR_RORIC_BITS;
    gpio_put(LCD_PIN_CS, 1);
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

static void lcd_dma_start(const uint16_t *src, uint32_t count, bool read_inc) {
    channel_config_set_read_increment(&s_dma_cfg, read_inc);
    dma_channel_configure(LCD_DMA_CH, &s_dma_cfg,
                          &spi_get_hw(LCD_SPI)->dr, src, count, true);
    s_dma_busy = true;
    if (read_inc) mirror_data16_start(src, count);
    else mirror_data16_fill(src, count);
}

/* ---- public ------------------------------------------------------------ */

uint32_t lcd_reapply_baud(void) {
    return spi_set_baudrate(LCD_SPI, PICO_SHELL_LCD_SPI_HZ);
}

uint32_t lcd_init(void) {
    gpio_init(LCD_PIN_CS);
    gpio_init(LCD_PIN_DCX);
    gpio_init(LCD_PIN_RST);
    gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
    gpio_set_dir(LCD_PIN_DCX, GPIO_OUT);
    gpio_set_dir(LCD_PIN_RST, GPIO_OUT);
    gpio_put(LCD_PIN_CS, 1);
    gpio_put(LCD_PIN_RST, 1);

    uint32_t baud = spi_init(LCD_SPI, PICO_SHELL_LCD_SPI_HZ);
    gpio_set_function(LCD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_TX, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_RX, GPIO_FUNC_SPI);

    dma_channel_claim(LCD_DMA_CH);
    s_dma_cfg = dma_channel_get_default_config(LCD_DMA_CH);
    channel_config_set_transfer_data_size(&s_dma_cfg, DMA_SIZE_16);
    channel_config_set_dreq(&s_dma_cfg, spi_get_dreq(LCD_SPI, true));
    channel_config_set_write_increment(&s_dma_cfg, false);

    /* After the claim above: the mirror takes the first free channel, which
     * would be this one, and claiming it twice stops the program. Nothing
     * unless built with PICO_SHELL_HDMI. */
    mirror_init();

    /* Hardware reset: low for at least 10 us, 120 ms after release, then
     * software reset (hardware-notes.md §4.4). The 120 ms after SWRESET is
     * longer than the 5 ms minimum; it is the controller's own wait
     * before SLPOUT, and boot can afford it. */
    sleep_ms(10);
    gpio_put(LCD_PIN_RST, 0);
    sleep_ms(10);
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(120);
    lcd_cmd(CMD_SWRESET, NULL, 0);
    sleep_ms(120);

    /* ClockworkPi's panel settings (see the file header). */
    LCD_CMD(0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
                  0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F);   /* +gamma   */
    LCD_CMD(0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
                  0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F);   /* -gamma   */
    LCD_CMD(0xC0, 0x17, 0x15);                  /* power control 1        */
    LCD_CMD(0xC1, 0x41);                        /* power control 2        */
    LCD_CMD(0xC5, 0x00, 0x12, 0x80);            /* VCOM                   */
    LCD_CMD(CMD_MADCTL, MADCTL_VALUE);            /* (§4.4)                 */
    LCD_CMD(CMD_COLMOD, 0x55);                  /* RGB565 (§4.4)          */
    LCD_CMD(0xB0, 0x00);                        /* interface mode         */
    LCD_CMD(0xB1, 0xA0);                        /* frame rate             */
    lcd_cmd(CMD_INVON, NULL, 0);
    LCD_CMD(0xB4, 0x02);                        /* inversion control      */
    LCD_CMD(0xB6, 0x02, 0x02, 0x3B);            /* display function       */
    LCD_CMD(0xB7, 0x06);                        /* entry mode (§4.4, §4.5) */
    LCD_CMD(0xE9, 0x00);
    LCD_CMD(0xF7, 0xA9, 0x51, 0x2C, 0x82);      /* adjust control 3       */

    /* One scroll band over all 480 rows of frame memory, no fixed areas,
     * starting at row 0 (§4.4: initialise the scroll registers). */
    LCD_CMD(CMD_VSCRDEF, 0x00, 0x00, SHELL_PANEL_MEM_H >> 8, SHELL_PANEL_MEM_H & 0xFF, 0x00, 0x00);
    s_scroll = 0;
    lcd_scroll(0);

    lcd_cmd(CMD_SLPOUT, NULL, 0);
    sleep_ms(120);

    /* Clear visible frame memory before DISPON, so startup does not show
     * uninitialised pixels (hardware-notes.md §4.4). */
    lcd_fill(0, 0, SHELL_PANEL_W, SHELL_PANEL_H, 0x0000);
    lcd_cmd(CMD_DISPON, NULL, 0);
    sleep_ms(20);

    return baud;
}

void lcd_scroll(int lines) {
    int m = (int)SHELL_PANEL_MEM_H;
    s_scroll = (unsigned)((((int)s_scroll + lines) % m + m) % m);
    uint8_t start[2] = { (uint8_t)(s_scroll >> 8), (uint8_t)s_scroll };
    lcd_cmd(CMD_VSCSAD, start, 2);
}

void lcd_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t rgb565) {
    /* The DMA reads this one halfword w*h times; lcd_pixels_end waits it
     * out, so a local outlives the transfer. */
    uint16_t colour = rgb565;
    if (!w || !h) return;
    /* A window cannot wrap: fill the part past the end of frame memory
     * as a second rectangle. */
    unsigned top = (y + s_scroll) % SHELL_PANEL_MEM_H;
    if (top + h > SHELL_PANEL_MEM_H) {
        unsigned first = SHELL_PANEL_MEM_H - top;
        lcd_fill(x, y, w, first, rgb565);
        lcd_fill(x, y + first, w, h - first, rgb565);
        return;
    }
    lcd_window(x, y, w, h);
    lcd_pixels_begin();
    lcd_dma_start(&colour, (uint32_t)w * h, false);
    lcd_pixels_end();
}

void lcd_blit_begin(unsigned x, unsigned y, unsigned w, unsigned h) {
    lcd_window(x, y, w, h);
    lcd_pixels_begin();
}

void lcd_blit_row(const uint16_t *px, unsigned n) {
    lcd_dma_wait();
    lcd_dma_start(px, n, true);
}

void lcd_blit_end(void) {
    lcd_pixels_end();
}

/* CTRL 0x01 puts the FPGA's decoder back to its power-on values, but the
 * panel keeps its own, so what was sent to the panel is sent again: the
 * orientation, the inversion, the scroll band and offset, and display on.
 * They are the ones lcd_init sets, with the offset as it is now. The
 * picture is not sent: the caller redraws it. */
void lcd_mirror_resync(void) {
    if (!mirror_active()) return;
    mirror_ctrl_resync();
    uint8_t madctl[1] = { MADCTL_VALUE };
    uint8_t band[6] = { 0x00, 0x00, SHELL_PANEL_MEM_H >> 8, SHELL_PANEL_MEM_H & 0xFF, 0x00, 0x00 };
    uint8_t start[2] = { (uint8_t)(s_scroll >> 8), (uint8_t)s_scroll };
    mirror_cmd(CMD_MADCTL);
    mirror_data(madctl, 1);
    mirror_cmd(CMD_INVON);
    mirror_cmd(CMD_VSCRDEF);
    mirror_data(band, 6);
    mirror_cmd(CMD_VSCSAD);
    mirror_data(start, 2);
    mirror_cmd(CMD_DISPON);
}
