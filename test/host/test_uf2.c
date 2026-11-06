/* test_uf2.c — running programs (design.md §3.3, §4.3, §10 M2): the
 * binary info reader, the UF2 check and the differential flash, and the
 * shell's lookup, `menu` and messages, over a FAT image and a fake app
 * partition.
 *
 * The fake flash behaves as NOR flash does: erase sets a sector to 0xFF,
 * and programming can only clear bits. It counts its operations and can
 * lose power after any number of them, after which nothing more reaches
 * it. Synthetic UF2s test each rule; the emulators' and the loader's own
 * UF2s, fetched by tools/fetch-suites.sh, are checked where present. */

#include <stdio.h>
#include <string.h>

#include "bininfo.h"
#include "files.h"
#include "host_disk.h"
#include "run.h"
#include "shell.h"
#include "test_util.h"
#include "uf2.h"

#ifndef SHELL_SUITES_DIR
#define SHELL_SUITES_DIR "test/suites"
#endif

/* ---- the fake app partition -------------------------------------------- */

#define FLASH_BASE 0x10000000u
#define FLASH_SIZE (4u << 20)
#define FLASH_END  (FLASH_BASE + FLASH_SIZE - 8192u)   /* stage3's, for 4 MB */

static uint8_t flash_mem[FLASH_SIZE];
static long    flash_ops, flash_erases, flash_programs;
static long    flash_power = -1;    /* operations left before power is lost, or -1 */

static bool flash_op(void) {
    if (flash_power == 0) return false;
    if (flash_power > 0) flash_power--;
    flash_ops++;
    return true;
}

static void fl_read(void *ctx, uint32_t addr, void *buf, uint32_t n) {
    (void)ctx;
    memcpy(buf, flash_mem + (addr - FLASH_BASE), n);
}

static bool fl_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    if ((addr - FLASH_BASE) % UF2_SECTOR || !flash_op()) return false;
    flash_erases++;
    memset(flash_mem + (addr - FLASH_BASE), 0xFF, UF2_SECTOR);
    return true;
}

static bool fl_program(void *ctx, uint32_t addr, const void *page) {
    (void)ctx;
    if ((addr - FLASH_BASE) % UF2_PAGE || !flash_op()) return false;
    flash_programs++;
    const uint8_t *p = page;
    for (uint32_t i = 0; i < UF2_PAGE; i++) flash_mem[addr - FLASH_BASE + i] &= p[i];
    return true;
}

static const uf2_flash_t flash = { FLASH_BASE, FLASH_END, fl_read, fl_erase, fl_program, NULL };

static void flash_junk(uint32_t seed) {
    for (uint32_t i = 0; i < FLASH_SIZE; i++) {
        seed = seed * 1103515245u + 12345u;
        flash_mem[i] = (uint8_t)(seed >> 16);
    }
}

static void flash_count_reset(void) {
    flash_ops = flash_erases = flash_programs = 0;
}

/* ---- making UF2s ------------------------------------------------------ */

#define FAM_RP2040   0xe48bff56u
#define FAM_ABSOLUTE 0xe48bff57u
#define FAM_ARM_S    0xe48bff59u
#define FAM_RISCV    0xe48bff5au
#define FAM_ARM_NS   0xe48bff5bu
#define FLAG_FAMILY  0x2000u

static uint8_t ubuf[4u << 20];
static size_t  ulen;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void block(uint32_t flags, uint32_t addr, uint32_t no, uint32_t num, uint32_t family,
                  const uint8_t *data) {
    uint8_t *b = ubuf + ulen;
    memset(b, 0, UF2_BLOCK);
    put32(b, 0x0A324655u);
    put32(b + 4, 0x9E5D5157u);
    put32(b + 8, flags);
    put32(b + 12, addr);
    put32(b + 16, UF2_PAGE);
    put32(b + 20, no);
    put32(b + 24, num);
    put32(b + 28, family);
    memcpy(b + 32, data, UF2_PAGE);
    put32(b + 508, 0x0AB16F30u);
    ulen += UF2_BLOCK;
}

/* The page a synthetic program puts at `addr`: varied, never all 0xFF,
 * and with a picobin block's first word at 0x13c in the image's second
 * page, where the SDK puts its IMAGE_DEF, the head of the block loop,
 * and a second block's at 0x510. */
static const uint8_t *page_for(uint32_t base, uint32_t addr, uint32_t seed) {
    static uint8_t page[UF2_PAGE];
    for (uint32_t i = 0; i < UF2_PAGE; i++) page[i] = (uint8_t)((addr + i) * 7u + seed * 31u + i / 3u);
    if (addr == base + 0x100u) put32(page + 0x3c, 0xffffded3u);
    if (addr == base + 0x500u) put32(page + 0x10, 0xffffded3u);
    return page;
}

/* A program of `pages` pages from `base`, skipping page `gap` (or -1),
 * with `seed` varying its bytes; e10: 0 none, 1 as picotool writes it,
 * 2 the malformed kind, counted among the program's blocks. */
static void make(uint32_t base, uint32_t pages, int gap, uint32_t seed, int e10, uint32_t family) {
    ulen = 0;
    uint32_t num = pages - (gap >= 0 ? 1u : 0u);
    uint8_t dummy[UF2_PAGE];
    memset(dummy, 0xEF, sizeof dummy);
    uint32_t no = 0;
    if (e10 == 1) block(FLAG_FAMILY, 0x10FFFF00u, 0, 2, FAM_ABSOLUTE, dummy);
    if (e10 == 2) {
        num++;
        block(FLAG_FAMILY, 0x10FFFF00u, no++, num, FAM_ABSOLUTE, dummy);
    }
    for (uint32_t p = 0; p < pages; p++) {
        if ((int)p == gap) continue;
        uint32_t a = base + p * UF2_PAGE;
        block(FLAG_FAMILY, a, no++, num, family, page_for(base, a, seed));
    }
}

