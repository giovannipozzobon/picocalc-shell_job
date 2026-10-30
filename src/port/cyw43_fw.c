/* cyw43_fw.c — the firmware's file (cyw43_fw.h). */
#include "cyw43_fw.h"

#include <string.h>

#include "cyw43_fw_stream.h"
#include "fat.h"

static FIL      s_fw;
static bool     s_open;
static uint32_t s_pos;   /* where the next read of the file begins */
static unsigned s_reads;
static unsigned s_fills;
static long     s_failed = -1;

/* The driver asks for 64 bytes at a time, in order; the card is read in
 * pieces of this, which FatFs takes in one multi-sector transfer. */
#define AHEAD 4096u
static uint8_t  s_ahead[AHEAD] __attribute__((aligned(4)));
static uint32_t s_ahead_at;      /* the file's offset of s_ahead[0] */
static uint32_t s_ahead_len;     /* what it holds */

const char *cyw43_fw_open(void) {
    cyw43_fw_close();
    files_mount();
    FRESULT r = f_open(&s_fw, CYW43_FW_PATH, FA_READ);
    if (r != FR_OK) return r == FR_NO_FILE || r == FR_NO_PATH ? "no firmware file on the card" : files_why(r);
    if (f_size(&s_fw) != CYW43_FW_FILE_LEN) {
        f_close(&s_fw);
        return "the firmware file is not this program's";
    }
    s_open = true;
    s_pos = 0;
    s_reads = 0;
    s_fills = 0;
    s_ahead_len = 0;
    s_failed = -1;
    return NULL;
}

void cyw43_fw_close(void) {
    if (s_open) f_close(&s_fw);
    s_open = false;
}

void cyw43_fw_stats(unsigned *reads, unsigned *fills, long *failed_at) {
    *reads = s_reads;
    *fills = s_fills;
    *failed_at = s_failed;
}

/* Fill the read-ahead from `offset`: whether it holds what was asked. */
static bool fill(uint32_t offset) {
    if (offset != s_pos && f_lseek(&s_fw, offset) != FR_OK) return false;
    UINT got = 0;
    s_ahead_len = 0;
    if (f_read(&s_fw, s_ahead, AHEAD, &got) != FR_OK || got == 0) {
        s_pos = UINT32_MAX;   /* where it is is not known */
        return false;
    }
    s_ahead_at = offset;
    s_ahead_len = got;
    s_pos = offset + got;
    s_fills++;
    return true;
}

int cyw43_fw_read(uint32_t offset, void *dst, size_t len) {
    s_reads++;
    uint8_t *out = dst;
    if (!s_open) {
        s_failed = (long)offset;
        return -1;
    }
    while (len > 0) {
        if (offset < s_ahead_at || offset >= s_ahead_at + s_ahead_len) {
            if (!fill(offset)) {
                s_failed = (long)offset;
                return -1;
            }
        }
        uint32_t n = s_ahead_at + s_ahead_len - offset;
        if (n > len) n = (uint32_t)len;
        memcpy(out, s_ahead + (offset - s_ahead_at), n);
        out += n;
        offset += n;
        len -= n;
    }
    return 0;
}
