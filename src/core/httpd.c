/* httpd.c — one HTTP connection (httpd.h, design.md §3.9). */
#include "httpd.h"

#include <stdio.h>
#include <string.h>

#include "fat.h"
#include "path.h"

#define HDR_PATH_MAX 48u

enum { ST_LINE, ST_BODY, ST_UPLOAD, ST_SEND };
enum { SRC_NONE, SRC_MEM, SRC_FILE, SRC_LIST };

static void note(httpd_t *h, const char *fmt, const char *name) {
    if (!h->env->note) return;
    char text[64];
    snprintf(text, sizeof text, fmt, name);
    h->env->note(h->env->ctx, text);
}

/* ---- the response ------------------------------------------------------- */

/* The card's reasons (files_why and web.c's) as HTTP statuses. */
static const char *status_for(const char *why) {
    static const struct {
        const char *why, *status;
    } table[] = {
        { "no such file or directory", "404 Not Found" },
        { "file exists", "409 Conflict" },
        { "is a directory", "409 Conflict" },
        { "file in use", "409 Conflict" },
        { "permission denied", "403 Forbidden" },
        { "read-only", "403 Forbidden" },
        { "card is write-protected", "403 Forbidden" },
        { "not the root", "403 Forbidden" },
        { "invalid name", "400 Bad Request" },
        { "name too long", "400 Bad Request" },
        { "cannot move it into itself", "400 Bad Request" },
        { "no space left on the card", "507 Insufficient Storage" },
        { "no card", "503 Service Unavailable" },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(why, table[i].why) == 0) return table[i].status;
    return "500 Internal Server Error";
}

/* The header, in out. `length` is the body's, or -1 if it ends with the
 * connection. */
static void begin(httpd_t *h, const char *status, const char *type, int64_t length,
                  const char *extra) {
    int n = snprintf((char *)h->out, sizeof h->out,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n"
                     "X-Content-Type-Options: nosniff\r\n%s",
                     status, type, extra ? extra : "");
    if (length >= 0) n += snprintf((char *)h->out + n, sizeof h->out - (size_t)n, "Content-Length: %lld\r\n", (long long)length);
    n += snprintf((char *)h->out + n, sizeof h->out - (size_t)n, "\r\n");
    h->out_len = (size_t)n;
    h->out_off = 0;
    h->state = ST_SEND;
    h->src = SRC_NONE;
    h->src_done = true;
}

static void send_text(httpd_t *h, const char *status, const char *type, const char *body) {
    size_t n = strlen(body);
    if (n > 512) n = 512;
    begin(h, status, type, (int64_t)n, "Cache-Control: no-store\r\n");
    memcpy(h->out + h->out_len, body, n);
    h->out_len += n;
}

static void send_error(httpd_t *h, const char *status, const char *msg) {
    send_text(h, status, "text/plain", msg);
}

static void send_why(httpd_t *h, const char *why) {
    send_error(h, status_for(why), why);
}

static void send_ok(httpd_t *h) {
    send_text(h, "200 OK", "application/json", "{\"ok\":true}");
}

/* A card path from the query, as the card spells it: false, with the
 * answer made, if there is none or it is refused. */
static bool query_path(httpd_t *h, const char *key, char out[SHELL_PATH_MAX + 1]) {
    static char utf8[WEB_TARGET_MAX + 1];
    if (!web_query_get(h->query, key, utf8, sizeof utf8)) {
        send_error(h, "400 Bad Request", "bad path");
        return false;
    }
    const char *why = web_card_path(utf8, out);
    if (why) {
        send_error(h, "400 Bad Request", why);
        return false;
    }
    return true;
}

static bool clock_now(httpd_t *h, int64_t *utc_s, int *off_min) {
    *off_min = 0;
    return h->env->now && h->env->now(h->env->ctx, utc_s, off_min);
}

/* ---- routes ------------------------------------------------------------- */

static void route_page(httpd_t *h) {
    begin(h, "200 OK", "text/html; charset=utf-8", (int64_t)h->env->page_len,
          "Cache-Control: no-cache\r\nContent-Encoding: gzip\r\n");
    h->src = SRC_MEM;
    h->mem = h->env->page;
    h->mem_left = h->env->page_len;
    h->src_done = h->mem_left == 0;
}

