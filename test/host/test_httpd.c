#define _GNU_SOURCE
/* test_httpd.c — web.uf2's HTTP connection (design.md §3.9, M13 step 3)
 * over FAT16 and FAT32 images: requests fed to it as a browser would,
 * in pieces of every size, and the responses read back whole.
 *
 * The lwIP code that moves the bytes is not here; it is the port's, and is
 * checked on the board. */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "files.h"
#include "host_disk.h"
#include "httpd.h"
#include "test_util.h"

/* ---- the environment -------------------------------------------------- */

static uint8_t  page[6000];
static bool     clock_set;
static char     notes[256][64];
static unsigned note_count;

static bool env_now(void *ctx, int64_t *utc_s, int *off_min) {
    (void)ctx;
    if (!clock_set) return false;
    *utc_s = 1710074096;   /* 2024-03-10 12:34:56 UTC */
    *off_min = -300;
    return true;
}

static void env_note(void *ctx, const char *text) {
    (void)ctx;
    if (note_count < 256) snprintf(notes[note_count++], sizeof notes[0], "%s", text);
}

static const httpd_env_t env = { page, sizeof page, env_now, env_note, NULL };

/* ---- a browser -------------------------------------------------------- */

typedef struct {
    int      status;
    char     head[8192];
    uint8_t  body[1 << 18];
    size_t   body_len;
    bool     finished, failed;
} resp_t;

static resp_t r;

/* The value of a header in the last response, or NULL. */
static const char *header(const char *name) {
    static char val[512];
    size_t n = strlen(name);
    for (const char *p = strchr(r.head, '\n'); p; p = strchr(p + 1, '\n')) {
        p++;
        if (strncasecmp(p, name, n) == 0 && p[n] == ':') {
            const char *v = p + n + 1;
            while (*v == ' ') v++;
            size_t len = strcspn(v, "\r\n");
            memcpy(val, v, len);
            val[len] = '\0';
            return val;
        }
    }
    return NULL;
}

/* Send `req` in pieces of `piece` bytes and read the response. `drop`
 * closes the connection after that many bytes, with the response unread. */
static void exchange_n(const void *req, size_t n, size_t piece, long drop) {
    static httpd_t h;
    memset(&r, 0, sizeof r);
    httpd_init(&h, &env);
    const uint8_t *q = req;
    size_t sent = 0;
    while (sent < n) {
        size_t take = n - sent < piece ? n - sent : piece;
        if (drop >= 0 && sent + take > (size_t)drop) take = (size_t)drop - sent;
        httpd_feed(&h, q + sent, take);
        sent += take;
        if (drop >= 0 && sent >= (size_t)drop) {
            httpd_close(&h);
            return;
        }
    }
    static uint8_t all[1 << 18];
    size_t total = 0;
    const uint8_t *p;
    size_t got;
    while (!httpd_finished(&h) && !httpd_failed(&h) && (got = httpd_peek(&h, &p)) > 0) {
        size_t take = got < 1460 ? got : 1460;   /* a TCP segment at a time */
        if (total + take > sizeof all) break;
        memcpy(all + total, p, take);
        total += take;
        httpd_advance(&h, take);
    }
    r.finished = httpd_finished(&h);
    r.failed = httpd_failed(&h);
    httpd_close(&h);
    all[total < sizeof all ? total : sizeof all - 1] = '\0';
    const uint8_t *end = memmem(all, total, "\r\n\r\n", 4);
    if (!end) {
        memcpy(r.head, all, total < sizeof r.head - 1 ? total : sizeof r.head - 1);
        return;
    }
    size_t hl = (size_t)(end - all) + 4;
    memcpy(r.head, all, hl < sizeof r.head - 1 ? hl : sizeof r.head - 1);
    if (sscanf(r.head, "HTTP/1.1 %d", &r.status) != 1) r.status = 0;
    r.body_len = total - hl;
    memcpy(r.body, all + hl, r.body_len);
    r.body[r.body_len] = '\0';
}

/* The request text: `method target`, headers and a body. */
static size_t build(char *out, size_t size, const char *method, const char *target, const char *headers,
                    const void *body, long blen) {
    size_t n = (size_t)snprintf(out, size, "%s %s HTTP/1.1\r\nHost: picocalc.local\r\n%s", method, target,
                                headers ? headers : "");
    if (blen >= 0) n += (size_t)snprintf(out + n, size - n, "Content-Length: %ld\r\n", blen);
    n += (size_t)snprintf(out + n, size - n, "\r\n");
    if (blen > 0) {
        memcpy(out + n, body, (size_t)blen);
        n += (size_t)blen;
    }
    return n;
}

