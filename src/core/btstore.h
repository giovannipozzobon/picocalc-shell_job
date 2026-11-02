/* btstore.h — the Bluetooth pairing's file, /bluetooth/pairing.dat
 * (design.md §3.10).
 *
 * The format is picocalc-extras' (btkbd.c, MIT), written by its `btpair`
 * and read here, so a keyboard paired there connects in the shell: a magic
 * and 20 tagged records of up to 72 bytes, held whole in RAM and saved
 * whole, always 1,604 bytes, little-endian as the RP2350 and the host
 * keep their structures. BTstack keeps its link keys and BLE bondings in
 * the records, by tag, and btkbd.c its note of which keyboard and which
 * mouse are ours.
 *
 * Portable: FatFs's API only (fat.h), with no BTstack in it, so the host
 * tests it. A save goes through `pairing.dat.new` as the shell's other
 * writes do (EL §8.6); `btpair` rewrites the file in place. */
#ifndef SHELL_BTSTORE_H
#define SHELL_BTSTORE_H

#include <stdbool.h>
#include <stdint.h>

#define BT_TLV_MAX   20
#define BT_TLV_DATA  72       /* (a BLE bonding takes about 64 bytes) */
#define BT_STORE_MAGIC 0x324B4250u     /* "PBK2" */
#define BT_STORE_DIR   "/bluetooth"
#define BT_STORE_PATH  "/bluetooth/pairing.dat"

/* btkbd.c's own records: the paired keyboard's address and name. */
#define BT_TAG_KEYBOARD    0x4B424430u   /* "KBD0": a classic keyboard */
#define BT_TAG_KEYBOARD_LE 0x4B424C30u   /* "KBL0": a BLE keyboard, its address type, name */
#define BT_TAG_MOUSE       0x50545230u   /* "PTR0": a classic mouse, the same way */
#define BT_TAG_MOUSE_LE    0x50544C30u   /* "PTL0": a BLE mouse */

typedef struct {
    uint32_t tag;       /* 0: the record is free */
    uint32_t len;
    uint8_t  data[BT_TLV_DATA];
} bt_record_t;

typedef struct {
    uint32_t    magic;
    bt_record_t r[BT_TLV_MAX];
} bt_store_t;

_Static_assert(sizeof(bt_store_t) == 4u + BT_TLV_MAX * (8u + BT_TLV_DATA),
               "pairing.dat is 1,604 bytes, as btpair writes it");

typedef enum {
    BT_STORE_NEW,        /* no file, or no card: an empty store */
    BT_STORE_OK,
    BT_STORE_UPGRADED,   /* btpair 1.0's file (16 records of 48 bytes), carried over: save it */
    BT_STORE_BAD,        /* a file that is not this: an empty store; saving would replace it */
} bt_store_load_t;

/* Read the file into `s` (always left usable, with the magic). */
bt_store_load_t bt_store_load(bt_store_t *s);

/* Write it, making /bluetooth if need be: NULL, or why not. */
const char *bt_store_save(const bt_store_t *s);

/* The records, as BTstack's TLV calls want them. get: the record's whole
 * length, or 0 if there is none, copying at most `size` bytes into `buf`
 * (which may be NULL). put: 0, or 1 if the tag is 0, the data is over
 * BT_TLV_DATA or there is no free record. delete: whether it was there. */
bt_record_t *bt_store_find(bt_store_t *s, uint32_t tag);
int bt_store_get(const bt_store_t *s, uint32_t tag, uint8_t *buf, uint32_t size);
int bt_store_put(bt_store_t *s, uint32_t tag, const uint8_t *data, uint32_t size);
bool bt_store_delete(bt_store_t *s, uint32_t tag);

#endif /* SHELL_BTSTORE_H */
