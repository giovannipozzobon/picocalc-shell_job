/* loader.c — the shell's link to UF2 Loader (loader.h).
 *
 * After UF2 Loader v2.5 (GPL-3.0, pelrun): common/bootloader/proginfo.c
 * for the scratch commands, the flash end, the partition and the remap,
 * and ui/uf2.c for erasing and programming through the bootrom. The
 * protocol is stage3's, unchanged from 2.3 to 2.5 (2.2 had no commands),
 * so the shell runs programs only under those versions (design.md §8).
 */

#include "loader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "boot/picobin.h"
#include "hardware/flash.h"
#include "hardware/structs/qmi.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"

#include "bininfo.h"
#include "picocalc_shell.h"

/* proginfo.h */
#define BL_MAGIC 0xe98cc638u
enum { BOOT_DEFAULT, BOOT_SD, BOOT_UPDATE, BOOT_RAM };

/* stage3 leaves { magic, flash end } in the vector table of the RAM
 * program it loads, in the hole at 0x20 the Cortex-M33 does not use. */
#define BLINFO_ADDR (SRAM_BASE + 0x20u)

static_assert(PICOCALC_SHELL_LOADER_MAGIC == BL_MAGIC, "picocalc_shell.h: the magic");
static_assert(PICOCALC_SHELL_BOOT_SD == BOOT_SD, "picocalc_shell.h: BOOT_SD");
static_assert(PICOCALC_SHELL_ARGS_MAGIC0 == UF2_ARGS_MAGIC0 &&
                  PICOCALC_SHELL_ARGS_MAGIC1 == UF2_ARGS_MAGIC1 &&
                  PICOCALC_SHELL_ARGS_RECORD == UF2_ARGS_RECORD,
              "picocalc_shell.h: the arguments' record");
static_assert(XIP_BASE == 0x10000000u && SRAM_BASE == UF2_SRAM_BASE && SRAM_END == UF2_SRAM_END,
              "uf2.h: the RP2350's address map");

/* stage3's versions with this protocol. 2.5-shell is 2.5 with M7's patch
 * (patches/uf2loader-boot-shell.patch), whose commands are 2.5's. */
static const char *const k_known[] = { "2.3", "2.4", "2.4.1", "2.5", "2.5-shell" };

static uint8_t __attribute__((aligned(4))) s_workarea[4096];   /* the bootrom's, for the table */
static char s_version[16];
/* The path stage3 loads after BOOT_RAM. It must be in SRAM that the
 * reboot and stage3 leave alone until stage3 has opened the file, as
 * the loader's menu keeps its own (get_short_path in ui/uf2.c): below
 * stage3's own 8 KB of RAM (stage3/memmap_2350.ld), which loader_init
 * checks, since where .bss puts it moves with the shell's layout. */
#define STAGE3_RAM_BASE (0x20080000u - 0x2000u)
static char s_ram_path[SHELL_PATH_MAX + 1];
static char s_problem[64];

/* The app partition, partition 0. The table is loaded afresh: after a
 * RAM or watchdog boot, as over SWD, the bootrom has none loaded (the
 * loader's menu, always started by stage3 from flash, does not need to). */