static bool put(const char *path, const void *data, size_t n) {
    FIL f;
    UINT w;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT r = f_write(&f, data, (UINT)n, &w);
    return f_close(&f) == FR_OK && r == FR_OK && w == n;
}

/* The block header field i of block k of ubuf. */
static uint8_t *field(size_t k, unsigned i) {
    return ubuf + k * UF2_BLOCK + 4u * i;
}

/* ---- checking a UF2 directly -------------------------------------------- */

static FIL f;

static uf2_result_t check_file(const char *path, uf2_info_t *info) {
    if (f_open(&f, path, FA_READ) != FR_OK) return UF2_CARD;
    uf2_result_t r = uf2_check(&f, &flash, info, NULL, NULL);
    f_close(&f);
    return r;
}

/* Check, then write: the result of the write, or of a check that
 * refused. */
static uf2_result_t flash_file(const char *path, uf2_info_t *info) {
    if (f_open(&f, path, FA_READ) != FR_OK) return UF2_CARD;
    uf2_result_t r = uf2_check(&f, &flash, info, NULL, NULL);
    if (r == UF2_FLASH) r = uf2_write(&f, &flash, info, NULL, NULL);
    f_close(&f);
    return r;
}

/* Check ubuf, as /t.uf2. */
static uf2_result_t check_ubuf(uf2_info_t *info) {
    if (!put("/t.uf2", ubuf, ulen)) return UF2_CARD;
    return check_file("/t.uf2", info);
}

/* Whether flash holds the synthetic program (pages from base, gap left
 * erased within its sectors, the loader's proginfo words erased). */
static bool flash_holds(uint32_t base, uint32_t pages, int gap, uint32_t seed) {
    for (uint32_t p = 0; p < pages; p++) {
        uint32_t a = base + p * UF2_PAGE;
        const uint8_t *want = page_for(base, a, seed);
        const uint8_t *got = flash_mem + (a - FLASH_BASE);
        for (uint32_t i = 0; i < UF2_PAGE; i++) {
            uint8_t w = (int)p == gap ? 0xFF : want[i];
            if (a == FLASH_BASE && i >= 0x20 && i < 0x28) w = 0xFF;
            if (got[i] != w) return false;
        }
    }
    /* The rest of its last sector is erased. */
    uint32_t end = base + pages * UF2_PAGE, sec_end = (end + UF2_SECTOR - 1) & ~(UF2_SECTOR - 1);
    for (uint32_t a = end; a < sec_end; a++)
        if (flash_mem[a - FLASH_BASE] != 0xFF) return false;
    return true;
}

/* Whether the block loop's head is in place at base: the bootrom's
 * search of the first 4 KB starts from it. */
static bool bootable(uint32_t base) {
    return get32(flash_mem + (base - FLASH_BASE) + 0x13c) == 0xffffded3u;
}

