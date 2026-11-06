/* test_parse.c — a line into commands (parse.h, design.md §4.4).
 *
 * The words come out as typed, quotes and all; test_expand.c checks
 * what they become. */

#include <string.h>

#include "parse.h"
#include "test_util.h"

static parse_t p;

/* The commands as text: words joined by '|', redirections as <x >x >>x,
 * commands joined by their separators. */
static const char *show(void) {
    static char buf[1024];
    static const char *const seps[] = { "", " ; ", " && ", " || ", " | " };
    buf[0] = '\0';
    for (int i = 0; i < p.ncmds; i++) {
        const parse_cmd_t *c = &p.cmds[i];
        strcat(buf, seps[c->sep]);
        for (int j = 0; j < c->argc; j++) {
            if (j) strcat(buf, "|");
            strcat(buf, c->argv[j]);
        }
        if (c->argv[c->argc] != NULL) strcat(buf, "(argv not NULL-ended)");
        if (c->in) strcat(buf, " <"), strcat(buf, c->in);
        if (c->out) strcat(buf, c->append ? " >>" : " >"), strcat(buf, c->out);
    }
    return buf;
}

int main(void) {
    static const struct { const char *line, *want; } good[] = {
        { "",                          "" },
        { "   ",                       "" },
        { "# a comment",               "" },
        { "ls",                        "ls" },
        { "  ls  -l   /home ",         "ls|-l|/home" },
        { "echo 'a  b' \"c d\"",       "echo|'a  b'|\"c d\"" },
        { "echo it\\'s",               "echo|it\\'s" },
        { "echo a\\ b",                "echo|a\\ b" },
        { "echo 'x'\"y\"z",            "echo|'x'\"y\"z" },
        { "echo '' \"\"",              "echo|''|\"\"" },
        { "echo \"a\\\"b\\\\c\\d\"",   "echo|\"a\\\"b\\\\c\\d\"" },
        { "echo '\\'",                 "echo|'\\'" },
        { "echo a#b # rest",           "echo|a#b" },
        { "echo '#' \\#",              "echo|'#'|\\#" },
        { "echo $HOME",                "echo|$HOME" },
        { "echo '>' \\; \"&&\"",       "echo|'>'|\\;|\"&&\"" },
        { "echo 'a|b' a\\|b",          "echo|'a|b'|a\\|b" },
        { "ls > out",                  "ls >out" },
        { "ls >out",                   "ls >out" },
        { "ls>>out",                   "ls >>out" },
        { "cat < in > out",            "cat <in >out" },
        { "> out ls -a",               "ls|-a >out" },
        { "cat <'my file'",            "cat <'my file'" },
        { "a;b",                       "a ; b" },
        { "a ; b ;",                   "a ; b" },
        { "a && b || c",               "a && b || c" },
        { "a&&b||c;d",                 "a && b || c ; d" },
        { "mkdir x && cd x > log",     "mkdir|x && cd|x >log" },
        { "ls | more",                 "ls | more" },
        { "a|b|c",                     "a | b | c" },
        { "cat < f | grep x > g",      "cat <f | grep|x >g" },
        { "a | b && c || d | e ; f",   "a | b && c || d | e ; f" },
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        bool ok = parse_line(&p, good[i].line);
        CHECK(ok, "'%s': %s", good[i].line, p.error);
        if (ok)
            CHECK(strcmp(show(), good[i].want) == 0, "'%s' gave '%s', want '%s'", good[i].line,
                  show(), good[i].want);
    }

    static const char *const bad[] = {
        "echo 'a",
        "echo \"a",
        "echo a\\",
        "ls &",
        "ls |",
        "| ls",
        "ls | | more",
        "ls > f | more",
        "ls | more < f",
        "; ls",
        "ls ;; ls",
        "ls &&",
        "ls ||",
        "&& ls",
        "ls >",
        "ls > ;",
        "ls < #x",
        "> out",
        "> out ; ls",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool ok = parse_line(&p, bad[i]);
        CHECK(!ok && p.error, "'%s' was taken: '%s'", bad[i], show());
    }

    /* The limits: SHELL_WORDS_MAX words, SHELL_CMDS_MAX commands. */
    char line[SHELL_LINE_MAX + 1] = "";
    for (unsigned i = 0; i < SHELL_WORDS_MAX; i++) strcat(line, "w ");
    CHECK(parse_line(&p, line) && p.cmds[0].argc == (int)SHELL_WORDS_MAX, "the most words");
    strcat(line, "w");
    CHECK(!parse_line(&p, line), "one word more");
    line[0] = '\0';
    for (unsigned i = 0; i < SHELL_CMDS_MAX; i++) strcat(line, "a;");
    CHECK(parse_line(&p, line) && p.ncmds == (int)SHELL_CMDS_MAX, "the most commands");
    strcat(line, "a");
    CHECK(!parse_line(&p, line), "one command more");

    /* The longest line of the shortest words fits the text buffer. */
    memset(line, 0, sizeof line);
    for (unsigned i = 0; i < SHELL_LINE_MAX; i++) line[i] = (i % 2) ? ';' : 'a';
    CHECK(!parse_line(&p, line) || p.ncmds <= (int)SHELL_CMDS_MAX, "no overrun");
    memset(line, 0, sizeof line);
    memset(line, 'q', SHELL_LINE_MAX);
    CHECK(parse_line(&p, line) && strlen(p.cmds[0].argv[0]) == SHELL_LINE_MAX, "one long word");
    /* The error stream, and which number counts. */
    const parse_cmd_t *c = &p.cmds[0];
    CHECK(parse_line(&p, "a 2> e") && c->argc == 1 && strcmp(c->err, "e") == 0 && !c->err_append,
          "2>");
    CHECK(parse_line(&p, "a 2>>e") && strcmp(c->err, "e") == 0 && c->err_append, "2>>");
    CHECK(parse_line(&p, "a 2>&1") && c->argc == 1 && c->err_to_out && !c->err_to_file, "2>&1");
    CHECK(parse_line(&p, "a > o 2>&1") && c->err_to_out && c->err_to_file, "> o 2>&1");
    CHECK(parse_line(&p, "a 2>&1 > o") && c->err_to_out && !c->err_to_file, "2>&1 > o");
    CHECK(parse_line(&p, "a >&2") && c->out_to_err && !c->out, ">&2");
    CHECK(parse_line(&p, "a 1>&2") && c->out_to_err, "1>&2");
    CHECK(parse_line(&p, "a 1> o") && strcmp(c->out, "o") == 0, "1>");
    CHECK(parse_line(&p, "a 2>&1 | b") && p.ncmds == 2 && c->err_to_out, "into a pipe");
    CHECK(parse_line(&p, "a 2 > o") && c->argc == 2 && strcmp(c->argv[1], "2") == 0 && !c->err,
          "2 alone is a word");
    CHECK(parse_line(&p, "a x2> o") && c->argc == 2 && strcmp(c->argv[1], "x2") == 0 && !c->err,
          "x2 is a word");
    CHECK(parse_line(&p, "a 12> o") && c->argc == 2 && strcmp(c->argv[1], "12") == 0, "12 is a word");
    CHECK(!parse_line(&p, "a >&3") && strstr(p.error, ">&1 and >&2"), ">&3");
    CHECK(!parse_line(&p, "a >&2x"), ">&2x");
    CHECK(!parse_line(&p, "a >&2 | b") && strstr(p.error, "piped"), ">&2 and a pipe");
    CHECK(!parse_line(&p, "a 2>") && strstr(p.error, "no file"), "2> nothing");

    static char longer[SHELL_LINE_MAX + 2];
    memset(longer, 'q', SHELL_LINE_MAX + 1);
    CHECK(!parse_line(&p, longer) && strcmp(p.error, "line too long") == 0, "one character more");

    TEST_DONE();
}
