/* dos_path.c — C:\ paths, wildcards, and DOS's words for failures
 * (dos.h). Pure string work, but for the FatFs result codes. */

#include <string.h>

#include "dos.h"
#include "files.h"
#include "path.h"

static char fold(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

bool dos_other_drive(const char *arg) {
    char d = fold(arg[0]);
    return d >= 'A' && d <= 'Z' && arg[1] == ':' && d != 'C';
}

bool dos_resolve(char out[SHELL_PATH_MAX + 1], const char *cwd, const char *arg) {
    /* As typed, only '\\' separates names; '/' starts a switch (§4.7). The
     * card's own paths, as in cwd, use '/' and do not come through arg. */
    if (dos_other_drive(arg) || strchr(arg, '/')) return false;
    if (fold(arg[0]) == 'C' && arg[1] == ':') arg += 2;
    char buf[SHELL_PATH_MAX + 1];
    size_t n = strlen(arg);
    if (n > SHELL_PATH_MAX) return false;
    for (size_t i = 0; i <= n; i++) buf[i] = arg[i] == '\\' ? '/' : arg[i];
    return path_resolve(out, cwd, buf);
}

void dos_show(char *out, size_t size, const char *path) {
    if (size == 0) return;
    size_t n = 0;
    for (const char *s = "C:"; *s && n + 1 < size; s++) out[n++] = *s;
    for (const char *s = path; *s && n + 1 < size; s++) out[n++] = *s == '/' ? '\\' : *s;
    out[n] = '\0';
}

bool dos_wild(const char *s) {
    return strpbrk(s, "*?") != NULL;
}

/* One part of a name, s[0, sn), against one of a pattern, p[0, pn). */
static bool part(const char *p, size_t pn, const char *s, size_t sn) {
    const char *pe = p + pn, *se = s + sn;
    const char *star = NULL, *resume = NULL;
    for (;;) {
        if (p < pe && *p == '*') {
            while (p < pe && *p == '*') p++;
            star = p;
            resume = s;
            continue;
        }
        if (s == se) {
            /* What is left of the pattern may match nothing. */
            while (p < pe && (*p == '?' || *p == '*')) p++;
            return p == pe;
        }
        if (p < pe && (*p == '?' || fold(*p) == fold(*s))) {
            p++;
            s++;
        } else if (star) {
            p = star;
            s = ++resume;
        } else {
            return false;
        }
    }
}

bool dos_match(const char *pat, const char *name) {
    const char *pdot = strrchr(pat, '.');
    if (!pdot) return part(pat, strlen(pat), name, strlen(name));
    const char *ndot = strrchr(name, '.');
    size_t nbase = ndot ? (size_t)(ndot - name) : strlen(name);
    const char *next = ndot ? ndot + 1 : name + nbase;
    return part(pat, (size_t)(pdot - pat), name, nbase) &&
           part(pdot + 1, strlen(pdot + 1), next, strlen(next));
}

/* One part of a mask over one of a name, appended to out at *n. */
static bool apply(char *out, size_t size, size_t *n, const char *m, size_t mn, const char *s,
                  size_t sn) {
    const char *se = s + sn;
    for (size_t i = 0; i < mn; i++) {
        char c = m[i];
        if (c == '*') {
            while (s < se) {
                if (*n + 1 >= size) return false;
                out[(*n)++] = *s++;
            }
            continue;
        }
        if (c == '?') {
            if (s == se) continue;
            c = *s;
        }
        if (s < se) s++;
        if (*n + 1 >= size) return false;
        out[(*n)++] = c;
    }
    return true;
}

bool dos_rename(char *out, size_t size, const char *mask, const char *name) {
    size_t n = 0;
    const char *mdot = strrchr(mask, '.');
    if (!mdot) {
        if (!apply(out, size, &n, mask, strlen(mask), name, strlen(name))) return false;
    } else {
        const char *ndot = strrchr(name, '.');
        size_t nbase = ndot ? (size_t)(ndot - name) : strlen(name);
        const char *next = ndot ? ndot + 1 : name + nbase;
        if (!apply(out, size, &n, mask, (size_t)(mdot - mask), name, nbase)) return false;
        size_t dot = n;
        if (n + 1 >= size) return false;
        out[n++] = '.';
        if (!apply(out, size, &n, mdot + 1, strlen(mdot + 1), next, strlen(next))) return false;
        if (n == dot + 1) n = dot;   /* no extension: no '.' */
    }
    if (n == 0 || size == 0) return false;
    out[n] = '\0';
    return true;
}

const char *dos_why(FRESULT r) {
    switch (r) {
    case FR_OK:                  return "OK";
    case FR_NO_FILE:             return "File not found";
    case FR_NO_PATH:             return "Path not found";
    case FR_INVALID_NAME:        return "Invalid filename";
    case FR_DENIED:
    case FR_EXIST:               return "Access denied";
    case FR_NOT_READY:           return "Not ready reading drive C";
    case FR_WRITE_PROTECTED:     return "Write protect error writing drive C";
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:       return "Invalid media type reading drive C";
    case FR_LOCKED:              return "Sharing violation reading drive C";
    case FR_TOO_MANY_OPEN_FILES: return "Too many files open";
    case FR_DISK_ERR:
    default:                     return "General failure reading drive C";
    }
}

const char *dos_say(const char *why) {
    /* files_why's words are the same strings each time. */
    for (int r = FR_DISK_ERR; r <= FR_INVALID_PARAMETER; r++)
        if (why == files_why((FRESULT)r)) return dos_why((FRESULT)r);
    if (strcmp(why, "read-only") == 0 || strcmp(why, "is a directory") == 0)
        return "Access denied";
    if (strcmp(why, "no space left on the card") == 0) return "Insufficient disk space";
    if (strcmp(why, "name too long") == 0) return "Invalid path";
    if (strcmp(why, "left as .new") == 0) return "Write failed; the new file is left as .NEW";
    return why;
}
