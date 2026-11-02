/* mirror.h — a copy of the panel's transactions to a Tang Nano 20K, which
 * shows the 320x320 screen on HDMI (design.md §3.11; hardware-notes.md
 * §12).
 *
 * From pico_tang20k_hdmi's firmware/picocalc-text-starter/drivers/mirror.[ch]
 * (the owner's, over Blair Leduc's MIT starter): the link and its packets
 * as they are there. Left out: the link test, the pause for measuring and
 * the clock change, which the shell does not use. Added: the stationary
 * read of mirror_data16_fill() (lcd_fill sends one pixel w*h times), and
 * a PIO and a state machine taken wherever the board has one free.
 *
 * Packets, each framed by CS_N on GP28: 0x00 and a command byte, 0x01 and
 * data (RGB565 MSB first, as on the panel's wire), 0x80 and a control
 * byte. Only lcd.c calls this, from thread context, so a packet is never
 * cut by another; an interrupt only pauses SCK, which the FPGA waits out.
 *
 * Built with PICO_SHELL_HDMI, this is the link; without it every call is
 * nothing and the shell is as it was. */
#ifndef SHELL_MIRROR_H
#define SHELL_MIRROR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if PICO_SHELL_HDMI

/* 1 MiB with no errors on loose wires; 20 MHz failed there (pico_tang20k_hdmi, phase 2). */
#define MIRROR_HZ 15000000u

/* GP20 high, GP28 high, the PIO set up, and a reset packet to the FPGA. */
void mirror_init(void);
bool mirror_active(void);
uint32_t mirror_hz(void);

void mirror_cmd(uint8_t cmd);
void mirror_data(const uint8_t *buf, size_t n);
/* Open a data packet and start the DMA for n halfwords, MSB first, and
 * return; mirror_wait() finishes the packet. The fill reads one halfword
 * n times. */
void mirror_data16_start(const uint16_t *buf, size_t n);
void mirror_data16_fill(const uint16_t *px, size_t n);
void mirror_wait(void);

/* CTRL 0x01: the FPGA's decoder back to its power-on state. */
void mirror_ctrl_resync(void);

#else

static inline void mirror_init(void) {}
static inline bool mirror_active(void) { return false; }
static inline uint32_t mirror_hz(void) { return 0; }
static inline void mirror_cmd(uint8_t cmd) { (void)cmd; }
static inline void mirror_data(const uint8_t *buf, size_t n) { (void)buf; (void)n; }
static inline void mirror_data16_start(const uint16_t *buf, size_t n) { (void)buf; (void)n; }
static inline void mirror_data16_fill(const uint16_t *px, size_t n) { (void)px; (void)n; }
static inline void mirror_wait(void) {}
static inline void mirror_ctrl_resync(void) {}

#endif

#endif /* SHELL_MIRROR_H */
