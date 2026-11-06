/* uf2.c — a UF2 program: the check and the differential flash (uf2.h).
 *
 * The block rules follow UF2 Loader's ui/uf2.c (GPL-3.0; pelrun 2025,
 * from muzkr's hachi, 2024): blocks with a bad magic, marked not for
 * main flash, or for another family are skipped, so a UF2 for several
 * chips works; the RP2350-E10 dummy block is skipped, and a UF2 that
 * counted it among its blocks is still read; the program's blocks must
 * then run 0..n-1 (1..n-1 in that case), each a 256-byte page. The
 * loader clears its proginfo words in the first page (proginfo.c), and
 * so does this, so a program flashed by either compares equal.
 *
 * Here, unlike the loader, a block is placed by building each 4 KB
 * sector in a buffer before comparing or writing it, so blocks must
 * ascend; the SDK writes them so. A page the program does not fill is
 * left erased, as the loader's erase leaves it. */

#include "uf2.h"

#include <string.h>

#define MAGIC_START0 0x0A324655u
#define MAGIC_START1 0x9E5D5157u
#define MAGIC_END    0x0AB16F30u
#define FLAG_NOT_MAIN_FLASH 0x00000001u
#define FLAG_FAMILY_ID      0x00002000u

#define FAMILY_ABSOLUTE     0xe48bff57u
#define FAMILY_RP2350_ARM_S 0xe48bff59u
#define FAMILY_RP2350_RISCV 0xe48bff5au
#define FAMILY_RP2350_ARM_NS 0xe48bff5bu

/* RP2350-E10: picotool's dummy block, which the bootrom needs and the
 * app must not get. */
#define E10_ADDR 0x10FFFF00u

/* The start of a picobin block (boot/picobin.h). */
#define PICOBIN_BLOCK_START 0xffffded3u

/* The loader's proginfo words in the app's vector table (proginfo.c). */
#define PROGINFO_OFFSET 0x20u
#define PROGINFO_SIZE   8u

#define PAGES_PER_SECTOR (UF2_SECTOR / UF2_PAGE)

static uint8_t s_blk[UF2_BLOCK];
static uint8_t s_sector[UF2_SECTOR];
static uint8_t s_page[UF2_PAGE];
static uint8_t s_differ[SHELL_FLASH_SECTORS_MAX / 8];

typedef enum { KIND_NONE, KIND_FLASH, KIND_RAM } kind_t;

/* scan_next's "no more blocks, and the program is whole": a value no
 * pass returns for it. */
#define SCAN_END UF2_STOPPED

/* Reading the program's blocks in order, skipping the others. */
typedef struct {
    FIL     *f;
    const uf2_flash_t *flash;
    bool     any;          /* a block with UF2's magic was seen */
    bool     malformed;    /* the E10 block was counted in the program's */
    kind_t   kind;
    uint32_t num, next;    /* the program's block count, the next block_no */
    uint32_t last;         /* the last block's address */
    uint32_t addr;         /* the current block's address */
    const uint8_t *data;   /* and its payload */
} scan_t;

static uint32_t get_word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t word(unsigned i) {
    return get_word(s_blk + 4u * i);
}

static bool our_family(uint32_t flags, uint32_t family) {
    return (flags & FLAG_FAMILY_ID) &&
           (family == FAMILY_RP2350_ARM_S || family == FAMILY_RP2350_ARM_NS ||
            family == FAMILY_RP2350_RISCV);
}

static void scan_start(scan_t *sc, FIL *f, const uf2_flash_t *flash) {
    memset(sc, 0, sizeof *sc);
    sc->f = f;
    sc->flash = flash;
}

/* The program's next block, in sc->addr and sc->data: UF2_FLASH or
 * UF2_RAM for a block, SCAN_END at the end of a whole program, or
 * the reason the file is refused. */