static void route_info(httpd_t *h) {
    char json[96];
    if (web_info_json(json, sizeof json) == 0) {
        send_error(h, "503 Service Unavailable", "the card cannot be read");
        return;
    }
    send_text(h, "200 OK", "application/json", json);
}

static void route_list(httpd_t *h) {
    static char path[SHELL_PATH_MAX + 1], utf8[3u * SHELL_PATH_MAX + 1], json[6u * SHELL_PATH_MAX + 8];
    if (!query_path(h, "path", path)) return;
    const char *why = web_list_open(&h->u.list, path);
    if (why) {
        send_why(h, why);
        return;
    }
    if (!web_oem_to_utf8(utf8, sizeof utf8, path) || !web_json_string(json, sizeof json, utf8)) {
        web_list_close(&h->u.list);
        send_error(h, "500 Internal Server Error", "the name cannot be shown");
        return;
    }
    int64_t now_s;
    clock_now(h, &now_s, &h->off_min);
    begin(h, "200 OK", "application/json", -1, "Cache-Control: no-store\r\n");
    h->out_len += (size_t)snprintf((char *)h->out + h->out_len, sizeof h->out - h->out_len,
                                   "{\"path\":%s,\"entries\":[", json);
    h->src = SRC_LIST;
    h->src_done = false;
    h->first_entry = true;
}

/* How a file is shown: text as text, the page's own kinds of file as
 * they are, and anything that could run in the page as plain text. */
static const char *view_type(const char *name) {
    static const struct {
        const char *ext, *type;
    } table[] = {
        { "txt", "text/plain; charset=utf-8" }, { "cfg", "text/plain; charset=utf-8" },
        { "log", "text/plain; charset=utf-8" }, { "md", "text/plain; charset=utf-8" },
        { "csv", "text/plain; charset=utf-8" }, { "ini", "text/plain; charset=utf-8" },
        { "c", "text/plain; charset=utf-8" },   { "h", "text/plain; charset=utf-8" },
        { "bat", "text/plain; charset=utf-8" }, { "sh", "text/plain; charset=utf-8" },
        { "htm", "text/plain; charset=utf-8" }, { "html", "text/plain; charset=utf-8" },
        { "json", "application/json" },         { "pdf", "application/pdf" },
        { "png", "image/png" },                 { "jpg", "image/jpeg" },
        { "jpeg", "image/jpeg" },               { "gif", "image/gif" },
    };
    const char *dot = strrchr(name, '.');
    if (dot) {
        for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
            const char *a = dot + 1, *b = table[i].ext;
            while (*a && *b && ((*a | 0x20) == *b)) a++, b++;
            if (!*a && !*b) return table[i].type;
        }
    }
    return "application/octet-stream";
}

/* `name` as RFC 5987 percent-encoded UTF-8 for Content-Disposition:
 * false if it does not fit. */
static bool encode_name(char *out, size_t size, const char *name) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)name; *s; s++) {
        bool plain = (*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') ||
                     (*s >= 'a' && *s <= 'z') || *s == '.' || *s == '-' || *s == '_';
        if (o + (plain ? 1u : 3u) + 1u > size) return false;
        if (plain) {
            out[o++] = (char)*s;
        } else {
            out[o++] = '%';
            out[o++] = hex[*s >> 4];
            out[o++] = hex[*s & 15];
        }
    }
    out[o] = '\0';
    return true;
}

