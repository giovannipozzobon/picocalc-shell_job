/* btstore.c — the pairing's file (btstore.h). Adapted from picocalc-extras'
 * btkbd.c (MIT, Chris Torkildson), whose records and file it keeps. */
#include "btstore.h"

#include <string.h>

#include "fat.h"

/* btpair 1.0's file: 16 records of 48 bytes. */
#define OLD_MAGIC 0x314B4250u   /* "PBK1" */
#define OLD_COUNT 16
#define OLD_DATA  48

typedef struct {
    uint32_t tag;
    uint32_t len;
    uint8_t  data[OLD_DATA];
} old_record_t;

bt_store_load_t bt_store_load(bt_store_t *s) {
    /* The file may be either size, so it is read into the larger of the
     * two and told by its magic and its length. */
    static union {
        bt_store_t   cur;
        uint8_t      raw[sizeof(bt_store_t) + 1];
    } buf;
    UINT got = 0;
    bool found = false;
    memset(&buf, 0, sizeof buf);
    FIL f;
    if (f_open(&f, BT_STORE_PATH, FA_READ) == FR_OK) {
        found = true;
        if (f_read(&f, buf.raw, sizeof buf.raw, &got) != FR_OK) got = 0;
        f_close(&f);
    }
    memset(s, 0, sizeof *s);
    s->magic = BT_STORE_MAGIC;
    if (!found) return BT_STORE_NEW;

    if (got >= 4u + OLD_COUNT * sizeof(old_record_t) && buf.cur.magic == OLD_MAGIC) {
        /* carried over, to be saved in the new form */
        const old_record_t *old = (const old_record_t *)(buf.raw + 4);
        for (int i = 0; i < OLD_COUNT; i++) {
            s->r[i].tag = old[i].tag;
            s->r[i].len = old[i].len <= OLD_DATA ? old[i].len : OLD_DATA;
            memcpy(s->r[i].data, old[i].data, OLD_DATA);
        }
        return BT_STORE_UPGRADED;
    }
    if (got != sizeof *s || buf.cur.magic != BT_STORE_MAGIC) return BT_STORE_BAD;
    *s = buf.cur;
    return BT_STORE_OK;
}

const char *bt_store_save(const bt_store_t *s) {
    static files_safe_t w;
    FRESULT r = f_mkdir(BT_STORE_DIR);
    if (r != FR_OK && r != FR_EXIST) return files_why(r);
    const char *why = files_safe_open(&w, BT_STORE_PATH);
    if (why) return why;
    files_safe_write(&w, s, sizeof *s);
    return files_safe_commit(&w);
}

bt_record_t *bt_store_find(bt_store_t *s, uint32_t tag) {
    for (int i = 0; i < BT_TLV_MAX; i++)
        if (s->r[i].tag == tag) return &s->r[i];
    return NULL;
}

int bt_store_get(const bt_store_t *s, uint32_t tag, uint8_t *buf, uint32_t size) {
    const bt_record_t *r = bt_store_find((bt_store_t *)s, tag);
    if (!r || tag == 0) return 0;
    uint32_t n = r->len < size ? r->len : size;
    if (buf) memcpy(buf, r->data, n);
    return (int)r->len;
}

int bt_store_put(bt_store_t *s, uint32_t tag, const uint8_t *data, uint32_t size) {
    if (size > BT_TLV_DATA || tag == 0) return 1;
    bt_record_t *r = bt_store_find(s, tag);
    if (!r) r = bt_store_find(s, 0);
    if (!r) return 1;
    r->tag = tag;
    r->len = size;
    memset(r->data, 0, sizeof r->data);
    memcpy(r->data, data, size);
    return 0;
}

bool bt_store_delete(bt_store_t *s, uint32_t tag) {
    bt_record_t *r = tag ? bt_store_find(s, tag) : NULL;
    if (!r) return false;
    memset(r, 0, sizeof *r);
    return true;
}
