/* mirror.c — the QSPI link to the FPGA (mirror.h).
 *
 * From pico_tang20k_hdmi's drivers/mirror.c (the owner's; see mirror.h for
 * what differs). Single bytes go through the PIO's FIFO by hand (autopull
 * at 8 bits); pixels go by DMA at 16 bits, so each halfword leaves high
 * byte first as on the panel's wire. The change between the two is made
 * with the state machine stopped, inside the same packet, after the type
 * byte. */

#include "mirror.h"

#if PICO_SHELL_HDMI

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "mirror.pio.h"

#define MIRROR_D0  2    /* GP2..GP5 = D0..D3 (J703 pins 2-5) */
#define MIRROR_SCK 21   /* J703 pin 6 */
#define MIRROR_CSN 28   /* J703 pin 7 */
#define PSRAM_CS   20   /* kept HIGH: the PicoCalc's PSRAM is not used */

#define T_CMD  0x00
#define T_DATA 0x01
#define T_CTRL 0x80

static PIO s_pio;
static uint s_sm, s_off;
static int s_dma = -1;
static bool s_ready;
static bool s_pixel_mode;

static void sm_config(bool pixel) {
    pio_sm_config c = mirror_qspi_program_get_default_config(s_off);
    sm_config_set_out_pins(&c, MIRROR_D0, 4);
    sm_config_set_sideset_pins(&c, MIRROR_SCK);
    sm_config_set_out_shift(&c, false, true, pixel ? 16 : 8);   /* MSB first, autopull */
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (2.0f * (float)MIRROR_HZ));
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_init(s_pio, s_sm, s_off, &c);   /* empties the FIFO, restarts at .wrap_target */
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_pixel_mode = pixel;
}

/* The state machine stalls on an empty FIFO: the last nibble is out and SCK is low. */
static void drain(void) {
    while (!pio_sm_is_tx_fifo_empty(s_pio, s_sm)) tight_loop_contents();
    s_pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + s_sm);
    while (!(s_pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + s_sm)))) tight_loop_contents();
}

static inline void put_byte(uint8_t b) {
    pio_sm_put_blocking(s_pio, s_sm, (uint32_t)b << 24);
}

static void cs_low(void) {
    gpio_put(MIRROR_CSN, 0);
}

static void cs_high(void) {
    gpio_put(MIRROR_CSN, 1);
    busy_wait_at_least_cycles(clock_get_hz(clk_sys) / 4000000u + 1u);   /* 250 ns between packets */
}

void mirror_init(void) {
    if (s_ready) return;

    gpio_init(PSRAM_CS);
    gpio_set_dir(PSRAM_CS, GPIO_OUT);
    gpio_put(PSRAM_CS, 1);

    gpio_init(MIRROR_CSN);
    gpio_put(MIRROR_CSN, 1);
    gpio_set_dir(MIRROR_CSN, GPIO_OUT);

    /* Any PIO with room, as the CYW43 driver takes one too. GP2..GP21. */
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&mirror_qspi_program, &s_pio, &s_sm,
                                                          &s_off, MIRROR_D0, MIRROR_SCK - MIRROR_D0 + 1, true))
        return;
    for (uint p = MIRROR_D0; p < MIRROR_D0 + 4; p++) pio_gpio_init(s_pio, p);
    pio_gpio_init(s_pio, MIRROR_SCK);
    pio_sm_set_pins_with_mask(s_pio, s_sm, 0, (0xFu << MIRROR_D0) | (1u << MIRROR_SCK));
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, (0xFu << MIRROR_D0) | (1u << MIRROR_SCK),
                                 (0xFu << MIRROR_D0) | (1u << MIRROR_SCK));
    /* Slow edges: loose wires with no series resistors ring. */
    for (uint p = MIRROR_D0; p < MIRROR_D0 + 4; p++) {
        gpio_set_drive_strength(p, GPIO_DRIVE_STRENGTH_2MA);
        gpio_set_slew_rate(p, GPIO_SLEW_RATE_SLOW);
    }
    gpio_set_drive_strength(MIRROR_SCK, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_slew_rate(MIRROR_SCK, GPIO_SLEW_RATE_SLOW);
    gpio_set_drive_strength(MIRROR_CSN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_slew_rate(MIRROR_CSN, GPIO_SLEW_RATE_SLOW);

    s_dma = dma_claim_unused_channel(true);
    sm_config(false);
    s_ready = true;

    mirror_ctrl_resync();
}

bool mirror_active(void) { return s_ready; }
uint32_t mirror_hz(void) { return MIRROR_HZ; }

static void packet2(uint8_t type, uint8_t b) {
    cs_low();
    put_byte(type);
    put_byte(b);
    drain();
    cs_high();
}

void mirror_ctrl_resync(void) {
    if (!s_ready) return;
    packet2(T_CTRL, 0x01);
}

void mirror_cmd(uint8_t cmd) {
    if (!s_ready) return;
    packet2(T_CMD, cmd);
}

void mirror_data(const uint8_t *buf, size_t n) {
    if (!s_ready || n == 0) return;
    cs_low();
    put_byte(T_DATA);
    for (size_t i = 0; i < n; i++) put_byte(buf[i]);
    drain();
    cs_high();
}

static void data16(const uint16_t *buf, size_t n, bool step) {
    if (!s_ready || n == 0) return;
    cs_low();
    put_byte(T_DATA);
    drain();
    sm_config(true);   /* 16-bit autopull; SCK stays low */

    dma_channel_config dc = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
    channel_config_set_read_increment(&dc, step);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(s_pio, s_sm, true));
    dma_channel_configure(s_dma, &dc, &s_pio->txf[s_sm], buf, n, true);
}

void mirror_data16_start(const uint16_t *buf, size_t n) { data16(buf, n, true); }
void mirror_data16_fill(const uint16_t *px, size_t n) { data16(px, n, false); }

void mirror_wait(void) {
    if (!s_ready || !s_pixel_mode) return;
    dma_channel_wait_for_finish_blocking(s_dma);
    drain();
    cs_high();
    sm_config(false);   /* back to bytes */
}

#endif /* PICO_SHELL_HDMI */