static int test_rules(void) {
    uf2_info_t info;

    /* Not a UF2 at all, and an empty file. */
    CHECK(put("/text.uf2", "hello\n", 6) && check_file("/text.uf2", &info) == UF2_NOT_UF2,
          "text");
    CHECK(put("/empty.uf2", "", 0) && check_file("/empty.uf2", &info) == UF2_NOT_UF2, "empty");

    /* Each RP2350 family, with and without the E10 block, both kinds. */
    const uint32_t fams[] = { FAM_ARM_S, FAM_ARM_NS, FAM_RISCV };
    for (int e10 = 0; e10 < 3; e10++)
        for (int k = 0; k < 3; k++) {
            make(FLASH_BASE, 40, -1, 1, e10, fams[k]);
            uf2_result_t r = check_ubuf(&info);
            CHECK(r == UF2_FLASH && info.blocks == 40 && info.lo == FLASH_BASE &&
                      info.hi == FLASH_BASE + 40 * UF2_PAGE && info.sectors == 3,
                  "e10 %d family %d: r %d blocks %u sectors %u", e10, k, r, info.blocks,
                  info.sectors);
        }

    /* Another chip's: not for this device. A UF2 for two chips is fine. */
    make(FLASH_BASE, 8, -1, 1, 0, FAM_RP2040);
    CHECK(check_ubuf(&info) == UF2_NOT_HERE, "RP2040");
    size_t rp2040 = ulen;
    static uint8_t both[64 * UF2_BLOCK];
    memcpy(both, ubuf, rp2040);
    make(FLASH_BASE, 8, -1, 2, 1, FAM_ARM_S);
    memcpy(both + rp2040, ubuf, ulen);
    CHECK(put("/both.uf2", both, rp2040 + ulen) && check_file("/both.uf2", &info) == UF2_FLASH &&
              info.blocks == 8,
          "two chips: blocks %u", info.blocks);
    /* No family named at all: not known to be for this chip. */
    make(FLASH_BASE, 8, -1, 1, 0, FAM_ARM_S);
    for (size_t k = 0; k < 8; k++) put32(field(k, 2), 0);
    CHECK(check_ubuf(&info) == UF2_NOT_HERE, "no family");

    /* Past the end of the partition, or below it. */
    make(FLASH_END - 4 * UF2_PAGE, 5, -1, 1, 0, FAM_ARM_S);
    CHECK(check_ubuf(&info) == UF2_NOT_HERE, "past the end");
    make(FLASH_END - 4 * UF2_PAGE, 4, -1, 1, 0, FAM_ARM_S);
    CHECK(check_ubuf(&info) == UF2_FLASH, "to the end");
    make(0x0FFFFF00u, 4, -1, 1, 0, FAM_ARM_S);
    CHECK(check_ubuf(&info) == UF2_NOT_HERE, "below the partition");

    /* RAM programs, to stage3's limit. */
    make(UF2_SRAM_BASE, 64, -1, 1, 0, FAM_ARM_S);
    CHECK(check_ubuf(&info) == UF2_RAM && info.blocks == 64 && info.sectors == 0, "RAM");
    make(UF2_RAM_LOAD_END - 4 * UF2_PAGE, 4, -1, 1, 0, FAM_ARM_S);
    CHECK(check_ubuf(&info) == UF2_RAM, "RAM to the end");
    make(UF2_RAM_LOAD_END - 4 * UF2_PAGE, 5, -1, 1, 0, FAM_ARM_S);
    CHECK(check_ubuf(&info) == UF2_NOT_HERE, "RAM into stage3's");

    /* Damaged: a block missing, repeated, out of order, misnumbered, the
     * count changing, a short payload, a misaligned address, the file cut
     * short, flash and RAM mixed. */
    make(FLASH_BASE, 20, -1, 1, 0, FAM_ARM_S);
    static uint8_t good[64 * UF2_BLOCK];
    size_t glen = ulen;
    memcpy(good, ubuf, glen);
    /* Block 7 dropped. */
    memmove(ubuf + 7 * UF2_BLOCK, ubuf + 8 * UF2_BLOCK, glen - 8 * UF2_BLOCK);
    ulen = glen - UF2_BLOCK;
    CHECK(check_ubuf(&info) == UF2_BAD, "a block missing");
    memcpy(ubuf, good, glen);
    ulen = glen;
    memcpy(ubuf + 8 * UF2_BLOCK, ubuf + 7 * UF2_BLOCK, UF2_BLOCK);
    CHECK(check_ubuf(&info) == UF2_BAD, "a block repeated");
    memcpy(ubuf, good, glen);
    put32(field(5, 3), FLASH_BASE + 2 * UF2_PAGE);
    CHECK(check_ubuf(&info) == UF2_BAD, "out of order");
    memcpy(ubuf, good, glen);
    put32(field(6, 3), FLASH_BASE + 5 * UF2_PAGE);
    CHECK(check_ubuf(&info) == UF2_BAD, "an address repeated");
    memcpy(ubuf, good, glen);
    put32(field(0, 5), 1);
    CHECK(check_ubuf(&info) == UF2_BAD, "first block misnumbered");
    memcpy(ubuf, good, glen);
    put32(field(9, 6), 21);
    CHECK(check_ubuf(&info) == UF2_BAD, "count changing");
    memcpy(ubuf, good, glen);
    put32(field(9, 4), 128);
    CHECK(check_ubuf(&info) == UF2_BAD, "short payload");
    memcpy(ubuf, good, glen);
    put32(field(9, 3), FLASH_BASE + 9 * UF2_PAGE + 4);
    CHECK(check_ubuf(&info) == UF2_BAD, "misaligned");
    memcpy(ubuf, good, glen);
    ulen = glen - UF2_BLOCK - 100;
    CHECK(check_ubuf(&info) == UF2_BAD, "cut short");
    ulen = glen;
    put32(field(19, 3), UF2_SRAM_BASE + 0x1000u);
    CHECK(check_ubuf(&info) == UF2_BAD, "flash and RAM");
    memcpy(ubuf, good, glen);
    /* A block with a bad magic, or marked not for flash, is skipped,
     * which leaves one missing. */
    put32(field(3, 0), 0);
    CHECK(check_ubuf(&info) == UF2_BAD, "bad magic");
    memcpy(ubuf, good, glen);
    put32(field(3, 2), FLAG_FAMILY | 1u);
    CHECK(check_ubuf(&info) == UF2_BAD, "not main flash");
    memcpy(ubuf, good, glen);
    CHECK(check_ubuf(&info) == UF2_FLASH, "the good one");

    /* A gap inside a program: the loader allows one (non-contiguous UF2s). */
    make(FLASH_BASE, 40, 20, 1, 1, FAM_ARM_S);
    CHECK(check_ubuf(&info) == UF2_FLASH && info.blocks == 39, "a gap");
    return 0;
}