static void get(const char *target, size_t piece) {
    static char req[1 << 10];
    size_t n = build(req, sizeof req, "GET", target, NULL, NULL, -1);
    exchange_n(req, n, piece, -1);
}

static void post(const char *target) {
    static char req[1 << 10];
    size_t n = build(req, sizeof req, "POST", target, NULL, NULL, 0);
    exchange_n(req, n, 1460, -1);
}

static void upload(const char *target, const void *body, long len, size_t piece) {
    static char req[(1 << 17) + 2048];
    size_t n = build(req, sizeof req, "POST", target, NULL, body, len);
    exchange_n(req, n, piece, -1);
}

#define STATUS(want, what) \
    CHECK(r.status == (want), "%s: status %d, wanted %d: %.60s", what, r.status, want, (char *)r.body)

/* ---- the card --------------------------------------------------------- */

static const char *slurp(const char *path, size_t *len) {
    static char buf[1 << 17];
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_READ) != FR_OK) return NULL;
    FRESULT fr = f_read(&f, buf, sizeof buf - 1, &n);
    f_close(&f);
    if (fr != FR_OK) return NULL;
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

static bool exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

static const char *pattern(size_t n, char seed) {
    static char buf[1 << 17];
    for (size_t i = 0; i < n; i++) buf[i] = (char)('a' + (i / 7 + (size_t)seed) % 26);
    buf[n] = '\0';
    return buf;
}

static bool put(const char *path, const char *data, size_t n) {
    FIL f;
    UINT w;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FRESULT fr = f_write(&f, data, (UINT)n, &w);
    return f_close(&f) == FR_OK && fr == FR_OK && w == n;
}

/* ---- the requests ----------------------------------------------------- */

