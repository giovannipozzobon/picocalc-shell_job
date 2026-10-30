/* cyw43_fw.c — the firmware's file (cyw43_fw.h). */
#include "cyw43_fw.h"

#include "cyw43_fw_stream.h"
#include "fat.h"

static FIL      s_fw;
static bool     s_open;
static uint32_t s_pos;   /* where the next read of the file begins */
static unsigned s_reads;
static long     s_failed = -1;

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
    s_failed = -1;
    return NULL;
}

void cyw43_fw_close(void) {
    if (s_open) f_close(&s_fw);
    s_open = false;
}

void cyw43_fw_stats(unsigned *reads, long *failed_at) {
    *reads = s_reads;
    *failed_at = s_failed;
}

int cyw43_fw_read(uint32_t offset, void *dst, size_t len) {
    s_reads++;
    if (!s_open) {
        s_failed = (long)offset;
        return -1;
    }
    if (offset != s_pos && f_lseek(&s_fw, offset) != FR_OK) {
        s_failed = (long)offset;
        return -1;
    }
    UINT got = 0;
    if (f_read(&s_fw, dst, (UINT)len, &got) != FR_OK || got != len) {
        s_pos = UINT32_MAX;   /* where it is is not known */
        s_failed = (long)offset;
        return -1;
    }
    s_pos = offset + (uint32_t)len;
    return 0;
}