static int test_flashing(void) {
    uf2_info_t info;
    flash_junk(1);

    /* First run over junk: every sector differs and is written. */
    make(FLASH_BASE, 100, 37, 1, 1, FAM_ARM_S);   /* 25 KB, 7 sectors, a gap */
    CHECK(put("/a.uf2", ubuf, ulen), "a.uf2");
    flash_count_reset();
    CHECK(check_file("/a.uf2", &info) == UF2_FLASH && info.sectors == 7 && info.differ == 7,
          "a: sectors %u differ %u", info.sectors, info.differ);
    CHECK(flash_ops == 0, "check writes nothing: %ld", flash_ops);
    flash_count_reset();
    CHECK(flash_file("/a.uf2", &info) == UF2_FLASH && info.written == 7 && info.pages == 99,
          "a: written %u pages %u", info.written, info.pages);
    CHECK(flash_erases == 7 && flash_programs == 99, "erases %ld programs %ld", flash_erases,
          flash_programs);
    CHECK(flash_holds(FLASH_BASE, 100, 37, 1), "flash holds a, gap erased, proginfo erased");

    /* Again: nothing differs, nothing is written. */
    flash_count_reset();
    CHECK(flash_file("/a.uf2", &info) == UF2_FLASH && info.differ == 0 && flash_ops == 0,
          "a again: differ %u ops %ld", info.differ, flash_ops);

    /* One page changed in the fifth sector: that sector, and the first,
     * which is always erased first and written last. */
    make(FLASH_BASE, 100, 37, 1, 1, FAM_ARM_S);
    size_t k = 70;   /* page 71 of the program: the gap shifts blocks by one */
    ubuf[UF2_BLOCK * (k + 1) + 32 + 5] ^= 0x5A;   /* +1 for the E10 block */
    CHECK(put("/a2.uf2", ubuf, ulen), "a2.uf2");
    flash_count_reset();
    CHECK(flash_file("/a2.uf2", &info) == UF2_FLASH && info.differ == 1 && info.to_write == 2 &&
              info.written == 2,
          "a2: differ %u to write %u written %u", info.differ, info.to_write, info.written);
    CHECK(flash_erases == 2, "a2: erases %ld", flash_erases);
    flash_count_reset();
    CHECK(flash_file("/a2.uf2", &info) == UF2_FLASH && info.differ == 0, "a2 again");

    /* A shorter program over a longer one: its sectors only. */
    make(FLASH_BASE, 30, -1, 2, 1, FAM_ARM_S);
    CHECK(put("/b.uf2", ubuf, ulen), "b.uf2");
    CHECK(flash_file("/b.uf2", &info) == UF2_FLASH && info.sectors == 2 && info.written == 2 &&
              flash_holds(FLASH_BASE, 30, -1, 2),
          "b: sectors %u written %u", info.sectors, info.written);

    /* A page the program does not fill, holding junk, is erased: the
     * result is what the loader's erase-then-program leaves. */
    make(FLASH_BASE, 32, -1, 3, 1, FAM_ARM_S);
    CHECK(put("/c.uf2", ubuf, ulen) && flash_file("/c.uf2", &info) == UF2_FLASH, "c");
    make(FLASH_BASE, 32, 9, 3, 1, FAM_ARM_S);
    CHECK(put("/c2.uf2", ubuf, ulen), "c2.uf2");
    CHECK(check_file("/c2.uf2", &info) == UF2_FLASH && info.differ == 1,
          "a page no longer filled differs: %u", info.differ);
    CHECK(flash_file("/c2.uf2", &info) == UF2_FLASH && flash_holds(FLASH_BASE, 32, 9, 3), "c2");

    /* A flash program that fails to read back. */
    make(FLASH_BASE, 20, -1, 4, 1, FAM_ARM_S);
    CHECK(put("/d.uf2", ubuf, ulen), "d.uf2");
    flash_power = 3;
    CHECK(flash_file("/d.uf2", &info) == UF2_WRITE_FAILED, "power lost");
    flash_power = -1;
    CHECK(!bootable(FLASH_BASE), "nothing bootable after a failed write");
    CHECK(flash_file("/d.uf2", &info) == UF2_FLASH && flash_holds(FLASH_BASE, 20, -1, 4),
          "d written after");
    return 0;
}

/* Power lost after every number of flash operations, switching from one
 * program to another: flash holds the old program untouched, the new
 * one whole, or nothing bootable. */
static int test_power_loss(void) {
    static uint8_t before[FLASH_SIZE / 4];
    uf2_info_t info;
    flash_junk(7);
    make(FLASH_BASE, 160, -1, 5, 1, FAM_ARM_S);
    CHECK(put("/old.uf2", ubuf, ulen) && flash_file("/old.uf2", &info) == UF2_FLASH, "old");
    make(FLASH_BASE, 150, 60, 6, 1, FAM_ARM_S);
    CHECK(put("/new.uf2", ubuf, ulen), "new.uf2");
    memcpy(before, flash_mem, sizeof before);
    flash_count_reset();
    CHECK(flash_file("/new.uf2", &info) == UF2_FLASH, "new, whole");
    long total = flash_ops;
    CHECK(total > 40, "ops %ld", total);
    for (long n = 0; n < total; n++) {
        memcpy(flash_mem, before, sizeof before);
        flash_power = n;
        uf2_result_t r = flash_file("/new.uf2", &info);
        flash_power = -1;
        bool old = memcmp(flash_mem, before, sizeof before) == 0;
        CHECK(r == UF2_WRITE_FAILED, "cut after %ld: r %d", n, r);
        CHECK(old || !bootable(FLASH_BASE), "cut after %ld: bootable and not the old program", n);
        CHECK(!flash_holds(FLASH_BASE, 150, 60, 6) || n == total, "cut after %ld: whole?", n);
    }
    return 0;
}

/* The card failing at every point of the write's reads: no sector is
 * written half built, and nothing bootable is left that is not whole. */
