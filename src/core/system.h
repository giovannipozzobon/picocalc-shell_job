/* system.h — the PicoCalc's own commands (design.md §4.5): `help`
 * (`man` in the UNIX flavour), `clear`, `phosphor`, `battery`,
 * `backlight`, `info`, `poweroff`, `reboot` and `bootsel`; and the UNIX
 * flavour's `date`, and the W build's `ntp` (design.md §3.8).
 *
 * Portable: the southbridge, the terminal's ink and the reboots are the
 * port's, through shell_system_t (shell.h). With a hook missing, as on
 * the host, its command says it cannot be done here.
 *
 * `poweroff`, `reboot` and `bootsel` leave the shell: each saves the
 * history first, as running a program does, and is refused with a
 * redirection or in a pipe. `reboot` comes back to the shell, not the
 * last program (design.md §3.1). */
#ifndef SHELL_SYSTEM_H
#define SHELL_SYSTEM_H

#include "shell.h"

int cmd_help(shell_t *sh, int argc, char **argv);
int cmd_clear(shell_t *sh, int argc, char **argv);
int cmd_phosphor(shell_t *sh, int argc, char **argv);
int cmd_battery(shell_t *sh, int argc, char **argv);
int cmd_backlight(shell_t *sh, int argc, char **argv);
int cmd_info(shell_t *sh, int argc, char **argv);
int cmd_poweroff(shell_t *sh, int argc, char **argv);
int cmd_reboot(shell_t *sh, int argc, char **argv);
int cmd_bootsel(shell_t *sh, int argc, char **argv);
int cmd_date(shell_t *sh, int argc, char **argv);
/* Only in the W build (ntp.h). */
int cmd_ntp(shell_t *sh, int argc, char **argv);
/* The W build's Bluetooth keyboard, what it is doing (design.md §3.10). */
int cmd_bt(shell_t *sh, int argc, char **argv);
/* Resends the screen to the HDMI mirror, in the builds that have one (design.md §3.11). */
int cmd_hdmi(shell_t *sh, int argc, char **argv);
/* Only in the W build too (httpd.h, design.md §3.9). */
int cmd_web(shell_t *sh, int argc, char **argv);

/* Whether `fn` leaves the shell, for run_cmd to refuse a redirection. */
bool system_leaves(shell_cmd_fn fn);

#endif /* SHELL_SYSTEM_H */
