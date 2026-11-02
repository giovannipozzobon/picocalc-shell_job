/* test_btstore.c — the Bluetooth pairing's file (design.md §3.10, M14
 * step 3) over FAT16 and FAT32 images: its records, its format byte for
 * byte (it is picocalc-extras' btpair's, which reads what is saved here),
 * the older file it carries over, files that are not it, and a card that
 * fails while it is saved. */

#include <stdio.h>
#include <string.h>

#include "btstore.h"
#include "fat.h"
#include "host_disk.h"
#include "test_util.h"

static bt_store_t st;

/* ---- the records ------------------------------------------------------ */

static int test_records(void) {
    memset(&st, 0, sizeof st);
    st.magic = BT_STORE_MAGIC;
    static const uint8_t key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

    CHECK(bt_store_get(&st, 0x11, NULL, 0) == 0, "none there");

    /* A keyboard and a mouse, each its own record (btpair 2.1), the
     * classic and the BLE ones, and the one's removal leaves the other. */
    static const uint8_t pair[7] = { 1, 2, 3, 4, 5, 6, 'M' };
    CHECK(bt_store_put(&st, BT_TAG_KEYBOARD, pair, 7) == 0 && bt_store_put(&st, BT_TAG_MOUSE, pair, 7) == 0 &&
              bt_store_put(&st, BT_TAG_MOUSE_LE, pair, 7) == 0,
          "keyboard and mouse records");
    CHECK(bt_store_find(&st, BT_TAG_KEYBOARD) != bt_store_find(&st, BT_TAG_MOUSE) &&
              bt_store_find(&st, BT_TAG_MOUSE) != bt_store_find(&st, BT_TAG_MOUSE_LE),
          "three records");
    CHECK(bt_store_delete(&st, BT_TAG_MOUSE) && bt_store_get(&st, BT_TAG_KEYBOARD, NULL, 0) == 7 &&
              bt_store_get(&st, BT_TAG_MOUSE, NULL, 0) == 0 && bt_store_get(&st, BT_TAG_MOUSE_LE, NULL, 0) == 7,
          "forgetting the mouse leaves the keyboard");
    bt_store_delete(&st, BT_TAG_KEYBOARD);
    bt_store_delete(&st, BT_TAG_MOUSE_LE);
    CHECK(bt_store_put(&st, 0x11, key, sizeof key) == 0, "put");
    uint8_t got[32] = { 0 };
    CHECK(bt_store_get(&st, 0x11, got, sizeof got) == 16 && memcmp(got, key, 16) == 0, "get");
    CHECK(bt_store_get(&st, 0x11, NULL, 0) == 16, "the length, with no buffer");
    memset(got, 0xAA, sizeof got);
    CHECK(bt_store_get(&st, 0x11, got, 4) == 16 && memcmp(got, key, 4) == 0 && got[4] == 0xAA,
          "a small buffer takes what fits and the length is whole");

    /* A tag replaced, not repeated; the rest of its data cleared. */
    static const uint8_t two[2] = { 9, 9 };
    CHECK(bt_store_put(&st, 0x11, two, sizeof two) == 0 && bt_store_get(&st, 0x11, got, sizeof got) == 2,
          "replaced");
    int used = 0;
    for (int i = 0; i < BT_TLV_MAX; i++) used += st.r[i].tag != 0;
    CHECK(used == 1, "one record: %d", used);
    CHECK(st.r[0].data[2] == 0, "cleared");

    CHECK(bt_store_put(&st, 0, key, 4) == 1, "tag 0 is a free record");
    CHECK(bt_store_get(&st, 0, got, sizeof got) == 0, "and there is none by it");
    uint8_t big[BT_TLV_DATA + 1] = { 0 };
    CHECK(bt_store_put(&st, 0x22, big, sizeof big) == 1, "over the data size");
    CHECK(bt_store_put(&st, 0x22, big, BT_TLV_DATA) == 0, "at it");

    /* Full: 20 records. */
    for (uint32_t t = 100; t < 100 + BT_TLV_MAX; t++) bt_store_put(&st, t, two, 2);
    used = 0;
    for (int i = 0; i < BT_TLV_MAX; i++) used += st.r[i].tag != 0;
    CHECK(used == BT_TLV_MAX, "full: %d", used);
    CHECK(bt_store_put(&st, 0x999, two, 2) == 1, "no room");
    CHECK(bt_store_put(&st, 0x22, two, 2) == 0, "an old tag still goes in");

    CHECK(bt_store_delete(&st, 0x22) && !bt_store_delete(&st, 0x22), "delete, once");
    CHECK(!bt_store_delete(&st, 0), "tag 0 is not a record");
    CHECK(bt_store_put(&st, 0x999, two, 2) == 0, "room again");
    return 0;
}

