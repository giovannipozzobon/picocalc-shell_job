/* screen.h — draws the terminal's grid on the panel (design.md §4.1).
 *
 * The renderer half of term.h's contract: hardware scroll for whole-screen
 * scrolls, then the dirty rows, each drawn only from its first changed
 * cell to its last. The glyph drawing, bold, underline and the underline
 * cursor are picocalc-text-starter's drivers/lcd.c (MIT, the owner's). */
#ifndef SHELL_SCREEN_H
#define SHELL_SCREEN_H

#include <stdint.h>

#include "term.h"

/* After lcd_init(), which leaves the panel black at scroll offset 0. */
void screen_init(void);

/* Draw what changed since the last call. Output shows the cursor at once
 * and restarts its blink. */
void screen_flush(term_t *t, uint64_t now_us);

/* Draw every row again at the next flush, whether or not the shadow says
 * it is drawn: for a display that missed what was drawn (the HDMI mirror
 * after lcd_mirror_resync(), which sends no picture). */
void screen_repaint(term_t *t);

/* The cursor's blink, 500 ms on and 500 ms off, as the starter's. Call
 * from the main loop. */
void screen_tick(term_t *t, uint64_t now_us);

/* Rows and cells drawn since start, for the log. */
void screen_counts(uint32_t *rows, uint32_t *cells);

#endif /* SHELL_SCREEN_H */
