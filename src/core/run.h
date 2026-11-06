/* run.h — programs: found by name, checked, flashed if they differ, and
 * started by a reboot through stage3; and `menu` (design.md §3.3, §4.3).
 *
 * Portable. What stage3 is told, and how, is the port's (loader.c); the
 * core sees the app partition as a uf2_flash_t and a boot hook. */
#ifndef SHELL_RUN_H
#define SHELL_RUN_H

#include <stdbool.h>

#include "shell.h"
#include "uf2.h"

/* $PATH's default: where programs are looked for (design.md §4.3). */
#define SHELL_PROGRAM_DIR "/pico2-apps"
#define SHELL_PROGRAM_EXT ".uf2"

/* Where a command's name would be run from, or NULL, saying nothing: a
 * program or a script, as `which` shows. */
const char *run_which(shell_t *sh, const char *name);

/* The script a command's name means in the UNIX flavour, or NULL: a path
 * to a file named .sh or starting #!, or a name ending .sh on $PATH. */
const char *run_script_path(shell_t *sh, const char *name);
/* The loader's own menu, kept beside the shell (design.md §3.2). */
#define SHELL_MENU_UF2 "/BOOT2350.ORG"

typedef struct shell_loader {
    /* NULL when programs can be run; otherwise why not, as "no UF2
     * Loader partition table". */
    const char *problem;
    uf2_flash_t flash;
    /* Reboot through stage3: with `ram` NULL it starts the program in
     * flash; otherwise it loads the RAM program at that path, every
     * name in it 8.3 (stage3 has no long names). Never returns on the
     * board. `info` is the run's, for the development build's log. */
    void (*boot)(void *ctx, const char *ram, const uf2_info_t *info);
    void *ctx;
} shell_loader_t;

/* A command that is not a builtin: a program by name or path. Prints
 * "not found", status 127, if there is none. */
int run_program(shell_t *sh, int argc, char **argv, bool redirected);

/* The RAM program at `path`, which must be one, as `name`: `menu`.
 * Returns only if it cannot be run, or on the host. */
int run_ram(shell_t *sh, const char *name, const char *path);

/* The RAM program `name`, looked up as any program is, for the W
 * build's `ntp`: 127, with "not found" said, if there is none. */
int run_ram_program(shell_t *sh, const char *name);

int cmd_menu(shell_t *sh, int argc, char **argv);

#endif /* SHELL_RUN_H */