/* ---- the file --------------------------------------------------------- */

static bool put_file(const char *path, const void *data, size_t n) {
    FIL f;
    UINT w;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT r = f_write(&f, data, (UINT)n, &w);
    return f_close(&f) == FR_OK && r == FR_OK && w == n;
}

static size_t slurp(const char *path, uint8_t *buf, size_t size) {
    FIL f;
    UINT n = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) return (size_t)-1;
    f_read(&f, buf, (UINT)size, &n);
    f_close(&f);
    return n;
}

static void le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static bool exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

static int file_tests(bool fat32) {
    const char *kind = fat32 ? "FAT32" : "FAT16";
    CHECK(host_disk_create(NULL, fat32 ? 64u << 20 : 32u << 20, fat32), "%s image", kind);
    files_mount();
    bt_store_t s;

    /* No file: new, empty, usable. */
    CHECK(bt_store_load(&s) == BT_STORE_NEW && s.magic == BT_STORE_MAGIC && bt_store_get(&s, 1, NULL, 0) == 0,
          "%s: no file", kind);

    /* Saved: /bluetooth made, the whole file, no .new, and read back. */
    static const uint8_t addr[6] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };
    uint8_t kbd[7 + 8];
    memcpy(kbd, addr, 6);
    kbd[6] = 0;
    memcpy(kbd + 7, "K380", 5);
    CHECK(bt_store_put(&s, BT_TAG_KEYBOARD, kbd, 12) == 0, "put");
    CHECK(bt_store_save(&s) == NULL, "%s: save", kind);
    CHECK(exists("/bluetooth/pairing.dat") && !exists("/bluetooth/pairing.dat.new"), "%s: the file, no .new", kind);
    static uint8_t raw[2048];
    CHECK(slurp(BT_STORE_PATH, raw, sizeof raw) == 1604, "%s: 1604 bytes", kind);
    bt_store_t back;
    CHECK(bt_store_load(&back) == BT_STORE_OK, "%s: load", kind);
    uint8_t got[BT_TLV_DATA];
    CHECK(bt_store_get(&back, BT_TAG_KEYBOARD, got, sizeof got) == 12 && memcmp(got, kbd, 12) == 0,
          "%s: the record kept", kind);
    CHECK(memcmp(&back, &s, sizeof s) == 0, "%s: all of it", kind);
    CHECK(bt_store_save(&back) == NULL && bt_store_save(&back) == NULL, "saved over and over");

    /* The format, byte for byte, as btpair keeps it: "PBK2", then each
     * record's tag, its length, and 72 bytes. */
    memset(raw, 0, sizeof raw);
    memcpy(raw, "PBK2", 4);
    le32(raw + 4, BT_TAG_KEYBOARD);
    le32(raw + 8, 12);
    memcpy(raw + 12, kbd, 12);
    static uint8_t file[2048];
    CHECK(slurp(BT_STORE_PATH, file, sizeof file) == 1604 && memcmp(file, raw, 1604) == 0,
          "%s: the file is btpair's layout", kind);
    CHECK(BT_STORE_MAGIC == 0x324B4250u && memcmp("PBK2", &(uint32_t){ BT_STORE_MAGIC }, 4) == 0, "magic");
    CHECK(memcmp(&(uint32_t){ BT_TAG_KEYBOARD }, "0DBK", 4) == 0, "KBD0 on the card");
    /* btpair 2.1 also keeps a mouse: "PTR0" and "PTL0". */
    CHECK(memcmp(&(uint32_t){ BT_TAG_MOUSE }, "0RTP", 4) == 0, "PTR0 on the card");
    CHECK(memcmp(&(uint32_t){ BT_TAG_MOUSE_LE }, "0LTP", 4) == 0, "PTL0 on the card");

    /* A file btpair wrote: read, with a BLE record. */
    memset(raw, 0, sizeof raw);
    memcpy(raw, "PBK2", 4);
    le32(raw + 4 + 5 * 80, BT_TAG_KEYBOARD_LE);
    le32(raw + 8 + 5 * 80, 3);
    memcpy(raw + 12 + 5 * 80, "xyz", 3);
    CHECK(put_file(BT_STORE_PATH, raw, 1604), "btpair's file");
    CHECK(bt_store_load(&s) == BT_STORE_OK && bt_store_get(&s, BT_TAG_KEYBOARD_LE, got, sizeof got) == 3 &&
              memcmp(got, "xyz", 3) == 0,
          "%s: read what btpair wrote", kind);

    /* btpair 1.0's file: 16 records of 48 bytes, carried over. */
    memset(raw, 0, sizeof raw);
    memcpy(raw, "PBK1", 4);
    le32(raw + 4, 0x10);
    le32(raw + 8, 4);
    memcpy(raw + 12, "abcd", 4);
    le32(raw + 4 + 56, 0x20);
    le32(raw + 8 + 56, 200);   /* more than a record holds: cut */
    memset(raw + 12 + 56, 'z', 48);
    CHECK(put_file(BT_STORE_PATH, raw, 4 + 16 * 56), "the old file");
    CHECK(bt_store_load(&s) == BT_STORE_UPGRADED && s.magic == BT_STORE_MAGIC, "%s: upgraded", kind);
    CHECK(bt_store_get(&s, 0x10, got, sizeof got) == 4 && memcmp(got, "abcd", 4) == 0, "old record 1");
    CHECK(bt_store_get(&s, 0x20, got, sizeof got) == 48 && got[47] == 'z', "old record 2, cut to 48");
    CHECK(bt_store_save(&s) == NULL && slurp(BT_STORE_PATH, file, sizeof file) == 1604 &&
              memcmp(file, "PBK2", 4) == 0,
          "%s: saved in the new form", kind);
    CHECK(put_file(BT_STORE_PATH, raw, 100), "a cut old file");
    CHECK(bt_store_load(&s) == BT_STORE_BAD, "%s: not whole", kind);

    /* Files that are not it. */
    static const struct {
        const char *what;
        size_t      len;
        const char *magic;
    } bad[] = {
        { "empty", 0, "" },
        { "short", 100, "PBK2" },
        { "a byte short", 1603, "PBK2" },
        { "a byte over", 1605, "PBK2" },
        { "another magic", 1604, "NOPE" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(raw, 0x5A, sizeof raw);
        memcpy(raw, bad[i].magic, strlen(bad[i].magic));
        CHECK(put_file(BT_STORE_PATH, raw, bad[i].len), "bad file");
        CHECK(bt_store_load(&s) == BT_STORE_BAD && s.magic == BT_STORE_MAGIC && bt_store_get(&s, 0x5A5A5A5A, NULL, 0) == 0,
              "%s: %s is an empty store", kind, bad[i].what);
    }

    /* A directory where the file should be. */
    CHECK(f_unlink(BT_STORE_PATH) == FR_OK && f_mkdir(BT_STORE_PATH) == FR_OK, "a directory");
    CHECK(bt_store_load(&s) == BT_STORE_BAD || bt_store_load(&s) == BT_STORE_NEW, "%s: a directory", kind);
    CHECK(bt_store_save(&s) != NULL, "%s: not written over a directory", kind);
    return 0;
}