static int test_protocol(void) {
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    for (size_t i = 0; i < sizeof page; i++) page[i] = (uint8_t)(i * 7 + 3);

    /* The page, whole across several buffers, in any size of piece. */
    static const size_t pieces[] = { 1, 2, 5, 64, 1460, 100000 };
    for (size_t k = 0; k < sizeof pieces / sizeof pieces[0]; k++) {
        get("/", pieces[k]);
        STATUS(200, "page");
        CHECK(r.finished && !r.failed, "page: finished");
        CHECK(r.body_len == sizeof page && memcmp(r.body, page, sizeof page) == 0, "page: %zu bytes", r.body_len);
        CHECK(header("Content-Encoding") && strcmp(header("Content-Encoding"), "gzip") == 0, "gzip");
        CHECK(header("Content-Length") && atol(header("Content-Length")) == (long)sizeof page, "length");
        CHECK(header("Connection") && strcmp(header("Connection"), "close") == 0, "close");
    }
    get("/index.html", 1460);
    STATUS(200, "/index.html");

    /* Headers of any size, and ones that arrive in parts. */
    {
        static char req[8192];
        size_t n = (size_t)snprintf(req, sizeof req, "GET /api/info HTTP/1.1\r\nCookie: ");
        memset(req + n, 'c', 5000);
        n += 5000;
        n += (size_t)snprintf(req + n, sizeof req - n, "\r\nAccept: */*\r\n\r\n");
        exchange_n(req, n, 1460, -1);
        STATUS(200, "a 5000-byte header");
        exchange_n(req, n, 3, -1);
        STATUS(200, "...in pieces of 3");
    }
    {
        static const char lf[] = "GET /api/info HTTP/1.1\nHost: x\n\n";
        exchange_n(lf, sizeof lf - 1, 4, -1);
        STATUS(200, "bare LF line ends");
    }

    /* Mistakes. */
    static const struct {
        const char *req;
        int         status;
    } bad[] = {
        { "GET /nope HTTP/1.1\r\n\r\n", 404 },
        { "DELETE /api/list HTTP/1.1\r\n\r\n", 404 },
        { "GET /api/mkdir?path=/x HTTP/1.1\r\n\r\n", 404 },
        { "POST /api/info HTTP/1.1\r\n\r\n", 404 },
        { "garbage\r\n\r\n", 400 },
        { "GET /x HTTP/2.0\r\n\r\n", 400 },
        { "GET /api/info HTTP/1.1\r\nContent-Length: abc\r\n\r\n", 400 },
        { "POST /api/upload?path=/x HTTP/1.1\r\n\r\n", 411 },
        { "POST /api/upload?path=/x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 501 },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        exchange_n(bad[i].req, strlen(bad[i].req), 1460, -1);
        CHECK(r.status == bad[i].status, "%zu: status %d, wanted %d", i, r.status, bad[i].status);
    }
    {
        static char big[WEB_TARGET_MAX + 200];
        memset(big, 'a', sizeof big);
        memcpy(big, "GET /", 5);
        memcpy(big + sizeof big - 20, " HTTP/1.1\r\n\r\n", 14);
        exchange_n(big, sizeof big - 6, 1460, -1);
        CHECK(r.status == 414 || r.status == 400, "a long request line: %d", r.status);
    }
    return 0;
}

static int card_tests(bool fat32) {
    const char *kind = fat32 ? "FAT32" : "FAT16";
    CHECK(host_disk_create(NULL, fat32 ? 64u << 20 : 32u << 20, fat32), "%s image", kind);
    files_mount();
    note_count = 0;

    get("/api/info", 1460);
    STATUS(200, "info");
    CHECK(strstr((char *)r.body, kind) && strstr((char *)r.body, "\"free\":"), "%s: info '%s'", kind, r.body);
    CHECK(header("Content-Type") && strstr(header("Content-Type"), "json"), "json");

    /* mkdir, and its mistakes. */
    post("/api/mkdir?path=/up");
    STATUS(200, "mkdir");
    CHECK(strcmp((char *)r.body, "{\"ok\":true}") == 0, "mkdir body");
    post("/api/mkdir?path=/up");
    STATUS(409, "mkdir again");
    post("/api/mkdir?path=/nope/x");
    STATUS(404, "mkdir under nothing");
    post("/api/mkdir?path=/a:b");
    STATUS(400, "mkdir a bad name");
    post("/api/mkdir");
    STATUS(400, "mkdir without a path");
    post("/api/mkdir?path=/up/%2e%2e/x");
    STATUS(400, "mkdir with ..");
    post("/api/mkdir?path=/caf%C3%A9");
    STATUS(200, "mkdir with an accent");
    CHECK(exists("/caf\x82"), "%s: the accent is CP437 on the card", kind);

    /* Uploads: every size, in every size of piece, whole and with the time. */
    static const size_t sizes[] = { 0, 1, 511, 512, 513, 4096, 40000, 100000 };
    static const size_t pieces[] = { 1, 7, 1460, 1 << 16 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (size_t k = 0; k < sizeof pieces / sizeof pieces[0]; k++) {
            if (sizes[i] > 5000 && pieces[k] == 1) continue;   /* a byte at a time is slow */
            const char *data = pattern(sizes[i], (char)(i + k));
            upload("/api/upload?path=/up/f.bin&mtime=1710074096", data, (long)sizes[i], pieces[k]);
            STATUS(200, "upload");
            size_t len = 0;
            const char *got = slurp("/up/f.bin", &len);
            CHECK(got && len == sizes[i] && memcmp(got, data, sizes[i]) == 0, "%s: %zu bytes in %zu: read back",
                  kind, sizes[i], pieces[k]);
            CHECK(!exists("/up/f.bin.new"), "%s: no .new left", kind);
        }
    }
    FILINFO fno;
    CHECK(f_stat("/up/f.bin", &fno) == FR_OK, "stat");
    CHECK(web_fat_to_unix(fno.fdate, fno.ftime, 0) == 1710074096, "%s: the time kept", kind);

    /* No mtime: the time now, when the clock is set. */
    clock_set = true;
    upload("/api/upload?path=/up/now.txt", "x", 1, 1460);
    STATUS(200, "upload, no mtime");
    CHECK(f_stat("/up/now.txt", &fno) == FR_OK && web_fat_to_unix(fno.fdate, fno.ftime, -300) == 1710074096,
          "the clock's time");
    clock_set = false;
    upload("/api/upload?path=/up/old.txt&mtime=5", "x", 1, 1460);
    STATUS(200, "upload, an mtime before 1980");
    CHECK(f_stat("/up/old.txt", &fno) == FR_OK && fno.fdate != 0, "a date is there");

    /* Uploads that fail are answered once the body is read. */
    {
        const char *big = pattern(20000, 5);
        post("/api/mkdir?path=/up/d");
        upload("/api/upload?path=/up/d", big, 20000, 1460);
        STATUS(409, "upload onto a directory");
        CHECK(r.finished, "...and answered");
        upload("/api/upload?path=/nodir/x", big, 20000, 1460);
        STATUS(404, "upload into nothing");
        upload("/api/upload?path=x", big, 20000, 1460);
        STATUS(400, "upload with a bad path");
        upload("/api/upload", big, 20000, 1460);
        STATUS(400, "upload with no path");
        CHECK(f_chmod("/up/old.txt", AM_RDO, AM_RDO) == FR_OK, "chmod");
        upload("/api/upload?path=/up/old.txt", big, 20000, 1460);
        STATUS(403, "upload onto a read-only file");
        CHECK(strcmp(slurp("/up/old.txt", NULL), "x") == 0, "...which stays");
        CHECK(f_chmod("/up/old.txt", 0, AM_RDO) == FR_OK, "chmod back");
    }

    /* A connection dropped half way leaves the old file and no .new. */
    {
        CHECK(put("/up/keep.txt", "old\n", 4), "keep.txt");
        static char req[1 << 15];
        const char *data = pattern(9000, 3);
        size_t n = build(req, sizeof req, "POST", "/api/upload?path=/up/keep.txt", NULL, data, 9000);
        exchange_n(req, n, 1460, (long)n - 4000);
        CHECK(strcmp(slurp("/up/keep.txt", NULL), "old\n") == 0 && !exists("/up/keep.txt.new"),
              "%s: a dropped upload leaves the old file", kind);
        exchange_n(req, n, 1460, 40);   /* before its headers end */
        CHECK(strcmp(slurp("/up/keep.txt", NULL), "old\n") == 0, "dropped in the headers");
    }

    /* The files, shown and sent whole. */
    {
        const char *data = pattern(70000, 9);
        CHECK(put("/up/big.txt", data, 70000), "big.txt");
        get("/api/file?path=/up/big.txt", 1460);
        STATUS(200, "file");
        CHECK(r.finished && r.body_len == 70000 && memcmp(r.body, data, 70000) == 0, "%s: 70000 bytes down", kind);
        CHECK(header("Content-Length") && atol(header("Content-Length")) == 70000, "length");
        CHECK(header("Content-Type") && strncmp(header("Content-Type"), "text/plain", 10) == 0, "type");
        CHECK(!header("Content-Disposition"), "shown, not saved");
        get("/api/file?path=/up/big.txt&dl=1", 1460);
        STATUS(200, "download");
        CHECK(header("Content-Disposition") && strstr(header("Content-Disposition"), "attachment") &&
                  strstr(header("Content-Disposition"), "filename*=UTF-8''big.txt"),
              "disposition: %s", header("Content-Disposition"));
        CHECK(strcmp(header("Content-Type"), "application/octet-stream") == 0, "download type");
        CHECK(put("/up/page.html", "<script>alert(1)</script>", 25), "page.html");
        get("/api/file?path=/up/page.html", 1460);
        CHECK(strncmp(header("Content-Type"), "text/plain", 10) == 0, "html is shown as text");
        CHECK(header("X-Content-Type-Options") != NULL, "nosniff");
        CHECK(put("/up/empty", "", 0), "empty");
        get("/api/file?path=/up/empty", 1460);
        STATUS(200, "an empty file");
        CHECK(r.finished && r.body_len == 0, "...empty");
        CHECK(put("/up/caf\x82.txt", "ok", 2), "accent file");
        get("/api/file?path=/up/caf%C3%A9.txt&dl=1", 1460);
        STATUS(200, "an accented name");
        CHECK(strstr(header("Content-Disposition"), "filename*=UTF-8''caf%C3%A9.txt"), "encoded name: %s",
              header("Content-Disposition"));
        /* A name whose encoding is too long for the header is replaced there. */
        {
            char card[SHELL_PATH_MAX + 1] = "/up/", target[1024] = "/api/file?dl=1&path=/up/";
            for (int i = 0; i < 80; i++) {
                strcat(card, "\x82");
                strcat(target, "%C3%A9");
            }
            CHECK(put(card, "ok", 2), "long accented name");
            get(target, 1460);
            STATUS(200, "a long accented name");
            CHECK(header("Content-Disposition") && strstr(header("Content-Disposition"), "filename*=UTF-8''download"),
                  "long name replaced: %.80s", header("Content-Disposition"));
        }
        get("/api/file?path=/up/nope", 1460);
        STATUS(404, "file not there");
        get("/api/file?path=/up", 1460);
        STATUS(409, "file is a directory");
    }

    /* The listing. */
    get("/api/list?path=/up", 1460);
    STATUS(200, "list");
    {
        const char *b = (const char *)r.body;
        CHECK(strncmp(b, "{\"path\":\"/up\",\"entries\":[", 25) == 0, "%s: list start: %.40s", kind, b);
        CHECK(r.body_len >= 2 && strcmp(b + r.body_len - 2, "]}") == 0, "list end");
        CHECK(strstr(b, "{\"n\":\"d\",\"d\":true,"), "a directory: %s", b);
        CHECK(strstr(b, "{\"n\":\"f.bin\",\"d\":false,\"s\":100000,"), "a file");
        CHECK(strstr(b, "{\"n\":\"caf\xC3\xA9.txt\",\"d\":false,\"s\":2,"), "UTF-8 name");
        CHECK(strstr(b, "\"t\":1710074096}"), "the time, through the offset");   /* clock off: offset 0 */
    }
    get("/api/list?path=/nope", 1460);
    STATUS(404, "list nothing");
    get("/api/list?path=/up/f.bin", 1460);
    CHECK(r.status != 200, "list a file: %d", r.status);
    get("/api/list?path=%2F", 1460);
    STATUS(200, "list the root");
    CHECK(strncmp((char *)r.body, "{\"path\":\"/\",", 12) == 0, "root path");

    /* A directory of 300 files is sent over several buffers, whole. */
    post("/api/mkdir?path=/many");
    for (int i = 0; i < 300; i++) {
        char name[64];
        snprintf(name, sizeof name, "/many/file-number-%03d.txt", i);
        CHECK(put(name, "x", 1), "many %d", i);
    }
    get("/api/list?path=/many", 1460);
    STATUS(200, "a long listing");
    {
        unsigned count = 0;
        for (const char *p = (const char *)r.body; (p = strstr(p, "{\"n\":")); p += 5) count++;
        CHECK(count == 300, "%s: %u entries of 300", kind, count);
        unsigned joined = 0;
        for (const char *p = (const char *)r.body; (p = strstr(p, "},{\"n\":")); p += 7) joined++;
        CHECK(joined == 299, "%s: %u entries joined by commas, of 299", kind, joined);
        CHECK(r.body_len > HTTPD_OUT_MAX * 2u, "it took several buffers: %zu", r.body_len);
        CHECK(strcmp((char *)r.body + r.body_len - 2, "]}") == 0, "its end");
        CHECK(header("Content-Length") == NULL, "no length: it ends with the connection");
    }

    /* Rename and delete. */
    post("/api/rename?from=/up/now.txt&to=/up/d/then.txt");
    STATUS(200, "rename");
    CHECK(exists("/up/d/then.txt") && !exists("/up/now.txt"), "moved");
    post("/api/rename?from=/up/d/then.txt&to=/up/keep.txt");
    STATUS(409, "rename onto a file");
    post("/api/rename?from=/up&to=/up/d/up");
    STATUS(400, "rename into itself");
    post("/api/rename?from=/nope&to=/x");
    STATUS(404, "rename nothing");
    post("/api/rename?from=/up/d");
    STATUS(400, "rename with no target");
    post("/api/rename?from=/&to=/x");
    STATUS(403, "rename the root");
    post("/api/delete?path=/up/keep.txt");
    STATUS(200, "delete");
    CHECK(!exists("/up/keep.txt"), "deleted");
    post("/api/delete?path=/up/keep.txt");
    STATUS(404, "delete nothing");
    post("/api/delete?path=/");
    STATUS(403, "delete the root");
    post("/api/delete?path=/many");
    STATUS(200, "delete a tree");
    CHECK(!exists("/many") && !exists("/many/file-number-000.txt"), "%s: tree gone", kind);

    /* What the panel was told. */
    bool saw_receiving = false, saw_received = false, saw_deleted = false, saw_sending = false;
    for (unsigned i = 0; i < note_count; i++) {
        saw_receiving |= strncmp(notes[i], "Receiving ", 10) == 0;
        saw_received |= strncmp(notes[i], "Received ", 9) == 0;
        saw_deleted |= strncmp(notes[i], "Deleted ", 8) == 0;
        saw_sending |= strncmp(notes[i], "Sending ", 8) == 0;
    }
    CHECK(saw_receiving && saw_received && saw_deleted && saw_sending, "notes: %u", note_count);
    return 0;
}

/* ---- a card that fails ------------------------------------------------- */

static int fault_tests(void) {
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    CHECK(put("/a.txt", "old\n", 4), "a.txt");
    const char *data = pattern(60000, 1);

    /* The card dies a few sectors into an upload, at each point in turn:
     * the browser is told, the old file stays or the new one is whole. */
    unsigned failed = 0, whole = 0;
    for (long cut = 0; cut < 140; cut += 7) {
        host_disk_fail_writes_after(cut);
        upload("/api/upload?path=/a.txt", data, 60000, 1460);
        int status = r.status;
        host_disk_fail_writes_after(-1);
        files_mount();
        size_t len = 0;
        const char *got = slurp("/a.txt", &len);
        if (status == 200) {
            whole++;
            CHECK(got && len == 60000 && memcmp(got, data, 60000) == 0, "cut %ld: told ok, and whole", cut);
        } else {
            failed++;
            CHECK(status >= 400, "cut %ld: status %d", cut, status);
            CHECK(!got || strcmp(got, "old\n") == 0, "cut %ld: not half: %zu bytes", cut, got ? len : 0u);
        }
        CHECK(!exists("/a.txt.new") || status != 200, "cut %ld: no .new after success", cut);
    }
    CHECK(failed > 10 && whole > 0, "both outcomes were tried: %u failed, %u whole", failed, whole);
    put("/a.txt", "old\n", 4);

    /* A card too small. */
    CHECK(host_disk_create(NULL, 8u << 20, false), "small image");
    files_mount();
    {
        static char big[(1 << 17) + 4096];
        memset(big, 'z', sizeof big);
        /* 12 MB, in 128 KB pieces of one request. */
        static char head[512];
        size_t hn = (size_t)snprintf(head, sizeof head,
                                     "POST /api/upload?path=/big.bin HTTP/1.1\r\nContent-Length: %ld\r\n\r\n",
                                     12L << 20);
        static httpd_t h;
        httpd_init(&h, &env);
        httpd_feed(&h, head, hn);
        for (long sent = 0; sent < (12L << 20); sent += (long)sizeof big)
            httpd_feed(&h, big, (12L << 20) - sent < (long)sizeof big ? (size_t)((12L << 20) - sent) : sizeof big);
        const uint8_t *p;
        size_t got = httpd_peek(&h, &p);
        CHECK(got > 0 && strncmp((const char *)p, "HTTP/1.1 507", 12) == 0, "a full card: %.30s", got ? (const char *)p : "");
        httpd_close(&h);
        CHECK(!exists("/big.bin") && !exists("/big.bin.new"), "...and nothing is left");
    }

    /* The card fails while a file is being sent: the connection must be
     * aborted, not closed as if the file were whole. */
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    CHECK(put("/f.bin", pattern(80000, 2), 80000), "f.bin");
    host_disk_fail_reads_after(60);
    get("/api/file?path=/f.bin", 1460);
    host_disk_fail_reads_after(-1);
    CHECK(r.failed && r.body_len < 80000, "a read that fails aborts: failed %d, %zu bytes", r.failed, r.body_len);
    files_mount();
    get("/api/file?path=/f.bin", 1460);
    CHECK(r.status == 200 && r.body_len == 80000 && !r.failed, "and it is fine again");

    /* No card. */
    host_disk_close();
    get("/api/info", 1460);
    STATUS(503, "info with no card");
    get("/api/list?path=/", 1460);
    CHECK(r.status >= 500, "list with no card: %d", r.status);
    post("/api/mkdir?path=/x");
    CHECK(r.status >= 500, "mkdir with no card: %d", r.status);
    return 0;
}

int main(void) {
    if (test_protocol()) return 1;
    if (card_tests(false)) return 1;
    if (card_tests(true)) return 1;
    if (fault_tests()) return 1;
    TEST_DONE();
}
