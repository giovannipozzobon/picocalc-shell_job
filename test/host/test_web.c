/* test_web.c — web.uf2's requests and the card (design.md §3.9, M13
 * step 2): the request line, the headers and the query; names in UTF-8
 * and CP437; JSON; timestamps; and each operation over FAT16 and FAT32
 * images, with a card that fails part way through an upload. */

#include <stdio.h>
#include <string.h>

#include "files.h"
#include "host_disk.h"
#include "test_util.h"
#include "web.h"

/* ---- the request ------------------------------------------------------ */

static web_method_t line(const char *s, char *path, char *query) {
    return web_request_line(s, strlen(s), path, 128, query, 128);
}

static int test_request(void) {
    char path[128], query[128];

    CHECK(line("GET / HTTP/1.1", path, query) == WEB_GET && strcmp(path, "/") == 0 && !query[0],
          "GET /");
    CHECK(line("GET /api/list?path=/a%20b&x=1 HTTP/1.0", path, query) == WEB_GET &&
              strcmp(path, "/api/list") == 0 && strcmp(query, "path=/a%20b&x=1") == 0,
          "GET with a query: '%s' '%s'", path, query);
    CHECK(line("POST /api/mkdir?path=/d HTTP/1.1", path, query) == WEB_POST, "POST");
    CHECK(line("DELETE /x HTTP/1.1", path, query) == WEB_OTHER, "another method");
    CHECK(line("GET /x?", path, query) == WEB_BAD, "no version");
    CHECK(line("GET /x HTTP/2.0", path, query) == WEB_BAD, "not HTTP/1");
    CHECK(line("GET /x HTTP/1.1 extra", path, query) == WEB_BAD, "extra word");
    CHECK(line("GET x HTTP/1.1", path, query) == WEB_BAD, "target not starting with /");
    CHECK(line("GET  HTTP/1.1", path, query) == WEB_BAD, "empty target");
    CHECK(line("", path, query) == WEB_BAD, "empty line");
    CHECK(line("GET", path, query) == WEB_BAD, "method only");
    char big[700];
    memset(big, 'a', sizeof big);
    memcpy(big, "GET /", 5);
    memcpy(big + 650, " HTTP/1.1", 10);
    CHECK(web_request_line(big, 659, path, 128, query, 128) == WEB_BAD, "a long path");
    CHECK(line("GET /api/list?path=/x HTTP/1.1", path, query) == WEB_GET, "again");
    char tiny[4];
    CHECK(web_request_line("GET /abcdef HTTP/1.1", 20, tiny, sizeof tiny, query, 128) == WEB_BAD,
          "a path over the buffer");

    const char *h = "Host: picocalc.local\r\ncontent-LENGTH:  1234 \r\nAccept: */*\r\n";
    CHECK(web_content_length(h, strlen(h)) == 1234, "content-length");
    h = "Host: x\r\n";
    CHECK(web_content_length(h, strlen(h)) == -1, "no content-length");
    h = "Content-Length: 12x\r\n";
    CHECK(web_content_length(h, strlen(h)) == -2, "not a number");
    h = "Content-Length: 99999999999\r\n";
    CHECK(web_content_length(h, strlen(h)) == -2, "too long a number");
    h = "Content-Length:\r\n";
    CHECK(web_content_length(h, strlen(h)) == -2, "empty");
    h = "X-Content-Length: 5\r\n";
    CHECK(web_content_length(h, strlen(h)) == -1, "another header");
    h = "Content-Length: 0\r\n";
    CHECK(web_content_length(h, strlen(h)) == 0, "zero");

    char v[32];
    CHECK(web_query_get("path=/a%20b&to=%2Fc+d", "path", v, sizeof v) && strcmp(v, "/a b") == 0,
          "path: '%s'", v);
    CHECK(web_query_get("path=/a%20b&to=%2Fc+d", "to", v, sizeof v) && strcmp(v, "/c d") == 0,
          "to: '%s'", v);
    CHECK(!web_query_get("path=/a", "pa", v, sizeof v), "a key's start is not the key");
    CHECK(!web_query_get("path=/a", "x", v, sizeof v), "missing key");
    CHECK(!web_query_get("path=%zz", "path", v, sizeof v), "bad %%");
    CHECK(!web_query_get("path=%2", "path", v, sizeof v), "short %%");
    CHECK(!web_query_get("path=%00", "path", v, sizeof v), "NUL");
    CHECK(web_query_get("path=", "path", v, sizeof v) && !v[0], "empty value");
    CHECK(!web_query_get("path=abcdef", "path", v, 6), "does not fit");
    CHECK(web_query_get("path=abcde", "path", v, 6) && strcmp(v, "abcde") == 0, "just fits");
    CHECK(web_query_get("a=1&path=/x", "path", v, sizeof v) && strcmp(v, "/x") == 0, "second key");
    return 0;
}

