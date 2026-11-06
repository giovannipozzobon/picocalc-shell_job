/* parse.c — one typed line into commands (parse.h). */

#include "parse.h"

#include <stddef.h>
#include <string.h>

static bool fail(parse_t *p, const char *why) {
    p->error = why;
    return false;
}

static bool is_blank(char c) { return c == ' ' || c == '\t'; }

/* Characters that end an unquoted word. */
static bool is_meta(char c) {
    return c == ';' || c == '&' || c == '|' || c == '<' || c == '>';
}

/* One word from *sp into p->text at *tp, as typed: its quotes and
 * escapes are checked here and removed when it is expanded (expand.h).
 * *sp is at its first character, which is neither blank nor a
 * metacharacter. */
static bool word(parse_t *p, const char **sp, char **tp) {
    const char *s = *sp;
    const char *start = s;
    while (*s && !is_blank(*s) && !is_meta(*s)) {
        if (*s == '\'') {
            const char *e = strchr(s + 1, '\'');
            if (!e) return fail(p, "unterminated '");
            s = e + 1;
        } else if (*s == '"') {
            s++;
            while (*s != '"') {
                if (!*s) return fail(p, "unterminated \"");
                if (*s == '\\' && s[1]) s++;
                s++;
            }
            s++;
        } else if (*s == '\\') {
            if (!s[1]) return fail(p, "\\ at the end of the line");
            s += 2;
        } else {
            s++;
        }
    }
    memcpy(*tp, start, (size_t)(s - start));
    *tp += s - start;
    *(*tp)++ = '\0';
    *sp = s;
    return true;
}

bool parse_line(parse_t *p, const char *line) {
    memset(p, 0, sizeof *p);
    /* A typed line is never longer; a script's could be, and the words'
     * text, which is never longer than the line, has to fit. */
    if (strlen(line) > SHELL_LINE_MAX) return fail(p, "line too long");
    const char *s = line;
    char *t = p->text;
    size_t nwords = 0;   /* slots used in p->words, NULLs included */
    size_t nargs = 0;    /* words alone */
    parse_cmd_t *c = NULL;
    sep_t sep = SEP_FIRST;

    for (;;) {
        while (is_blank(*s)) s++;
        if (*s == '#' || *s == '\0') break;

        if (*s == ';' || *s == '&' || *s == '|') {
            sep_t next;
            if (s[0] == '&' && s[1] == '&') next = SEP_AND, s += 2;
            else if (s[0] == '|' && s[1] == '|') next = SEP_OR, s += 2;
            else if (s[0] == ';') next = SEP_SEMI, s += 1;
            else if (s[0] == '|') next = SEP_PIPE, s += 1;
            else return fail(p, "& is not supported");
            if (!c || c->argc == 0) return fail(p, "a separator with no command before it");
            if (next == SEP_PIPE && (c->out || c->out_to_err)) return fail(p, "output both redirected and piped");
            p->words[nwords++] = NULL;
            c = NULL;
            sep = next;
            continue;
        }

        if (!c) {
            if (p->ncmds == (int)SHELL_CMDS_MAX) return fail(p, "too many commands");
            c = &p->cmds[p->ncmds++];
            c->sep = sep;
            c->argv = &p->words[nwords];
            c->start = (unsigned)(s - line);
        }

        if (*s == '<' && c->sep == SEP_PIPE) return fail(p, "input both piped and redirected");

        /* 1> and 2>: a number alone, right against the '>'. */
        int fd = 1;
        if ((s[0] == '1' || s[0] == '2') && s[1] == '>') fd = *s++ - '0';

        if (*s == '>' && s[1] == '&') {
            if ((s[2] != '1' && s[2] != '2') || (s[3] && !is_blank(s[3]) && !is_meta(s[3])))
                return fail(p, "only >&1 and >&2 are supported");
            if (fd == 2 && s[2] == '1') {
                c->err = NULL;
                c->err_to_out = true;
                c->err_to_file = c->out != NULL;
            } else if (fd == 1 && s[2] == '2') {
                c->out = NULL;
                c->out_to_err = true;
            }
            s += 3;
            c->end = (unsigned)(s - line);
            continue;
        }

        if (*s == '<' || *s == '>') {
            bool out = *s == '>';
            bool append = out && s[1] == '>';
            s += append ? 2 : 1;
            while (is_blank(*s)) s++;
            if (!*s || *s == '#' || is_meta(*s)) return fail(p, "a redirection with no file");
            char *name = t;
            if (!word(p, &s, &t)) return false;
            c->end = (unsigned)(s - line);
            if (fd == 2) {
                c->err = name;
                c->err_append = append;
                c->err_to_out = false;
            } else if (out) {
                c->out = name;
                c->append = append;
                c->out_to_err = false;
            } else {
                c->in = name;
            }
            continue;
        }

        if (nargs == SHELL_WORDS_MAX) return fail(p, "too many words");
        nargs++;
        p->words[nwords++] = t;
        c->argc++;
        if (!word(p, &s, &t)) return false;
        c->end = (unsigned)(s - line);
    }

    if (c && c->argc == 0) return fail(p, "a redirection with no command");
    /* A line may end with ';', as in sh, but not with && or ||. */
    if (!c && p->ncmds > 0 && sep != SEP_SEMI) return fail(p, "a separator with no command after it");
    if (c) p->words[nwords] = NULL;
    return true;
}