static void route_file(httpd_t *h) {
    static char path[SHELL_PATH_MAX + 1], utf8[3u * SHELL_PATH_MAX + 1], extra[512];
    static char dl[8];
    if (!query_path(h, "path", path)) return;
    static FILINFO fno;
    FRESULT r = files_stat(path, &fno);
    if (r != FR_OK) {
        send_why(h, files_why(r));
        return;
    }
    if (fno.fattrib & AM_DIR) {
        send_error(h, "409 Conflict", "is a directory");
        return;
    }
    r = f_open(&h->u.file, path, FA_READ);
    if (r != FR_OK) {
        send_why(h, files_why(r));
        return;
    }
    const char *name = path_base(path);
    bool download = web_query_get(h->query, "dl", dl, sizeof dl) && strcmp(dl, "0") != 0;
    snprintf(extra, sizeof extra, "Cache-Control: no-store\r\n");
    if (download) {
        static const char head[] = "Cache-Control: no-store\r\nContent-Disposition: attachment; filename*=UTF-8''";
        memcpy(extra, head, sizeof head);
        char *enc = extra + sizeof head - 1;
        size_t room = sizeof extra - (sizeof head - 1) - 3u;   /* CR LF, NUL */
        if (!web_oem_to_utf8(utf8, sizeof utf8, name) || !encode_name(enc, room, utf8))
            encode_name(enc, room, "download");   /* a name too long for the header */
        strcat(extra, "\r\n");
    }
    begin(h, "200 OK", download ? "application/octet-stream" : view_type(name), (int64_t)f_size(&h->u.file),
          extra);
    h->src = SRC_FILE;
    h->file_left = (uint32_t)f_size(&h->u.file);
    h->src_done = h->file_left == 0;
    if (h->src_done) f_close(&h->u.file);
    note(h, "Sending %.30s", name);
}

static void route_mkdir(httpd_t *h) {
    static char path[SHELL_PATH_MAX + 1];
    if (!query_path(h, "path", path)) return;
    const char *why = web_mkdir(path);
    if (why) send_why(h, why);
    else send_ok(h);
}

static void route_delete(httpd_t *h) {
    static char path[SHELL_PATH_MAX + 1];
    if (!query_path(h, "path", path)) return;
    const char *why = web_delete(path);
    if (why) {
        send_why(h, why);
        return;
    }
    note(h, "Deleted %.32s", path_base(path));
    send_ok(h);
}

static void route_rename(httpd_t *h) {
    static char from[SHELL_PATH_MAX + 1], to[SHELL_PATH_MAX + 1];
    if (!query_path(h, "from", from) || !query_path(h, "to", to)) return;
    const char *why = web_rename(from, to);
    if (why) send_why(h, why);
    else send_ok(h);
}

/* ---- an upload ---------------------------------------------------------- */

static void upload_fail(httpd_t *h, const char *status, const char *why) {
    if (h->up_open) {
        web_upload_abort(&h->u.up);
        h->up_open = false;
    }
    if (!h->up_err) {
        h->up_status = status;
        h->up_err = why;
    }
}

/* The headers of an upload are in; the body is written as it arrives. */
static void upload_begin(httpd_t *h) {
    static char path[SHELL_PATH_MAX + 1], utf8[WEB_TARGET_MAX + 1], num[24];
    h->state = ST_UPLOAD;
    h->up_err = NULL;
    h->up_open = false;
    if (!web_query_get(h->query, "path", utf8, sizeof utf8)) {
        upload_fail(h, "400 Bad Request", "bad path");
        return;
    }
    const char *why = web_card_path(utf8, path);
    if (why) {
        upload_fail(h, "400 Bad Request", why);
        return;
    }
    int64_t now_s = -1, mtime = -1;
    int off = 0;
    bool known = clock_now(h, &now_s, &off);
    if (web_query_get(h->query, "mtime", num, sizeof num)) {
        int64_t v = 0;
        bool ok = num[0] != '\0';
        for (const char *c = num; *c && ok; c++) {
            if (*c < '0' || *c > '9' || v > 100000000000LL) ok = false;
            else v = v * 10 + (*c - '0');
        }
        if (ok && v > 315532800) mtime = v;   /* FAT has nothing before 1980 */
    }
    if (mtime < 0 && known) mtime = now_s;
    why = web_upload_begin(&h->u.up, path, mtime, off);
    if (why) {
        upload_fail(h, status_for(why), why);
        return;
    }
    h->up_open = true;
    note(h, "Receiving %.30s", path_base(path));
}

