/* web.h — what web.uf2 does with a request, apart from the network
 * (design.md §3.9): reading the request, the card's names in UTF-8 and
 * back, JSON text, timestamps, and each operation on the card.
 *
 * Portable: FatFs's API only, over whatever disk the port gives it. It
 * keeps the shell's rules for the card: a file is written through
 * name.new and put in place whole (EL §8.6, fat.h), and a name FAT
 * cannot hold is refused before FatFs sees it. The card's names are CP437
 * (ffconf.h) and a browser's are UTF-8, so every name is converted both
 * ways; one the page sends that CP437 cannot spell is refused.
 *
 * Needs files_mount() done. One upload, one listing and one delete at a
 * time, as the shell's own commands are. */
#ifndef SHELL_WEB_H
#define SHELL_WEB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "ff.h"

/* ---- the request -------------------------------------------------------- */

typedef enum {
    WEB_BAD,       /* not an HTTP/1.x request line, or a target too long */
    WEB_GET,
    WEB_POST,
    WEB_OTHER,     /* a well-formed request of another method */
} web_method_t;

/* "GET /api/list?path=/a%20b HTTP/1.1", without its CR LF: the method,
 * and the target split at the first '?' into path[psz] and query[qsz]
 * (both as sent, not decoded; query is "" with none). */
web_method_t web_request_line(const char *line, size_t n, char *path, size_t psz, char *query,
                              size_t qsz);

/* The Content-Length in the header lines (each ending in CR LF, as many
 * as were read): its value, or -1 with none, -2 if it is not a number or
 * is over 2^31. The name is matched without regard to case. */
int64_t web_content_length(const char *headers, size_t n);

/* The value of `key` in a query, decoded (%XX, and '+' as a space):
 * false if it is not there, does not fit `size` with its NUL, or has a
 * bad %XX or a NUL in it. */
bool web_query_get(const char *query, const char *key, char *out, size_t size);

/* ---- names -------------------------------------------------------------- */

/* A name on the card (CP437) as UTF-8, and back. False if it does not
 * fit, or, going back, is not UTF-8 or has a character CP437 lacks. */
bool web_oem_to_utf8(char *out, size_t size, const char *oem);
bool web_utf8_to_oem(char *out, size_t size, const char *utf8);

/* A path from the page, in UTF-8, as the card's absolute path in CP437:
 * NULL, or why not. It must start with '/'; `.` and repeated '/' are
 * taken out, and `..`, a control character and any of \ : * ? " < > | are
 * refused, as is a name CP437 cannot spell. */
const char *web_card_path(const char *utf8, char out[SHELL_PATH_MAX + 1]);

/* The string as JSON text, with its quotes, into out: its length, or 0
 * if it does not fit. The bytes are UTF-8 as they are. */
size_t web_json_string(char *out, size_t size, const char *utf8);

/* ---- time --------------------------------------------------------------- */

/* A Unix time (UTC) and FAT's local timestamp, with the local offset
 * from UTC in minutes (the clock's record has it, as it was when the shell
 * was left). False if the year is not 1980 to 2099. */
bool web_unix_to_fat(int64_t unix_s, int off_min, uint16_t *fdate, uint16_t *ftime);
int64_t web_fat_to_unix(uint16_t fdate, uint16_t ftime, int off_min);

/* ---- the card ----------------------------------------------------------- */

/* {"type":"FAT32","total":n,"free":n}, in bytes, into out: its length,
 * or 0 if the card cannot be read or it does not fit. */
size_t web_info_json(char *out, size_t size);

typedef struct {
    DIR     dir;
    FILINFO fno;
    bool    open;
} web_list_t;

/* List a directory (a card path): NULL, or why not. */
const char *web_list_open(web_list_t *l, const char *path);
/* The next entry as {"n":"name","d":false,"s":size,"t":unix} in out,
 * which should hold WEB_ENTRY_MAX: its length, or 0 at the end or after
 * an error, and then *why is NULL or what it was. */
size_t web_list_next(web_list_t *l, int off_min, char *out, size_t size, const char **why);
void web_list_close(web_list_t *l);

/* Make a directory, rename a file or directory (not onto another, not
 * the root, not into itself), delete a file or a directory with all
 * under it (not the root): NULL, or why not. */
const char *web_mkdir(const char *path);
const char *web_rename(const char *from, const char *to);
const char *web_delete(const char *path);

/* An upload: the bytes of a file written to path.new, and put in place
 * whole by web_upload_end() with the modification time `unix_s` (UTC;
 * negative for the time now). */
#include "fat.h"
typedef struct {
    files_safe_t w;
    bool         open;
    int64_t      unix_s;
    int          off_min;
} web_upload_t;

const char *web_upload_begin(web_upload_t *u, const char *path, int64_t unix_s, int off_min);
/* The first failure is kept and the later writes do nothing. */
void web_upload_write(web_upload_t *u, const void *buf, size_t n);
/* NULL, or why the old file (if any) stays. */
const char *web_upload_end(web_upload_t *u);
/* The connection dropped: nothing is left behind. */
void web_upload_abort(web_upload_t *u);

#endif /* SHELL_WEB_H */
