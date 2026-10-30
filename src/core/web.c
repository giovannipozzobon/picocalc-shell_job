/* web.c — web.uf2's requests and the card (web.h, design.md §3.9). */
#include "web.h"

#include <stdio.h>
#include <string.h>

#include "clock.h"
#include "path.h"

/* ---- the request -------------------------------------------------------- */

web_method_t web_request_line(const char *line, size_t n, char *path, size_t psz, char *query,
                              size_t qsz) {
    const char *sp = memchr(line, ' ', n);
    if (!sp) return WEB_BAD;
    size_t mlen = (size_t)(sp - line);
    const char *t = sp + 1;
    const char *end = line + n;
    const char *sp2 = memchr(t, ' ', (size_t)(end - t));
    if (!sp2 || sp2 == t) return WEB_BAD;
    const char *ver = sp2 + 1;
    if (end - ver < 8 || memcmp(ver, "HTTP/1.", 7) != 0 || end - ver != 8) return WEB_BAD;
    if (ver[7] < '0' || ver[7] > '9') return WEB_BAD;
    if (*t != '/') return WEB_BAD;

    const char *q = memchr(t, '?', (size_t)(sp2 - t));
    size_t plen = (size_t)((q ? q : sp2) - t);
    size_t qlen = q ? (size_t)(sp2 - q) - 1u : 0;
    if (plen + 1u > psz || qlen + 1u > qsz || plen + qlen + 1u > WEB_TARGET_MAX) return WEB_BAD;
    memcpy(path, t, plen);
    path[plen] = '\0';
    if (q) memcpy(query, q + 1, qlen);
    query[qlen] = '\0';

    if (mlen == 3 && memcmp(line, "GET", 3) == 0) return WEB_GET;
    if (mlen == 4 && memcmp(line, "POST", 4) == 0) return WEB_POST;
    return WEB_OTHER;
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int64_t web_content_length(const char *headers, size_t n) {
    static const char name[] = "content-length:";
    const size_t nlen = sizeof name - 1;
    const char *p = headers, *end = headers + n;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *next = eol ? eol + 1 : end;
        size_t len = (size_t)((eol ? eol : end) - p);
        if (len >= nlen) {
            size_t i = 0;
            while (i < nlen && lower(p[i]) == name[i]) i++;
            if (i == nlen) {
                const char *v = p + nlen, *e = p + len;
                while (v < e && (*v == ' ' || *v == '\t')) v++;
                while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
                if (v == e) return -2;
                int64_t value = 0;
                for (; v < e; v++) {
                    if (*v < '0' || *v > '9') return -2;
                    value = value * 10 + (*v - '0');
                    if (value > 0x7fffffff) return -2;
                }
                return value;
            }
        }
        p = next;
    }
    return -1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool web_query_get(const char *query, const char *key, char *out, size_t size) {
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > klen && p[klen] == '=' && memcmp(p, key, klen) == 0) {
            size_t o = 0;
            for (const char *v = p + klen + 1; v < end; v++) {
                char c = *v;
                if (c == '%') {
                    int h, l;
                    if (end - v < 3 || (h = hexval(v[1])) < 0 || (l = hexval(v[2])) < 0) return false;
                    c = (char)(h * 16 + l);
                    v += 2;
                } else if (c == '+') {
                    c = ' ';
                }
                if (c == '\0' || o + 1u >= size) return false;
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

/* ---- names -------------------------------------------------------------- */

bool web_oem_to_utf8(char *out, size_t size, const char *oem) {
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)oem; *s; s++) {
        uint32_t u = *s;
        if (u >= 0x80) {
            u = ff_oem2uni(*s, FF_CODE_PAGE);
            if (u == 0) u = '?';
        }
        size_t need = u < 0x80 ? 1u : u < 0x800 ? 2u : 3u;
        if (o + need + 1u > size) return false;
        if (need == 1) {
            out[o++] = (char)u;
        } else if (need == 2) {
            out[o++] = (char)(0xC0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (u >> 12));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    if (o + 1u > size) return false;
    out[o] = '\0';
    return true;
}

bool web_utf8_to_oem(char *out, size_t size, const char *utf8) {
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)utf8;
    while (*s) {
        uint32_t u;
        size_t extra;
        if (*s < 0x80) {
            u = *s;
            extra = 0;
        } else if ((*s & 0xE0) == 0xC0) {
            u = *s & 0x1Fu;
            extra = 1;
        } else if ((*s & 0xF0) == 0xE0) {
            u = *s & 0x0Fu;
            extra = 2;
        } else {
            return false;   /* a continuation byte, or beyond the BMP */
        }
        s++;
        for (size_t i = 0; i < extra; i++, s++) {
            if ((*s & 0xC0) != 0x80) return false;
            u = (u << 6) | (*s & 0x3Fu);
        }
        if ((extra == 1 && u < 0x80) || (extra == 2 && u < 0x800)) return false;   /* overlong */
        unsigned c = u < 0x80 ? u : ff_uni2oem(u, FF_CODE_PAGE);
        if (c == 0 || o + 2u > size) return false;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return true;
}

const char *web_card_path(const char *utf8, char out[SHELL_PATH_MAX + 1]) {
    static char oem[SHELL_PATH_MAX + 1];
    if (utf8[0] != '/') return "the path must start with /";
    if (!web_utf8_to_oem(oem, sizeof oem, utf8)) return "a name the card cannot spell";
    for (const char *c = oem; *c; c++) {
        unsigned char u = (unsigned char)*c;
        if (u < 0x20 || u == 0x7F || strchr("\\:*?\"<>|", *c)) return "invalid name";
    }
    /* `..` is refused rather than followed: the page never sends it. */
    for (const char *c = oem; *c;) {
        const char *e = c;
        while (*e && *e != '/') e++;
        if (e - c == 2 && c[0] == '.' && c[1] == '.') return "invalid name";
        c = *e ? e + 1 : e;
    }
    if (!path_resolve(out, "/", oem)) return "name too long";
    return NULL;
}

size_t web_json_string(char *out, size_t size, const char *utf8) {
    size_t o = 0;
    if (size < 3) return 0;
    out[o++] = '"';
    for (const unsigned char *s = (const unsigned char *)utf8; *s; s++) {
        char esc[7];
        size_t n;
        if (*s == '"' || *s == '\\') {
            esc[0] = '\\';
            esc[1] = (char)*s;
            n = 2;
        } else if (*s == '\n') {
            memcpy(esc, "\\n", 2);
            n = 2;
        } else if (*s == '\r') {
            memcpy(esc, "\\r", 2);
            n = 2;
        } else if (*s == '\t') {
            memcpy(esc, "\\t", 2);
            n = 2;
        } else if (*s < 0x20 || *s == 0x7F) {
            n = (size_t)snprintf(esc, sizeof esc, "\\u%04x", *s);
        } else {
            esc[0] = (char)*s;
            n = 1;
        }
        if (o + n + 2u > size) return 0;   /* the closing quote and the NUL */
        memcpy(out + o, esc, n);
        o += n;
    }
    out[o++] = '"';
    out[o] = '\0';
    return o;
}

/* ---- time --------------------------------------------------------------- */

bool web_unix_to_fat(int64_t unix_s, int off_min, uint16_t *fdate, uint16_t *ftime) {
    clock_tm_t tm;
    clock_gmt(unix_s + (int64_t)off_min * 60, &tm);
    if (tm.year < 1980 || tm.year > 2099) return false;
    clock_to_fat(&tm, fdate, ftime);
    return true;
}

int64_t web_fat_to_unix(uint16_t fdate, uint16_t ftime, int off_min) {
    clock_tm_t tm;
    clock_from_fat(fdate, ftime, &tm);
    if (tm.year < 1980) return 0;   /* an entry with no date */
    return clock_seconds(&tm) - (int64_t)off_min * 60;
}

/* ---- the card ----------------------------------------------------------- */

size_t web_info_json(char *out, size_t size) {
    const char *type;
    uint64_t total, avail;
    if (files_space(&type, &total, &avail) != FR_OK) return 0;
    int n = snprintf(out, size, "{\"type\":\"%s\",\"total\":%llu,\"free\":%llu}", type,
                     (unsigned long long)total, (unsigned long long)avail);
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

const char *web_list_open(web_list_t *l, const char *path) {
    FRESULT r = f_opendir(&l->dir, path);
    l->open = r == FR_OK;
    return r == FR_OK ? NULL : files_why(r);
}

size_t web_list_next(web_list_t *l, int off_min, char *out, size_t size, const char **why) {
    static char name[WEB_ENTRY_MAX], json[WEB_ENTRY_MAX];
    *why = NULL;
    if (!l->open) return 0;
    for (;;) {
        FRESULT r = f_readdir(&l->dir, &l->fno);
        if (r != FR_OK) {
            *why = files_why(r);
            return 0;
        }
        if (!l->fno.fname[0]) return 0;
        if (!web_oem_to_utf8(name, sizeof name, l->fno.fname) ||
            !web_json_string(json, sizeof json, name))
            continue;   /* cannot be shown: left out, as a name too long for a path is */
        bool dir = (l->fno.fattrib & AM_DIR) != 0;
        int n = snprintf(out, size, "{\"n\":%s,\"d\":%s,\"s\":%lu,\"t\":%lld}", json,
                         dir ? "true" : "false", dir ? 0ul : (unsigned long)l->fno.fsize,
                         (long long)web_fat_to_unix(l->fno.fdate, l->fno.ftime, off_min));
        if (n > 0 && (size_t)n < size) return (size_t)n;
    }
}

void web_list_close(web_list_t *l) {
    if (l->open) f_closedir(&l->dir);
    l->open = false;
}

const char *web_mkdir(const char *path) {
    if (strcmp(path, "/") == 0) return files_why(FR_EXIST);
    FRESULT r = f_mkdir(path);
    return r == FR_OK ? NULL : files_why(r);
}

const char *web_rename(const char *from, const char *to) {
    if (strcmp(from, "/") == 0 || strcmp(to, "/") == 0) return "not the root";
    if (path_within(to, from)) return "cannot move it into itself";
    FRESULT r = f_rename(from, to);
    return r == FR_OK ? NULL : files_why(r);
}

/* A file or an empty directory, whatever its attributes. */
static FRESULT remove_any(const char *path) {
    f_chmod(path, 0, AM_RDO | AM_HID | AM_SYS);
    return f_unlink(path);
}

/* A tree is deleted depth first, with a stack of open directories and
 * no recursion (the stack is small), a directory once what was in it
 * is gone. A failure is kept, the first, and the rest is still tried. */
static DIR     s_dirs[SHELL_TREE_DEPTH];
static size_t  s_lens[SHELL_TREE_DEPTH];
static char    s_tree[SHELL_PATH_MAX + 1];
static FILINFO s_entry;

const char *web_delete(const char *path) {
    if (strcmp(path, "/") == 0) return "not the root";
    FRESULT r = files_stat(path, &s_entry);
    if (r != FR_OK) return files_why(r);
    if (!(s_entry.fattrib & AM_DIR)) {
        r = remove_any(path);
        return r == FR_OK ? NULL : files_why(r);
    }
    const char *why = NULL;
    strcpy(s_tree, path);
    r = f_opendir(&s_dirs[0], s_tree);
    if (r != FR_OK) return files_why(r);
    s_lens[0] = strlen(s_tree);
    unsigned depth = 1;
    while (depth > 0) {
        unsigned top = depth - 1u;
        s_tree[s_lens[top]] = '\0';
        r = f_readdir(&s_dirs[top], &s_entry);
        if (r != FR_OK || !s_entry.fname[0]) {
            f_closedir(&s_dirs[top]);
            depth--;
            if (r != FR_OK && !why) why = files_why(r);
            r = remove_any(s_tree);   /* the directory, now empty if all went well */
            if (r != FR_OK && !why) why = files_why(r);
            continue;
        }
        size_t len = s_lens[top], n = strlen(s_entry.fname);
        if (len + 1u + n > SHELL_PATH_MAX) {
            if (!why) why = "name too long";
            continue;
        }
        s_tree[len] = '/';
        memcpy(s_tree + len + 1u, s_entry.fname, n + 1u);
        if (!(s_entry.fattrib & AM_DIR)) {
            r = remove_any(s_tree);
        } else if (depth == SHELL_TREE_DEPTH) {
            if (!why) why = "too many levels";
            continue;
        } else {
            r = f_opendir(&s_dirs[depth], s_tree);
            if (r == FR_OK) s_lens[depth++] = strlen(s_tree);
        }
        if (r != FR_OK && !why) why = files_why(r);
    }
    return why;
}

/* ---- an upload ---------------------------------------------------------- */

const char *web_upload_begin(web_upload_t *u, const char *path, int64_t unix_s, int off_min) {
    u->open = false;
    u->unix_s = unix_s;
    u->off_min = off_min;
    const char *why = files_safe_open(&u->w, path);
    if (!why) u->open = true;
    return why;
}

void web_upload_write(web_upload_t *u, const void *buf, size_t n) {
    if (u->open) files_safe_write(&u->w, buf, n);
}

const char *web_upload_end(web_upload_t *u) {
    if (!u->open) return "no upload";
    u->open = false;
    const char *why = files_safe_commit(&u->w);
    if (why) return why;
    uint16_t fdate, ftime;
    if (u->unix_s >= 0 && web_unix_to_fat(u->unix_s, u->off_min, &fdate, &ftime)) {
        FILINFO fno;
        fno.fdate = fdate;
        fno.ftime = ftime;
        f_utime(u->w.path, &fno);   /* the file is whole; a time left as now is harmless */
    }
    return NULL;
}

void web_upload_abort(web_upload_t *u) {
    if (!u->open) return;
    u->open = false;
    files_safe_abort(&u->w);
}