static uf2_result_t scan_next(scan_t *sc, FRESULT *fr) {
    for (;;) {
        UINT n;
        *fr = f_read(sc->f, s_blk, UF2_BLOCK, &n);
        if (*fr != FR_OK) return UF2_CARD;
        if (n < UF2_BLOCK) {
            /* The end. */
            if (sc->kind == KIND_NONE) return sc->any ? UF2_NOT_HERE : UF2_NOT_UF2;
            return sc->next == sc->num ? SCAN_END : UF2_BAD;
        }
        if (word(0) != MAGIC_START0 || word(1) != MAGIC_START1 || word(127) != MAGIC_END)
            continue;
        sc->any = true;
        uint32_t flags = word(2), addr = word(3), size = word(4);
        uint32_t no = word(5), num = word(6), family = word(7);
        if (flags & FLAG_NOT_MAIN_FLASH) continue;
        if ((flags & FLAG_FAMILY_ID) && family == FAMILY_ABSOLUTE && no == 0 && addr == E10_ADDR) {
            if (num != 2) sc->malformed = true;
            continue;
        }
        if (!our_family(flags, family)) continue;

        if (size != UF2_PAGE || addr % UF2_PAGE || num == 0 || no >= num) return UF2_BAD;
        bool ram = addr >= UF2_SRAM_BASE && addr < UF2_SRAM_END;
        if (sc->kind == KIND_NONE) {
            if (no != (sc->malformed ? 1u : 0u)) return UF2_BAD;
            sc->num = num;
            sc->kind = ram ? KIND_RAM : KIND_FLASH;
        } else {
            if (num != sc->num || no != sc->next) return UF2_BAD;
            if (ram != (sc->kind == KIND_RAM)) return UF2_BAD;
            if (addr <= sc->last) return UF2_BAD;
        }
        if (ram) {
            if (addr + UF2_PAGE > UF2_RAM_LOAD_END) return UF2_NOT_HERE;
        } else {
            const uf2_flash_t *fl = sc->flash;
            if (addr < fl->base || addr > fl->end || fl->end - addr < UF2_PAGE ||
                (addr - fl->base) / UF2_SECTOR >= SHELL_FLASH_SECTORS_MAX)
                return UF2_NOT_HERE;
        }
        sc->next = no + 1;
        sc->last = addr;
        sc->addr = addr;
        sc->data = s_blk + 32;
        return ram ? UF2_RAM : UF2_FLASH;
    }
}

static uint32_t sector_of(const uf2_flash_t *fl, uint32_t addr) {
    return (addr - fl->base) / UF2_SECTOR;
}

static uint32_t sector_addr(const uf2_flash_t *fl, uint32_t sec) {
    return fl->base + sec * UF2_SECTOR;
}

/* Put the block in the sector being built. */
static void place(const scan_t *sc) {
    const uf2_flash_t *fl = sc->flash;
    uint8_t *page = s_sector + (sc->addr - fl->base) % UF2_SECTOR;
    memcpy(page, sc->data, UF2_PAGE);
    uint32_t pi = fl->base + PROGINFO_OFFSET;
    if (pi >= sc->addr && pi - sc->addr < UF2_PAGE)
        memset(page + (pi - sc->addr), 0xFF, PROGINFO_SIZE);
}

/* Whether flash holds the sector built. */
static bool sector_matches(const uf2_flash_t *fl, uint32_t sec) {
    for (uint32_t p = 0; p < PAGES_PER_SECTOR; p++) {
        fl->read(fl->ctx, sector_addr(fl, sec) + p * UF2_PAGE, s_page, UF2_PAGE);
        if (memcmp(s_page, s_sector + p * UF2_PAGE, UF2_PAGE) != 0) return false;
    }
    return true;
}

static bool differs(uint32_t sec) {
    return s_differ[sec / 8] & (1u << (sec % 8));
}

/* A record saying the program takes arguments, in this block's page, into
 * `name`. A name that is empty, not ended or not text is no record. */
static void find_args(const uint8_t *page, char name[UF2_ARGS_NAME]) {
    for (uint32_t off = 0; off < UF2_PAGE && !name[0]; off += UF2_ARGS_RECORD) {
        const uint8_t *r = page + off;
        if (get_word(r) != UF2_ARGS_MAGIC0 || get_word(r + 4) != UF2_ARGS_MAGIC1) continue;
        const uint8_t *text = r + 8;
        const uint8_t *end = memchr(text, 0, UF2_ARGS_NAME);
        if (!end || end == text) continue;
        bool ok = true;
        for (const uint8_t *c = text; c < end; c++) ok = ok && *c >= 0x20 && *c != 0x7F;
        if (ok) memcpy(name, text, (size_t)(end - text) + 1);
    }
}

