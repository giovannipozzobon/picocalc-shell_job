# Milestone log

What each milestone verified, on which board, on what date, and what was
not checked, newest first. `design.md` §10 holds each milestone's scope
and done-when criteria; this file keeps the full record (EL §14.3).

Each entry gives:

- the milestone and the branch, with the date;
- what was **checked on the host**: the tests added, planted bugs caught,
  and the memory report;
- what was **checked on the board**: which board (physical identity, not
  just the build target, HW §2.1), which build, who checked it (the owner's
  checks on the panel count, and say so), and the log under `out/`;
- **measurements** beside the estimates they replace (EL §14.4);
- a **not checked** list, carried forward until something checks each item
  or a decision drops it.

## M16 — The mouse in the shell, step 1: the wheel (2026-11-02, branch `m15-hdmi`)

**Built 2026-11-02; the owner tried the wheel on the board the same day: it scrolls the history and works in vi** (design.md §3.12). The owner ran
`mouse-test.uf2` with a Bluetooth mouse on a Pico 2 W: the pointer was
smooth, a gain of 0.5 with no acceleration was right, the wheel's
direction and the buttons were as shown (the other mouse, a keyboard's
trackpad, to be tried later). The shell now reads the wheel: `KEY_WHEEL_UP`
and `KEY_WHEEL_DOWN`, from `bt_source()` into the key ring, are a step of
the history at the prompt, three lines in vi and EDIT, and three rows of
`more` going back.

**Checked on the host.** `test_keys` (the codes pass), `test_line` (the
history), `test_text` (`more`: forward ignored, back three rows),
`test_vi` and `test_edit` (three lines, forward and back, and forward at the
top staying). A planted change of `SHELL_WHEEL_LINES` to 2 failed five
tests (`test_text`, `test_vi`, `test_edit` and their W-size twins). All 33
tests pass; the W builds (release, HDMI, DOS) and the Pico 2 release build
with no warnings: the UNIX W release build 495,104 B, 29,184 B free.

**Not confirmed on the board.** The wheel in `more` of a long file and in
EDIT; a mouse and a keyboard together (no key lost, the order of the
wheel's notches and the keys); and no mouse.

## M14, follow-up — btpair 2.1's keyboard and mouse (2026-11-02, branch `m15-hdmi`)

picocalc-extras' `btkbd.c` was rewritten for a mouse beside the keyboard
(btpair 2.0 and 2.1). The shell's `btkbd.c` was brought to it (design.md
§3.10, the end), keeping the shell's changes, since the old one would have
taken a mouse that connects by itself for the keyboard.

**Checked on the host.** The new original against the shell's file: the
only differences are the shell's own (the storage on `btstore.c`, the
firmware's open and close, the tags' names). `test_btstore`: the two mouse
tags' bytes (`PTR0`, `PTL0`), and a keyboard and mouse records beside each
other, one removed with the other kept. All 33 host tests pass; the W
builds (UNIX, DOS, release, HDMI) build with no warnings. RAM: the UNIX W
release build 494,724 B, 29,564 B free (was 492,620); the development W
build 497,664 B (was 495,424), 26,624 B free; DOS W 486,960 B, 37,328 B
free.

**`mouse-test.uf2` built 2026-11-02** (design.md §3.10): a RAM program in
three pages (the numbers, a pixel pointer, a cell pointer), 212,884 B of
RAM, no warnings. Not run on the board; the host has no test for it.

