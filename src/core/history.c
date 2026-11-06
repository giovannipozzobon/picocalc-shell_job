/* history.c — the lines typed at the prompt (history.h). */

#include "history.h"

#include <string.h>

#include "files.h"
#include "flavour.h"

/* Lines, oldest first, each NUL-ended. */
static char     s_hist[SHELL_HISTORY_BYTES];
static size_t   s_used;
static unsigned s_count;
static bool     s_changed;

static FIL          s_file;
static files_safe_t s_save;
static reader_t     s_rd;
static char         s_line[SHELL_LINE_MAX + 1];

void history_clear(void) {
    s_used = 0;
    s_count = 0;
    s_changed = false;
}

const char *history_get(unsigned i) {
    if (i >= s_count) return NULL;
    const char *p = s_hist;
    while (i--) p += strlen(p) + 1;
    return p;
}

unsigned history_count(void) {
    return s_count;
}

static bool blank(const char *s) {
    for (; *s; s++)
        if (*s != ' ' && *s != '\t') return false;
    return true;
}

void history_add(const char *line) {
    size_t n = strlen(line) + 1;
    if (blank(line) || n > sizeof s_hist) return;
    if (s_count && strcmp(history_get(s_count - 1), line) == 0) return;
    while (s_used + n > sizeof s_hist) {
        size_t first = strlen(s_hist) + 1;
        memmove(s_hist, s_hist + first, s_used - first);
        s_used -= first;
        s_count--;
    }
    memcpy(s_hist + s_used, line, n);
    s_used += n;
    s_count++;
    s_changed = true;
}

void history_load(void) {
    if (files_open_startup(&s_file, HISTORY_FILE) != FR_OK) return;
    history_clear();
    reader_init(&s_rd, &(input_t){ &s_file, NULL });
    bool whole;
    long n;
    while ((n = reader_line(&s_rd, s_line, sizeof s_line, &whole)) >= 0)
        if (whole) history_add(s_line);
    f_close(&s_file);
    s_changed = false;
}

const char *history_save(void) {
    if (!s_changed) return NULL;
    FRESULT r = f_mkdir("/.shell");
    if (r != FR_OK && r != FR_EXIST) return files_why(r);
    const char *err = files_safe_open(&s_save, HISTORY_FILE);
    if (err) return err;
    for (unsigned i = 0; i < s_count; i++) {
        const char *line = history_get(i);
        files_safe_write(&s_save, line, strlen(line));
        files_safe_write(&s_save, "\n", 1);
    }
    err = files_safe_commit(&s_save);
    if (!err) s_changed = false;
    return err;
}

const char *history_keep_cwd(const char *cwd) {
    FRESULT r = f_mkdir("/.shell");
    if (r != FR_OK && r != FR_EXIST) return files_why(r);
    const char *err = files_safe_open(&s_save, CWD_FILE);
    if (err) return err;
    files_safe_write(&s_save, cwd, strlen(cwd));
    files_safe_write(&s_save, "\n", 1);
    return files_safe_commit(&s_save);
}

bool history_take_cwd(char out[SHELL_PATH_MAX + 1]) {
    if (files_open_startup(&s_file, CWD_FILE) != FR_OK) return false;
    reader_init(&s_rd, &(input_t){ &s_file, NULL });
    bool whole;
    long n = reader_line(&s_rd, s_line, sizeof s_line, &whole);
    f_close(&s_file);
    f_unlink(CWD_FILE);
    if (n <= 0 || !whole || s_line[0] != '/' || (size_t)n > SHELL_PATH_MAX) return false;
    memcpy(out, s_line, (size_t)n + 1);
    return true;
}

static void save_line(const char *line) {
    files_safe_write(&s_save, line, strlen(line));
    files_safe_write(&s_save, "\n", 1);
}

const char *history_keep_args(const char *cwd, const char *program, const char *name, int argc,
                              char **argv) {
    FRESULT r = f_mkdir("/.shell");
    if (r != FR_OK && r != FR_EXIST) return files_why(r);
    const char *err = files_safe_open(&s_save, ARGS_FILE);
    if (err) return err;
    save_line("picocalc-args 1");
    save_line(SHELL_DOS ? "dos" : "unix");
    save_line(cwd);
    save_line(program);
    save_line(name);
    for (int i = 1; i < argc; i++) save_line(argv[i]);
    return files_safe_commit(&s_save);
}

void history_drop_args(void) {
    f_unlink(ARGS_FILE);
    f_unlink(ARGS_FILE ".new");
}

int cmd_history(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc != 1) {
        shell_error(sh, "usage: history\n");
        return 1;
    }
    for (unsigned i = 0; i < s_count && !shell_interrupted(sh); i++)
        out_printf(&sh->out, "%4u  %s\n", i + 1, history_get(i));
    return 0;
}