uf2_result_t uf2_check(FIL *f, const uf2_flash_t *flash, uf2_info_t *info,
                       bool (*stop)(void *ctx), void *ctx) {
    memset(info, 0, sizeof *info);
    memset(s_differ, 0, sizeof s_differ);
    if ((info->fr = f_lseek(f, 0)) != FR_OK) return UF2_CARD;
    scan_t sc;
    scan_start(&sc, f, flash);
    bool open = false;      /* a sector is being built */
    uint32_t cur = 0;
    uf2_result_t r;
    while ((r = scan_next(&sc, &info->fr)) == UF2_FLASH || r == UF2_RAM) {
        if (info->blocks++ == 0) info->lo = sc.addr;
        info->hi = sc.addr + UF2_PAGE;
        find_args(sc.data, info->args_name);
        if (r == UF2_RAM) continue;
        uint32_t sec = sector_of(flash, sc.addr);
        if (!open || sec != cur) {
            if (open && !sector_matches(flash, cur)) {
                s_differ[cur / 8] |= (uint8_t)(1u << (cur % 8));
                info->differ++;
            }
            if (stop && stop(ctx)) return UF2_STOPPED;
            open = true;
            cur = sec;
            info->sectors++;
            memset(s_sector, 0xFF, sizeof s_sector);
        }
        place(&sc);
    }
    if (r != SCAN_END) return r;
    if (sc.kind == KIND_RAM) return UF2_RAM;
    if (!sector_matches(flash, cur)) {
        s_differ[cur / 8] |= (uint8_t)(1u << (cur % 8));
        info->differ++;
    }
    /* uf2_write rewrites the first sector whenever it writes at all. */
    info->to_write = info->differ;
    if (info->differ && !differs(sector_of(flash, info->lo))) info->to_write++;
    return UF2_FLASH;
}

/* Whether a page holds a picobin block's first word. The bootrom finds
 * an image by its first block (the IMAGE_DEF), which must start in the
 * first 4 KB, and a block is whole only with its first word in place. */
static bool has_block_start(const uint8_t *page) {
    for (uint32_t i = 0; i < UF2_PAGE; i += 4) {
        uint32_t w = (uint32_t)page[i] | (uint32_t)page[i + 1] << 8 |
                     (uint32_t)page[i + 2] << 16 | (uint32_t)page[i + 3] << 24;
        if (w == PICOBIN_BLOCK_START) return true;
    }
    return false;
}

/* Program the sector built, already erased, and read it back. In the
 * program's first sector, the pages holding a block's first word go
 * last, highest first, so the lowest block, where the bootrom's search
 * starts and the block loop's head, is the very last page written: the
 * image is bootable only once every page is in place. */
static bool program_sector(const uf2_flash_t *fl, uint32_t sec, bool first, uf2_info_t *info) {
    for (int late = 0; late < (first ? 2 : 1); late++) {
        for (uint32_t i = 0; i < PAGES_PER_SECTOR; i++) {
            uint32_t p = late ? PAGES_PER_SECTOR - 1 - i : i;
            const uint8_t *page = s_sector + p * UF2_PAGE;
            if (first && has_block_start(page) != (late == 1)) continue;
            bool blank = true;
            for (uint32_t i = 0; i < UF2_PAGE && blank; i++) blank = page[i] == 0xFF;
            if (blank) continue;
            if (!fl->program(fl->ctx, sector_addr(fl, sec) + p * UF2_PAGE, page)) return false;
            info->pages++;
        }
    }
    if (!sector_matches(fl, sec)) return false;
    info->written++;
    return true;
}

uf2_result_t uf2_write(FIL *f, const uf2_flash_t *flash, uf2_info_t *info,
                       void (*tick)(void *ctx), void *ctx) {
    if (info->differ == 0) return UF2_FLASH;
    uint32_t first = sector_of(flash, info->lo);

    /* No bootable image from here until the last write. */
    if (!flash->erase(flash->ctx, sector_addr(flash, first))) return UF2_WRITE_FAILED;

    /* Every other sector that differs, as the file reaches it; then
     * the first, built again from the start of the file. */
    for (int round = 0; round < 2; round++) {
        if ((info->fr = f_lseek(f, 0)) != FR_OK) return UF2_WRITE_FAILED;
        scan_t sc;
        scan_start(&sc, f, flash);
        bool open = false;
        uint32_t cur = 0;
        uf2_result_t r;
        for (;;) {
            r = scan_next(&sc, &info->fr);
            /* The card failed, or the file is not what uf2_check read:
             * stop before the sector being built is written, half filled. */
            if (r != UF2_FLASH && r != SCAN_END) return UF2_WRITE_FAILED;
            bool end = r == SCAN_END;
            uint32_t sec = end ? 0 : sector_of(flash, sc.addr);
            if (open && (end || sec != cur)) {
                if (round == 1) {
                    /* cur is the first sector, whole: write it and stop. */
                    if (tick) tick(ctx);
                    return program_sector(flash, cur, true, info) ? UF2_FLASH : UF2_WRITE_FAILED;
                }
                if (cur != first && differs(cur)) {
                    if (tick) tick(ctx);
                    if (!flash->erase(flash->ctx, sector_addr(flash, cur)) ||
                        !program_sector(flash, cur, false, info))
                        return UF2_WRITE_FAILED;
                }
                open = false;
            }
            if (end) break;
            if (!open) {
                open = true;
                cur = sec;
                memset(s_sector, 0xFF, sizeof s_sector);
            }
            place(&sc);
        }
    }
    return UF2_WRITE_FAILED;
}
