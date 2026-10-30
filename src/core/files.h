/* files.h — the card's files: M1's builtins and redirection
 * (design.md §4.2, §4.5).
 *
 * Portable: FatFs's API only, over whatever disk the port gives it (the
 * SD card on the board, an image file on the host). Paths are resolved
 * against the shell's own current directory (path.h). Every write goes
 * to `name.new`, which is closed, then the old file is unlinked and the
 * new one renamed into place (EL §8.6), so a write that fails leaves the
 * old file as it was. Nothing here holds the card between commands, so
 * a card changed at the prompt is simply a new card. */
#ifndef SHELL_FILES_H
#define SHELL_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat.h"
#include "shell.h"

/* $HOME's default: cd with no operand, and the starting directory when
 * the card has it (design.md §4.3). */
#define SHELL_HOME "/home"

/* Whether ls and globs leave the name out: `.name`, macOS's `._name`
 * (hardware-notes.md §7.1), and anything FAT marks hidden or system,
 * such as Windows's `System Volume Information` (design.md §4.2). */
bool files_hidden(const FILINFO *fno);

/* Whether `path` (absolute) is a directory on the card. */
bool files_is_dir(const char *path);

/* Open a file the shell reads at startup (/etc/profile, the history):
 * if it is missing but name.new is there, the power went between the
 * unlink and the rename of a safe write, and name.new is whole. */
FRESULT files_open_startup(FIL *f, const char *path);

/* `> arg` or `>> arg` for the next command: sh->out becomes the file,
 * one for each level of sh->depth. False, with the error printed, if it
 * cannot be opened. */
bool files_redirect(shell_t *sh, const char *arg, bool append);
/* After the command: put the file in place, or keep the old one and say
 * why. False if the output was lost. The caller puts sh->out back. */
bool files_redirect_end(shell_t *sh);

/* `< arg` for the next command: sh->in. */
bool files_input(shell_t *sh, const char *arg);
/* After it: close the file. The caller puts sh->in back. */
void files_input_end(shell_t *sh);

/* Open `arg` to read, for a command that takes files: FR_OK, or the
 * failure with the error printed as "cmd: arg: why". */
FRESULT files_open_read(shell_t *sh, const char *cmd, const char *arg, FIL *f);

/* Leading options, such as -l -a or -la, up to "--" or the first
 * operand: each letter of `allowed` found sets its bit in *flags. The
 * index of the first operand, or -1 after printing the usage. */
int files_options(shell_t *sh, int argc, char **argv, const char *allowed, unsigned *flags,
                  const char *usage);

/* Make `path` (absolute, a directory) the current directory, each name
 * spelled as the card has it: NULL, or why not. */
const char *files_chdir(shell_t *sh, const char *path);

/* Copy the file `src` to `dst`, both absolute, through dst.new: NULL,
 * or why not, with *which the one of the two it is about; or
 * FILES_STOPPED if Break, Esc or Ctrl-C stopped it, and nothing was put
 * in place. */
extern const char FILES_STOPPED[];
const char *files_copy(shell_t *sh, const char *src, const char *dst, const char **which);

/* A walk down the tree under a directory, for the DOS flavour's /S,
 * XCOPY and DELTREE: depth first, in the card's order, one directory
 * open at each level (SHELL_TREE_DEPTH of them). files_walk opens the
 * root: FR_OK, or why not. Each files_walk_next gives one of:
 *   WALK_FILE   a file: *path, and its entry in *fno;
 *   WALK_DIR    a directory, entered if *r is FR_OK; otherwise not, *r
 *               saying why (FILES_TOO_DEEP: more levels than there are);
 *   WALK_LEAVE  a directory entered before, root included, now closed,
 *               after everything below it; *r is FR_OK, or why its
 *               listing ended early;
 *   WALK_DONE   the end.
 * A name too long for a path comes as WALK_FILE or WALK_DIR with *r
 * FR_INVALID_NAME, *path its directory's. *path is good until the next
 * step. One walk at a time; it shares cp -r's and rm -r's stack. */
typedef enum { WALK_FILE, WALK_DIR, WALK_LEAVE, WALK_DONE } walk_t;
#define FILES_TOO_DEEP FR_INT_ERR
FRESULT files_walk(const char *root);
walk_t files_walk_next(const char **path, const FILINFO **fno, FRESULT *r);
/* Right after WALK_DIR entered one: go no further down it, and have no
 * WALK_LEAVE for it. */
void files_walk_skip(void);
/* Stop: close every directory open. */
void files_walk_end(void);

/* The card in a few words, for `info`: "FAT32, 30G, 12G free". */
FRESULT files_card(char *buf, size_t size);

int cmd_cat(shell_t *sh, int argc, char **argv);
int cmd_cd(shell_t *sh, int argc, char **argv);
int cmd_cp(shell_t *sh, int argc, char **argv);
int cmd_df(shell_t *sh, int argc, char **argv);
int cmd_ls(shell_t *sh, int argc, char **argv);
int cmd_mkdir(shell_t *sh, int argc, char **argv);
int cmd_mv(shell_t *sh, int argc, char **argv);
int cmd_pwd(shell_t *sh, int argc, char **argv);
int cmd_rm(shell_t *sh, int argc, char **argv);
int cmd_rmdir(shell_t *sh, int argc, char **argv);
int cmd_touch(shell_t *sh, int argc, char **argv);

#endif /* SHELL_FILES_H */
