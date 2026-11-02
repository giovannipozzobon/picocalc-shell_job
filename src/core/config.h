/* config.h — every fixed capacity in the shell (design.md §5, §7; EL §2.1).
 *
 * Portable: no SDK header. Nothing elsewhere duplicates one of these, so
 * the SRAM budget in design.md §7 is a link-time fact. */
#ifndef SHELL_CONFIG_H
#define SHELL_CONFIG_H

/* The panel (hardware-notes.md §4.1) and the fonts' cell (design.md §4.1). */
#define SHELL_PANEL_W      320u
#define SHELL_PANEL_H      320u
#define SHELL_PANEL_MEM_H  480u   /* frame memory rows, for hardware scroll (§4.8) */
#define SHELL_GLYPH_H      10u

/* The terminal's grid: 32 rows of 40 columns, in each flavour's 8x10 font
 * (design.md §4.1, §4.7). */
#define TERM_ROWS          32u
#define TERM_COLS          40u
#define TERM_PARAMS        16u    /* CSI parameters kept; more are dropped */

/* One line typed at the prompt, without its terminator, and what the
 * parser makes of it (parse.h): words across all its commands, and
 * commands joined by ;, && and ||. */
#define SHELL_LINE_MAX     255u
#define SHELL_PROMPT_MAX   320u   /* the prompt as shown: a whole path, or DOS's PROMPT */
#define SHELL_WORDS_MAX    32u
#define SHELL_CMDS_MAX     8u

/* A line's commands after expansion (expand.h): the words one command
 * may have once variables and globs are expanded, and their text. */
#define SHELL_ARGS_MAX     128u
#define SHELL_ARGS_TEXT    4096u

/* How deep `sh` scripts nest: the typed line is level 0. Each level has
 * its own parsed line, expanded words and redirections. */
#define SHELL_DEPTH        4u

/* The UNIX flavour's select (design.md §4.4), at each level: its text
 * from `select` to `done`, in pieces, and the words being offered by it
 * and any select inside it. */
#define SHELL_SELECT_TEXT  1024u
#define SHELL_SELECT_PARTS 64u
#define SHELL_SELECT_WORDS 512u
#define SHELL_SELECT_ITEMS 64u

/* Variables (vars.h), packed as NAME=value: all of them together. */
#define SHELL_VARS_BYTES   1024u

/* Pipes (pipe.h): two RAM buffers, shared; a pipe with none, or whose
 * output outgrows its buffer, goes through a file in /.shell. The pool
 * holds two pipes for each level. */
#define SHELL_PIPE_BUF     16384u
#define SHELL_PIPE_RAM     2u
#define SHELL_PIPES        (2u * SHELL_DEPTH)

/* History (history.h): the lines kept, packed, oldest dropped first. */
#define SHELL_HISTORY_BYTES 2048u

/* Tab completion (line.c): the names offered, packed, and how many. */
#define SHELL_COMPLETE_NAMES 2048u
#define SHELL_COMPLETE_MAX   128u

/* web.uf2 (web.h): a request's target, "/api/x?query", as read, and one
 * entry of a listing as JSON. A name on the card is up to 255 bytes of
 * CP437, which is up to 765 in UTF-8; FAT names hold no `"`, `\` or
 * control character, so JSON adds nothing to them. */
#define WEB_TARGET_MAX     600u
#define WEB_ENTRY_MAX      1024u
/* One connection's output buffer (httpd.h): a response's header and the
 * next piece of a file or listing. */
#define HTTPD_OUT_MAX      4096u

/* A time zone's name in TZ, such as EST or <+0530> (clock.h); a longer
 * one is cut. */
#define SHELL_ZONE_MAX     15u

/* /ntp/ntp.cfg (ntp.h): the file read, a line of it, and its values: a
 * server's name, an SSID (802.11's 32 bytes) and a WPA2 passphrase (63
 * characters, or 64 hex digits). */
#define NTP_CFG_BYTES      2048u
#define NTP_CFG_LINE_MAX   160u
#define NTP_SERVER_MAX     127u
#define NTP_SSID_MAX       32u
#define NTP_PASSWORD_MAX   64u