**Not checked on the board.** All of it: a keyboard alone paired with
btpair 2.1 (the file's format and the reconnection), a keyboard and a mouse
together, a mouse alone, a BLE mouse, a file paired by the earlier btpair
read now, and `usbdisk` and the battery with two devices connected.

## M15 — An HDMI mirror (2026-11-01, branch `m15-hdmi`)

**Built 2026-11-01; the owner ran it on the board 2026-11-02 and the
mirror works** (design.md §3.11), after the fix below. The
Pico's side of the owner's `pico_tang20k_hdmi` is in the shell as a build
option, `-DPICO_SHELL_HDMI=ON`: `src/port/mirror.[ch]` and `mirror.pio`,
the calls in `lcd.c`, `lcd_mirror_resync()`, `screen_repaint()`, and the
`hdmi` (`HDMI`) builtin, which resends the screen's state and redraws.
The FPGA's side was left as it is.

**Checked on the host.** `hdmi` in both flavours (`test_ntp`): not on
this machine without a mirror, the line the port gives, no arguments,
`man`/`HELP`; the builtin lists and the 40-column widths (`test_system`,
`test_dos`). All 33 host tests pass.

**Firmware builds**, all with `-Wall -Wextra -Werror`, no warnings:
Pico 2 release UNIX 450,924 B without the mirror, 452,572 B with it
(+1,648 B); DOS with it 437,248 B; Pico 2 W release 492,620 B without,
493,744 B with (+1,124 B, 30,544 B free). CMake stops a build with the
mirror and the UART on GP2/GP3.

**Found on the board, 2026-11-02 (the owner):** the first build gave a
black screen and no shell. `lcd_init` called `mirror_init()`, which takes
the first free DMA channel (0), before it claimed channel 0 for the panel
itself, and claiming a channel twice stops the program. Fixed by calling
`mirror_init()` after the panel's claim. Not reproduced here: the probe was
not connected, and no test on the host reaches `lcd.c`.

**Checked on the board, 2026-11-02 (the owner):** the HDMI mirror shows
the screen. The owner reported no more than that it works: the list below
was not confirmed one by one.

**Not confirmed.** The picture and colours against
the panel, scrolling past the frame memory's wrap, vi, the FPGA powered
after the PicoCalc and `hdmi`, the cost of a redraw with the mirror (the
owner's figures, +11.7% and +44%, are from its own starter), `usbdisk`,
`web` and a program run and back, and the W and DOS builds coming up.
`linktest`, `lcdbench` and `hdmipat` of the owner's project were not
brought over. The mirror's one-packet-per-row copy is as the starter's;
a packet for a whole blit has not been tried.

## M14 — A Bluetooth keyboard (2026-10-30, branch `m14-bluetooth`)

**Done 2026-10-30 for a classic keyboard in the W builds** (design.md
§3.10). A keyboard paired with picocalc-extras' `btpair` types in the
shell next to the PicoCalc's own, at the prompt and in everything that
reads keys. The radio starts after the first prompt when
`/bluetooth/pairing.dat` is on the card; its firmware is read from
`/bluetooth/cyw43-fw.bin`, not held in RAM; `bt` and `info` say what it is
doing.

**Checked on the host.** `test_btstore` (the pairing file's records,
format byte for byte, the older file, files that are not it, the card
failing at each of 40 points of a save, no card), `test_vi_compact` and
`test_edit_compact` (the W builds' 32 KB buffers, which found a 65536 written
into a test), and `bt` in both flavours in `test_ntp` / `test_ntp_dos`. 33
tests in all.

**Checked on the board, by the owner,** on a Pico 2 W, the development
build, UNIX flavour, with a card set aside:

- *Step 2*, `cyw43fw-test.uf2`: the radio comes up with the firmware read
  from the card (3,613 reads of 64 bytes, 906 ms); the first run, which
  closed the file after `cyw43_arch_init()`, never brought the chip up,
  because the driver loads the firmware at the first thing done to it.
- *Step 3*, `btkbd-test.uf2`: radio up in 1,027 ms; the paired keyboard
  connects and its keys come through; BTstack's `malloc` is 3,256 B once
  started and 7,248 B connected, not growing with the keys.
- *Step 4*, the shell: the keyboard types at the prompt; with the
  firmware file missing, the line `bluetooth: <why>` and the PicoCalc's
  keyboard going on; the keyboard usable about 1 s after the prompt after
  `reboot`, 2-3 s after coming back from `web`; `-Os` not noticeably slower.
- `usbdisk`, 8 MB each way to a Mac (`tools/usb-rate.py`): with the radio
  on, 660 KB/s written and 497 read; without, 663 and 612; both read back
  byte for byte. *The owner decided to leave the radio on in `usbdisk`.*

**Measurements** (link maps, development builds; estimates replaced): the
probe of step 1 put Bluetooth at 879 KB in the W shell; as built, the
firmware read from the card, `-Os` (the panel's and card's drivers stay at
`-O3`) and vi at 32 KB give the UNIX W shell 495,424 B, 28,864 free; DOS
W 484,728 B, 39,560 free; the release build 492,484 B, 31,804 free
(against 524,288). The Pico 2 build is 454,416 B, as before.

**The owner chose:** Bluetooth in the existing W builds, not a third;
BLE kept in the build; the firmware from the card; vi at 32 KB in the W
builds; the BTstack licence no obstacle; the radio left on in `usbdisk`;
a failed start to say one line and go on.

**The owner checked on a Pico 2 W (2026-10-30):** vi edited with the
Bluetooth keyboard, and the keyboard woke from standby and reconnected.

**Not checked.** BLE keyboards and mice in the shell; for how long a
sleeping keyboard stays connected; the shell with no pairing not starting the radio
(only seen from the log, not the board); a Pico 2 with a W build and a
`pairing.dat` (the pins of GP23-25 and GP29); the battery's cost with
the radio on; the DOS flavour on the board; the release build; a keyboard
paired by a `btpair` other than the owner's.

## M13 — A web file manager, steps 1-3 (2026-10-30, branch `m13-web`)

**Steps 1-3 done 2026-10-30; step 4, installing a program from the page,
is on hold at the owner's request** (design.md §3.9). `web.uf2`, a RAM
program run by the W builds' `web` (`WEB`), joins the network
`/ntp/ntp.cfg` names and serves the card to a browser at its address and
at `picocalc.local`: list, upload, download, rename, move, delete and make
folders, with PicoMultiLoader's page and API.

**Checked on the host.** `test_web` (requests, UTF-8 and CP437 names, JSON,
times, the card's operations over FAT16 and FAT32, a card failing during an
upload) and `test_httpd` (the HTTP connection driven as a browser would,
in pieces of 1 byte to 100 KB, uploads of 0 to 100,000 bytes, a card that
fails, a long listing), and `web` in `test_ntp`. Mutations to the file-send
failure, the upload's undo and the listing's separator were seen to fail.

**Checked on the board, by the owner,** on a Pico 2 W: the server joined
the network, and from a browser listing, upload, download, rename, make
folder and delete worked. Not recorded: the speeds, a file of several MB,
a second client.

**Measurements.** `web.uf2` 502,832 B of RAM (95.9%) in the development
build, 493,184 B in the release build, from the link map (before step 2's
`-Os`, which has since made it 458,036 B).

**Not checked.** Install from the page (not built); speed up and down
against `usbdisk`'s; a file of several MB compared byte for byte; a
dropped connection leaving nothing behind (done on the host only);
`picocalc.local` from a phone; several connections at once; the DOS
flavour's `WEB` on the board.

## M7 — Shell at power-on (2026-10-25, branch `m7-boot-shell`)

**Done 2026-10-25.** UF2 Loader 2.5's stage3 with a patch: power-on
gives the shell and Up/F1/F5 the program in flash; a command is read
first and not kept waiting for a key; a reboot with no command restarts
the program, with a 150 ms watch for a held key; no card starts the
program (design.md §10, M7). pelrun's loader is a submodule at v2.5 in
`third_party/uf2loader`; `patches/uf2loader-boot-shell.patch` changes
`stage3/stage3.c`, the card's polling in `common/petit_fatfs/diskio.c`
and the version (`2.5-shell`); `tools/build-loader.sh`
builds it, once for each size of flash: 4, 8 and 16 MB (the owner,
2026-10-25). The shell sends `BOOT_DEFAULT` to start a program and knows
`2.5-shell`.

**The owner chose**, during the milestone:

- the shell does not load RAM UF2s itself or stay resident across one;
  the patch takes the 0.5 s wait out instead;
- a program can restart itself, and a restart loop is broken with Up
  held or a power cycle;
- the commands in the watchdog's scratch registers stay stock 2.5's, so
  the shell and programs run under either loader.

**Checked on the host:** all 28 tests pass; nothing in the host build
changed. stage3 is 7,092 B of its 8 KB of flash (stock 6,740 B) and
7,792 B of its 8 KB of RAM, as stock (SDK 2.3.1, toolchain 15.2). Every
firmware build compiles clean, with the new `restart-test` and
`restart-ram`.

**Checked on the board:**

- Claude, on the Plus 2 W on the probe (RP2350B), 2026-10-25. Its flash
  held a copy of `exit-test` at offset 0, not the loader; the first
  512 KB was saved (`out/m7-plus2w-flash-before-loader.bin`) and the
  patched stage3 written at offset 0 over SWD, with `exit-test` in the
  app partition. POWMAN's `CHIP_RESET` was found useless for telling a
  power-on from a reboot, and the patch changed to the watchdog's
  `REASON` (HW §8.4). OpenOCD's reset gave the shell; a watchdog
  reboot with no command, `exit-test`; `exit-test`'s exit, the shell.
  The trips were timed under both loaders (below). The card's shell was
  replaced over `USBDISK` with this branch's DOS W development build
  (the old one in `out/m7-card-BOOT2350-before.uf2`), which runs
  programs under `2.5-shell`; `AUTOEXEC.BAT` ran `NTP` through
  `BOOT_RAM` and came back; `restart-test` restarted itself five times
  and OpenOCD's reset broke the loop (`out/m7-*.log`, `out/m7-trips-*.txt`).
- The owner, on the same Plus 2 W, 2026-10-25: power-on with no key
  gave the shell; Up held at power-on gave the program (`restart-test`);
  Up held while `restart-test` looped gave the shell; the power key
  switched it off during the loop, and power-on gave the shell.
- Claude, on the same Plus 2 W, 2026-10-25: `restart-ram`, a RAM
  program, restarted itself through `BOOT_RAM` with the 8.3 path it read
  from the card (`/PICO2~13/RESTA~89.UF2`) about every 1.9 s, and
  OpenOCD's reset broke the loop to the shell
  (`out/m7-restart-ram-*.log`).
- The owner, on the same Plus 2 W, 2026-10-25: the power key during
  `restart-ram`'s loop, and power-on, gave the shell.
- The owner, on the Plus 2 W, 2026-10-25, with the final patch (the
  card polled without sleeping): power-on gave the shell, and Up held
  at power-on the program, as before the polling change.
- The owner, on a Pico Plus 2, 2026-10-25: the patched loader built
  with `PICO_BOARD=pimoroni_pico_plus2_rp2350` (a 16,372 KB app
  partition), installed in BOOTSEL, worked as expected.
- The owner, on the Pico Plus 2 and the Plus 2 W, 2026-10-25:
  `bootloader_rp2350_16mb.uf2` installed in BOOTSEL, with this branch's
  release shell, worked on both.
- The owner, on the Plus 2 and Plus 2 W, 2026-10-25: the board's RESET
  button (the RUN pin), pressed while a program ran, gave the shell with
  no key held and the program with Up held, as a power-on does.
- The owner, on the Plus 2, 2026-10-25: the patched loader (16 MB
  build) with a second card, not the test card, worked. A shell
  built before M7 reported "UF2 Loader 2.5-shell is unknown" in `info`
  and would not run programs: only this branch's `loader.c` knows the
  name, so the patched loader needs a shell from M7 on.

**Measurements** (Plus 2 W, DOS W development build, timed on the Mac
from the reboot or Enter to the next first UART line; three runs each;
the final patch, with the card polled without sleeping):

| Trip | Stock 2.5 | Patched |
|---|---|---|
| `BOOT_DEFAULT` to the program | 0.522 s | 0.021–0.023 s |
| No command (a program's own restart) to the program | 0.522 s | 0.173–0.175 s |
| `BOOT_SD` to the shell's first line (0.43 s of it the shell's start) | 1.545–1.546 s | 0.852–0.853 s |
| `BOOT_RAM` to `restart-ram` (118 KB) | 0.816–0.821 s | 0.227–0.229 s |
| `EXIT-TEST` typed, Enter to the program, repeat run | M2: ~0.9 s | 0.139 s |

The polling change alone is about 0.19 s of the `BOOT_SD` row (1.045 s
with the patch's sleeping polls). Sampling the program counter over SWD
found the wait, but inflated every load it timed (2.4–4.8 s), and the
first exit timings included `exit-test`'s 1.5 s pause: together they
made stage3 look about 2.1 s slow before the cause was found (HW §7.1).

**Not checked:**

- the patched loader's polling on cards other than the test card and
  the owner's second card (its make and timings not recorded);
- the 4 and 8 MB builds on a board (`bootloader_rp2350_4mb.uf2` and
  `_8mb`, each the Pico 2's board with only the flash size changed;
  checked only by their partition tables, the flash size the C code
  gets, and the 4 MB build differing from the tested `pico2` build by
  its board name); no 8 MB board has run one;
- `REASON` read directly after a real power-on or a reset by RUN (read
  only after OpenOCD's reset; both behave as zero, below);
- a Pico 2 W with the patched loader; the Plus 2 W's timings above are
  from the `pico2` build, with a 4,088 KB partition, as its stock loader
  had;
- a program started from `/etc/profile` or `AUTOEXEC.BAT` that loops
  through the shell (not M7's).

## M12 — Date and time (2026-10-22 to 2026-10-24, branch `m12-time`)

**Done 2026-10-24.** Both flavours keep the date and time in the
RP2350's AON timer, with its record in POWMAN scratch 0-2; read it from a
DS3231 at power-on; set it by hand (`date`, `DATE`, `TIME`) or, in the
W build, by NTP through `ntp.uf2`; stamp files with it; and hand it, with
the local offset by `TZ`, to programs through `picocalc_shell.h`
(design.md §3.8). Five steps, in four commits: steps 1 and 2 (aa322e6),
step 3 (8c203eb), step 4 (f9e3e5d), and step 5.

**The owner chose**, during the milestone:

- `ntp` finds `ntp.uf2` through `PATH`, as any program is found, the DOS
  flavour's current directory first (2026-10-22);
- `ntp` is silent when it set the clock, and says why when it did not
  (2026-10-23): first made silent in both cases, then corrected;
- the AON timer's drift against NTP is not measured: the clock is for
  general use (2026-10-23);
- a RAM program returning to the shell at power-on cannot be done by the
  shell with the stock loader, since power-off clears every scratch
  register and stage3 clears its command as it reads it; noted under M7
  (2026-10-23).

**Checked on the host:** five new tests. `test_clock` (the UNIX flavour)
and `test_dos_time` (the DOS flavour): the calendar, `TZ` across both
changes of daylight saving and east of UTC, `date` and `DATE`/`TIME` in
each form and their mistakes, `COUNTRY=`'s orders and separators,
`ls -l`'s six-month rule, the times each command writes over FAT16 and
FAT32, `COPY` keeping the source's date, the clock not set, the
DS3231's note said once, `AUTOEXEC.BAT`'s ask-only-when-needed script
(now checked to say "not set" once), and the offset given to a program
when the shell is left. `test_ds3231`: the chip present, absent, stuck
and with `OSF` set, its BCD in either hour form and century, written when
the time is set and at the next start after NTP or a program set it,
NTP's result in the record, and `picocalc_shell.h`'s two functions built
against the model of POWMAN: a program's view of the time and offset, a
program's time taken by the shell, set after a power-on with no record,
and a broken record refused. `test_ntp` and `test_ntp_dos`: `ntp.cfg`'s
parser and the builtin's once-per-power-on rule with a fake record and
loader, the `PATH` lookup, redirection refused, and a program that never
reports. All 28 host tests pass. Every firmware build compiles clean:
UNIX and DOS, Pico 2 and W, development and release, with `exit-test`
(which checks the header still builds without `hardware_powman`),
`clock-test`, `rtc-stop` and `ntp`.

**Checked on the board** (HW §8.4-8.6 hold each in full):

- Claude, on the Plus 2 W on the probe (RP2350B), 2026-10-22: the
  development UART moved to GP2/GP3 and the probe rewired; the clock kept
  through stage3's `BOOT_SD`, a program round trip and `REBOOT`; the
  DS3231 written and read back over SWD runs; NTP setting the clock
  through `ntp.uf2` and its failures with no file and no network
  (`out/m12-*.log`).
- The owner, on the Plus 2 W: the DS3231 keeping the time through a power
  cycle, and through `POWEROFF` and a power-on with Up held; written from
  NTP's time; a wrong Wi-Fi password reported; `clock-test` showing the
  shell's time and offset, and the shell the minute it added; a file's
  time right in `DIR` and in `ls -l`, and on a Mac; `/etc/profile` with
  `export TZ=EST5EDT` and `ntp` starting without a loop.
- The owner, on a Pico 2 W with no DS3231, release build: NTP setting
  the clock with `ntp.uf2` found through `PATH`; setting the time by hand
  returning at once.
- The owner, on a Pimoroni Pico Plus 2 (no radio) with the chip sealed in
  the case: the W build's `NTP` in `AUTOEXEC.BAT` failing cleanly at
  power-on, without a loop; the time set by hand kept through a power
  cycle; and, after `rtc-stop` stopped the chip at power-off,
  `AUTOEXEC.BAT` asking for the date and time at power-on and not again
  after a program.

**Found on the board, and fixed:** the first join reported "no network"
with the network in range; `ntp.uf2` now asks again until its timeout,
as the SDK's own join does. Setting the time on a board with no DS3231
paused up to a second, waiting for the chip's next second; it now writes
the nearest second at once. `AUTOEXEC.BAT`'s example said "Current date
is not set" twice; the guide's example now sends `DATE /T` to `NUL`.

**Measured:**

- Each watchdog reboot loses 2.0 ms (±0.5) of the AON timer, so a
  program round trip loses 4 ms (Plus 2 W, DOS W development build,
  2026-10-24, read over SWD against the Mac's clock; HW §8.4). The design
  asked for the loss per trip; the earlier runs showed only that it was
  under a second.
- NTP: 7.6 s in `ntp.uf2` and 13.8 s from Enter to the prompt, against
  the estimate of 5-10 s; the clock within 1 ms of the Mac's after it
  (Plus 2 W, HW §8.6).
- `ntp.uf2`: 390,396 B text plus bss in the release build, against the
  estimate of about 340 KB, within stage3's 512 KB; 386,376 B
  (`__end__`, 2026-10-24) once it stopped linking `loader.c`.
- The shell, release builds, `__end__`, 2026-10-24: the UNIX flavour
  447,812 B (Pico 2) and 450,340 B (W); the DOS flavour 431,080 B and
  433,600 B (design.md §7).

**Not checked:**

- a Pico 2 (RP2350A) with the W build: the owner has none;
- WPA3-only networks, which the WPA2-AES join does not support;
- the cause of the 2 ms per reboot; the time lost per power cycle with a
  DS3231, below a second; the module's own pull-ups; the AON timer's and
  the chip's drift (dropped by the owner);
- a program calling `picocalc_shell_set_time()` after a power-on that
  started it without the shell: on the host only.

**Open:** the CYW43 driver's licence against GPL-3.0 (THIRD-PARTY.md)
blocks releasing `ntp.uf2` until the owner decides. *Settled
2026-10-24:* a GPL-3.0 §7 permission on the owner's code (design.md §9),
with `ntp.uf2` no longer linking `src/port/loader.c`; both W builds
built clean, and its ELF has no symbol from `loader.c` or `core/uf2.c`.
Claude's run of the development `ntp.elf` from SRAM over SWD on the
Plus 2 W (2026-10-24): NTP set the clock in 5.7 s, and its `BOOT_SD`
brought up the card's shell.

## M11 — Menus (2026-10-18, branch `m11-menus`)

**Done 2026-10-18.** The owner tested it on a Pico 2 W, with both
flavours' release builds, and reports that it works as expected. Which
of design.md §10's checks were run one by one was not recorded; this is
the run that covers a program started from a menu and the
return to the shell, which the Plus 2 W could not do without its loader
(below).

**The owner chose** (2026-10-18) bash's `select` with `break` for the
UNIX flavour, and DOS 6's `CHOICE` with all its switches for the DOS
flavour (design.md, the decision table; §4.4, §4.7, §10). `select` is the
UNIX flavour's one compound command; the parser now records each
command's place in its line (`parse_cmd_t.start`, `end`) so `unix.c` can
cut a line at `select`, `do` and `done`. `shell_hooks_t` has a new
`pollkey`, for `CHOICE /T`, in the port, the host shell and the tests.

**Checked on the host:** the new `test_select` covers one line and
several in a script, a select inside another, `break`, `break 2`,
`break 9` and `break` outside one, an empty line, a wrong number, `PS3`
and `REPLY`, a glob and a variable in the words, no words, the menu's
columns, the end of the input, Ctrl-C, Esc and Break, sixteen
mistakes, a quoted `select`, a mistake inside a script's select, a
script with no `done`, `break` not reaching out of `sh`, and no
keyboard. `test_dos` covers `CHOICE`'s default keys, `/C` with and without
`:`, quoted text, `/N`, `/S`, a key not among them, `/T` running out on
a fake clock and not restarted by a wrong key, `/T` with a key and with 0
seconds, ten mistakes, Esc and the end of input, and a menu batch file
with `IF ERRORLEVEL` and `GOTO`, over FAT16 and FAT32. `test_system`'s
`man` columns take `break` in. All 23 host tests pass. The development,
release and DOS release builds compile clean. The guides' examples were
run in the host shells.

**Checked on the board:** Claude, on the Plus 2 W on the probe (RP2350B
rev 2, id 7458DC82A89AAC12), both flavours' development builds run from
SRAM over SWD (`tools/swd-ram.sh`), keys typed over the UART, on its test
card; the log is `out/m11-unix.log`, the screen read with
`tools/uart-screen.sh`. UNIX: a one-line menu running `pwd`, a wrong
number, an empty line, `break`, `$?`, `$REPLY`; Ctrl-C (status 130);
a menu over several lines in `/etc/profile` running as the shell
started and leaving for the prompt on `break`; a program in a menu
refused (`cannot run programs: no UF2 Loader partition table`) and the
menu going on. DOS: a menu in `AUTOEXEC.BAT` (`CHOICE /C:12Q /T:Q,5`,
`IF ERRORLEVEL`, `GOTO`) at the start, `2` and `1` chosen, then `Q`
taken by `/T` with nothing pressed; a key not among them sending BEL;
Esc stopping `CHOICE /T:N,30`. `/etc` and the scratch directory were
removed afterwards. `AUTOEXEC.BAT`, which typed as one blank line,
was overwritten for the test and put back as one line break (1 byte);
its exact bytes before were not recorded.

**Found on the board, and fixed:** a menu row exactly 40 columns wide
(the twelve-word case) was followed by a blank row on the panel, though
not in the UART's copy: the terminal wraps as soon as a row is full, so
the line break started another. Menu rows now stop short of the 40th
column (`test_select` checks it); seen right on the panel after.
**Found on the board, and fixed after:** `ls` did the same for a row
whose last column is its widest (six 12-character names gave a blank row
after each row on the panel), and Tab's listing (`line.c`) used the same
rule. Both now stop short of the 40th column; `test_line` checks both on
the terminal's grid with those six names, and failed before the fix.
On the panel (the same board and build, the probe back, log
`out/m11-ls.log`), `ls` and Tab's listing of those names show two
columns with no blank row.
`DIR /W` (rows of at most 39) and `man`/`HELP` (at most 38) were already
short of it.

**Measured** (release builds, `__end__`, against main built the same
day): the UNIX flavour 432,068 B, 13,092 B more; the DOS flavour
409,244 B, 1,552 B more (design.md §7). `CHOICE /T:Q,5`'s default came
4.61 s after the last key typed, timed on the Mac; the count starts at
the prompt, a little after that key, so it agrees with 5 s by eye, not
measured from the board's clock.

**Not checked:**

- On the Plus 2 W: a program run from a menu and coming back, and a menu
  at a real power-on through stage3. It was found with no UF2 Loader in
  flash (`info`: `loader none in flash`), so programs cannot run on it
  and power-on does not reach the card. The owner's Pico 2 W run is
  taken for these (above).
- Claude's runs used only the development builds; the release builds
  ran in the owner's run alone.

## After M9 — one font, 40 columns, in the UNIX flavour (2026-10-18)

**The owner chose** (2026-10-18) the Master 512's font as the UNIX
flavour's only font (design.md, the decision table; §4.1). `font` is
gone, the terminal ignores mode 4264 in both flavours, and the grid, the
panel's shadow and vi's rows are 40 columns wide: `TERM_COLS` is the only
width, and `term_t.cols`, `shell_t.wide` and `shell_cols()` are gone. The
owner's `font-8x10.c` and `font-5x10.c` are out of the build. The Master
512's font, supplied in code page 437 order, was arranged as the
terminal's 128 glyphs with the DEC graphics at 0x00-0x1F, as IBM's is.

**Checked on the host:** `test_screen` checks every pixel against the
Master 512's font; `test_skeleton` its width; `test_shell` that `font`
is not found. The 64-column cases in `test_term`, `test_text`,
`test_files`, `test_vi`, `test_system` and `test_edit` are gone. All 22
host tests pass. The development, release and DOS builds compile clean.

**Measured** (release builds, `__end__`): the UNIX flavour 418,984 B,
13,868 B less than before; the DOS flavour 407,700 B, 268 B less
(design.md §7).

**Not checked:**

- On a board: none of it. To do, on the panel: the Master 512's glyphs,
  bold, the checkerboard for a byte above 0x7F, and line drawing
  (`echo` of `ESC ( 0 lqk ESC ( B`) joining across rows.

## After M9 — exit without a loader (2026-10-17, branch `exit-no-loader`)

`picocalc_shell_exit()` returns false when UF2 Loader did not start the
program, as well as with no card, and `picocalc_shell_loader_present()`
is new (design.md §3.4). `exit-test` reports it on the panel.

**Checked on the host:** the development, release and DOS builds, and
`exit-test`, compile clean. All 22 host tests pass (the header has no
host test: it is SDK-only).

**Not checked:**

- On a board: none of it. No Debug Probe was attached. To do on the
  Plus 2 W: `exit-test` from the card under the loader says "yes", and a
  key returns to the shell; copied on in BOOTSEL with no loader (which
  needs UF2 Loader put back after), it says "no", and a key gets false.
  That `ATRANS0` reads nonzero under stage3 is from the loader's source,
  not yet read back (hardware-notes.md §7.2).

## After M9 — the DOS flavour in 40 columns only (2026-10-15)

**The owner chose** (2026-10-15) to drop 64 columns from the DOS
flavour, so it always shows IBM's font (design.md, the decision table;
§4.7). `FONT` is gone, the terminal ignores mode 4264 in this flavour,
and everything sized for 64 columns is sized for 40.

**Checked on the host:** `test_edit` checks that `FONT 5x10` is not a
command and that `ECHO`ing the 64-column escape leaves 40 columns;
`test_dos` lost its case for `DIR` in 64 columns. All 23 host tests
pass. The UNIX flavour's tests are unchanged.

**Measured** (release builds, `__end__`): the DOS flavour 407,416 B,
15,712 B less than before; the UNIX flavour unchanged (design.md §7).

**Checked on the board:** the Plus 2 W on the probe, the DOS flavour's
development build from SRAM, driven by Claude over UART1
(`out/dos40.log`): `FONT 5x10` gave `Bad command or file name`, the
grid stayed 40 x 32, and EDIT's Help menu fitted at the right edge and
left back to `C:\>`. The `DIR` listing was lost to noise in the capture.

**Checked by the owner** (2026-10-15, on their Pico 2 W, the DOS
flavour's release build, on the panel): the `DIR` listing is right in
40 columns, `FONT` is not a command, and EDIT's boxes and menus, in
IBM's line glyphs, join up.

**Not checked:** nothing carried forward.

## After M9 — EDIT in 40 columns (2026-10-15)

EDIT runs in the shell's font instead of switching to 64 columns
(design.md §4.7, *In 40 columns*; the decision table, 2026-10-15).

**Checked on the host:** `test_edit` now runs in 40 columns by default
and checks 64 after `FONT 5x10`, staying 64 after; a 46-column line
scrolled to its end; the keys help in 40; the read-only reason wrapped
in its box; the ruler hidden by a message too long for both. All 23
host tests pass over FAT16 and FAT32.

**Checked on the board:** the Plus 2 W on the probe, the DOS flavour's
development build from SRAM (`tools/swd-ram.sh`), driven by Claude over
UART1 (`out/edit40.log`, `out/edit40b.log`): `EDIT T40.TXT`, a 60-column
line typed and scrolled sideways, F1's box, Options Display's question,
then Brk and No, back at `C:\>` in 40 columns. The screen was read
from the grid, not seen on the panel.

**Not checked:** nothing carried forward; the owner's run of the
40-column-only release build (above) covered the panel, IBM's line
glyphs in EDIT's boxes, and a Pico 2 W.

## M9 — the DOS flavour, step 3, and M9 done (branch `m9-dos`, 2026-10-14)

**Done.** Step 3 of three is built: the Alt chords measured and
written in HW §6.3, then EDIT (design.md §4.7, *As built at M9,
step 3*; `src/core/edit.c`). M9's done-when is met: copying, renaming
and deleting on the Plus 2 W (step 1), and on the owner's Pico 2 W
running a program by name and coming back, `AUTOEXEC.BAT` at a real
power-on, and EDIT's change to `/atom/pico-atom.cfg` read by pico-atom
(below); both flavours' RAM and EDIT's opening time are measured.

**The chords** (the Plus 2 W on the probe, the DOS flavour's
development build from SRAM, the owner pressing keys on the panel while
Claude captured UART1 with the new RS logging, `out/m9-keys-1.log`, 495
events): a tap of Alt is a press and a release with no hold between;
Alt with a letter sends the capital, Alt+I Insert, Alt with an arrow
the arrow; Ctrl with X, C and V the lower-case letter; no overflow and
no I²C error. Alt+C and Alt+2 sent nothing and some keys under Alt had
no release, once, unexplained. Shift with the arrows was not captured;
the MCU's source swallows it, so **the owner chose** (2026-10-14) Alt
with the arrows to select and Ctrl+X, C and V to cut, copy and paste,
in place of the design's Shift keys.

**Checked on the host:**

- All 23 test binaries pass; the UNIX flavour's unchanged, vi's
  included, after its file code moved to `editor_file.c`.
- **`test_keys`**: Alt not reported at the prompt (Alt+F is `F`, no
  tap); reported, a tap, Alt+F, Alt+Left, Alt+I still Insert, Alt+F3
  still F3, no tap after a key or once held, Ctrl+Right and Ctrl+Home,
  Ctrl+X still `^X`, Ctrl+Left plain at the prompt; the UART's US and
  US US.
- **`test_edit`**, new, over FAT16 and FAT32: the screen (menu bar,
  title, text, status, 64 columns, the cursor, 40 columns after);
  typing and Save; the menus by a tap, across and down, Enter, Esc,
  Alt and a letter; Help and F1; Find's box; Exit asking, No, Cancel,
  Yes (Enter), the end of input not saving; Alt with the arrows
  selecting, Ctrl+X and Ctrl+V, Ctrl+C, Alt+Down and Del, typing over
  a selection, the Edit menu's Cut and Paste; Ctrl+Y in the middle and
  on the last line, and on an empty file changing nothing; Ctrl with the
  arrows by words, Ctrl+Home and End; the goal column past a short line
  and onto a tab; Enter's indentation; overtype and `OVR`; Find in
  either case, F3, not found; the done-when edit of a CR LF settings
  file, kept CR LF; Change of three, to nothing, Esc; UNTITLED saved by
  name, Save As to another directory, Save As over a file asking, Open,
  a failed Open keeping the text, New asking to save, a new file by
  name; a read-only file shown and not saved; a directory, two names
  and a file over 64 KB refused; chords off after EDIT.
- **Found by the owner** on the panel: the menu bar and menus were
  black, so their black letters could not be seen. The terminal's
  256-colour table, from the starter, was wrong: its grey ramp climbed
  to white at 247 and fell back to black at 248, the bar's grey, and its
  colour cube was wrong too. `term.c` now works colours 16-255 out by
  xterm's formula; `test_term` checks the ramp and the cube, and
  `test_edit` the bar's cells (black on 168 grey, letters white) and the
  text's blue. With the old table both fail. After the fix the owner
  saw the menu bar and the File menu drawn correctly on the panel (the
  Plus 2 W on the probe, the DOS flavour's development build from SRAM,
  2026-10-14).
- **Found by the owner**: DIR's lines were 23 columns, so a long name
  pushed its size past the end of the line. The size is now at the
  right of the screen, short of the last column (39 or 63), and a name
  too long to leave room puts its size on the next line. `test_dos`
  checks the lines in 40 columns, a long name, one too long, and one in
  64 columns. On the Plus 2 W (the DOS development build from SRAM,
  2026-10-14), `DIR \LUA\LUA` fitted every line, `asteroids.lua` and
  `mandelbrot.lua` included. Then, at the owner's asking, as DOS showed
  them: `<DIR>` in its own column, 15, apart from the sizes, and sizes
  and totals with a comma every three digits (`1,234,566`); the totals'
  field grew to 14. `test_dos` checks both, a long directory's name,
  and one too long for `<DIR>`; `DIR \` on the Plus 2 W fitted every
  line (`20,971,520`, `31,766,970,368 bytes free`). Last, at the
  owner's asking, the lines keep to 40 columns in the 64-column font
  too, so the sizes stay near their names: checked by `test_dos` and,
  with `FONT 5x10`, by `DIR \LUA\LUA` on the Plus 2 W. The two totals
  lines' numbers end in one column, so `bytes` lines up: `test_dos`
  checks the columns (a planted misalignment failed 4 checks), and
  `DIR \` on the Plus 2 W showed them lined up.
- **The DOS flavour's font**, at the owner's asking: IBM's PC BIOS 8x8
  from VileR's font pack (CC BY-SA 4.0), drawn in the 8x10 cell by
  `tools/mkfont-ibm.py`, for 40 columns only. The link maps show each
  flavour links only its own 40-column font; RAM (release, `__end__`):
  DOS 422,196 B (+96), UNIX 434,364 B (-344, the 256-colour table now
  code). Shown on the Plus 2 W from SRAM with `DIR \LUA\LUA`; the owner
  saw it on the panel and approved it.
- **Planted bugs**, each caught: a splice of nothing marking the file
  changed (2 failures), Alt with an arrow restarting the selection (2),
  Exit not asking (too many to list), a held Alt counted as a tap (1).

**Checked on the board:** the Plus 2 W on the probe, by Claude, the DOS
flavour's development build from SRAM over SWD (`tools/swd-ram.sh`),
on the test card, typed over UART1 with US for Alt
(`out/m9-keys-1.log`): EDIT typed and saved a file; a tap of Alt opened
the File menu, drawn with its box; Alt+Right five times and Ctrl+X cut
five letters; Brk asked to save and No left the file as it was; a new
file typed, saved, a Change and Exit with Yes, then TYPE showed it. A
41,573-byte file (three Lua files joined with TYPE `>>`) opened. The
card's own files were only read; `\EDTEST` and the two files made in
the root were removed after.

**Measured:**

| | Estimate | Measured |
|---|---|---|
| EDIT opening a 41 KB file | under a second, as vi (M3) | 0.247 s, 0.247 s, 0.248 s from the Enter to the status row on the UART (Plus 2 W, development build, 2026-10-14). About 0.245 s of it is the 2,823 bytes mirrored at 115,200 baud, so the release build is faster; not timed there |
| The DOS flavour's RAM (release, `__end__`) | — | 422,100 B, 100,872 B more than step 2: EDIT's 64 KB buffer, 8 KB clipboard, two 4 KB cell screens, and its code |
| The UNIX flavour's RAM (release) | unchanged | 434,708 B, 772 B less than step 2 |

**Checked by the owner on a Pico 2 W** (2026-10-14, the DOS flavour
from the card as `/BOOT2350.UF2` in place of the UNIX one, started by
UF2 Loader after the UNIX flavour's `reboot`): EDIT changed
`/atom/pico-atom.cfg` and pico-atom showed the change; in EDIT, Alt with
the arrows selected, copy and paste worked, Ctrl with the arrows moved
by words, Home and End worked, and Ctrl+Home and Ctrl+End went to the
top and the bottom of the file; pico-atom was run from the DOS prompt
by name, and the shell came back by F1 held at power-on; `@` at the
start of a line in a batch file hid that line; and `C:\AUTOEXEC.BAT`
ran at a real power-on, its `@` line hidden. (A first `autoexec.bat`
that did not run at start was in `C:\HOME`, not the root, so the shell
was right not to run it: design.md §4.7 names only `C:\AUTOEXEC.BAT`.)
Later the same day, also on the Pico 2 W: Alt+`,` and Alt+`.` (the
backlights) did not open EDIT's menu; EDIT opened a 40 KB file at once,
by eye; and Options, Display's phosphor colours worked. At the owner's
asking, a question's buttons now show each one's letter in red, as
MS-DOS's EDIT did, and the keys are taken from those letters, so C is
Cancel too; `test_edit` checks the colours and C, and the owner saw the
red letters on the Plus 2 W's panel.

**From the review of PR #8** (2026-10-14): a test's 64-byte buffer cut
DIR /P's expected text at 63 characters, so on macOS the check passed
on a prefix and on Linux GCC refused to build it; the buffer is 128 B
and the whole text is checked. Every host source was then checked with
GCC's `-Wformat-truncation` (the Arm toolchain's, `-fsyntax-only`, for
want of a host GCC); nothing else was cut. `EXIT > NUL`, and any
program or command that leaves the shell with `> NUL`, was refused as
redirected; NUL leaves no file, so it is now dropped for them, and a
redirection to a file is still refused. A quoted command name, `"DIR"`,
stays `Bad command or file name`, as in DOS 5. A lowercase
`autoexec.bat` at the root of the Plus 2 W's test card does run when
the shell is started from SRAM over SWD (`out/m9-autoexec-1.log`).

**Not checked:**

- All of steps 1's and 2's lists, which stand.
- EDIT's opening time measured in the release build (by eye only:
  at once), and EDIT's deepest stack.
- Each of EDIT's boxes and dialogs on the panel, one by one.

## M9 — the DOS flavour, step 2 (branch `m9-dos`, 2026-10-14)

**In progress.** Step 2 of three is built: batch files and
`AUTOEXEC.BAT` (design.md §4.7, *As built at M9, step 2*;
`src/core/dos_batch.c`). EDIT (step 3) is to come, and M9's done-when
checks on the board with it.

**Checked on the host** (the same tools as step 1):

- All 22 test binaries pass; the UNIX flavour's unchanged.
- **`test_dos`**, new, over FAT16 and FAT32: `%0`–`%3` with `,` between
  words and a missing one empty, `%%`, quotes kept in a parameter, `%1`
  left alone at the prompt; `@ECHO OFF` and ECHO back on after; REM with
  a `>` making no file; ECHO ON showing each line after the prompt, a
  label not shown, `@` hiding one; a SHIFT loop over eleven words with
  GOTO `:label` in another case and a label with words after it; a
  missing label; GOTO at the prompt; IF EXIST of a file, a directory, a
  wildcard with and without a match, IF NOT EXIST, ERRORLEVEL after a
  failed TYPE (and not after a false IF), `a == b` with blanks, quoted
  words, a case difference, IF after IF; IF's four syntax errors, and IF
  after a `|`; FOR at the prompt with `%x` (shown with ECHO ON, hidden by
  `@`) and in a file with `%%x`, over a wildcard with a path, a hidden
  file left out, nested, an empty set, four syntax errors; CALL coming
  back, a batch file without CALL taking the caller's place, and from
  inside a FOR; CALL of a builtin, CALL with `>`; a batch file with `>`
  or in a pipe refused; a file running itself six times without CALL;
  CALL nested past three; a batch file ahead of a program here, on PATH,
  by path and as `NAME.BAT`, and not as `NAME.TXT`; a 300-character line
  and the line after it; `AUTOEXEC.BAT` at start and its absence;
  Break stopping an endless GOTO loop and a FOR.
- **Planted bugs**, each caught: ECHO not put back after a batch file
  (8 failures), a batch file run from another coming back instead of
  taking its place (8), IF NOT inverted (6).

**Checked on the board:** the Plus 2 W on the probe, by Claude, the DOS
flavour's release build run from SRAM over SWD (`tools/swd-ram.sh`),
on the test card, typed with `tools/swd-drive.py`: a batch file written
line by line with `ECHO … >>` in a new `\DOSTEST`, then run as `T a,b c`,
with SHIFT, GOTO, IF `"%1"==""` and a FOR over `*.BAT`, all as on the
host; an endless `GOTO` loop stopped by Esc (`^C`, the screen still
after); CALL coming back and a second batch file taking the first's
place; `IF EXIST *.BAT`; a `C:\AUTOEXEC.BAT` (the card had none) setting
PROMPT and CD at the next start, with ECHO on after. After the stack
change below: a FOR CALLing a batch file and three IFs on one line.
`AUTOEXEC.BAT` and `\DOSTEST` were removed after. **Found on the board:**
a batch file looping through quick builtins could not be stopped, since
nothing in the loop looked for Break; the batch file and FOR loops now
look before each line and each turn. (It was found because typing more
than the tool's 64-key ring at once mangled the file into a loop: the
tool's limit, not the shell's.)

**Measured** (link map, release builds): the DOS flavour 321,228 B of
RAM (step 1: 301,628 B), 12,416 B of it the four batch levels; the UNIX
flavour unchanged at 435,480 B. `-fstack-usage` first showed about
2.6 KB of stack per CALL level (IF's and FOR's buffers), so about 11 KB
three levels deep with a COPY under them, over the 8 KB stack; with
those buffers made static and IF after IF a loop, about 720 B a level,
and *estimated* 5.8 KB at the deepest. The prompt came up 501 ms after
the SWD reset with a four-line `AUTOEXEC.BAT`, 496 ms without.

**Not checked:**

- All of step 1's list, which stands.
- A batch file that runs a program: it leaves the shell, as a typed
  name does, but no program was run from the SRAM build.
- A program in `AUTOEXEC.BAT` running at every start; the user guide
  says to take the card out to edit it.
- The deepest stack on the board: the 5.8 KB is a sum of
  `-fstack-usage` frames, not a measurement.

## M9 — the DOS flavour, step 1 (branch `m9-dos`, 2026-10-14)

**In progress.** Step 1 of three (design.md §10) is built: the build
option, the DOS flavour's parser, `C:` paths, the prompt and its
commands (design.md §4.7, *As built at M9, step 1*). Batch files and
`AUTOEXEC.BAT` (step 2) and EDIT (step 3) are to come, and M9's
done-when checks on the board with them.

**Checked on the host** (macOS, Apple clang 21, `-Wall -Wextra -Werror`,
C11; firmware with Arm GCC 15.2 and Pico SDK 2.3.1):

- The UNIX flavour's 21 test binaries pass unchanged after `shell.c`'s
  command language moved to `unix.c`; 22 with `test_dos`.
- All four firmware builds compile clean: each flavour, development and
  release. The host builds `shell` and `shell-dos`.
- **`test_dos`**, on its own: `dos_match` (17 patterns: `*.*`, `*.`,
  `?` matching none at a part's end, long names with several dots),
  `dos_rename` (7 masks), `dos_resolve` (`C:\`, `C:X`, `..\`, `/`, other
  drives), `dos_show`, `dos_expand` (`%name%` in either case, `%%`, an
  unset one, a lone `%`, too long), and `dos_parse` (`DIR/W/P`, `cd..`,
  `CD\`, `ECHO.`, a program's name with a `-`, a `.BAT` name not cut,
  quotes in words and redirections, `>>`, pipes with `|` inside quotes,
  blank lines, four syntax errors).
- **`test_dos`** over FAT16 and FAT32 images, each command as typed: the
  banner and `C:\HOME>` as the card spells HOME; CD (alone, `..`, `\`,
  `C:\`, `/HOME`, a missing one); MD over one there; `A:` and `C:`; `Bad
  command or file name`; ECHO with `>`, `>>`, `ECHO.`, and `> NUL`
  making no file; TYPE of a file, a missing one and a directory; COPY of
  one, under a mask, into a directory, onto itself, several into one,
  none, and a directory's files; DIR's header with the label, its lines
  and totals, `/B`, `NAME` as `NAME.*`, `/W`, `/A` with hidden names, `/S`
  and `/S /B` from the root, an unknown switch, a pipe into FIND; `/P`
  over 40 files, going on with Space and stopped by Esc, and not pausing
  into a pipe; REN by mask, onto another file, a change of case, a path
  refused, a directory; ATTRIB `+R`, `-R +H`, showing, hidden files
  included, a bad letter; DEL of a read-only file, `*.*` answered N then
  Y, `/P` answered N and Y, none, a hidden name in full; RD of a full
  directory, an empty one and the current one; XCOPY `/S` with a hidden
  file left out, a cyclic copy, no match, one file; MOVE into a
  directory, a directory renamed, several into a file, a directory into
  itself, a list with commas replacing a file; DELTREE answered N then Y
  with a read-only file inside, `/Y` of a file, the current directory;
  FIND with `/I /N`, `/V /C`, a pipe, `<`, a missing file, nothing to
  find; MORE into a file; SET and `%NAME%`, clearing, a syntax error;
  PATH, `PATH ;`, setting it in capitals; PROMPT's codes, its width for
  the line editor, the default, ECHO OFF hiding it; programs found in
  the current directory, on PATH and by path; VER, VOL, `VOL D:`, MEM
  with and without the port's figures; HELP and `help copy`; PHOSPHOR's
  word in capitals; F3, Esc and Tab (a directory, then a command) at the
  prompt; a COPY whose writes fail part way, leaving the old file.

**Checked on the board:** the Plus 2 W on the probe, by Claude, the DOS
flavour's release build run from SRAM over SWD (`tools/swd-ram.sh`, so
not through stage3), on the test card, driven by `tools/swd-drive.py`:
the banner and `C:\>` (the card has no `HOME`), VER, MEM, VOL (the card's
label `PICOCALC` and serial number), INFO, DIR /W and DIR /B of the root,
`DIR /B | FIND /V "["`, ATTRIB of the root with its hidden `._` files,
`DIR PICO2-APPS /B`; then MD, CD, `ECHO hi > A.TXT`, COPY, `REN *.TXT
*.OLD`, DIR /B and TYPE in a new `\DOSTEST`, removed after with
`DELTREE /Y`. All as on the host. The banner and VER wrapped at 40
columns with a long version string and were shortened.

**Measured** (link map, release builds): the UNIX flavour 435,480 B of
RAM (M8: 432,516 B), the DOS flavour 301,628 B. The DOS commands' deepest
call, COPY into FatFs, uses about 3 KB of the 8 KB stack
(`-fstack-usage`).

**Not checked:**

- The DOS flavour's `BOOT2350.UF2` from the card through stage3, and so
  running a program by name and coming back (M9's done-when, with steps
  2 and 3).
- Typing on the PicoCalc's own keyboard: F3, Esc and Tab were tested on
  the host; on the board, only keys typed over SWD.
- `FONT`, `USBDISK`, `POWEROFF`, `REBOOT`, `BOOTSEL` and `EXIT` in the
  DOS flavour on the board; they are the UNIX flavour's commands.
- The UNIX flavour on the board after the refactor: its host tests pass
  unchanged, but it was not run there.
- One name in the test card's root shows as blank in both flavours; it
  is not `Icon\r`, and was not identified. `ls` printed it the same way
  before M9.
- A card formatted by another tool for VOL's label and serial number:
  only the test card (formatted on a Mac) and FatFs's own images.

## M8 — the rest of v1 (branch `m8-rest`, 2026-10-14)

**Done.** The twelve commands are built, checked on the host, run by
Claude on the Plus 2 W, and checked by the owner on a Pico 2 W, the
phosphors with their new values included.
`docs/user-guide-unix.md` explains every command for users.

**Checked on the host** (macOS, Apple clang 21, `-Wall -Wextra -Werror`,
C11; firmware with Arm GCC 15.2 and Pico SDK 2.3.1):

- Both firmware builds compile clean, development and release; 21 host
  test binaries pass (M6: 20).
- **`test_text`**, `head`: the default 10; `-n 3`, `-n3` and `-3`; `-n 0`;
  more than there are; no final line break; `==> name <==` headers for
  two files; `<`, a pipe in and a pipe out; a missing file beside a good
  one; the usages (`-n` alone, `-n x`, `-x`); `--`. `tail`: the same
  forms; an empty file; blank lines; a 60 KB file through the 4 KB ring;
  1,000 lines asked of it, giving the 409 whole lines kept and the note;
  a 5,000-byte line, giving its last 4,096 bytes and the note; a
  1,535-byte line whole. `wc`: all three counts; `-l`, `-w`, `-c` and
  `-lc`; a total for two files, and with one missing; `-l <` printed
  alone; `| wc`; an empty file; a word across the 512-byte reads.
- **`test_system`** (new), against a fake of the port: each command that
  needs the port says `not on this machine` without it, and `info` still
  shows the shell and the card. `help` in three columns at 40 and four at
  64, `help name` for every builtin (each line under 40 columns), a name
  that is not a builtin, `help | grep`. `clear` homes and blanks the
  grid, and writes its escape to a file with `>`. `phosphor`: each ink,
  drawn at once, shown with no operand, a bad name leaving it as it was.
  `battery`: percent, charging, `>`, the southbridge failing. `backlight`:
  both, one, set and stepped as the MCU does, clamped, 256 and `-1` and
  `x` refused before any write, the southbridge failing for both. `info`:
  the port's lines, the card, the battery, and the battery failing.
  `poweroff`, `reboot` and `bootsel`: the port called once with the right
  request, the history on the card before it, and each refused with `>`
  (no `.new` left), in a pipe and with an operand.
- `test_shell` used `help` as its unknown command; it now uses `halp`.
- **Planted bugs**, 10, one at a time; every one failed a test: `wc`
  counting a word again at each read (`test_text`); the history not saved
  before leaving (`test_system`); `backlight` taking 256 (`test_system`);
  `head` showing a line too many (`test_text`); leaving allowed in a pipe
  (`test_system`); `phosphor` not remembered (`test_system`); `tail`
  starting on a cut line (`test_text`); `wc` with no total for two files
  (`test_text`); `help` ignoring the screen's width (`test_system`);
  `phosphor` waiting for the next colour reset (`test_system`).

**Checked on the board:** Claude, on the Plus 2 W (RP2350B rev 2, id
`7458DC82A89AAC12`), development build from SRAM over SWD
(`tools/swd-ram.sh`), keys typed over the UART, on its test card,
2026-10-14 (`out/m8-board-1.log`):

- `help` in three columns, `help tail`. `info`: chip and clock, the id,
  424 KB of 520 KB SRAM, UF2 Loader 2.5 with 4088 KB for programs,
  "the shell was not started by UF2 Loader" (true over SWD), keyboard
  firmware `0x00`, the card as `FAT32, 30G, 30G free`, the battery.
- `battery`: `100%`, on battery with no USB to the PicoCalc.
- `backlight`: `lcd 32`, `kbd 0` as found. Set and read back, the screen
  240, 100, 0 and 32 again, the keyboard 255, 100 and 0; 300 refused.
  The firmware kept each value as written, with none of the stepping its
  source does (hardware-notes.md §4.11, updated). Left as found.
- `phosphor amber` and back to `default`; `clear`, the screen dump
  blank with the prompt on row 0.
- `wc /BOOT2350.ORG` (559 lines, 3015 words, 89,088 bytes),
  `head -3` and `tail -2` of `/atom/pico-atom.cfg`, `ls / | wc -l`, and
  `cat /BOOT2350.ORG | tail -1 | wc -c` through two pipes.
- `reboot > /r.txt` and `reboot now` refused, no file left. `reboot`:
  stage3 loaded the card's `/BOOT2350.UF2`, which is an M2 build
  (`1e4cfc8`), so the shell came back, though not this one. This build,
  loaded again over SWD, read the history the reboot had saved: its last
  lines were the marker typed before it and the `reboot` lines.
- `bootsel`: its message, then over SWD the core was in the boot ROM
  (pc `0x5e74`, sp `0x50100fc8` in USB RAM): BOOTSEL. No drive on the
  Mac, as no cable joined the PicoCalc's USB to it.
- Later the same day, with the owner's M8 release build on the card
  (`e130b13-dirty`, built from what was committed as `1d18bd2`): started
  with `tools/swd-shell.sh` and driven by `tools/swd-drive.py` with an
  ELF rebuilt from `1d18bd2` to the same layout, checked by its screen
  and counters reading cleanly. A marker line, then `reboot`: stage3
  loaded the card's shell afresh (prompt 484 ms after the reset), and
  `history | tail -3` showed the marker and the `reboot`.
- `poweroff > /p.txt` refused. `poweroff`: its message, then nothing
  more in 14 s (the 10 s "still on" never came), and SWD could no
  longer reach the chip: the power was off.

- **The owner, on a Pico 2 W, 2026-10-14:** every command worked as
  designed. `backlight` changed the backlights on the panel, and
  `battery` showed `charging` while the PicoCalc was charging.
  `backlight lcd 33` read back as `lcd 33`, not the 32 the MCU's source
  would make it. `wc` and `tail` on a 40 KB file finished at once, and
  `wc` on a 534 KB file took about a second (by eye, about 500 KB/s).
  `poweroff` switched it off, on battery and on USB power. `bootsel` entered BOOTSEL, and the RP2350's
  drive showed on the computer. The other checks were not reported in
  detail. The phosphors did not look as designed: amber showed as pure
  yellow, green as pure green, white as pure white. Amber's value,
  `RGB(255, 255, 51)`, is a yellow: the starter defines the phosphors but
  never draws with them, so they had not been seen. Sending the starter's
  panel registers in place of ClockworkPi's gamma and power block was
  tried, did not change amber, and was put back. The owner chose new
  values: green `RGB(51, 255, 51)`, amber `RGB(255, 183, 0)`, white as it
  was, and they look right on the panel ("works very well").

**Measurements:**

| What | Estimate | Measured |
|---|---|---|
| RAM, development / release (link map) | — | 436,364 B / 432,628 B (M6: 427,004 B / 423,260 B), 9.4 KB more: code and tables about 6.6 KB, `help`'s text most of the rest; `tail` shares `more`'s buffer |

**Not checked:**

- Whether the MCU's reply to a backlight write already holds the new
  level; the port reads it back instead.
- `tail` and `wc` timed with a clock rather than by eye.
- Everything carried forward from M6 and M4.

## M6 — USB speed (branch `m6-usb-speed`, 2026-10-13)

**Done.** Multi-block reads and writes in `sd.c`, a 16 KB MSC buffer,
and FatFs's runs of sectors through the same calls. On the Plus 2 W,
release build, `usbdisk` writes at about 610 KB/s (M4: 219 KB/s) and
reads at about 700 KB/s (M4: 432 KB/s), a 32 MB file each way compared
byte for byte; a 20 MB `cp` on the card takes 37 s (M4: 93 s).

**Checked on the host** (macOS, Apple clang 21, `-Wall -Wextra -Werror`,
C11; firmware with Arm GCC 15.2 and Pico SDK 2.3.1, TinyUSB 0.18):

- Both builds compile clean, development and release; 20 host test
  binaries pass.
- **`test_sd`**, new: `src/port/sd.c` itself, against a model of a card
  in SPI mode byte by byte (`fake_sd.c`) behind a fake of the SDK calls
  it makes (`fake_sdk/`), with time counted in bus bytes. One block goes
  by CMD17 and CMD24; 2 and 32 blocks by one CMD18 and its CMD12, and one
  CMD25 and its stop token, on SDHC's block addresses and SDSC's byte
  addresses, neighbours untouched; CMD12's stuff byte skipped. An error
  token mid-read and a write error mid-write: the transfer still stopped,
  the blocks before the error written and none after, and the next
  command working. A card busy 20 ms a block finishes; at 100 ms and
  400 ms a block, and with reads as slow, the transfer stops and returns
  within 2 s; a card that stops answering returns within 2 s; and a
  card busy when called, then slow to the transfer's bound, then busy
  past the stop, returns within 2 s each way (added after Muse's review:
  the transfer's bound had started after its command's ready wait, so
  the four waits stacked to 2.5 s; the bound now starts at the call). After
  every call the card is between commands and not busy, and the model
  saw no command, token or deselect out of turn.
- **Planted bugs**, 14, one at a time; every one failed `test_sd`: the
  stuff byte not skipped; no total bound on writes, and on reads; no wait
  for busy before the stop token; the raw block number as SDSC's address
  for CMD25, and for CMD18; no CMD12 after a read error; no stop token
  after a write error; one block short; no wait for busy after the stop
  token, and after CMD12; the data response not checked; no wait for
  busy between blocks; and a 2 s total bound, which goes past 2 s.
- From M4, after Muse's review: Tab completion now offers programs from
  an empty `$PATH` entry (`PATH=`, a trailing `:`) as the current
  directory, as the search that runs them does (`test_line`).
- `tools/usb-rate.py`, the measurement, run against a FAT disk image on
  the Mac: written, read back without the cache, compared, removed.

**Checked on the board:** Claude, on the Plus 2 W (RP2350B rev 2, id
`7458DC82A89AAC12`), each build run from SRAM over SWD, on its test card
(32 GB SDHC, FAT32), USB to a MacBook Pro (macOS) through a USB-C hub,
2026-10-13 (`out/m6-board-m4dev.log`, `out/m6-board-m6dev.log`,
`out/m6-board-m4cp.log`). "Before" is M4's build of `main` (`1f07eb9`),
"after" this branch:

- `usbdisk` on each of the four builds; `tools/usb-rate.py` wrote 32 MB
  of random bytes to the card through the Mac's volume, read them back
  past the Mac's cache, and found them byte for byte as written, every
  run (six runs). Ejecting on the Mac brought the shell back each time,
  with `$?` 0 and no message of a failed read or write.
- FatFs's multi-block path: `cp /umac0.img` (20,971,520 B) into a
  scratch directory, twice, on the M6 development build; both copies,
  read back on the Mac past its cache, were identical to the original.
  The same copy on M4's build, for the time. The directory was removed
  with `rm -r` after.
- **The owner, on a Pico 2 W, with a different SD card, 2026-10-14:**
  `usbdisk` to an iPad mini. Directories created, and files uploaded
  and downloaded, from the iPad; transfers noticeably faster than M4's,
  and solid (by use, not timed). The iPad has no eject, so Esc was the
  way back to the shell.

**Measurements** (rates in KB of 1,024 B; the copy timed from Enter to
the prompt on the UART mirror, to about 0.2 s):

| What | Estimate | Measured |
|---|---|---|
| RAM, development / release (link map) | +16 KB of MSC buffer | 427,004 B / 423,260 B (M4: 410,004 B / 406,260 B); the buffer 15,872 B more, the code about 1 KB |
| `usbdisk` write, Mac to card, 32 MB, release | faster than M4's ~170 KB/s by eye | M4 219 KB/s; M6 603 and 615 KB/s |
| `usbdisk` read, card to Mac, 32 MB, release | — | M4 432 KB/s; M6 696 and 697 KB/s |
| the same, development build | — | M4 213 / 404 KB/s; M6 597 and 552 / 695 and 619 KB/s |
| `cp` of 20 MB on the card, development build | — | M4 93.2 s; M6 37.2 s (about 550 KB/s, reading and writing) |

The USB transfers and the card's take turns (design.md §4.2), so neither
rate is the bus's or the card's alone.

**Not checked:**

- Windows and Linux; the rates on the iPad, and on the owner's card.
- vi's save through the multi-block path (it goes through the same
  `disk_write` as `cp`).
- The transfer's 1 s bound on a real card that slows down: only the
  model's (`test_sd`).
- Other buffer sizes than 16 KB, and overlapping the USB and card
  transfers.

## M4 — comfort (branch `m4-comfort`, 2026-10-13)

**Done.** History, Tab completion, globs, pipes, variables, `more`,
`grep`, `sh` and `/etc/profile` were checked on the host and on the
Plus 2 W. `usbdisk` was checked by Claude up to the USB cable, and by the
owner with a computer: files copied to and from the card.

**Checked on the host** (macOS, Apple clang 21, `-Wall -Wextra -Werror`,
C11; firmware with Arm GCC 15.2 and Pico SDK 2.3.1, TinyUSB 0.18):

- Both builds compile clean, development and release; 19 host test
  binaries pass.
- **`test_expand`**: `path_match` on its own (24 cases: ranges, `!` and
  `^`, `]` first, an unclosed `[`, escapes, a backtracking worst case);
  the quoting cases `test_parse` had before, now through expansion;
  `$NAME`, `${NAME}`, `$?` after a failure, a missing command and a
  success; `$` before anything else; empty words dropped; assignments,
  `set`, `unset`, `export`, a full table keeping the old value, and
  `NAME=value cmd` refused; `$HOME` for `cd`. Globs over FAT16 and FAT32:
  sorting with case folded, `?`, brackets, hidden names (dot, AppleDouble
  and the FAT hidden bit) left out unless the pattern starts with `.`,
  another directory as typed, no match, quoted and escaped parts, a glob
  in a directory's name left alone, a variable's value not globbed,
  `rm *.txt`, and 129 matches refused.
- **`test_pipe`**, over FAT16 and FAT32: small pipes with no file; three
  stages; `|` with `<` and `>`; `ls` one name a line into a pipe; errors
  to the terminal; the status and `&&`/`||` over a pipeline; a 40 KB
  pipe spilling to `/.shell` and coming out whole, and its file gone; four
  stages, two of them large; a card failing as the pipe spills, said and
  recovered from; a stop in the first stage; `menu` in a pipe refused.
  Scripts: variables, `cd` and redirection inside; CR LF and no final
  line break; a mistake reported with file and line and the script going
  on; a line too long; `sh > f` and `sh | grep`; nesting past three
  refused and the level put back; a stop. `/etc/profile` at startup,
  from `profile.new` alone, and with none. The history file: blank and
  repeated lines not kept, written only on leaving and only if changed,
  read back, a failed write keeping the old file and saying so, and the
  oldest dropped. `usbdisk` through a fake hook: refused with no USB,
  the history saved first, FatFs unmounted during the hook and mounted
  after, an error passed back, and refusal with `>`, in a pipe and with
  an operand.
- **`test_text`**: `more` on the grid with keys typed: no prompt for a
  short file, 31 rows then `--More--` in reverse on the last, Space,
  Enter, each way to stop, the end of input, wrapped lines counting as
  rows in 40 and 64 columns, a pipe, `<`, two files with headers, and
  copying when not to the terminal. `grep`: case, `-v -n -c -i` and
  combined, CR LF with `$`, `\<`, names for several files, no final line
  break, the three statuses, a bad pattern, the usages, `<` and `|`, and
  a 1.5 KB line matched in pieces.
- **`test_line`**, on the grid: each editing key at the start, middle
  and end; a 71-character line over two rows with Home, End, Left and
  Right across the row, an insert pushing text over, and a shrinking
  line blanking the row it left; a line exactly a row long, Backspace
  back over the wrap and forward again; a full line; moves after the
  screen scrolled; the prompt on its own row after `echo -n`. History:
  Up and Down, the typed line restored, an edited recall run and kept,
  Ctrl-P and Ctrl-N. Tab: a builtin, a program without `.uf2`, a shared
  part, the list on a second Tab, only programs as commands, case, a
  file, a directory with `/` and the card's case, into it, a name with a
  space escaped, after `;` and `>`, an absolute path, a hidden name asked
  for, no match, mid-line, and a sorted listing of a directory.
- `test_parse` checks the raw words and `|` (four new good lines, five
  new bad); `test_uf2` checks `$PATH` (a second directory, a relative
  one, empty, unset) and programs refused in a pipe.
- **Planted bugs**, 10, one at a time; every one failed a test: globs
  matching hidden names (`test_expand`); a spill dropping what was in
  RAM (`test_pipe`); a pipeline not setting `$?` (`test_expand`); the
  cursor's column off by one across rows (`test_line`); the history
  written every time (`test_pipe`); `sh` not putting the level back
  (`test_pipe`); `more` showing a row too many (`test_text`); `> f |`
  accepted (`test_parse`); `$PATH` stopping at its first directory
  (`test_uf2`); completion not folding case (`test_line`).
- **The stack** (`-fstack-usage`, by hand): each `sh` level adds about
  232 B, so vi's `:s` three levels down is about 5.7 KB of the 8 KB.

**Checked on the board:** Claude, on the Plus 2 W (RP2350B rev 2, id
`7458DC82A89AAC12`), development build from SRAM over SWD, keys typed
over the UART, on the test card in a scratch directory since removed,
2026-10-13 (`out/m4-board-1.log`):

- `*.txt`, `[a-b]*` and a glob with no match; `cat *.txt | grep -n o`;
  `$X`, `${X}x`, `"$X"`, `'$X'`, `$?` after success and after a missing
  command (127); `$PATH` and `$HOME`.
- `cat /BOOT2350.ORG | cat | grep -c .`: 89 KB through two pipes, both
  spilling to `/.shell`, counted 558 lines, and `/.shell` was empty after.
- A script made with `echo >`, run with `sh`: its output, its pipe and
  its `$?`.
- `ls -la / | more`: `--More--` after 31 rows, and Space showed the rest.
- Tab completed `ec` to `echo `, `/readme` to `/readme.md `, `gr` to
  `grep `, and `/pi` to `/pic` (shared by `pico2-apps` and `pics`); Up
  recalled a line, Left five times, and an insert there ran as edited;
  `history` listed 22 lines.
- `usbdisk`: the message, then no disk on the Mac, as no cable is there;
  Esc came back with status 0, the card mounted again, and
  `/.shell/history` written (23 lines).
- `/etc/profile` made on the card; after a restart over SWD it ran
  before the first prompt (its `cd` and its message), and Up recalled the
  last line of the session before, read from `/.shell/history`.
- **The owner, on a Pico 2 W, release build, 2026-10-13:** `usbdisk`
  offered the card to a Mac, and files were copied to and from it.
  Ejecting on the Mac brought the shell back by itself, so macOS sends
  START STOP UNIT on eject. Taking the card out during it gave
  `usbdisk: the card was taken out`, and the disk went from the Mac. A
  30 MB PDF took about 3 minutes to copy from the Mac to the card (by
  eye).
  `more` and the history worked, and a line piped through four commands,
  `ls | grep txt | grep ls | grep g`. Lines longer than a row were
  edited correctly across the wrap. The history came back after `menu`,
  after `usbdisk` and after a program, and lines typed since the last of
  those were lost on switching off, as designed (design.md §4.4).
- The release build, started over SWD and driven by `tools/swd-drive.py`:
  a glob into a pipe, a variable and `$?`, and Tab, each as on the
  development build.

**Measurements:**

| What | Estimate | Measured |
|---|---|---|
| RAM, development / release (link map) | +32 KB of pipe buffer | 410,004 B / 406,260 B (M3: 290,628 B / 286,496 B); pipes 32 KB, the levels' parsed lines and words 21 KB, open files 13 KB, TinyUSB 15 KB |
| Deepest stack | — | about 5.7 KB (vi's `:s` three `sh` levels down), of 8 KB |
| Reset to prompt, with a profile and a 23-line history (Plus 2 W, from SRAM) | — | 550 ms development, 494 ms release; the LCD was up at 440 ms |
| `usbdisk` write, Mac to card (the owner, Pico 2 W, release build) | — | a 30 MB PDF in about 3 minutes, about 170 KB/s, by eye |

**Not checked:**

- `usbdisk`: the rate measured rather than by eye, reading from the card
  at all, and a computer other than a Mac. Making it faster is M6
  (design.md §10).
- How long a spilled pipe takes, and the history's write on leaving.
- The line editor in the host shell: it assumes the panel's 40 or 64
  columns and its wrap at once, where the macOS terminal is wider and
  wraps late, so a long line there is drawn wrongly. The board is right
  (the owner's run).
- Carried from M3: the release build from the card on the Plus 2 W; a
  CR LF file near 64 KB; the stack's depth on the board.
- Carried from M2: the owner's timings; a power cut that leaves the
  first sector's last page half programmed; a key pressed between Enter
  and the reboot; long programs; an RP2040 or oversized UF2 on the card;
  the read-back catching a flash fault; the emulators themselves after
  the shell flashed them; the southbridge's version read failing (-2)
  from the card.
- Carried from M1: time taken by `ls` of a large directory, `df` with
  FSINFO not trusted, and a large `cp`.
- Carried from M0: the start time from power-on through stage3, 75 MHz by
  eye, the cursor's blink by eye, every key code.

## M3 — vi (branch `m3-vi`, 2026-10-13)

**Done.** vi edited `/atom/pico-atom.cfg` on the Plus 2 W's test card,
and pico-atom then started, read it at power-on, and showed the setting
the edit added (the owner, on the panel).

**Checked on the host** (macOS, Apple clang 21, `-Wall -Wextra -Werror`,
C11; firmware with Arm GCC 15.2 and Pico SDK 2.3.1):

- Both builds compile clean, development and release. GCC's
  `-Wformat-truncation` is off for `editor_vi.c` alone: its Ctrl-G report
  is cut to the status row on purpose.
- **Pico Logo's own tests** for the lifted files, unchanged but for one
  include, through `test/host/unity.h`: `test_editor_vi` (255),
  `test_editor_pattern` (54, with a differential against POSIX
  `<regex.h>`), `test_editor_undo` (19) and `test_editor_lines` (18), all
  passing with the page at 31 lines.
- **`test_vi`**, through the shell over FAT16 and FAT32 images, keys typed
  from a script, the screen read from the terminal's grid as vi left it,
  and the file read back through FatFs:
  - open, edit, `:w`, `:wq`, `ZZ`, `:x`, undo; the report on opening and
    on writing, and the clean screen after;
  - a CR LF settings file changed as the board check does it; a mixed
    file keeping its CRs; no final line break kept, with LF and with CR LF;
    an empty file staying empty, then gaining a line break with text; a
    file of one empty line; NUL shown and kept;
  - a new file made by `:wq` and not by `ZZ` or `:x`; a name against the
    current directory;
  - `:q` refused with a change, `:q!`, Brk from insert mode, and the end
    of input, none of which write;
  - `view`, `vi -R` and a file FAT marks read-only: never written, and
    said;
  - a directory, a missing directory, the usages, no keyboard, no card;
  - 65,536 bytes refused with its size and the file untouched; 65,535
    bytes going round unchanged, and an insert into it refused;
  - a yank larger than the register refused and said;
  - the screen: the first 31 of 100 lines and `~` past the end, `G`, `zt`,
    tabs, `^M`, `^A` and `^@`, a 60-column line scrolled sideways and back,
    `v` and `V` reversed, the mode on the status row, the command line
    with the cursor on it, 64 columns in font 5x10, and moving along a
    line sending no text;
  - a card that fails part way through a save, at every sector of it:
    afterwards the old file is whole, or the new one is, or the old is
    gone and a whole `name.new` is there, and a failure is said with vi
    still open.
- **Planted bugs**, 7, one at a time, each rebuilt clean; every one
  failed a test: CR LF not put back (`test_vi`); the old file removed
  before writing (`test_vi`, the fault loop); the size limit one too
  many; the final line break dropped; `:wq` treated as `ZZ`; an undo step
  for every typed key; `$` broken in the key layer (`test_editor_vi` and
  `test_vi`). `gg`, `nG` and `ngg` were added to `test_vi` afterwards;
  the key layer already had them, with Pico Logo's tests.
- **The stack** (`-fstack-usage`, worked by hand): vi's `:s` with a
  32-byte pattern is the deepest chain, about 5.0 KB, since the pattern
  matcher recurses once per atom at 120 B a level. That is more than the
  4 KB design.md gave the stack, which was scratch Y alone; the SDK's
  `__StackLimit` is the bottom of scratch X, 8 KB down, and nothing is
  in scratch X while core 1 is idle. design.md §7 is corrected.

**Checked on the board:** Claude, on the Plus 2 W (RP2350B rev 2, id
`7458DC82A89AAC12`), development build from SRAM over SWD
(`tools/swd-ram.sh`), keys typed over the UART, 2026-10-13
(`out/m3-ram-dev-1.log`):

- `/atom/pico-atom.cfg` on the test card held 123 bytes, LF, six
  settings (the log holds them). vi showed it, its first line cut at 40
  columns, and the report `"/atom/pico-atom.cfg" 6L, 123B`; the screen
  dump matched the panel's grid.
- `Go`, `perf = on`, Esc, `:wq`: `7L, 133B written`; `cat` showed the new
  line and `ls -l` 133 bytes, with no `.new` left.
- Then `tools/swd-shell.sh`: stage3 loaded the card's shell (the M2
  build), and `pico-atom` wrote 10 of its 39 sectors and rebooted into
  pico-atom, which reads the file at power-on. `perf = on` puts its
  figures along the top of the screen.
- **The owner, 2026-10-13:** pico-atom's figures were along the top of
  the panel, so it took `perf = on`. The test card's file still has it.
- **The owner, on a Pico 2 W, release build, 2026-10-13:**
  `docs/design.md`, 41 KB, opened in vi in under a second. From the
  PicoCalc's keyboard, `gg`, `G`, `hjkl`, the arrows, Esc, Brk, and Ctrl
  keys such as Ctrl-F and Ctrl-B all worked.

**Measurements:**

| What | Estimate | Measured |
|---|---|---|
| RAM, development / release (link map) | +80 KB for vi's buffers | 290,628 B / 286,496 B (M2: 158,224 B / 154,284 B); vi is 35 KB of code and 94 KB of `.bss` |
| Deepest stack | under 4 KB | about 5.0 KB (vi's `:s`), of 8 KB |
| Opening a 41 KB file | — | under a second (the owner, Pico 2 W, release build, by eye) |

**Not checked:**

- The release build on the Plus 2 W; the owner ran it from the card on
  a Pico 2 W. The development build ran only from SRAM.
- A CR LF file on the card, a file near 64 KB, and saving a large file:
  not timed (a 41 KB file opened in under a second, by eye).
- The stack's depth on the board: no high-water mark was taken.
- The host shell types the macOS terminal's arrow keys as Esc `[` A,
  which vi takes as three keys; hjkl work.
- Carried from M2: the owner's timings; the release build on the Plus 2
  W; a power cut that leaves the first sector's last page half
  programmed; a key pressed between Enter and the reboot; long programs;
  an RP2040 or oversized UF2 on the card; the read-back catching a flash
  fault; the emulators themselves after the shell flashed them (pico-atom
  started here, unseen); the southbridge's version read failing (-2)
  from the card.
- Carried from M1: time taken by `ls` of a large directory, `df` with
  FSINFO not trusted, and a large `cp`.
- Carried from M0: the start time from power-on through stage3, 75 MHz by
  eye, the cursor's blink by eye, every key code.

**Review of the PR** (Muse, 2026-10-13). No blocking bug in the new
code; these were fixed:

- **The host build failed on Linux** (Muse): `test_editor_vi.c`, lifted
  from Pico Logo, used `SIZE_MAX` without `<stdint.h>`, which macOS's
  headers bring in and glibc's do not. `vi.c` now includes it as well,
  rather than through `vi.h`. Built on macOS only here; Muse's Linux
  build passed with the include.
- **`menu > f` still emptied `f`** (carried from M2's review): `menu`
  refused the redirection, but only after the shell had opened it, and
  the redirection was then committed, creating `f` or emptying the one
  there. The shell now refuses it before opening it; `test_uf2` checks
  that a file already there is kept, for `>` and `>>`.

## M2 — Run (branch `m2-run`, 2026-10-13)

**Done.** The gate held: the first-run time is close to its estimate, so
64 KB block erase was not added. Checked on the Plus 2 W, which now has
UF2 Loader 2.5, and by the owner on a Pico 2 W.

**Checked on the host** (macOS, Apple clang 21, `-Wall -Wextra -Werror`,
C11; firmware with Arm GCC 15.2 and Pico SDK 2.3.1):

- Both builds compile clean, development and release, and `exit-test`.
- `test_uf2`, over a FAT16 image and a fake 4 MB app partition that
  behaves as NOR flash (erase to 0xFF, programming only clears bits),
  counts its operations, and can lose power after any number of them:
  - **Binary info:** a synthetic image (strings in flash and, through the
    mapping table, in RAM; a pointer out of the image; an unterminated
    string; a cut image; no header); and the real ones: stage3 and the
    loader's menu (2.2, the published build), pico-oric, pico-atom and
    pico-ace by name.
  - **The rules:** each RP2350 family, with and without the E10 block and
    the malformed kind; another chip's blocks refused, a UF2 for two chips
    taken; no family refused; past the partition's end, to it, below it;
    RAM programs to stage3's limit and past it; damaged files (a block
    missing, repeated, out of order, misnumbered, the count changing, a
    short payload, misaligned, the file cut short, flash and RAM mixed,
    a bad magic, one marked not for flash); a gap.
  - **Flashing:** a first run over junk writes every sector, and flash
    then holds the program with the gap erased and the proginfo words
    cleared; the check writes nothing; a second run writes nothing; one
    changed page writes its sector and the first; a shorter program
    writes only its sectors; a page no longer filled is erased; a failed
    write leaves nothing bootable, and the next run completes.
  - **Power lost after every number of flash operations** (40 or more),
    switching between two programs: each time, flash holds the old
    program untouched or nothing bootable, never a part of the new one
    that could boot.
  - **The real UF2s:** the three emulators flash, each again writes
    nothing, and pico-oric after pico-ace wrote 39 of its 44 sectors; the
    loader's menu is a RAM program; stage3 (family absolute) and the
    RP2040 menu are not for this device.
  - **The shell:** not found, with and without `/pico2-apps`; by name, in
    any case, by absolute and relative path; a path as given; a directory;
    arguments and redirection refused; a builtin beating a program of the
    same name; not for this device, not a UF2, damaged, each with flash
    untouched; Ctrl-C in the check; a flash failure's message; RAM
    programs by their 8.3 path (`/PICO2-~1/LONGPR~1.UF2`); `menu` missing,
    with an operand, a flash program (refused, never flashed) and a RAM
    one; a loader problem and no loader; no card.
- **Planted bugs**, 24, one at a time in a scratch copy, each rebuilt
  clean; every one failed `test_uf2`: the proginfo words not cleared; the
  first sector not erased first; the IMAGE_DEF page programmed first;
  blocks allowed to descend; no family accepted; RP2040 accepted; the
  malformed E10 kind not noticed; misnumbering after it; a short file
  accepted; RAM to the end of SRAM; never differing; always differing;
  the partition's end unchecked; a differing sector not erased; Ctrl-C
  ignored; writing when nothing differs; `menu` flashed; arguments
  allowed; long names given to stage3; programs before builtins; a loader
  problem ignored; a path with no card saying "not found"; an unbounded
  binary info string; the mapping table ignored. A first try at "the E10
  block not skipped" changed nothing: its family, absolute, is skipped
  anyway, so the address rule only serves to notice the malformed kind.

**Checked on the board:** Claude, on the Plus 2 W (RP2350B rev 2, id
`7458DC82A89AAC12`), development build, 2026-10-13.

- **UF2 Loader installed.** With the owner's approval, its first 512 KB of
  flash was saved, and UF2 Loader 2.5's stage3, built for `pico2` from a
  copy of `references/uf2loader` against SDK 2.3.1 (the loader pins
  2.2.0), was written at offset 0 over SWD and verified. That overwrote
  the start of pico-oric there. The owner put the shell, the loader's 2.5
  menu as `/BOOT2350.ORG`, the three emulators and `exit-test` on the
  board's test card.
- **From SRAM over SWD** (`out/m2-ram-dev-3.log`): the partition (0x2000,
  4,088 KB) and stage3's version (2.5) were read, and programs refused,
  as stage3 had not started the shell. The first tries found neither:
  after a boot from SRAM, flash is not set up (hardware-notes.md §2.7).
- **From the card** (`tools/swd-shell.sh`; `out/m2-card-dev-*.log`): stage3
  2.5 loaded the shell, which read the flash end (0x103fe000) and could
  run programs. pico-oric's first run wrote 44 sectors, and afterwards all
  703 pages in the partition matched its UF2, proginfo words erased (read
  back over SWD). Its second run wrote nothing.
- **`exit-test`** ran and called `picocalc_shell_exit()`: the card was
  found, and stage3 loaded the shell again. Three times, timed.
- **A reset in the middle of a write** (a watchdog reset over SWD, no
  command, about a second into pico-atom's 2.7 s write): stage3 found no
  image and loaded the shell; the partition's first sector was all 0xFF;
  the next `pico-atom` wrote the 25 sectors left.
- **`menu`** handed `/BOOT2350.ORG` to stage3 by `BOOT_RAM`, and SRAM then
  held the loader's menu (`uf2load` 2.5, read over SWD).
- Ctrl-C right after Enter stopped the check with nothing written and no
  reboot; `pico-ace x` was refused.
- **`exit-test` waiting for a key** (after the owner's run, below; written
  into the partition over SWD and started by stage3,
  `out/m2-exit-key.log`): it showed its instructions on the panel, a key
  typed over the UART found the card in, and the shell came back. With
  the card out: not tried here. On the owner's Pico 2 W the first build
  of it returned at once without a key. It took any UART byte as a key,
  and the likely cause is that GP5, with nothing on it, floats and reads
  as bytes; not yet confirmed. GP5 is now
  pulled up, what is waiting is dropped before the prompt, only a
  printable character or CR counts, and the panel says which key woke
  it and from where; that build waited on the Plus 2 W
  (`out/m2-exit-key-2.log`), where the probe holds GP5, so the fault
  itself was not seen here.
- **The owner, on the Pico 2 W, 2026-10-13, that build:** it waited for a
  key; with the card out, a key gave the no-card message and
  `picocalc_shell_exit()` returned false; with the card back, a key
  rebooted into the shell.
- **The owner, on the Pico 2 W, release build, 2026-10-13:** M2 worked.
- **The owner, on a Pico 2 W, UF2 Loader 2.5, 2026-10-13:** M2 worked as
  expected. Power cut during a write: at the next power-on the PicoCalc
  showed the shell.

**Review of the PR** (Muse, then Sonnet, 2026-10-13). Both found no
blocking bug; these were fixed:

- **A card failure while writing programmed the sector being built,
  half filled** (Muse). In the first sector, whose IMAGE_DEF page comes
  early in the file, that could have left a bootable image most of whose
  first sector was erased. `uf2_write` now stops before writing on any
  read error. `test_card_loss` fails the card's reads after every number
  of sectors of a write and checks that no sector is half written and
  nothing bootable is left that is not whole; the host disk can now fail
  reads as it fails writes.
- **Two blocks in the first sector** (Sonnet): their pages went in
  ascending order, so the loop's head could be in place before the rest.
  They now go highest first, the head last. The synthetic programs carry
  a second block, and the power-loss test checks for the head.
- `menu > f` would have left `f.new` behind across the reboot (Muse):
  `menu` now refuses a redirection.
- A repeated block address is now tested (Muse).
- The `BOOT_RAM` path's buffer is checked at start-up to lie below
  stage3's RAM (Sonnet); it is at `0x2002082c`.
- The development build pulls GP5 up, as `exit-test` does (Sonnet).
- "writing N of M" counts the first sector, which every write rewrites;
  a write failure says flash *may* not hold a whole program, since a
  failed first erase may leave the old one; a seek failure reported "ok"
  (all Sonnet).
- The timing gate's text compared Enter-to-reboot with estimates that
  included the boot (Sonnet); design.md §2.2 now compares the totals.

Each fix's planted opposite (writing on a read error, lowest page first,
a repeated address allowed, the first sector not counted, `menu`
redirected) fails `test_uf2`. Not taken: accepting `name.uf2` as a name
(lookup stays as sh's). Left to the owner: telling pelrun that the
header, offered under MIT as well, carries the loader's `BOOT_SD` values.
The fixes were checked on the host only; the card's shell on both boards
is the build before them.

**Measurements** (Plus 2 W, development build, UF2 Loader 2.5,
2026-10-13). Times from Enter are host timestamps on the UART lines
(a script in the session's scratchpad, not committed); the passes are the
shell's own clock.

| What | Estimate | Measured |
|---|---|---|
| First run, Enter to reboot | ~3 s (with the boot) | pico-oric 3.00 s (check 0.33, write 2.65 s), pico-atom 2.96 s, pico-ace 2.72 s; pico-oric over junk 3.32 s (write 3.00 s) |
| Run again, Enter to reboot | ~0.5 s (with the boot) | pico-oric 0.40 s, pico-atom 0.35 s, pico-ace 0.33 s |
| Reboot to the program's first line | — | 0.53 s, three runs within 2 ms (`exit-test`) |
| Exit to the shell's prompt | ~0.5 s | 1.35 s: banner 1.26 s after `exit-test` started, prompt 0.09 s later; three runs within 3 ms |
| A sector written (erase, 16 pages, read back) | ~56 ms (datasheet) | about 61 ms |
| RAM, development / release (link map) | ~504 KB available | 158,064 B / 154,140 B; after the review's fixes 158,224 B / 154,284 B |

So a first run is about 3.5 s to the program and a repeat about 0.9 s.
Both reboot figures are stage3's: its 0.5 s wait for a key at every
start, which the estimates left out.

**Not checked:**

- The owner's timings; the release build on the Plus 2 W.
- A power cut that leaves the first sector's last page half programmed:
  the owner's cut came back to the shell, but where in the write it fell
  is not known.
- Pressing a key at the prompt between Enter and the reboot; long
  programs (more than the emulators' 176 KB); a UF2 on the card for
  RP2040 or past the partition (host only).
- The write's read-back catching a flash fault: no test makes flash
  fail silently.
- The emulators themselves running after the shell flashed them: flash
  matched pico-oric's UF2 byte for byte, but its screen was not looked at.
- The southbridge's version read failed (-2) at every start from the card
  on this board, and not over SWD: not looked into.
- Carried from M1: time taken by `ls` of a large directory, `df` with
  FSINFO not trusted, and a large `cp`.
- Carried from M0: the start time from power-on through stage3 (now
  timed from a reboot: 1.26 s to the banner), 75 MHz by eye, the
  cursor's blink by eye, every key code.

## M1 — Files (branch `m1-files`, 2026-10-12)

**Done.** Timings of card work are carried forward (below).

**Checked on the host** (macOS, Apple clang 21, `-Wall -Wextra -Werror`,
C11; firmware with Arm GCC 15.2 and Pico SDK 2.3.1):

- Both builds compile clean, development and release.
- `test_path`: `.`, `..` (at the root too), repeated and trailing `/`,
  the length limit, and FAT's case-blind comparisons.
- `test_parse`: quoting, escapes, comments, `;` `&&` `||`, the three
  redirections, attached or not, and fifteen lines it must refuse; the
  word and command limits.
- `test_files`, once over a FAT16 image and once over FAT32, both
  formatted by FatFs: every M1 builtin through `shell_run`, as typed.
  Files hold LF, never CR LF; `>>` keeps what was there; `cat a > a`
  reads the old file; errors go to the terminal, not into the file. `ls`
  sorts without regard to case, goes down the columns in 40 and 64
  columns and one per line into a file, hides `.name` and `._name`, and
  with more than 512 names lists them all unsorted, with a note. `cd`
  shows the card's spelling. `cp`, `cp -r` (made, or merged into),
  `mv` (rename, into a directory, over a file, a change of case), `rm`,
  `rm -r` (which carries on past a read-only file), `rmdir`, `mkdir -p`
  and `touch`, each with its errors: the same file, into itself, a file
  in the way, read-only, not empty, deeper than 16 levels. FatFs's lock
  refuses a file written twice at once.
- **Faults**, the same test: a card that stops taking writes after *n*
  sectors, for every *n* across a `cp` over an existing file. Afterwards
  the old file is whole, or the new one is, or the old is gone and a
  whole `name.new` remains (EL §8.6); success is never claimed for a
  copy that did not land. A full card: `cp` and `>>` say so and keep the
  old file, and `df` shows 100 %. No card: each command says so and the
  shell carries on. Ctrl-C stops `cp` and puts nothing half-copied in
  place.
- **Planted bugs**, 26, one at a time in a scratch copy, each rebuilt
  clean; every one failed at least one test: writes straight to the file
  (no `.new`); a failed write published; `..` keeping its slash; `ls`
  sorting with case, across instead of down, columns into a file, hiding
  only `._`; `||` after success; `cp` onto itself; `mv` into itself;
  `cd` showing the typed case; `>` writing CR LF; `>>` not copying the
  old file; `cp -r` not descending; `rm -r` stopping at the first
  failure; a stopped `cp` putting its part in place; unknown options
  ignored; a line ending in `&&`; Ctrl-C not lasting the line; `rmdir`
  leaving emptiness to FatFs; the start directory kept from before; `df`
  counting nothing used; `\` escaping everything in double quotes;
  `path_within` without its root case; after the owner's decision on
  hidden files, `ls` hiding by name only, and not hiding system files.
  The first try missed `df`'s used figure, and the full-card `df` check
  was added for it.
- The host shell, `build/host/shell card.img`, made a FAT32 image and ran
  `mkdir -p`, `cd`, `>`, `ls -l`, `cat`, `df`.

**Checked on the board:**

- **The owner, on a Pico 2 W, release build, 2026-10-12:** every M1
  command, cards taken out and put back, no card, and cards formatted by
  other tools; all worked as designed. This covers the writing commands,
  the card swap (`card_poll` and the remount) and other tools' formats,
  which the host tests cannot.
- **Claude, on the Plus 2 W** (RP2350B rev 2, id
`7458DC82A89AAC12`), development build from SRAM (`tools/swd-ram.sh`), the
card that was in it, **reading only** (`out/m1-ram-dev-1.log`, `-2.log`):
  the shell mounted a 30 GB FAT32 card at the first command and `pwd`,
  `ls`, `ls /`, `ls -la /` and `df` were right. The card holds a
  39,644-byte file whose long name is two spaces. `ls` shows it as it
  is, but FatFs strips trailing spaces from a name it is given, so the
  shell cannot name it to remove it.

After the owner's run, `ls` was changed to hide FAT's hidden and system
names as well (design.md §4.2, settled 2026-10-12). **The owner then
checked, on the same board, 2026-10-12:** hidden and system names are
hidden by `ls` and shown by `ls -a`; and a card pulled out during a
`cp -r` gave a card I/O error, with the card fine afterwards.

**Review of the PR** (Muse AI, 2026-10-12): the logic held up; the host
build did not build on Linux. Fixed: `src/host/main.c` asks for POSIX
(glibc hides `clock_gettime` under strict C11); the deep-path test's
buffers are sized so GCC cannot see a truncation; and the FatFs/disk
dependency is declared both ways, so GNU ld, which scans each archive
once, links `test_shell`. These fixes are checked on macOS only; nothing
here has a Linux toolchain. Found while fixing them: `shell_run` did not
check a line's length, and one longer than SHELL_LINE_MAX would have
overrun the parser's text; `parse_line` now refuses it, with tests. Left
as it is: `nosuch > f` makes no `f`, where sh would.

**Measurements:** none yet. Card timings are those in HW §7.1.

| What | Estimate | Measured |
|---|---|---|
| RAM, development / release (link map) | ~504 KB available | 140,908 B / 137,388 B |

**Not checked:**

- Time taken by `ls` of a large directory, `df` on a card whose FSINFO is
  not trusted (FatFs then reads the whole FAT, which can take seconds and
  does not poll the keyboard), and a large `cp`.
- Variables: moved to M4 (design.md §10).
- Carried from M0: the start time from power-on through stage3,
  `tools/swd-shell.sh` on a board with the loader, 75 MHz by eye, the
  cursor's blink by eye, every key code.

## M0 — Skeleton (branch `m0-skeleton`, 2026-10-12)

**Done**, but one measurement is carried forward: the start time from power-on
through stage3 (below).

**Checked on the board:**

- **The owner, on a Pico 2 W, release build, from the card** (`/BOOT2350.UF2`,
  Up held at power-on): the shell started from the loader, the banner and
  `/>` showed in both fonts, and the display test ran correctly on the
  panel, its 2,000 scrolled lines included, so the 0/480/0 scroll band
  works. Its report: 186.98 rows/s, 138,949.17 chars/s, 7,890.73
  displayed cps.
- **Claude, on a Plus 2 W** (RP2350B rev 2, id `7458DC82A89AAC12`), over
  the Debug Probe. This board has no UF2 Loader: pico-oric is at flash
  offset 0 (QMI address translation is the identity), so the shell was
  started from SRAM with `tools/swd-ram.sh` and nothing was written to
  flash. That path skips stage3, so it does not check the boot chain.
  - Development build (`out/m0-ram-dev-3.log`, `-4.log`): the UART banner
    (`chip=RP2350B rev=2`, clk_sys 150 MHz, southbridge version `0x00`,
    LCD SPI 25 MHz configured); the screen mirrored over the UART; typed
    keys, Backspace, Ctrl-C, `nope: not found`, `font 5x10` and back; the
    FS screen dump matching the mirror in both fonts; GS stats with 0 key
    overflows and 0 I²C errors.
  - Release build, driven over SWD (`tools/swd-drive.py`): `displaytest`
    ran to the end, with `R` in the box as on the host.

**Measurements** (Plus 2 W, release, 2026-10-12):

| What | Estimate | Measured |
|---|---|---|
| Shell start, power-on to prompt | ~0.5 s (design.md §2.2) | not measured: needs the loader |
| Shell start, bootrom's reset to prompt, no stage3 | — | 560.3–560.5 ms over 3 runs: `main` at 8.5–8.7 ms, LCD up at 485.1–485.3 ms |
| The same at 75 MHz, the default from M0's end | — | 463.2–463.4 ms over 3 runs: LCD up at 437.2–437.4 ms |
| Display test, 25 MHz | — | Plus 2 W 187.43 lines/s, 7,890.73 displayed cps; Pico 2 W (owner) 186.98 lines/s, 7,890.73 displayed cps |
| Display test, 75 MHz (`build/pico-release-75`, timing only) | — | 511 lines/s, 12,666 displayed cps |
| RAM, development / release (link map) | ~504 KB available | 72,892 B / 69,380 B |

The character phase took the same time on both boards to the reported
hundredth (138,949.17 chars/s, 7,603,852 µs on the Plus 2 W): it runs
from SRAM at the same clock and SPI rate, with nothing else running, so it
is deterministic. The rows phase differs by 0.24 %.

At 25 MHz, of the 560 ms, about 400 ms is `lcd_init`'s datasheet waits, about 66 ms
is clearing the panel at 25 MHz, and about 75 ms is the first prompt
redrawing 32 rows that are already black: the renderer's shadow starts
unknown. Seeding the shadow with blanks after the clear would save the
last of these.

**Checked on the host** (macOS, Apple clang 21, `-Wall -Wextra -Werror`,
C11; firmware with Arm GCC 15.2 and Pico SDK 2.3.1):

- Both builds compile clean: `-DPICO_SHELL_HOST=ON` (the core and six
  test binaries) and the firmware, development and release
  (`-DPICO_SHELL_UART=OFF`). The firmware is `BOOT2350.uf2`: family
  `rp2350-arm-s`, 147 blocks at `0x20000000`–`0x20009300` (stage3 accepts
  up to `0x20080000`), its image definition at `0x2000015c`, inside the
  first 4 KB that stage3's `rom_chain_image(SRAM_BASE, 0x1000)` searches.
- `test_skeleton`: every core header builds with no SDK; config.h agrees
  with the fonts and the panel. Putting `#include "pico/stdlib.h"` in
  `config.h` breaks the host build, as it should (EL §2.1).
- `test_term`: text, wrap, cursor motion, erase, SGR and the reverse
  semantics, character sets, scrolls and regions, modes, reports. The
  renderer contract is checked by replaying 20,000 random runs of
  sequences through a model of the panel (48 rows of frame memory,
  hardware scroll, dirty rows only) and requiring the panel to equal the
  grid after every flush; a control renderer that ignores the scroll must
  differ, and does.
- `test_displaytest`: the starter's display test, byte for byte, in both
  fonts: after the 2,000 rows, rows 0–30 hold rows 1970–2000 in their
  xterm colours; after the 60,000 characters, the DEC box and the last
  letter (`R`, colour 16 + 59999 % 215) are in place; the byte count
  matches; the panel model matches the grid after every write.
- `test_shell`: the banner and `/>` in both fonts, typing, Backspace,
  Ctrl-C, the line limit, `font`, and `displaytest` stopping when asked.
- `test_keys`: presses type, holds and releases do not (a release with no
  press included), Caps Lock is accepted, Ctrl chords, UART bytes.
- `test_screen` (added after the PR's review): the port's renderer,
  `src/port/screen.c`, built on the host against a model of the panel
  (`fake_lcd.c`: 480 rows of frame memory, the scroll offset, windows
  that must not wrap). After every flush of 1,500 random runs of
  sequences, in both fonts, every panel pixel must be the one the grid
  calls for, with the cursor at the terminal's cursor and nowhere else; a
  panel that ignores the scroll must differ, and does. Also: scrolling
  under a visible cursor leaves no stray underline, the blink's two
  phases, output showing the cursor at once, and a one-cell change
  sending one cell's 80 pixels.
- CSI parameters saturate at 65535, and an out-of-range 256-colour index
  or RGB component is ignored (also from the review); `test_term` checks
  both.
- **Planted bugs**, one at a time in a scratch copy, each rebuilt clean;
  every one failed at least one test: the dirty mask not moved on a
  scroll up (term, displaytest), moved the wrong way on a scroll down
  (term), the scroll not counted (term, displaytest), CUU's starter
  default of no move (term), deferred wrap (term, displaytest), DEC
  graphics off by one (term, displaytest), regions ignored (term), no
  LF to CR LF (shell, displaytest), releases typing (keys), no
  checkerboard for 0x80 and above (term), reverse not swapping (term).
  In `screen.c`, each caught by `test_screen`: the cursor erased after the
  scroll instead of before, the cursor kept through a scroll, a span one
  cell short in the shadow, the shadow indexed by panel row, no bold, a
  font change keeping the shadow, no blink, output not restarting the
  blink, the scroll sent the wrong way. In `term.c`: parameters wrapping,
  and a colour index taken modulo 256.

**Not checked:**

- Shell start time from power-on, through stage3's 0.5 s key wait and the
  card read, against the ~0.5 s estimate. Needs a board with the loader
  and the development build on its card, or the GS line; or the
  `s_prompt_us` read over SWD.
- `tools/swd-shell.sh` (the loader's `BOOT_SD` command over SWD): on the
  Plus 2 W the watchdog reset happened, but there is no stage3 there to
  take the command. Check it on a board with the loader.
- 75 MHz on the panel by eye. The owner made it the default after M0's
  measurements (design.md §4.1); M1's board run is the first at that rate.
- The cursor's blink and the scroll's underline by eye on the panel; both
  are now checked on the host against the panel model (`test_screen`).
- Every key code and event on the southbridge (HW §6.2); key events are
  not logged in this build, since the UART carries the screen.
