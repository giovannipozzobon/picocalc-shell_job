/* btkbd.h -- a Bluetooth keyboard for the PicoCalc (Pico 2 W)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Torkildson. Taken from picocalc-extras for the
 * shell (THIRD-PARTY.md).
 *
 * Keys typed on a Bluetooth keyboard come out of btkbd_read() in the same
 * form as the PicoCalc's own keyboard gives them from sb_read_keyboard():
 * (state << 8) | code, with KEY_STATE_PRESSED or KEY_STATE_RELEASED and the
 * codes in the text starter's keyboard.h. Ctrl, Shift and Alt come as
 * their own presses and releases (KEY_MOD_CTRL and so on), letters in
 * lower case, other characters already shifted -- just as the PicoCalc
 * sends them -- so a program can mix the two keyboards in one loop.
 *
 * In the shell, kbd.c takes them from btkbd_read() into the ring the
 * southbridge's keys go in (design.md §3.10).
 *
 * Both kinds of keyboard work: "classic" Bluetooth ones, and Bluetooth Low
 * Energy (BLE) ones, which speak HID over GATT.
 *
 * Bluetooth runs in the background. Pairing is done with the btpair
 * program; other programs only reconnect, and needn't call btkbd_poll().
 *
 * The pairing is kept on the SD card, in /bluetooth/pairing.dat (btstore.h),
 * so every program that uses this finds the same keyboard. The radio's
 * firmware is read from /bluetooth/cyw43-fw.bin (cyw43_fw.h).
 *
 *     files_mount();
 *     btkbd_init();   // false, with btkbd_status() saying why, if the
 *                     // firmware file is not there or the radio does not start
 */
#ifndef BTKBD_H
#define BTKBD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
	BTKBD_OFF,		/* the radio didn't start */
	BTKBD_STARTING,
	BTKBD_IDLE,		/* no keyboard paired */
	BTKBD_WAITING,		/* paired: waiting for the keyboard to connect */
	BTKBD_SEARCHING,
	BTKBD_PAIRING,		/* connecting to a keyboard chosen from the search */
	BTKBD_CONNECTED,
} btkbd_state_t;

/* start the radio; reconnects to the keyboard paired before, if any */
bool btkbd_init(void);
/* save a new pairing to the card: call it now and then while pairing */
void btkbd_poll(void);
/* the next key event from the Bluetooth keyboard alone, or 0 */
uint16_t btkbd_read(void);

btkbd_state_t btkbd_state(void);
/* a short line saying what's happening, for the screen */
const char *btkbd_status(void);
/* the name of the paired keyboard, or "" */
const char *btkbd_name(void);

/* pairing: search for keyboards in pairing mode (about 10 seconds) */
void btkbd_search(void);
int btkbd_found(void);			/* how many found so far */
const char *btkbd_found_name(int i);
bool btkbd_found_le(int i);		/* a Bluetooth Low Energy keyboard */
void btkbd_pair(int i);
/* while pairing: what to type on the keyboard (then Enter), or NULL */
const char *btkbd_code(void);
/* a line of details for testing */
const char *btkbd_diag(void);
const char *btkbd_trace(void);	/* the last few Bluetooth events */
const char *btkbd_trace_line(int line);	/* the last 24, in two lines: 0 the older, 1 the newer */
const char *btkbd_smp(int i);	/* BLE pairing and L2CAP signalling messages, 0-9: "out"/"in" and the bytes */
/* built with BTKBD_LOG: every Bluetooth packet since starting, in PacketLogger
   format (Wireshark reads it), and how many didn't fit; otherwise none */
const uint8_t *btkbd_log(size_t *len, uint32_t *lost);
/* a trackpad or mouse on the keyboard, for testing: how many reports have
   come, the movement added up since the start, and the buttons held now */
void btkbd_pointer(unsigned *reports, int *x, int *y, int *wheel, int *buttons);
/* forget the paired keyboard */
void btkbd_forget(void);

#endif
