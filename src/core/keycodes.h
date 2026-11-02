/* keycodes.h — the southbridge's key codes (design.md §4.1).
 *
 * The table is picocalc-text-starter's drivers/keyboard.h (MIT, the
 * owner's), with its SDK include and driver prototypes dropped, checked
 * against hardware-notes.md §6.2. The input stream the shell reads uses
 * the same numbers: a byte below 0x80 is a character, one of 0x80 or
 * above is one of these keys. */
#ifndef SHELL_KEYCODES_H
#define SHELL_KEYCODES_H

/* Event states in the southbridge's FIFO (hardware-notes.md §6.2). */
#define KEY_STATE_IDLE      0x00
#define KEY_STATE_PRESSED   0x01
#define KEY_STATE_HOLD      0x02
#define KEY_STATE_RELEASED  0x03

#define KEY_MOD_ALT         0xA1
#define KEY_MOD_SHL         0xA2
#define KEY_MOD_SHR         0xA3
#define KEY_MOD_SYM         0xA4
#define KEY_MOD_CTRL        0xA5

#define KEY_BACKSPACE       0x08
#define KEY_TAB             0x09
#define KEY_ENTER           0x0A
#define KEY_RETURN          0x0D
#define KEY_SPACE           0x20

#define KEY_ESC             0xB1
#define KEY_LEFT            0xB4
#define KEY_UP              0xB5
#define KEY_DOWN            0xB6
#define KEY_RIGHT           0xB7

#define KEY_BREAK           0xD0
#define KEY_INSERT          0xD1
#define KEY_HOME            0xD2
#define KEY_DEL             0xD4
#define KEY_END             0xD5
#define KEY_PAGE_UP         0xD6
#define KEY_PAGE_DOWN       0xD7

#define KEY_CAPS_LOCK       0xC1

/* The mouse's wheel, one notch each: the shell's own codes, which the
 * southbridge never sends (design.md §3.12). Forward, away from the user,
 * is UP. At the prompt they are Up and Down; in a pager, vi and EDIT
 * they are WHEEL_LINES of them (shell_getkey()). */
#define KEY_WHEEL_UP        0xDA
#define KEY_WHEEL_DOWN      0xDB

#define KEY_F1              0x81
#define KEY_F2              0x82
#define KEY_F3              0x83
#define KEY_F4              0x84
#define KEY_F5              0x85
#define KEY_F6              0x86
#define KEY_F7              0x87
#define KEY_F8              0x88
#define KEY_F9              0x89
#define KEY_F10             0x90

#define KEY_POWER           0x91

#endif /* SHELL_KEYCODES_H */