static void upload_data(httpd_t *h, const uint8_t *data, size_t n) {
    if (!h->up_open) return;   /* failed already: the body is read and dropped */
    web_upload_write(&h->u.up, data, n);
    if (h->u.up.w.why) upload_fail(h, status_for(h->u.up.w.why), h->u.up.w.why);
}

static void upload_end(httpd_t *h) {
    if (h->up_open) {
        h->up_open = false;
        const char *why = web_upload_end(&h->u.up);
        if (why) {
            h->up_status = status_for(why);
            h->up_err = why;
            note(h, "Upload of %.30s failed", path_base(h->u.up.w.path));
        } else {
            note(h, "Received %.30s", path_base(h->u.up.w.path));
        }
    }
    if (h->up_err) send_error(h, h->up_status, h->up_err);
    else send_ok(h);
}

/* ---- the request -------------------------------------------------------- */

static void dispatch(httpd_t *h) {
    const char *t = h->path;
    bool get = h->method == WEB_GET, post = h->method == WEB_POST;
    if (get && (strcmp(t, "/") == 0 || strcmp(t, "/index.html") == 0)) route_page(h);
    else if (get && strcmp(t, "/api/list") == 0) route_list(h);
    else if (get && strcmp(t, "/api/info") == 0) route_info(h);
    else if (get && strcmp(t, "/api/file") == 0) route_file(h);
    else if (post && strcmp(t, "/api/mkdir") == 0) route_mkdir(h);
    else if (post && strcmp(t, "/api/delete") == 0) route_delete(h);
    else if (post && strcmp(t, "/api/rename") == 0) route_rename(h);
    else send_error(h, "404 Not Found", "not found");
}

/* The blank line: the headers are in. */
static void headers_done(httpd_t *h) {
    bool upload = h->method == WEB_POST && strcmp(h->path, "/api/upload") == 0;
    if (upload && h->chunked) {
        send_error(h, "501 Not Implemented", "chunked uploads are not supported");
    } else if (upload && h->content_len < 0) {
        send_error(h, "411 Length Required", "a Content-Length is required");
    } else if (upload) {
        upload_begin(h);
        if (h->content_len == 0) upload_end(h);
    } else if (h->content_len > 0) {
        h->state = ST_BODY;
    } else {
        dispatch(h);
    }
}

static bool starts_with_nocase(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++)
        if ((*s | 0x20) != *prefix) return false;
    return true;
}

/* A line of the request, without its CR LF. */
static void line_done(httpd_t *h) {
    if (!h->got_request) {
        if (h->line_over) {
            send_error(h, "414 URI Too Long", "request too long");
            return;
        }
        h->method = web_request_line(h->line, h->line_len, h->path, sizeof h->path, h->query, sizeof h->query);
        if (h->method == WEB_BAD) {
            send_error(h, "400 Bad Request", "bad request");
            return;
        }
        h->got_request = true;
        h->content_len = -1;
        return;
    }
    if (h->line_len == 0) {
        headers_done(h);
        return;
    }
    if (h->line_over) return;   /* a long header we do not use */
    int64_t len = web_content_length(h->line, h->line_len);
    if (len == -2) {
        send_error(h, "400 Bad Request", "bad Content-Length");
        return;
    }
    if (len >= 0) h->content_len = len;
    if (starts_with_nocase(h->line, "transfer-encoding:")) h->chunked = true;
}

void httpd_init(httpd_t *h, const httpd_env_t *env) {
    memset(h, 0, sizeof *h);
    h->env = env;
    h->state = ST_LINE;
}

