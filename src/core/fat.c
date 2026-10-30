/* fat.c — the card through FatFs, with nothing of the shell in it
 * (fat.h). */

#include "fat.h"

#include <string.h>

static FATFS   s_fs;
static FILINFO s_fno;   /* files_safe_open's: the shell's stack is 4 KB (files.c) */

const char *files_why(FRESULT r) {
    switch (r) {
    case FR_OK:                  return "ok";
    case FR_DISK_ERR:            return "card I/O error";
    case FR_NOT_READY:           return "no card";
    case FR_NO_FILE:
    case FR_NO_PATH:             return "no such file or directory";
    case FR_INVALID_NAME:        return "invalid name";
    case FR_DENIED:              return "permission denied";
    case FR_EXIST:               return "file exists";
    case FR_WRITE_PROTECTED:     return "card is write-protected";
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:       return "no FAT volume on the card";
    case FR_LOCKED:              return "file in use";
    case FR_TOO_MANY_OPEN_FILES: return "too many open files";
    default:                     return "filesystem error";
    }
}

static const char *const NO_SPACE = "no space left on the card";

void files_mount(void) {
    f_mount(&s_fs, "", 0);
}

void files_unmount(void) {
    f_mount(NULL, "", 0);
}

FRESULT files_stat(const char *path, FILINFO *fno) {
    if (strcmp(path, "/") == 0) {
        DIR d;
        FRESULT r = f_opendir(&d, "/");
        if (r == FR_OK) f_closedir(&d);
        memset(fno, 0, sizeof *fno);
        fno->fattrib = AM_DIR;
        return r;
    }
    return f_stat(path, fno);
}


const char *files_safe_open(files_safe_t *w, const char *path) {
    w->why = NULL;
    size_t n = strlen(path);
    if (n + 4u > SHELL_PATH_MAX) return "name too long";
    FRESULT r = files_stat(path, &s_fno);
    if (r == FR_OK) {
        if (s_fno.fattrib & AM_DIR) return "is a directory";
        if (s_fno.fattrib & AM_RDO) return "read-only";
    } else if (r != FR_NO_FILE) {
        return files_why(r);
    }
    memcpy(w->path, path, n + 1);
    memcpy(w->tmp, path, n);
    memcpy(w->tmp + n, ".new", 5);
    r = f_open(&w->fil, w->tmp, FA_CREATE_ALWAYS | FA_WRITE);
    return r == FR_OK ? NULL : files_why(r);
}

void files_safe_write(files_safe_t *w, const void *buf, size_t n) {
    if (w->why) return;
    UINT done;
    FRESULT r = f_write(&w->fil, buf, (UINT)n, &done);
    if (r != FR_OK) w->why = files_why(r);
    else if (done < n) w->why = NO_SPACE;
}

void files_safe_abort(files_safe_t *w) {
    f_close(&w->fil);
    f_unlink(w->tmp);
}

const char *files_safe_commit(files_safe_t *w) {
    FRESULT r = f_close(&w->fil);
    if (!w->why && r != FR_OK) w->why = files_why(r);
    if (w->why) {
        f_unlink(w->tmp);
        return w->why;
    }
    r = f_unlink(w->path);
    if (r != FR_OK && r != FR_NO_FILE) {
        f_unlink(w->tmp);
        return files_why(r);
    }
    r = f_rename(w->tmp, w->path);
    /* The old file is gone and the new one is whole, under its .new name. */
    if (r != FR_OK) return "left as .new";
    return NULL;
}

FRESULT files_space(const char **type, uint64_t *total, uint64_t *avail) {
    DWORD free_clusters;
    FATFS *fs;
    FRESULT r = f_getfree("", &free_clusters, &fs);
    if (r != FR_OK) return r;
    uint64_t cluster = (uint64_t)fs->csize * FF_MAX_SS;
    *total = (uint64_t)(fs->n_fatent - 2u) * cluster;
    *avail = (uint64_t)free_clusters * cluster;
    *type = fs->fs_type == FS_FAT12 ? "FAT12" : fs->fs_type == FS_FAT16 ? "FAT16" : "FAT32";
    return FR_OK;
}

