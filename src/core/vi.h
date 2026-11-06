/* vi.h — the editor: `vi [-R] file` and `view file` (design.md §4.6).
 *
 * Pico Logo's vi key layer (editor_vi.h) decides what each key means;
 * this is the rest: the buffer, insert mode, the screen, and the file.
 *
 *   - The whole file is held in one buffer of VI_BUF_SIZE bytes. A larger
 *     file is refused, never truncated.
 *   - The file's line ending is kept: if every line break is CR LF, the
 *     CRs are taken off on reading and put back on writing; otherwise a
 *     CR is a character like any other (shown as ^M). Whether the file
 *     ends in a line break is kept too.
 *   - `view`, `vi -R`, and a file FAT marks read-only open read-only:
 *     they can be changed but not written.
 *   - Saving goes through name.new and a rename (files.h).
 *   - The screen is VI_TEXT_ROWS of text and a status row, drawn on the
 *     terminal with escape sequences; only rows that change are sent.
 *     Long lines are not wrapped: the view scrolls sideways to the
 *     cursor. Tabs stop every VI_TAB_STOP columns.
 *   - Keys come from the shell's getkey hook. Brk leaves at once from
 *     every mode, discarding changes, as in Pico Logo; so does the end of
 *     input. */
#ifndef SHELL_VI_H
#define SHELL_VI_H

#include <stddef.h>

#include "shell.h"

int cmd_vi(shell_t *sh, int argc, char **argv);
int cmd_view(shell_t *sh, int argc, char **argv);

/* vi's text buffer, for a command to borrow while vi is not running: vi
 * reads its file in afresh each time. */
char *vi_scratch(size_t *size);

#endif /* SHELL_VI_H */