void httpd_feed(httpd_t *h, const void *data, size_t n) {
    const uint8_t *d = data;
    while (n > 0 && h->state != ST_SEND) {
        if (h->state == ST_LINE) {
            const uint8_t *nl = memchr(d, '\n', n);
            size_t take = nl ? (size_t)(nl - d) + 1u : n;
            size_t body = nl ? take - 1u : take;
            if (h->line_len + body > sizeof h->line - 1u) {
                h->line_over = true;
            } else {
                memcpy(h->line + h->line_len, d, body);
                h->line_len += body;
            }
            d += take;
            n -= take;
            if (!nl) continue;
            if (h->line_len > 0 && h->line[h->line_len - 1] == '\r') h->line_len--;
            h->line[h->line_len] = '\0';
            line_done(h);
            h->line_len = 0;
            h->line_over = false;
            continue;
        }
        /* ST_BODY and ST_UPLOAD: the Content-Length's bytes. */
        int64_t left = h->content_len - h->received;
        size_t take = (int64_t)n < left ? n : (size_t)left;
        if (h->state == ST_UPLOAD) upload_data(h, d, take);
        h->received += (int64_t)take;
        d += take;
        n -= take;
        if (h->received >= h->content_len) {
            if (h->state == ST_UPLOAD) upload_end(h);
            else dispatch(h);
        }
    }
}

/* ---- sending ------------------------------------------------------------ */

/* The drained buffer, filled from the file, the listing or the page. */
static void refill(httpd_t *h) {
    h->out_len = h->out_off = 0;
    if (h->src == SRC_MEM) {
        size_t n = h->mem_left < sizeof h->out ? h->mem_left : sizeof h->out;
        memcpy(h->out, h->mem, n);
        h->mem += n;
        h->mem_left -= n;
        h->out_len = n;
        h->src_done = h->mem_left == 0;
    } else if (h->src == SRC_FILE) {
        UINT want = h->file_left < sizeof h->out ? h->file_left : (UINT)sizeof h->out, got = 0;
        FRESULT r = f_read(&h->u.file, h->out, want, &got);
        if (r != FR_OK || got == 0) {
            h->failed = true;   /* shorter than it said: not to be taken for whole */
            h->src_done = true;
            f_close(&h->u.file);
            h->src = SRC_NONE;
            return;
        }
        h->out_len = got;
        h->file_left -= got;
        if (h->file_left == 0) {
            h->src_done = true;
            f_close(&h->u.file);
            h->src = SRC_NONE;
        }
    } else if (h->src == SRC_LIST) {
        const char *why = NULL;
        while (h->out_len + 1u + WEB_ENTRY_MAX + 3u <= sizeof h->out) {
            size_t n = web_list_next(&h->u.list, h->off_min, (char *)h->out + h->out_len + 1u, WEB_ENTRY_MAX, &why);
            if (n == 0) break;
            if (h->first_entry) memmove(h->out + h->out_len, h->out + h->out_len + 1u, n);
            else h->out[h->out_len] = ',';
            h->out_len += n + (h->first_entry ? 0u : 1u);
            h->first_entry = false;
        }
        if (why) {
            h->failed = true;
            h->src_done = true;
            web_list_close(&h->u.list);
            h->src = SRC_NONE;
        } else if (h->out_len + 1u + WEB_ENTRY_MAX + 3u <= sizeof h->out) {
            /* The loop ended on the end of the directory. */
            memcpy(h->out + h->out_len, "]}", 2);
            h->out_len += 2;
            h->src_done = true;
            web_list_close(&h->u.list);
            h->src = SRC_NONE;
        }
    }
}

size_t httpd_peek(httpd_t *h, const uint8_t **p) {
    if (h->state != ST_SEND) return 0;
    if (h->out_off == h->out_len && !h->src_done) refill(h);
    *p = h->out + h->out_off;
    return h->out_len - h->out_off;
}

void httpd_advance(httpd_t *h, size_t n) {
    h->out_off += n;
    if (h->out_off > h->out_len) h->out_off = h->out_len;
}

bool httpd_finished(const httpd_t *h) {
    return h->state == ST_SEND && h->out_off == h->out_len && h->src_done;
}

bool httpd_failed(const httpd_t *h) {
    return h->failed;
}

void httpd_close(httpd_t *h) {
    if (h->state == ST_UPLOAD && h->up_open) {
        web_upload_abort(&h->u.up);
        h->up_open = false;
    }
    if (h->state == ST_SEND) {
        if (h->src == SRC_FILE) f_close(&h->u.file);
        if (h->src == SRC_LIST) web_list_close(&h->u.list);
        h->src = SRC_NONE;
    }
}
