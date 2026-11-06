/* uf2.h — a UF2 program: the check before flash is touched, and the
 * differential flash (design.md §3.3).
 *
 * Portable: the file comes through FatFs, and the flash through
 * uf2_flash_t, which is the port's bootrom calls on the board and an
 * array on the host. The rules for blocks are UF2 Loader's
 * (references/uf2loader/ui/uf2.c), with three differences: the whole
 * file is checked before anything is erased; blocks must ascend; and a
 * sector that already holds what the program puts there is not written.
 *
 * Two passes. uf2_check reads every block and checks it, and compares
 * each 4 KB sector the program covers with flash; it only reads. If any
 * sector differs, uf2_write reads the file again and writes just those
 * sectors. The program's first sector, which holds its IMAGE_DEF, is
 * erased before any other and written last, the page that starts the
 * IMAGE_DEF last of all, so a write cut short leaves no bootable image
 * and stage3 falls back to /BOOT2350.UF2, the shell. */
#ifndef SHELL_UF2_H
#define SHELL_UF2_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "ff.h"

#define UF2_BLOCK   512u
#define UF2_PAGE    256u    /* a block's payload, and a flash page */
#define UF2_SECTOR  4096u   /* a flash erase sector */

/* The RP2350's SRAM, and the part of it stage3 loads a RAM program into:
 * it keeps the top 8 KB for itself (stage3/uf2.c). */
#define UF2_SRAM_BASE      0x20000000u
#define UF2_SRAM_END       0x20082000u
#define UF2_RAM_LOAD_END   0x20080000u

/* A program that takes arguments says so with a record of this size,
 * aligned to it, so it lies whole in one block's page: two words of magic
 * and its name, NUL-ended (PICOCALC_SHELL_TAKES_ARGS in
 * include/picocalc_shell.h; design.md §3.5). */
#define UF2_ARGS_RECORD 64u
#define UF2_ARGS_MAGIC0 0x9a3f6c1du
#define UF2_ARGS_MAGIC1 0x5e8b27f4u
#define UF2_ARGS_NAME   (UF2_ARGS_RECORD - 8u)

/* The app partition, as the shell sees it. */
typedef struct {
    uint32_t base;   /* where it is mapped: 0x10000000 */
    uint32_t end;    /* the first address past what a program may use */
    void (*read)(void *ctx, uint32_t addr, void *buf, uint32_t n);
    bool (*erase)(void *ctx, uint32_t addr);                    /* one sector */
    bool (*program)(void *ctx, uint32_t addr, const void *page); /* one page */
    void *ctx;
} uf2_flash_t;

typedef enum {
    UF2_FLASH,          /* a flash program; after uf2_write, it is in flash */
    UF2_RAM,            /* a RAM program, for stage3 to load */
    UF2_NOT_UF2,        /* no UF2 block in the file */
    UF2_NOT_HERE,       /* no RP2350 blocks, or ones past the partition */
    UF2_BAD,            /* blocks missing, repeated, out of order or malformed */
    UF2_CARD,           /* the card failed: fr says how */
    UF2_STOPPED,        /* interrupted while checking; flash untouched */
    UF2_WRITE_FAILED,   /* flash, or the card, failed while writing */
} uf2_result_t;

typedef struct {
    uint32_t blocks;    /* the program's blocks */
    uint32_t lo, hi;    /* its first address, and the end of its last block */
    uint32_t sectors;   /* flash sectors it covers */
    uint32_t differ;    /* of those, the ones flash does not already hold */
    uint32_t to_write;  /* the sectors uf2_write will write: those, and the
                         * first whenever there are any */
    uint32_t written;   /* sectors uf2_write erased and programmed */
    uint32_t pages;     /* pages it programmed */
    FRESULT  fr;        /* for UF2_CARD, and a card failure in UF2_WRITE_FAILED */
    uint32_t check_us;  /* how long each pass took, for the caller to fill */
    uint32_t write_us;
    char     args_name[UF2_ARGS_NAME];  /* the name the program declared, or "" */
} uf2_info_t;

/* Pass one, from the start of `f`: flash is only read. `stop` is polled
 * once per sector and may be NULL. */
uf2_result_t uf2_check(FIL *f, const uf2_flash_t *flash, uf2_info_t *info,
                       bool (*stop)(void *ctx), void *ctx);

/* Pass two, after uf2_check gave UF2_FLASH for the same file and flash:
 * write the sectors that differ, each read back after. UF2_FLASH when
 * flash holds the program. `tick` is called once per sector written,
 * to keep the keyboard polled, and may be NULL; nothing stops a write
 * once begun. */
uf2_result_t uf2_write(FIL *f, const uf2_flash_t *flash, uf2_info_t *info,
                       void (*tick)(void *ctx), void *ctx);

#endif /* SHELL_UF2_H */