static int test_card_loss(void) {
    static uint8_t before[FLASH_SIZE / 4];
    uf2_info_t info;
    flash_junk(11);
    make(FLASH_BASE, 120, -1, 7, 1, FAM_ARM_S);
    CHECK(put("/old2.uf2", ubuf, ulen) && flash_file("/old2.uf2", &info) == UF2_FLASH, "old2");
    make(FLASH_BASE, 120, 50, 8, 1, FAM_ARM_S);
    CHECK(put("/new2.uf2", ubuf, ulen), "new2.uf2");
    memcpy(before, flash_mem, sizeof before);
    uint32_t nsec = (120 * UF2_PAGE + UF2_SECTOR - 1) / UF2_SECTOR;
    bool completed = false;
    for (long n = 0; !completed && n < 1000; n++) {
        memcpy(flash_mem, before, sizeof before);
        CHECK(f_open(&f, "/new2.uf2", FA_READ) == FR_OK, "open %ld", n);
        uf2_result_t r = uf2_check(&f, &flash, &info, NULL, NULL);
        CHECK(r == UF2_FLASH, "check %ld: %d", n, r);
        host_disk_fail_reads_after(n);
        r = uf2_write(&f, &flash, &info, NULL, NULL);
        host_disk_fail_reads_after(-1);
        f_close(&f);
        completed = r == UF2_FLASH;
        if (completed) {
            CHECK(flash_holds(FLASH_BASE, 120, 50, 8), "whole after %ld reads", n);
            break;
        }
        CHECK(r == UF2_WRITE_FAILED, "reads %ld: r %d", n, r);
        CHECK(!bootable(FLASH_BASE), "reads %ld: bootable and not whole", n);
        /* Every sector but the first is the old program's or the new one's. */
        for (uint32_t sec = 1; sec < nsec; sec++) {
            const uint8_t *now = flash_mem + sec * UF2_SECTOR, *old = before + sec * UF2_SECTOR;
            if (memcmp(now, old, UF2_SECTOR) == 0) continue;
            /* Compare with the new program's sector, built as flash_holds would. */
            bool whole = true;
            for (uint32_t pg = 0; pg < UF2_SECTOR / UF2_PAGE && whole; pg++) {
                uint32_t p = sec * (UF2_SECTOR / UF2_PAGE) + pg, a = FLASH_BASE + p * UF2_PAGE;
                const uint8_t *want = page_for(FLASH_BASE, a, 8);
                for (uint32_t i = 0; i < UF2_PAGE; i++) {
                    uint8_t w = (p == 50 || p >= 120) ? 0xFF : want[i];
                    if (now[pg * UF2_PAGE + i] != w) { whole = false; break; }
                }
            }
            CHECK(whole, "reads %ld: sector %u half written", n, sec);
        }
    }
    CHECK(completed, "the write never completed");
    return 0;
}

/* ---- binary info, and the real UF2s ---------------------------------- */

static uint8_t file_buf[4u << 20];

static size_t slurp_host(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/uf2/%s", SHELL_SUITES_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    size_t n = fread(file_buf, 1, sizeof file_buf, fp);
    fclose(fp);
    return n;
}

/* The program's image from its first block's address, 0xFF between. */
static uint8_t image[1u << 20];

static uint32_t decode(size_t n, uint32_t *addr) {
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (size_t i = 0; i + UF2_BLOCK <= n; i += UF2_BLOCK) {
        uint32_t a = get32(file_buf + i + 12);
        if (a == 0x10FFFF00u) continue;
        if (a < lo) lo = a;
        if (a + UF2_PAGE > hi) hi = a + UF2_PAGE;
    }
    if (hi <= lo || hi - lo > sizeof image) return 0;
    memset(image, 0xFF, sizeof image);
    for (size_t i = 0; i + UF2_BLOCK <= n; i += UF2_BLOCK) {
        uint32_t a = get32(file_buf + i + 12);
        if (a >= lo && a < hi) memcpy(image + (a - lo), file_buf + i + 32, UF2_PAGE);
    }
    *addr = lo;
    return hi - lo;
}

static int test_bininfo(void) {
    /* A synthetic image: header at 0x110, two entries, one string in
     * RAM through the mapping table. */
    static uint8_t img[2048];
    memset(img, 0, sizeof img);
    const uint32_t base = 0x10000000u;
    put32(img + 0x110, 0x7188ebf2u);
    put32(img + 0x114, base + 0x400);
    put32(img + 0x118, base + 0x408);
    put32(img + 0x11c, base + 0x500);
    put32(img + 0x120, 0xe71aa390u);
    put32(img + 0x400, base + 0x600);
    put32(img + 0x404, base + 0x610);
    /* entry: type 6, tag RP, id, pointer */
    img[0x600] = 6; img[0x602] = 'R'; img[0x603] = 'P';
    put32(img + 0x604, BININFO_ID_PROGRAM_NAME);
    put32(img + 0x608, base + 0x700);
    img[0x610] = 6; img[0x612] = 'R'; img[0x613] = 'P';
    put32(img + 0x614, BININFO_ID_PROGRAM_VERSION);
    put32(img + 0x618, 0x20000010u);      /* in RAM */
    put32(img + 0x500, base + 0x780);     /* source */
    put32(img + 0x504, 0x20000000u);      /* dest start */
    put32(img + 0x508, 0x20000100u);      /* dest end */
    strcpy((char *)img + 0x700, "prog");
    strcpy((char *)img + 0x790, "1.2.3");
    const char *s = bininfo_string(img, sizeof img, base, BININFO_ID_PROGRAM_NAME);
    CHECK(s && strcmp(s, "prog") == 0, "name: %s", s ? s : "(null)");
    s = bininfo_string(img, sizeof img, base, BININFO_ID_PROGRAM_VERSION);
    CHECK(s && strcmp(s, "1.2.3") == 0, "mapped version: %s", s ? s : "(null)");
    CHECK(!bininfo_string(img, sizeof img, base, 0x12345678u), "no such id");
    /* Damage: a pointer out of the image, an unterminated string, a
     * cut image, no header. */
    put32(img + 0x608, base + 0x7000);
    CHECK(!bininfo_string(img, sizeof img, base, BININFO_ID_PROGRAM_NAME), "pointer out");
    put32(img + 0x608, base + sizeof img - 2);
    img[sizeof img - 2] = 'x';
    img[sizeof img - 1] = 'y';
    CHECK(!bininfo_string(img, sizeof img, base, BININFO_ID_PROGRAM_NAME), "unterminated");
    CHECK(!bininfo_string(img, 0x300, base, BININFO_ID_PROGRAM_NAME), "cut");
    put32(img + 0x110, 0);
    CHECK(!bininfo_string(img, sizeof img, base, BININFO_ID_PROGRAM_NAME), "no header");

    /* The real ones. */
    struct { const char *file, *name, *version; } real[] = {
        { "stage3-pico2.uf2", "stage3",    "2.2" },
        { "loader-menu.uf2",  "uf2load",   "2.2" },
        { "pico-oric.uf2",    "pico-oric", NULL },
        { "pico-atom.uf2",    "pico-atom", NULL },
        { "pico-ace.uf2",     "pico-ace",  NULL },
    };
    for (size_t i = 0; i < sizeof real / sizeof real[0]; i++) {
        size_t n = slurp_host(real[i].file);
        if (!n) {
            printf("skip %s: not fetched (tools/fetch-suites.sh)\n", real[i].file);
            continue;
        }
        uint32_t addr = 0, len = decode(n, &addr);
        CHECK(len > 0, "%s: decoded", real[i].file);
        s = bininfo_string(image, len, addr, BININFO_ID_PROGRAM_NAME);
        CHECK(s && strcmp(s, real[i].name) == 0, "%s: name %s", real[i].file, s ? s : "(null)");
        s = bininfo_string(image, len, addr, BININFO_ID_PROGRAM_VERSION);
        CHECK(real[i].version ? s && strcmp(s, real[i].version) == 0 : s == NULL,
              "%s: version %s", real[i].file, s ? s : "(null)");
    }
    return 0;
}