/* ---- a card that fails -------------------------------------------------- */

static int fault_tests(void) {
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    bt_store_t a, b, s;
    memset(&a, 0, sizeof a);
    a.magic = BT_STORE_MAGIC;
    bt_store_put(&a, BT_TAG_KEYBOARD, (const uint8_t *)"old", 3);
    CHECK(bt_store_save(&a) == NULL, "the first file");
    b = a;
    bt_store_put(&b, BT_TAG_KEYBOARD_LE, (const uint8_t *)"newer", 5);

    /* The card dies at each point of a save: the file is the old one or the new one. */
    unsigned failed = 0, whole = 0;
    for (long cut = 0; cut < 40; cut++) {
        host_disk_fail_writes_after(cut);
        const char *why = bt_store_save(&b);
        host_disk_fail_writes_after(-1);
        files_mount();
        bt_store_load_t l = bt_store_load(&s);
        if (why) {
            failed++;
            /* Left as .new: the old is gone and the new is whole under that name. */
            if (l == BT_STORE_OK)
                CHECK(bt_store_get(&s, BT_TAG_KEYBOARD, NULL, 0) == 3, "cut %ld: old, or new, never half", cut);
        } else {
            whole++;
            CHECK(l == BT_STORE_OK && bt_store_get(&s, BT_TAG_KEYBOARD_LE, NULL, 0) == 5, "cut %ld: new", cut);
        }
        if (!why || l != BT_STORE_OK) bt_store_save(&a);   /* back to the old for the next */
        host_disk_fail_writes_after(-1);
    }
    CHECK(failed > 3 && whole > 0, "both outcomes: %u failed, %u whole", failed, whole);

    /* No card. */
    host_disk_close();
    CHECK(bt_store_save(&a) != NULL, "no card: not saved");
    CHECK(bt_store_load(&s) == BT_STORE_NEW && s.magic == BT_STORE_MAGIC, "no card: an empty store");
    return 0;
}

int main(void) {
    if (test_records()) return 1;
    if (file_tests(false)) return 1;
    if (file_tests(true)) return 1;
    if (fault_tests()) return 1;
    TEST_DONE();
}