static bool partition(uint32_t *offset, uint32_t *size) {
    if (rom_load_partition_table(s_workarea, sizeof s_workarea, true) != BOOTROM_OK) return false;
    uint32_t info[3];
    uint32_t want = PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION;
    if (rom_get_partition_table_info(info, 3, want | (0u << 24)) < 3) return false;
    if ((info[0] & PT_INFO_PARTITION_LOCATION_AND_FLAGS) == 0) return false;
    uint32_t first = (info[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                     PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (info[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                    PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    if (last < first || (last + 1u) * FLASH_SECTOR_SIZE > (16u << 20)) return false;
    *offset = first * FLASH_SECTOR_SIZE;
    *size = (last + 1u) * FLASH_SECTOR_SIZE - *offset;
    return true;
}

/* Map flash from `offset` at XIP_BASE, `size` bytes of it (bl_remap_flash). */
static void remap(uint32_t offset, uint32_t size) {
    static const uint32_t base_reset[] = { QMI_ATRANS0_BASE_RESET, QMI_ATRANS1_BASE_RESET,
                                           QMI_ATRANS2_BASE_RESET, QMI_ATRANS3_BASE_RESET };
    uint32_t sector_offset = offset >> 12;
    int remaining = (int)(size >> 12);
    for (int i = 0; i < 4; i++) {
        uint32_t v = (base_reset[i] + sector_offset) << QMI_ATRANS0_BASE_LSB;
        if (remaining >= (int)QMI_ATRANS0_SIZE_RESET) v |= QMI_ATRANS0_SIZE_RESET << QMI_ATRANS0_SIZE_LSB;
        else if (remaining > 0) v |= (uint32_t)remaining << QMI_ATRANS0_SIZE_LSB;
        qmi_hw->atrans[i] = v;
        remaining -= (int)QMI_ATRANS0_SIZE_RESET;
    }
    rom_flash_flush_cache();
}

/* ---- the app partition, for uf2.c --------------------------------------- */

static void fl_read(void *ctx, uint32_t addr, void *buf, uint32_t n) {
    (void)ctx;
    memcpy(buf, (const void *)addr, n);
}

static int flash_op(uint32_t op, uint32_t addr, uint32_t n, const void *buf) {
    cflash_flags_t flags = { (op << CFLASH_OP_LSB) |
                             (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                             (CFLASH_ASPACE_VALUE_RUNTIME << CFLASH_ASPACE_LSB) };
    int r = rom_flash_op(flags, addr, n, (uint8_t *)buf);
    rom_flash_flush_cache();
    return r;
}

/* Runtime addresses, through the remap: the bootrom also refuses any
 * outside the partition, by the partition table's permissions. */
static bool fl_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    return flash_op(CFLASH_OP_VALUE_ERASE, addr, FLASH_SECTOR_SIZE, NULL) >= 0;
}

static bool fl_program(void *ctx, uint32_t addr, const void *page) {
    (void)ctx;
    return flash_op(CFLASH_OP_VALUE_PROGRAM, addr, FLASH_PAGE_SIZE, page) >= 0;
}

/* ---- the reboot ---------------------------------------------------------- */

static void command(uint32_t mode, uint32_t arg) {
    watchdog_hw->scratch[1] = mode;
    watchdog_hw->scratch[2] = arg;
    watchdog_hw->scratch[0] = BL_MAGIC;
}

static void boot(void *ctx, const char *ram, const uf2_info_t *info) {
    (void)ctx;
#if PICO_SHELL_UART
    printf("\nrun: %lu blocks, %lu sectors, %lu differed, %lu written, %lu pages; "
           "check %lu us, write %lu us; %s %s\n",
           (unsigned long)info->blocks, (unsigned long)info->sectors,
           (unsigned long)info->differ, (unsigned long)info->written,
           (unsigned long)info->pages, (unsigned long)info->check_us,
           (unsigned long)info->write_us, ram ? "BOOT_RAM" : "reboot", ram ? ram : "");
    uart_tx_wait_blocking(uart_default);
#else
    (void)info;
#endif
    if (ram) {
        snprintf(s_ram_path, sizeof s_ram_path, "%s", ram);
        command(BOOT_RAM, (uint32_t)s_ram_path);
    } else {
        /* Every stage3 starts the app on this; M7's also skips its
         * watch for a key (design.md M7). */
        command(BOOT_DEFAULT, 0);
    }
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

void loader_reboot_shell(void) {
#if PICO_SHELL_UART
    uart_tx_wait_blocking(uart_default);
#endif
    command(BOOT_SD, 0);
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

/* ---- finding the loader ---------------------------------------------------- */

void loader_init(shell_loader_t *ld, loader_status_t *st) {
    memset(st, 0, sizeof *st);
    memset(ld, 0, sizeof *ld);
    ld->boot = boot;

    const volatile uint32_t *blinfo = (const volatile uint32_t *)BLINFO_ADDR;
    if (blinfo[0] == BL_MAGIC) st->flash_end = blinfo[1];

    /* After a flash boot, as of stage3, the bootrom has set up flash for
     * XIP. Started any other way, as over SWD, nothing has: set it up
     * here, in the bootrom's plain mode, so the partition and stage3's
     * version can still be reported; programs are refused below. */
    if (!st->flash_end) {
        rom_connect_internal_flash();
        rom_flash_exit_xip();
        rom_flash_flush_cache();
        rom_flash_enter_cmd_xip();
    }
    if (!partition(&st->part_offset, &st->part_size)) {
        st->part_offset = st->part_size = 0;
        ld->problem = "no UF2 Loader partition table";
        return;
    }

    /* stage3 is the image at the start of flash, before the partition:
     * read its version with flash mapped as it is at reset. */
    remap(0, st->part_offset);
    const char *v = bininfo_string((const uint8_t *)XIP_BASE, st->part_offset, XIP_BASE,
                                   BININFO_ID_PROGRAM_VERSION);
    if (v) {
        snprintf(s_version, sizeof s_version, "%s", v);
        st->version = s_version;
    }
    remap(st->part_offset, st->part_size);

    uint32_t end = XIP_BASE + st->part_size;
    if (st->flash_end && st->flash_end < end) end = st->flash_end;
    ld->flash = (uf2_flash_t){ XIP_BASE, end, fl_read, fl_erase, fl_program, NULL };

    if (!st->flash_end) {
        ld->problem = "not started by UF2 Loader";
        return;
    }
    if ((uintptr_t)s_ram_path + sizeof s_ram_path > STAGE3_RAM_BASE) {
        ld->problem = "BOOT_RAM path in stage3's RAM";
        return;
    }
    bool known = false;
    for (size_t i = 0; st->version && i < sizeof k_known / sizeof k_known[0]; i++)
        known |= strcmp(st->version, k_known[i]) == 0;
    if (!st->version) {
        ld->problem = "UF2 Loader version unreadable";
    } else if (!known) {
        snprintf(s_problem, sizeof s_problem, "UF2 Loader %s is unknown",
                 st->version);
        ld->problem = s_problem;
    }
}