static int test_real(void) {
    uf2_info_t info;
    struct { const char *file; uf2_result_t want; } real[] = {
        { "pico-oric.uf2",          UF2_FLASH },
        { "pico-atom.uf2",          UF2_FLASH },
        { "pico-ace.uf2",           UF2_FLASH },
        { "loader-menu.uf2",        UF2_RAM },
        { "stage3-pico2.uf2",       UF2_NOT_HERE },   /* family absolute */
        { "loader-menu-rp2040.uf2", UF2_NOT_HERE },
    };
    bool any = false;
    flash_junk(3);
    for (size_t i = 0; i < sizeof real / sizeof real[0]; i++) {
        size_t n = slurp_host(real[i].file);
        if (!n) continue;
        any = true;
        char path[64];
        snprintf(path, sizeof path, "/%s", real[i].file);
        CHECK(put(path, file_buf, n), "put %s", path);
        uf2_result_t r = check_file(path, &info);
        CHECK(r == real[i].want, "%s: %d, wanted %d", real[i].file, r, real[i].want);
        if (r != UF2_FLASH) continue;
        CHECK(info.blocks == n / UF2_BLOCK - 1, "%s: blocks %u of %zu", real[i].file, info.blocks,
              n / UF2_BLOCK);
    }
    if (!any) {
        printf("skip the real UF2s: not fetched (tools/fetch-suites.sh)\n");
        return 0;
    }
    /* Each emulator, then itself again, then the next. */
    const char *emus[] = { "/pico-oric.uf2", "/pico-atom.uf2", "/pico-ace.uf2", "/pico-oric.uf2" };
    for (size_t i = 0; i < 4; i++) {
        FILINFO fno;
        if (f_stat(emus[i], &fno) != FR_OK) continue;
        flash_count_reset();
        CHECK(flash_file(emus[i], &info) == UF2_FLASH, "%s", emus[i]);
        printf("%s: %u blocks, %u sectors, %u differed, %u written, %u pages; %ld erases\n",
               emus[i] + 1, info.blocks, info.sectors, info.differ, info.written, info.pages,
               flash_erases);
        flash_count_reset();
        CHECK(flash_file(emus[i], &info) == UF2_FLASH && info.differ == 0 && flash_ops == 0,
              "%s again: differ %u", emus[i], info.differ);
    }
    return 0;
}

/* ---- the shell ------------------------------------------------------- */

static shell_t sh;
static char    con[1 << 14];
static size_t  con_len;
static int     interrupt_after = -1;
static int     boots;
static char    boot_ram[SHELL_PATH_MAX + 1];
static bool    boot_was_ram;

static void sink(void *ctx, const char *buf, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n; i++)
        if (buf[i] != '\r' && con_len + 1 < sizeof con) con[con_len++] = buf[i];
    con[con_len] = '\0';
}

static uint64_t now(void *ctx) {
    (void)ctx;
    return 0;
}

static bool interrupted(void *ctx) {
    (void)ctx;
    if (interrupt_after < 0) return false;
    return interrupt_after-- == 0;
}

static void boot(void *ctx, const char *ram, const uf2_info_t *info) {
    (void)ctx;
    (void)info;
    boots++;
    boot_was_ram = ram != NULL;
    snprintf(boot_ram, sizeof boot_ram, "%s", ram ? ram : "");
}

static shell_loader_t loader = { NULL, { FLASH_BASE, FLASH_END, fl_read, fl_erase, fl_program, NULL },
                                 boot, NULL };

static int run(const char *line) {
    con_len = 0;
    con[0] = '\0';
    boots = 0;
    return shell_run(&sh, line);
}