/* A line as grep and more read it; a longer one is taken in pieces. */
#define SHELL_TEXT_LINE    512u

/* A path on the card, as the shell resolves it (path.h): FAT's own limit
 * on a name, and room for a few levels above it. */
#define SHELL_PATH_MAX     255u

/* Files (files.c): the buffer cat, cp and redirection copy through; the
 * names ls sorts, packed, and how many; how deep cp -r and rm -r go. */
#define SHELL_COPY_BUF     4096u
#define SHELL_LS_NAMES     16384u
#define SHELL_LS_MAX       512u
#define SHELL_TREE_DEPTH   16u

/* Running a program (uf2.c): the most 4 KB flash sectors an app
 * partition can have, 16 MB, for the bitmap of those that differ. */
#define SHELL_FLASH_SECTORS_MAX 4096u

/* out_printf formats into this, and out_write translates LF to CR LF in
 * chunks of this, so neither needs an allocator. */
#define SHELL_OUT_CHUNK    128u
#define SHELL_PRINTF_MAX   256u

/* usbdisk (design.md §4.2): TinyUSB's mass-storage buffer, the most of
 * one SCSI read or write moved to or from the card in one multi-block
 * transfer. A multiple of the 512-byte block, at most 32 KB (TinyUSB
 * counts a write's piece in 16 bits). */
#define SHELL_USB_BUF      16384u

/* Southbridge key events held between polls: more than its FIFO's 31
 * (hardware-notes.md §6.1), a power of two. */
#define SHELL_KEY_RING     64u

/* Lines a notch of the mouse's wheel moves in vi, EDIT and a pager
 * (keycodes.h; design.md §3.12). At the prompt a notch is one step of the
 * history. */
#define SHELL_WHEEL_LINES  3

/* vi (design.md §4.6, vi.c). The file is held whole in one buffer, its
 * NUL included, so a file of up to VI_BUF_SIZE - 1 bytes opens and a
 * larger one is refused. The copy buffer is the one register; a yank
 * larger than it is refused, not cut short. The undo journal drops its
 * oldest steps to make room (editor_undo.h). The screen is VI_TEXT_ROWS
 * of text and a status row. */
/* The W builds (SHELL_COMPACT, set by CMakeLists.txt) carry the radio's
 * driver and Bluetooth's BTstack, about 127 KB, and have room for them
 * by holding a smaller file in vi (design.md §3.10). The host's builds
 * and the Pico 2's keep the full size. */
#ifndef SHELL_COMPACT
#define SHELL_COMPACT 0
#endif
#if SHELL_COMPACT
#define VI_BUF_SIZE        32768u
#define VI_COPY_SIZE       4096u
#define VI_UNDO_SIZE       8192u
#else
#define VI_BUF_SIZE        65536u
#define VI_COPY_SIZE       8192u
#define VI_UNDO_SIZE       16384u
#endif
#define VI_TEXT_ROWS       31
#define VI_TAB_STOP        8u     /* a tab's width on screen */
#define VI_SHIFT_WIDTH     4u     /* the spaces > and < add and take */

/* EDIT (design.md §4.7, edit.c): the DOS flavour's editor, in its
 * 40 columns. The buffer and the clipboard are vi's sizes; the
 * screen is a menu bar, a title row, EDIT_TEXT_ROWS of text and a status
 * row. A Find or Change string holds up to EDIT_FIND_MAX bytes. */
#define EDIT_TEXT_ROWS     29
#define EDIT_FIND_MAX      48u

/* The vi key layer's own capacities, under Pico Logo's names (its
 * core/limits.h explains each). Its 40-column footer fits this status
 * row. */
#define LOGO_VI_TEXT_MAX          32
#define LOGO_VI_CMDLINE_MAX       (LOGO_VI_TEXT_MAX * 2 + 8)
#define LOGO_VI_MSG_MAX           40
#define LOGO_VI_SUB_EXPAND_MAX    256
#define LOGO_VI_PATTERN_STEPS_MAX 200000
#define LOGO_VI_COUNT_MAX         999999
#define LOGO_VI_REPEAT_MAX        8
#define LOGO_VI_INSERT_MAX        80

#endif /* SHELL_CONFIG_H */