/* ---- names, JSON and time -------------------------------------------- */

static int test_names(void) {
    char o[SHELL_PATH_MAX + 1], u[64];

    /* CP437: 0x82 is é, 0x81 ü, 0xE1 ß. */
    CHECK(web_oem_to_utf8(u, sizeof u, "caf\x82") && strcmp(u, "caf\xC3\xA9") == 0, "oem to utf8");
    CHECK(web_utf8_to_oem(o, sizeof o, "caf\xC3\xA9") && strcmp(o, "caf\x82") == 0, "utf8 to oem");
    CHECK(web_utf8_to_oem(o, sizeof o, "\xC3\xBC\xC3\x9F") && strcmp(o, "\x81\xE1") == 0, "ü ß");
    CHECK(!web_utf8_to_oem(o, sizeof o, "\xE4\xB8\xAD"), "a character CP437 lacks");
    CHECK(!web_utf8_to_oem(o, sizeof o, "\xC3"), "a cut sequence");
    CHECK(!web_utf8_to_oem(o, sizeof o, "\xC0\xAF"), "overlong");
    CHECK(!web_utf8_to_oem(o, sizeof o, "\x80"), "a stray continuation");
    CHECK(!web_utf8_to_oem(o, sizeof o, "\xF0\x9F\x98\x80"), "beyond the BMP");
    CHECK(!web_oem_to_utf8(u, 4, "abcd") && web_oem_to_utf8(u, 5, "abcd"), "oem to utf8 fit");
    CHECK(!web_oem_to_utf8(u, 4, "ab\x82"), "utf8 needs 2 bytes");

    CHECK(web_card_path("/", o) == NULL && strcmp(o, "/") == 0, "root");
    CHECK(web_card_path("/a/./b//c/", o) == NULL && strcmp(o, "/a/b/c") == 0, "normal: '%s'", o);
    CHECK(web_card_path("/caf\xC3\xA9/x", o) == NULL && strcmp(o, "/caf\x82/x") == 0, "accent");
    CHECK(web_card_path("a/b", o), "no leading /");
    CHECK(web_card_path("", o), "empty");
    CHECK(web_card_path("/a/../b", o), "..");
    CHECK(web_card_path("/..", o), ".. alone");
    CHECK(web_card_path("/a/...", o) == NULL && strcmp(o, "/a/...") == 0, "... is a name");
    static const char *const bad[] = { "/a:b", "/a*b", "/a?b", "/a\"b", "/a<b", "/a>b", "/a|b",
                                       "/a\\b", "/a\tb", "/a\x01", "/a\x7f" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(web_card_path(bad[i], o), "refused: %s", bad[i]);
    CHECK(web_card_path("/\xE4\xB8\xAD", o), "uncodable name");
    char longp[400];
    memset(longp, 'a', sizeof longp - 1);
    longp[0] = '/';
    longp[sizeof longp - 1] = '\0';
    CHECK(web_card_path(longp, o), "too long");

    char j[64];
    CHECK(web_json_string(j, sizeof j, "a\"b\\c\n\t\x01") != 0 &&
              strcmp(j, "\"a\\\"b\\\\c\\n\\t\\u0001\"") == 0,
          "escapes: %s", j);
    CHECK(web_json_string(j, sizeof j, "caf\xC3\xA9") == 7 && strcmp(j, "\"caf\xC3\xA9\"") == 0,
          "utf-8 stays");
    CHECK(web_json_string(j, sizeof j, "") == 2 && strcmp(j, "\"\"") == 0, "empty");
    CHECK(web_json_string(j, 5, "abc") == 0, "no room for the quote and NUL");
    CHECK(web_json_string(j, 6, "abc") == 5, "just room");

    /* 2024-03-10 12:34:56 UTC, with the local time 5 hours west of it. */
    uint16_t d, t;
    CHECK(web_unix_to_fat(1710074096, -300, &d, &t), "to fat");
    CHECK(d == (uint16_t)(((2024 - 1980) << 9) | (3 << 5) | 10), "date %04x", d);
    CHECK(t == (uint16_t)((7 << 11) | (34 << 5) | (56 / 2)), "time %04x", t);
    CHECK(web_fat_to_unix(d, t, -300) == 1710074096, "and back: %lld",
          (long long)web_fat_to_unix(d, t, -300));
    CHECK(web_fat_to_unix(0, 0, 0) == 0, "no date");
    CHECK(!web_unix_to_fat(0, 0, &d, &t), "1970 is not FAT's");
    CHECK(!web_unix_to_fat(4200000000LL, 0, &d, &t), "2103 is not FAT's");
    return 0;
}

/* ---- the card ---------------------------------------------------------- */

static const char *slurp(const char *path) {
    static char buf[1 << 17];
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_READ) != FR_OK) return NULL;
    FRESULT r = f_read(&f, buf, sizeof buf - 1, &n);
    f_close(&f);
    if (r != FR_OK) return NULL;
    buf[n] = '\0';
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