static int test_shell(void) {
    flash_junk(9);
    CHECK(run("prog") == 127 && strcmp(con, "prog: not found\n") == 0, "no dir: '%s'", con);
    CHECK(f_mkdir("/pico2-apps") == FR_OK, "mkdir");
    CHECK(run("prog") == 127 && strcmp(con, "prog: not found\n") == 0, "not there: '%s'", con);

    make(FLASH_BASE, 50, -1, 11, 1, FAM_ARM_S);
    CHECK(put("/pico2-apps/prog.uf2", ubuf, ulen), "prog.uf2");
    flash_count_reset();
    CHECK(run("prog") == 0 && boots == 1 && !boot_was_ram, "prog: '%s' boots %d", con, boots);
    /* con drops CR: each count rewrites the one line. */
    CHECK(strcmp(con, "writing 1 of 4 sectorswriting 2 of 4 sectors"
                      "writing 3 of 4 sectorswriting 4 of 4 sectors\n") == 0, "prog: '%s'", con);
    CHECK(flash_holds(FLASH_BASE, 50, -1, 11), "prog in flash");
    /* Again: no writing, straight to the reboot. */
    flash_count_reset();
    CHECK(run("prog") == 0 && boots == 1 && con_len == 0 && flash_ops == 0, "again: '%s'", con);
    /* FAT names: any case. A path, absolute or relative. */
    CHECK(run("PROG") == 0 && boots == 1, "PROG: '%s'", con);
    CHECK(run("/pico2-apps/prog.uf2") == 0 && boots == 1, "by path: '%s'", con);
    CHECK(run("cd /pico2-apps") == 0 && run("./prog.uf2") == 0 && boots == 1 &&
              run("cd /") == 0,
          "relative: '%s'", con);
    CHECK(run("./prog") == 127 && strcmp(con, "./prog: no such file or directory\n") == 0,
          "a path is as given: '%s'", con);
    CHECK(run("/pico2-apps") == 127 && strcmp(con, "/pico2-apps: is a directory\n") == 0,
          "a directory: '%s'", con);

    /* A program that has not declared takes no arguments (test_args has
     * the rest, design.md §3.5), and none takes a redirection. */
    CHECK(run("prog x") == 2 && boots == 0 && strcmp(con, "prog: takes no arguments\n") == 0,
          "argument: '%s'", con);
    CHECK(run("prog > out") == 2 && boots == 0, "redirection: '%s'", con);
    CHECK(run("nosuch x") == 127 && strcmp(con, "nosuch: not found\n") == 0, "'%s'", con);
    CHECK(run("echo x | prog") == 2 && boots == 0, "pipe: '%s'", con);
    CHECK(run("echo x | prog > out") == 2 && boots == 0, "pipe, redirection: '%s'", con);

    /* $PATH: each directory in turn, ':' between them, relative ones
     * from the current directory; an empty entry is the current
     * directory, as in sh. */
    CHECK(f_mkdir("/bin2") == FR_OK && put("/bin2/other2.uf2", ubuf, ulen), "bin2");
    CHECK(run("other2") == 127, "not in $PATH: '%s'", con);
    CHECK(run("PATH=/nope:/bin2:/pico2-apps; other2") == 0 && boots == 1, "second: '%s'", con);
    CHECK(run("prog") == 0 && boots == 1, "third: '%s'", con);
    CHECK(run("cd /; PATH=bin2; other2") == 0 && boots == 1, "relative: '%s'", con);
    CHECK(run("PATH=; prog") == 127 && boots == 0, "empty: '%s'", con);
    CHECK(run("cd /bin2; PATH=; other2") == 0 && boots == 1, "empty is here: '%s'", con);
    CHECK(run("cd /bin2; PATH=/nope:; other2") == 0 && boots == 1, "trailing ':': '%s'", con);
    CHECK(run("cd /bin2; PATH=/nope::/pico2-apps; other2") == 0 && boots == 1, "'::': '%s'",
          con);
    CHECK(run("cd /; PATH=/nope:; other2") == 127 && boots == 0, "not here: '%s'", con);
    CHECK(run("unset PATH; prog") == 127 && boots == 0, "unset: '%s'", con);
    CHECK(run("PATH=/pico2-apps; prog") == 0 && boots == 1, "back: '%s'", con);

    /* A builtin wins over a program of the same name. */
    CHECK(put("/pico2-apps/pwd.uf2", ubuf, ulen), "pwd.uf2");
    CHECK(run("pwd") == 0 && boots == 0 && strcmp(con, "/\n") == 0, "pwd: '%s'", con);

    /* Refused before flash is touched. */
    flash_count_reset();
    make(FLASH_BASE, 8, -1, 1, 0, FAM_RP2040);
    CHECK(put("/pico2-apps/old.uf2", ubuf, ulen), "old.uf2");
    CHECK(run("old") == 126 && strcmp(con, "old: not for this device\n") == 0 && boots == 0,
          "RP2040: '%s'", con);
    CHECK(put("/pico2-apps/text.uf2", "hello\n", 6) && run("text") == 126 &&
              strcmp(con, "text: not a UF2 program\n") == 0,
          "text: '%s'", con);
    make(FLASH_BASE, 20, -1, 1, 0, FAM_ARM_S);
    ulen -= UF2_BLOCK;
    CHECK(put("/pico2-apps/cut.uf2", ubuf, ulen) && run("cut") == 126 &&
              strcmp(con, "cut: damaged UF2: blocks missing or out of order\n") == 0,
          "cut: '%s'", con);
    CHECK(flash_ops == 0, "nothing written for a refusal");

    /* Ctrl-C while checking: nothing written, no reboot, ^C. */
    make(FLASH_BASE, 50, -1, 12, 1, FAM_ARM_S);
    CHECK(put("/pico2-apps/other.uf2", ubuf, ulen), "other.uf2");
    flash_count_reset();
    interrupt_after = 2;
    CHECK(run("other; pwd") == 130 && boots == 0 && flash_ops == 0 && strcmp(con, "^C\n") == 0,
          "stopped: '%s'", con);
    interrupt_after = -1;
    CHECK(flash_holds(FLASH_BASE, 50, -1, 11), "prog still in flash");

    /* A flash failure says so. */
    flash_power = 1;
    CHECK(run("other") == 1 && boots == 0 &&
              strstr(con, "other: writing flash failed (flash error); flash may not hold a whole program\n"),
          "failed: '%s'", con);
    flash_power = -1;
    CHECK(run("other") == 0 && boots == 1 && flash_holds(FLASH_BASE, 50, -1, 12), "other: '%s'", con);

    /* A RAM program goes to stage3 by its 8.3 path. */
    make(UF2_SRAM_BASE, 64, -1, 1, 0, FAM_ARM_S);
    CHECK(put("/pico2-apps/longprogramname.uf2", ubuf, ulen), "ram uf2");
    flash_count_reset();
    CHECK(run("longprogramname") == 0 && boots == 1 && boot_was_ram &&
              strcmp(boot_ram, "/PICO2-~1/LONGPR~1.UF2") == 0 && flash_ops == 0,
          "RAM: '%s' -> '%s'", con, boot_ram);
    CHECK(put("/pico2-apps/ram.uf2", ubuf, ulen) && run("ram") == 0 &&
              strcmp(boot_ram, "/PICO2-~1/RAM.UF2") == 0,
          "RAM, 8.3 already: '%s'", boot_ram);

    /* menu: the loader's menu, a RAM program, never flashed. */
    CHECK(run("menu") == 1 && boots == 0 &&
              strcmp(con, "menu: /BOOT2350.ORG: no such file or directory\n") == 0,
          "no menu: '%s'", con);
    CHECK(run("menu x") == 1 && strcmp(con, "usage: menu, with no operand or redirection\n") == 0,
          "'%s'", con);
    make(FLASH_BASE, 50, -1, 13, 1, FAM_ARM_S);
    CHECK(put("/BOOT2350.ORG", ubuf, ulen), "flash menu");
    flash_count_reset();
    CHECK(run("menu") == 1 && boots == 0 && flash_ops == 0 &&
              strcmp(con, "menu: /BOOT2350.ORG: not a RAM program\n") == 0,
          "flash menu refused: '%s'", con);
    make(UF2_SRAM_BASE, 64, -1, 1, 0, FAM_ARM_S);
    CHECK(put("/BOOT2350.ORG", ubuf, ulen), "menu");
    FILINFO fno;
    CHECK(run("menu > /m.txt") == 1 && boots == 0 && f_stat("/m.txt.new", &fno) == FR_NO_FILE &&
              f_stat("/m.txt", &fno) == FR_NO_FILE,
          "menu > f: '%s'", con);
    /* Refused before the redirection is opened: a file there is kept. */
    CHECK(put("/m.txt", "kept\n", 5) && run("menu >> /m.txt") == 1 && boots == 0 &&
              f_stat("/m.txt", &fno) == FR_OK && fno.fsize == 5 &&
              run("menu > /m.txt") == 1 && f_stat("/m.txt", &fno) == FR_OK && fno.fsize == 5 &&
              f_stat("/m.txt.new", &fno) == FR_NO_FILE,
          "menu > f, f kept: '%s'", con);
    CHECK(run("menu < /m.txt") == 1 && boots == 0 &&
              strcmp(con, "menu: not with a redirection or a pipe\n") == 0,
          "menu < f: '%s'", con);
    CHECK(run("menu") == 0 && boots == 1 && strcmp(boot_ram, "/BOOT2350.ORG") == 0,
          "menu: '%s'", boot_ram);

    /* No loader, or one the shell cannot use. */
    loader.problem = "UF2 Loader 2.2 is unknown";
    CHECK(run("prog") == 1 && boots == 0 &&
              strcmp(con, "prog: cannot run programs: UF2 Loader 2.2 is unknown\n") == 0,
          "problem: '%s'", con);
    CHECK(run("menu") == 1 && boots == 0, "menu with a problem: '%s'", con);
    loader.problem = NULL;
    shell_set_loader(&sh, NULL);
    CHECK(run("prog") == 1 && strcmp(con, "prog: cannot run programs: no UF2 Loader here\n") == 0,
          "no loader: '%s'", con);
    shell_set_loader(&sh, &loader);

    /* No card: a name is not found, a path says why. */
    host_disk_close();
    CHECK(run("prog") == 127 && strcmp(con, "prog: not found\n") == 0, "no card: '%s'", con);
    CHECK(run("/pico2-apps/prog.uf2") == 127 &&
              strcmp(con, "/pico2-apps/prog.uf2: no card\n") == 0,
          "no card, path: '%s'", con);
    return 0;
}

int main(void) {
    if (test_bininfo()) return 1;
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    if (test_rules() || test_flashing() || test_power_loss() || test_card_loss() || test_real())
        return 1;

    const out_t out = { sink, NULL, false };
    const shell_hooks_t hooks = { now, interrupted, NULL, NULL, NULL, NULL };
    shell_init(&sh, &out, &hooks, "test");
    shell_set_loader(&sh, &loader);
    shell_start(&sh);
    if (test_shell()) return 1;
    TEST_DONE();
}
