/* dos_parse.c — %NAME% and the DOS flavour's parser (dos.h). */

#include <string.h>

#include "dos.h"
#include "vars.h"

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

bool dos_expand(char *out, size_t size, const char *line, const char *const *params) {
    size_t n = 0;
    for (const char *s = line; *s; s++) {
        const char *value = NULL;
        size_t len = 1;
        if (*s == '%' && s[1] == '%') {
            value = "%";
            s++;
        } else if (*s == '%' && params && s[1] >= '0' && s[1] <= '9') {
            value = params[s[1] - '0'];
            if (!value) value = "";
            s++;
        } else if (*s == '%') {
            const char *end = strchr(s + 1, '%');
            size_t name = end ? (size_t)(end - s - 1) : 0;
            char buf[SHELL_LINE_MAX + 1];
            if (name > 0 && name <= SHELL_LINE_MAX && vars_is_name(s + 1, name)) {
                for (size_t i = 0; i < name; i++) buf[i] = upper(s[1 + i]);
                value = vars_get(buf, name);
                if (!value) value = "";
                s = end;
            }
        }
        if (value) len = strlen(value);
        else value = s;
        if (n + len >= size) return false;
        memcpy(out + n, value, len);
        n += len;
    }
    if (size == 0) return false;
    out[n] = '\0';
    return true;
}

/* ---- the parser ------------------------------------------------------- */

static bool blank(char c) {
    return c == ' ' || c == '\t';
}

static bool fail(dos_parse_t *p, const char *why) {
    p->error = why;
    return false;
}

/* Text into p->text, with its end checked. */
typedef struct {
    dos_parse_t *p;
    char        *t;
} writer_t;

static bool put(writer_t *w, char c) {
    if (w->t == w->p->text + sizeof w->p->text) return fail(w->p, "line too long");
    *w->t++ = c;
    return true;
}

/* One word from s[0, e) into the text, quotes removed: up to a blank,
 * or with `redir`, a '<' or '>', outside quotes. *sp moves past it. */
static bool word(writer_t *w, const char **sp, const char *e, bool redir, char **start) {
    const char *s = *sp;
    bool quoted = false;
    *start = w->t;
    while (s < e && (quoted || !(blank(*s) || (redir && (*s == '<' || *s == '>'))))) {
        if (*s == '"') quoted = !quoted;
        else if (!put(w, *s)) return false;
        s++;
    }
    *sp = s;
    return put(w, '\0');
}

/* One command, s[0, e), between pipes. */
static bool command(dos_parse_t *p, writer_t *w, size_t *nwords, const char *s, const char *e,
                    bool piped) {
    /* The command less its redirections, each left as a blank. */
    char rest[SHELL_LINE_MAX + 1];
    size_t rn = 0;
    char *in = NULL, *out = NULL;
    bool append = false, quoted = false;
    while (s < e) {
        if (quoted || (*s != '<' && *s != '>')) {
            if (*s == '"') quoted = !quoted;
            rest[rn++] = *s++;
            continue;
        }
        bool to = *s == '>';
        bool twice = to && s + 1 < e && s[1] == '>';
        s += twice ? 2 : 1;
        while (s < e && blank(*s)) s++;
        char *name;
        if (!word(w, &s, e, true, &name)) return false;
        if (!*name) return fail(p, "Syntax error");
        if (to) {
            out = name;
            append = twice;
        } else {
            in = name;
        }
        rest[rn++] = ' ';
    }
    rest[rn] = '\0';

    const char *r = rest;
    while (blank(*r)) r++;
    if (!*r) {
        if (in || out || piped) return fail(p, "Syntax error");
        return true;
    }
    if (p->ncmds == (int)SHELL_CMDS_MAX) return fail(p, "Too many commands");
    dos_cmd_t *c = &p->cmds[p->ncmds++];
    c->in = in;
    c->out = out;
    c->append = append;
    c->argv = &p->words[*nwords];

    /* The name, cut at '/', '\' or '.' after a builtin's name. */
    const char *n0 = r;
    while (*r && !blank(*r) && *r != '"') r++;
    const char *cut = n0;
    while (cut < r && *cut != '/' && *cut != '\\' && *cut != '.') cut++;
    if (cut > n0 && cut < r) {
        char head[SHELL_LINE_MAX + 1];
        memcpy(head, n0, (size_t)(cut - n0));
        head[cut - n0] = '\0';
        if (shell_is_builtin(head)) r = cut;
    }
    /* A '/' ends any command's name, as DOS's switch character did, so
     * PICO-ORIC/T is PICO-ORIC /T (design.md §4.7). */
    for (const char *k = n0; k < r; k++) {
        if (*k == '/') {
            r = k;
            break;
        }
    }
    if (*nwords == SHELL_WORDS_MAX) return fail(p, "Too many parameters");
    p->words[(*nwords)++] = w->t;
    c->argc = 1;
    for (const char *k = n0; k < r; k++)
        if (!put(w, *k)) return false;
    if (!put(w, '\0')) return false;

    /* The tail as typed, less blanks at its end. */
    const char *tend = r + strlen(r);
    while (tend > r && blank(tend[-1])) tend--;
    c->tail = w->t;
    for (const char *k = r; k < tend; k++)
        if (!put(w, *k)) return false;
    if (!put(w, '\0')) return false;

    /* Its words. */
    for (const char *k = r;;) {
        while (k < tend && blank(*k)) k++;
        if (k == tend) break;
        if (*nwords == SHELL_WORDS_MAX) return fail(p, "Too many parameters");
        char *start;
        if (!word(w, &k, tend, false, &start)) return false;
        p->words[(*nwords)++] = start;
        c->argc++;
    }
    p->words[(*nwords)++] = NULL;
    return true;
}

bool dos_parse(dos_parse_t *p, const char *line) {
    memset(p, 0, sizeof *p);
    if (strlen(line) > SHELL_LINE_MAX) return fail(p, "line too long");
    writer_t w = { p, p->text };
    size_t nwords = 0;
    bool piped = strchr(line, '|') != NULL;
    for (const char *s = line;;) {
        const char *e = s;
        bool quoted = false;
        while (*e && (quoted || *e != '|')) {
            if (*e == '"') quoted = !quoted;
            e++;
        }
        if (!command(p, &w, &nwords, s, e, piped)) return false;
        if (!*e) break;
        s = e + 1;
    }
    return true;
}
