/* kbd.c — southbridge key events (kbd.h). After pico-oric's src/port/kbd.c
 * (GPL-3.0, the owner's), with one core's ring in place of its two. */

#include "kbd.h"

#include "config.h"
#include "southbridge.h"

#define SB_FIFO_DEPTH 31u   /* hardware-notes.md §6.1 */

/* Power of two, so the indices can run free and wrap by mask. */
#define RING SHELL_KEY_RING
_Static_assert(RING > SB_FIFO_DEPTH && (RING & (RING - 1u)) == 0,
               "the ring must hold a full FIFO and be a power of two");

static uint16_t s_ring[RING];
static uint32_t s_head, s_tail;
static uint32_t s_overflows;
static uint16_t (*s_source)(void);

void kbd_set_source(uint16_t (*read)(void)) {
    s_source = read;
}

unsigned kbd_poll(void) {
    unsigned n = 0;
    for (; n < SB_FIFO_DEPTH; n++) {
        uint8_t r[2];
        if (sb_read(SB_REG_FIF, r) != SB_OK) break;
        if (r[0] == 0 && r[1] == 0) break;          /* empty */
        if (s_head - s_tail >= RING) {
            s_overflows++;
            continue;
        }
        s_ring[s_head++ % RING] = (uint16_t)(r[0] << 8 | r[1]);
    }
    if (s_source) {
        uint16_t e;
        while ((e = s_source()) != 0) {
            n++;
            if (s_head - s_tail >= RING) {
                s_overflows++;
                continue;
            }
            s_ring[s_head++ % RING] = e;
        }
    }
    return n;
}

bool kbd_pop(uint8_t *state, uint8_t *code) {
    if (s_tail == s_head) return false;
    uint16_t e = s_ring[s_tail++ % RING];
    *state = (uint8_t)(e >> 8);
    *code = (uint8_t)e;
    return true;
}

uint32_t kbd_overflows(void) {
    return s_overflows;
}