/* An upload in pieces of `piece` bytes. */
static const char *upload(const char *path, const char *data, size_t n, size_t piece,
                          int64_t unix_s) {
    static web_upload_t u;
    const char *why = web_upload_begin(&u, path, unix_s, 0);
    if (why) return why;
    for (size_t i = 0; i < n; i += piece) web_upload_write(&u, data + i, n - i < piece ? n - i : piece);
    return web_upload_end(&u);
}

/* The whole listing of a directory, as text, one entry a line. */
static const char *listing(const char *path) {
    static char text[8192];
    web_list_t l;
    char entry[WEB_ENTRY_MAX];
    const char *why;
    text[0] = '\0';
    if (web_list_open(&l, path)) return NULL;
    size_t n;
    while ((n = web_list_next(&l, 0, entry, sizeof entry, &why)) != 0) {
        strcat(text, entry);
        strcat(text, "\n");
    }
    web_list_close(&l);
    return why ? NULL : text;
}

static int card_tests(bool fat32) {
    const char *kind = fat32 ? "FAT32" : "FAT16";
    const char *why;
    CHECK(host_disk_create(NULL, fat32 ? 64u << 20 : 32u << 20, fat32), "%s image", kind);
    files_mount();

    char info[128];
    CHECK(web_info_json(info, sizeof info) > 0 && strstr(info, kind) && strstr(info, "\"total\":") &&
              strstr(info, "\"free\":"),
          "%s: info '%s'", kind, info);

    CHECK(web_mkdir("/up") == NULL && web_mkdir("/up/deep") == NULL, "%s: mkdir", kind);
    CHECK(web_mkdir("/up") != NULL, "%s: mkdir again: exists", kind);
    CHECK(web_mkdir("/") != NULL, "mkdir /");
    CHECK(web_mkdir("/nope/x") != NULL, "mkdir under nothing");

    /* An upload of several sizes and pieces, whole and with its time. */
    static const size_t sizes[] = { 0, 1, 511, 512, 513, 4096, 40000, 70000 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        const char *data = pattern(sizes[i], (char)i);
        why = upload("/up/f.bin", data, sizes[i], i % 2 ? 1460 : 4096, 1710074096);
        CHECK(!why, "%s: upload %zu: %s", kind, sizes[i], why);
        const char *got = slurp("/up/f.bin");
        CHECK(got && strlen(got) == sizes[i] && memcmp(got, data, sizes[i]) == 0,
              "%s: %zu bytes read back", kind, sizes[i]);
        CHECK(!exists("/up/f.bin.new"), "%s: no .new left", kind);
    }
    FILINFO fno;
    CHECK(f_stat("/up/f.bin", &fno) == FR_OK, "stat");
    CHECK(web_fat_to_unix(fno.fdate, fno.ftime, 0) == 1710074096, "%s: the time kept: %lld", kind,
          (long long)web_fat_to_unix(fno.fdate, fno.ftime, 0));

    /* With no time given, the file has the time FatFs gives, not 0. */
    CHECK(!upload("/up/now.txt", "x", 1, 1, -1), "no time");
    CHECK(f_stat("/up/now.txt", &fno) == FR_OK && fno.fdate != 0, "a date is there");

    /* A directory, a read-only file and a missing folder. */
    CHECK(upload("/up/deep", "x", 1, 1, -1) != NULL, "%s: upload onto a directory", kind);
    CHECK(upload("/nodir/x", "x", 1, 1, -1) != NULL, "%s: upload into nothing", kind);
    CHECK(f_chmod("/up/now.txt", AM_RDO, AM_RDO) == FR_OK, "chmod");
    CHECK(upload("/up/now.txt", "y", 1, 1, -1) != NULL && strcmp(slurp("/up/now.txt"), "x") == 0,
          "%s: a read-only file stays", kind);
    CHECK(f_chmod("/up/now.txt", 0, AM_RDO) == FR_OK, "chmod back");

    /* An upload dropped half way leaves the old file and no .new. */
    {
        web_upload_t u;
        CHECK(!upload("/up/keep.txt", "old\n", 4, 4, -1), "keep.txt");
        CHECK(web_upload_begin(&u, "/up/keep.txt", -1, 0) == NULL, "begin");
        web_upload_write(&u, pattern(9000, 3), 9000);
        web_upload_abort(&u);
        web_upload_abort(&u);
        CHECK(strcmp(slurp("/up/keep.txt"), "old\n") == 0 && !exists("/up/keep.txt.new"),
              "%s: abort keeps the old file", kind);
        CHECK(web_upload_end(&u) != NULL, "end after abort");
    }

    /* A name with an accent, through UTF-8 and back. */
    char p[SHELL_PATH_MAX + 1];
    CHECK(web_card_path("/up/caf\xC3\xA9.txt", p) == NULL, "accent path");
    CHECK(!upload(p, "ok", 2, 2, -1) && exists("/up/caf\x82.txt"), "%s: accented name", kind);

    /* The listing. */
    const char *l = listing("/up");
    CHECK(l, "%s: list", kind);
    CHECK(l && strstr(l, "{\"n\":\"deep\",\"d\":true,\"s\":0,"), "%s: directory entry:\n%s", kind, l);
    CHECK(l && strstr(l, "{\"n\":\"f.bin\",\"d\":false,\"s\":70000,\"t\":1710074096}"),
          "%s: file entry:\n%s", kind, l);
    CHECK(l && strstr(l, "{\"n\":\"caf\xC3\xA9.txt\",\"d\":false,\"s\":2,"), "%s: UTF-8 entry:\n%s",
          kind, l);
    CHECK(listing("/nope") == NULL, "list nothing");
    CHECK(listing("/up/f.bin") == NULL, "list a file");
    {
        web_list_t lst;
        char entry[16];
        const char *w;
        /* A buffer too small for an entry: the entry is left out. */
        CHECK(web_list_open(&lst, "/up") == NULL, "open");
        CHECK(web_list_next(&lst, 0, entry, sizeof entry, &w) == 0 && !w, "too small");
        web_list_close(&lst);
        web_list_close(&lst);
    }

    /* Rename and move. */
    CHECK(web_rename("/up/now.txt", "/up/deep/then.txt") == NULL && exists("/up/deep/then.txt") &&
              !exists("/up/now.txt"),
          "%s: move", kind);
    CHECK(web_rename("/up/deep/then.txt", "/up/keep.txt") != NULL, "%s: onto a file", kind);
    CHECK(strcmp(slurp("/up/keep.txt"), "old\n") == 0, "%s: ...which stays", kind);
    CHECK(web_rename("/up", "/up/deep/up") != NULL, "%s: into itself", kind);
    CHECK(web_rename("/up", "/UP/deep/up") != NULL, "%s: into itself, other case", kind);
    CHECK(web_rename("/", "/x") != NULL && web_rename("/up", "/") != NULL, "the root");
    CHECK(web_rename("/nope", "/x") != NULL, "rename nothing");
    CHECK(web_rename("/up/deep", "/up/DEEP2") == NULL && exists("/up/deep2/then.txt"),
          "%s: rename a directory", kind);

    /* Delete a file, and a tree with a read-only file and a hidden one. */
    CHECK(web_delete("/up/keep.txt") == NULL && !exists("/up/keep.txt"), "%s: delete a file", kind);
    CHECK(web_delete("/up/keep.txt") != NULL, "delete nothing");
    CHECK(web_mkdir("/up/deep2/a") == NULL && web_mkdir("/up/deep2/a/b") == NULL, "tree");
    CHECK(!upload("/up/deep2/a/b/x", "x", 1, 1, -1) && !upload("/up/deep2/a/y", "y", 1, 1, -1),
          "tree files");
    CHECK(f_chmod("/up/deep2/a/y", AM_RDO | AM_HID, AM_RDO | AM_HID) == FR_OK, "attributes");
    CHECK(web_delete("/up/deep2") == NULL && !exists("/up/deep2"), "%s: delete a tree", kind);
    CHECK(web_delete("/") != NULL, "delete the root");
    CHECK(web_delete("/up") == NULL && !exists("/up") && !exists("/up/f.bin"),
          "%s: delete the rest", kind);
    return 0;
}

