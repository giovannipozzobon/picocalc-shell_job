/* test_keys.c — southbridge events and UART bytes to input (keys.h;
 * hardware-notes.md §6.2, §6.3). */

#include "keycodes.h"
#include "keys.h"
#include "test_util.h"

int main(void) {
    keys_t k;
    keys_init(&k);

    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'a') == 'a', "a press types");
    CHECK(keys_event(&k, KEY_STATE_RELEASED, 'a') == KEYS_NONE, "a release does not");
    CHECK(keys_event(&k, KEY_STATE_HOLD, 'a') == KEYS_NONE, "nor a hold");
    CHECK(keys_event(&k, KEY_STATE_RELEASED, ' ') == KEYS_NONE, "a release with no press");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'j') == 'j' && keys_event(&k, KEY_STATE_PRESSED, 'j') == 'j',
          "auto-repeat arrives as more presses, and each types");

    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_ENTER) == '\r', "Enter is CR");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_BACKSPACE) == 0x08, "Backspace");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_ESC) == 0x1B, "Esc is ESC");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_CAPS_LOCK) == KEYS_NONE, "Caps Lock is accepted");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_SHL) == KEYS_NONE, "Shift alone types nothing");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_UP) == KEY_UP, "arrows pass as their codes");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_BREAK) == KEY_BREAK, "Break");
    /* The mouse's wheel, as bt_source() presses it. */
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_WHEEL_UP) == KEY_WHEEL_UP &&
              keys_event(&k, KEY_STATE_PRESSED, KEY_WHEEL_DOWN) == KEY_WHEEL_DOWN,
          "the wheel passes as its codes");

    /* Ctrl arrives as its own event beside the key (§6.3). */
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_CTRL) == KEYS_NONE, "Ctrl alone");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'c') == 0x03, "Ctrl-C");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'C') == 0x03, "Ctrl-C with Caps Lock");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, '[') == 0x1B, "Ctrl-[");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, '1') == '1', "Ctrl-1 is 1");
    CHECK(keys_event(&k, KEY_STATE_HOLD, KEY_MOD_CTRL) == KEYS_NONE && k.ctrl, "Ctrl held");
    CHECK(keys_event(&k, KEY_STATE_RELEASED, KEY_MOD_CTRL) == KEYS_NONE, "Ctrl up");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'c') == 'c', "c after Ctrl is let go");
    keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_CTRL);
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_LEFT) == KEY_LEFT, "Ctrl+Left at the prompt is Left");
    keys_event(&k, KEY_STATE_RELEASED, KEY_MOD_CTRL);

    CHECK(keys_uart(&k, '\r') == '\r' && keys_uart(&k, '\n') == '\r', "UART CR and LF are Enter");
    CHECK(keys_uart(&k, 0x7F) == 0x08 && keys_uart(&k, 0x08) == 0x08, "UART DEL and BS are Backspace");
    CHECK(keys_uart(&k, 0x81) == KEY_F1 && keys_uart(&k, 'x') == 'x', "UART bytes are themselves");
    CHECK(keys_uart(&k, 0x1F) == 0x1F, "UART US is itself while Alt is not reported");

    /* Alt, not reported: the MCU's capital, and nothing for a tap. */
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_ALT) == KEYS_NONE, "Alt alone");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'F') == 'F', "Alt+F is F at the prompt");
    CHECK(keys_event(&k, KEY_STATE_RELEASED, KEY_MOD_ALT) == KEYS_NONE, "Alt up");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_ALT) == KEYS_NONE &&
              keys_event(&k, KEY_STATE_RELEASED, KEY_MOD_ALT) == KEYS_NONE,
          "no tap while Alt is not reported");

    /* Alt, reported (EDIT). */
    keys_report_chords(true);
    keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_ALT);
    CHECK(keys_event(&k, KEY_STATE_RELEASED, KEY_MOD_ALT) == KEYS_ALT_TAP, "a tap of Alt");
    keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_ALT);
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'F') == (KEYS_ALT | 'f'), "Alt+F");
    CHECK(keys_event(&k, KEY_STATE_RELEASED, 'F') == KEYS_NONE, "F up");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_LEFT) == (KEYS_ALT | KEY_LEFT), "Alt+Left");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_INSERT) == KEY_INSERT, "Alt+I is Insert");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_F3) == KEY_F3, "Alt+F3 is F3");
    CHECK(keys_event(&k, KEY_STATE_RELEASED, KEY_MOD_ALT) == KEYS_NONE, "not a tap after a key");
    keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_ALT);
    keys_event(&k, KEY_STATE_HOLD, KEY_MOD_ALT);
    CHECK(keys_event(&k, KEY_STATE_RELEASED, KEY_MOD_ALT) == KEYS_NONE, "not a tap once held");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'f') == 'f', "f after Alt is let go");
    CHECK(keys_uart(&k, 0x1F) == KEYS_NONE && keys_uart(&k, 'F') == (KEYS_ALT | 'f'), "UART US F");
    CHECK(keys_uart(&k, 0x1F) == KEYS_NONE && keys_uart(&k, 0xB4) == (KEYS_ALT | KEY_LEFT),
          "UART US Left");
    CHECK(keys_uart(&k, 0x1F) == KEYS_NONE && keys_uart(&k, 0x1F) == KEYS_ALT_TAP, "UART US US");
    keys_event(&k, KEY_STATE_PRESSED, KEY_MOD_CTRL);
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_RIGHT) == (KEYS_CTRL | KEY_RIGHT), "Ctrl+Right");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, KEY_HOME) == (KEYS_CTRL | KEY_HOME), "Ctrl+Home");
    CHECK(keys_event(&k, KEY_STATE_PRESSED, 'x') == 0x18, "Ctrl+X still a control code");
    keys_event(&k, KEY_STATE_RELEASED, KEY_MOD_CTRL);
    keys_report_chords(false);

    TEST_DONE();
}
