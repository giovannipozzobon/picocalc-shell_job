/* httpd.h — one HTTP connection of web.uf2, apart from the network
 * (design.md §3.9): the request in, the response out, and the card in
 * between through web.h.
 *
 * Portable and with no lwIP in it, so the host's tests drive it as a
 * browser would: the port's TCP code (src/web/main.c) only moves the
 * bytes it receives into httpd_feed() and the bytes httpd_peek() offers
 * out to the socket. One request to a connection, always answered with
 * `Connection: close`. A response is made as it is sent, a piece of the
 * file or the listing at a time, so no file is ever held whole.
 *
 * The routes are PicoMultiLoader's (design.md §3.9):
 *   GET  /                          the page
 *   GET  /api/list?path=/d          a directory, as JSON
 *   GET  /api/info                  the card's type and space
 *   GET  /api/file?path=/f[&dl=1]   a file, shown or as a download
 *   POST /api/upload?path=/f[&mtime=unix]   the body becomes the file
 *   POST /api/mkdir?path=/d
 *   POST /api/delete?path=/p        a file, or a directory and all in it
 *   POST /api/rename?from=/a&to=/b
 * Each answers 200, or an error with the reason as plain text. An upload
 * that fails is answered once its body has been read, so that the browser
 * sees the answer and not a reset. */
#ifndef SHELL_HTTPD_H
#define SHELL_HTTPD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "web.h"

typedef struct {
    /* The page, gzipped, as served for `/`. */
    const uint8_t *page;
    size_t         page_len;
    /* The time now: true with the UTC time in seconds and the local
     * offset in minutes, false if the clock is not set. May be NULL. */
    bool (*now)(void *ctx, int64_t *utc_s, int *off_min);
    /* A short line for the panel about what is happening ("Receiving
     * x.txt"). May be NULL. */
    void (*note)(void *ctx, const char *text);
    void *ctx;
} httpd_env_t;

typedef struct {
    const httpd_env_t *env;
    int                state;
    char               line[WEB_TARGET_MAX + 41u];
    size_t             line_len;
    bool               line_over;      /* the line did not fit and the rest is skipped */
    bool               got_request;
    bool               chunked;
    web_method_t       method;
    char               path[48];
    char               query[WEB_TARGET_MAX + 1u];
    int64_t            content_len;
    int64_t            received;
    /* An upload that failed: answered when its body has been read. */
    const char        *up_status;
    const char        *up_err;
    bool               up_open;
    /* The response. */
    uint8_t            out[HTTPD_OUT_MAX];
    size_t             out_len, out_off;
    int                src;
    bool               src_done;
    bool               failed;         /* it broke off: the socket should be aborted */
    const uint8_t     *mem;
    size_t             mem_left;
    uint32_t           file_left;
    bool               first_entry;
    int                off_min;        /* the local offset, for the listing's times */
    union {
        FIL          file;
        web_list_t   list;
        web_upload_t up;
    } u;
} httpd_t;

void httpd_init(httpd_t *h, const httpd_env_t *env);

/* Bytes from the browser. Anything after the request is ignored. */
void httpd_feed(httpd_t *h, const void *data, size_t n);

/* The next bytes of the response, in *p: how many (0 if there are none
 * now, or the response is over; see httpd_finished). They stay valid
 * until httpd_advance() takes some. */
size_t httpd_peek(httpd_t *h, const uint8_t **p);
void httpd_advance(httpd_t *h, size_t n);

/* The whole response was taken: close the connection. */
bool httpd_finished(const httpd_t *h);
/* The response broke off (the card failed in the middle of a file): abort
 * the connection, so that the browser does not take it for whole. */
bool httpd_failed(const httpd_t *h);

/* Let go of what the connection holds: after the end of the response, or
 * when the connection drops. An upload not finished is undone. */
void httpd_close(httpd_t *h);

#endif /* SHELL_HTTPD_H */