/* ---- a card that fails -------------------------------------------------- */

static int fault_tests(void) {
    CHECK(host_disk_create(NULL, 32u << 20, false), "image");
    files_mount();
    CHECK(!upload("/a.txt", "old\n", 4, 4, -1), "a.txt");
    const char *data = pattern(60000, 1);

    /* The card dies a few sectors into an upload, at each point in turn:
     * the old file stays, or is replaced whole, and nothing half written
     * is under its own name. */
    for (long cut = 0; cut < 140; cut += 7) {
        host_disk_fail_writes_after(cut);
        const char *why = upload("/a.txt", data, 60000, 1460, -1);
        host_disk_fail_writes_after(-1);
        files_mount();
        const char *got = slurp("/a.txt");
        if (why) {
            CHECK(!got || strcmp(got, "old\n") == 0 || (!strcmp(why, "left as .new") && !got),
                  "cut %ld: '%s' left a.txt as %zu bytes", cut, why, got ? strlen(got) : 0u);
        } else {
            CHECK(got && strlen(got) == 60000 && memcmp(got, data, 60000) == 0, "cut %ld: whole", cut);
        }
    }

    /* A card too small. */
    CHECK(host_disk_create(NULL, 8u << 20, false), "small image");
    files_mount();
    char big[4096];
    memset(big, 'z', sizeof big);
    web_upload_t u;
    CHECK(web_upload_begin(&u, "/big.bin", -1, 0) == NULL, "begin");
    for (int i = 0; i < 3000; i++) web_upload_write(&u, big, sizeof big);
    const char *why = web_upload_end(&u);
    CHECK(why != NULL, "a full card is reported");
    CHECK(!exists("/big.bin") && !exists("/big.bin.new"), "and nothing is left");

    /* No card. */
    host_disk_close();
    CHECK(web_mkdir("/x") != NULL && web_delete("/x") != NULL && web_list_open(&(web_list_t){ 0 }, "/") != NULL,
          "no card");
    return 0;
}

int main(void) {
    if (test_request()) return 1;
    if (test_names()) return 1;
    if (card_tests(false)) return 1;
    if (card_tests(true)) return 1;
    if (fault_tests()) return 1;
    TEST_DONE();
}
