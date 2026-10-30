/* fat.h — the card through FatFs, with nothing of the shell in it
 * (design.md §4.2, §3.9).
 *
 * What a program that has FatFs and no shell needs: web.uf2 links this
 * and not files.c, whose commands pull the rest of the shell in with
 * them. The shell's own files.h includes it, so its callers see the same
 * names. Writes go through `name.new`, which is closed, then the old file
 * is unlinked and the new one renamed into place (EL §8.6), so a write
 * that fails leaves the old file as it was. */
#ifndef SHELL_FAT_H
#define SHELL_FAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "ff.h"

/* Register the volume; FatFs mounts it on first use, and again after
 * the port's disk layer reports the card changed. */
void files_mount(void);

/* Let the volume go, for usbdisk: nothing may be open. */
void files_unmount(void);

/* FatFs's result in words: "no card", "no such file or directory". */
const char *files_why(FRESULT r);

/* f_stat, with the root, which FatFs cannot stat, as a directory. */
FRESULT files_stat(const char *path, FILINFO *fno);

/* A write through name.new (EL §8.6), for any command that saves a file. */
typedef struct {
    FIL         fil;
    const char *why;                        /* the first failure, or NULL */
    char        path[SHELL_PATH_MAX + 1];
    char        tmp[SHELL_PATH_MAX + 5];    /* path + ".new" */
} files_safe_t;

/* Open `path`.new for writing: NULL, or why not. A directory or a file
 * marked read-only is refused. */
const char *files_safe_open(files_safe_t *w, const char *path);
/* Write to it; the first failure is kept in w->why and later writes do
 * nothing. */
void files_safe_write(files_safe_t *w, const void *buf, size_t n);
/* Close and remove name.new, leaving the old file. */
void files_safe_abort(files_safe_t *w);
/* Close name.new and put it in place: NULL, or why the old file stays. */
const char *files_safe_commit(files_safe_t *w);

/* The volume's type, size and free space, as df, info and DIR show them. */
FRESULT files_space(const char **type, uint64_t *total, uint64_t *avail);

#endif /* SHELL_FAT_H */
