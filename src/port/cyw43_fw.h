/* cyw43_fw.h — the CYW43439's firmware, read from the card (design.md
 * §3.10).
 *
 * A program built with cmake/cyw43_fw.cmake does not hold the radio's
 * firmware in RAM: cyw43_ll.c reads it, a block at a time, through
 * cyw43_fw_read() (cyw43_fw_stream.h), from /bluetooth/cyw43-fw.bin. The
 * file is built with the program, from the SDK's own array, and is copied
 * to the card by the user. It is open only while the radio starts. */
#ifndef SHELL_CYW43_FW_H
#define SHELL_CYW43_FW_H

#define CYW43_FW_PATH "/bluetooth/cyw43-fw.bin"

/* Open the file, before cyw43_arch_init(): NULL, or why it cannot be
 * used (no card, not there, or not the length this program was built
 * with, which means it is from another build). */
const char *cyw43_fw_open(void);

/* Close it, once the chip is up. cyw43_arch_init() does not load the
 * firmware: the driver does, at the first thing done to the chip (a
 * Wi-Fi call such as cyw43_arch_enable_sta_mode(), or Bluetooth's start),
 * so the file stays open until that has returned. */
void cyw43_fw_close(void);

/* What the driver has asked of the file since it was opened: the reads,
 * how many of them went to the card (the rest were in what it had read
 * ahead), and the offset of the last one that failed, -1 if none did. */
void cyw43_fw_stats(unsigned *reads, unsigned *fills, long *failed_at);

#endif /* SHELL_CYW43_FW_H */
