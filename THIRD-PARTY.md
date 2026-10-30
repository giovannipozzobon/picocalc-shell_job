# Third-party material

Everything in this repository is this project's own work under the GPL-3.0 in
[`LICENSE`](LICENSE), with the exceptions below. Each file taken from
elsewhere says where it came from in its header.

## The owner's emulators (pico-oric) — GPL-3.0

The same licence as this project, and the same owner.

| Here | From pico-oric | Changes |
|---|---|---|
| `src/port/lcd.c`, `lcd.h` | `src/port/lcd.c`, `lcd.h` | hardware vertical scroll added |
| `src/port/southbridge.c`, `.h` | the same | comments only |
| `src/port/kbd.c`, `.h` | the same | one core's ring in place of the cross-core rings |
| `src/port/board.c`, `.h` | the same | trimmed: no die temperature |
| `cmake/version.cmake` | the same | the header's name |
| `src/port/sd.c`, `sd.h` | the same | `sd_sectors()`, the card's size from its CSD, for `usbdisk`; multi-block reads and writes (CMD18, CMD25) |
| `src/port/diskio.c` | the same | `card_poll()`, for one mount held while the card stays in; runs of sectors in one transfer |
| `src/fatfs/ffconf.h` | `src/port/fatfs/ffconf.h` | the values its header lists |
| `src/core/ntp.c` (`/ntp/ntp.cfg`'s parser) | `src/core/settings.c` | cut down to five keys; quoted values |
| `test/host/test_util.h` | the same | the include guard |
| `tools/build.sh`, `uart-log.sh`, `uart-type.sh`, `uart-screen.sh` | `tools/` | names, keys and pacing |

## PicoMultiLoader — GPL-3.0, the owner's

The same licence as this project, and the same owner (who confirmed on
2026-10-30 that the code may be taken). The web file manager (design.md
§3.9) follows its `rp2xxx/src/webserver.c` and keeps its page and API.

| Here | From PicoMultiLoader | Changes |
|---|---|---|
| `src/web/index.html` | `common/web/index.html` | the Wi-Fi settings and, until M13 step 4, the Flash button taken out; the title and the card's type |
| `src/web/embed.cmake` | `rp2xxx/embed.cmake` | the header's names; no flash resource area |
| `src/web/lwipopts.h` | `rp2xxx/src/lwipopts.h` | no DNS client, no raw API; smaller pools |
| `src/core/httpd.c`, `web.c` | `rp2xxx/src/webserver.c` | the routes and the upload's way of working, rewritten with no lwIP in them, in CP437 and UTF-8, and through `name.new` |

## picocalc-extras — MIT

Copyright (c) 2026 Chris Torkildson, under the MIT licence, which is
compatible with the GPL-3.0. Its Bluetooth keyboard (design.md §3.10).

| Here | From picocalc-extras | Changes |
|---|---|---|
| `cmake/cyw43_fw.cmake` | `bluetooth/bluetooth.cmake` | the technique only (a copy of an SDK file patched at configure time, the build stopping if the SDK's code has changed); the patches are new |

## picocalc-text-starter — MIT, the owner's

<https://github.com/BlairLeduc/picocalc-text-starter>, Copyright (c) 2025
Blair Leduc, under the MIT licence, which is compatible with the GPL-3.0.

| Here | From the starter | Changes |
|---|---|---|
| `src/core/term.c`, `term.h` | `drivers/display.c`, `display.h` | draws into a cell grid; the list of departures is at the top of `term.c` |
| `src/port/screen.c` | `drivers/lcd.c` | its glyph drawing, bold, underline and cursor, over the emulators' `lcd.c` |
| `src/core/keycodes.h` | `drivers/keyboard.h` | the key code table only |

## Pico Logo — MIT, the owner's

Copyright (c) 2025-2026 Blair Leduc, under the MIT licence, which is
compatible with the GPL-3.0. Its vi key layer (design.md §4.6).

| Here | From Pico Logo | Changes |
|---|---|---|
| `src/core/editor_vi.c`, `editor_vi.h` | `devices/picocalc/editor_vi.c`, `.h` | keys from `keycodes.h`, capacities from `config.h`, a page of `VI_TEXT_ROWS` lines |
| `src/core/editor_pattern.c`, `.h` | the same | capacities from `config.h` |
| `src/core/editor_undo.c`, `.h` | the same | none |
| `src/core/editor_lines.c`, `.h` | the same | none |
| `src/core/vi.c` (part) | `devices/picocalc/editor.c`, `editor_vi_apply` and its helpers | adapted to the shell's buffer; the rest of `vi.c` is this project's |
| `test/host/test_editor_vi.c`, `test_editor_pattern.c`, `test_editor_undo.c`, `test_editor_lines.c` | `tests/` | `keycodes.h` for `keyboard.h`; run through `test/host/unity.h`, this project's shim for the parts of Unity they use |

## UF2 Loader — GPL-3.0, pelrun

<https://github.com/pelrun/uf2loader>, v2.5, under the GPL-3.0 like this
project. Its `ui/uf2.c` is itself from muzkr's hachi
(<https://github.com/muzkr/hachi>, Copyright (c) 2024 muzkr), modified by
pelrun in 2025.

| Here | From UF2 Loader | Changes |
|---|---|---|
| `src/core/uf2.c`, `uf2.h` | `ui/uf2.c` | its block rules, rebuilt as a check of the whole file before flash is touched and a write of only the sectors that differ (design.md §3.3) |
| `src/port/loader.c` | `common/bootloader/proginfo.c`, `ui/uf2.c` | the scratch commands, the flash end, the partition and the remap; erasing and programming through the bootrom |
| `include/picocalc_shell.h` | `common/bootloader/proginfo.c` | the `BOOT_SD` command's values only; the header is offered under the MIT licence as well (design.md §9) |
| `src/port/usbdisk.c` | `ui/lib/usb_msc/usb_msc.c` | its MSC callbacks; an eject ends the run, and a card taken out |
| `src/port/usb_descriptors.c` | `ui/lib/usb_msc/usb_descriptors.c` | the same numbers and strings; the full-speed descriptor only, the serial number through the SDK |
| `src/port/tusb_config.h` | `ui/lib/usb_msc/tusb_config.h` | comments and unused options dropped |
| `third_party/uf2loader` | the whole loader, a git submodule at v2.5 (`5c44a4b`) | none; `tools/build-loader.sh` builds stage3 from a copy |
| `patches/uf2loader-boot-shell.patch` | `stage3/stage3.c`, `common/petit_fatfs/diskio.c`, `CMakeLists.txt` | M7 (design.md §10): the shell at power-on, a restart's watch for a key, the program in flash with no card, the card polled without sleeping, the version `2.5-shell` |

The last three files started as TinyUSB's `msc` example (MIT, Copyright (c)
2019 Ha Thach, tinyusb.org), which the loader adapted. TinyUSB itself is
not in the tree: the firmware links the Pico SDK's copy.

## The fonts — the owner's

`src/core/font.h` is the owner's `references/fonts/font.h`, but for the fonts
it declares (design.md §4.1). The owner's `font-8x10.c` is no longer built
in, but its glyphs fill the DEC special graphics that code page 437 lacks,
and glyph 0x7F, in both flavours' fonts.

## The UNIX flavour's font — the Master 512's

`src/core/font-master-512-8x10.c` is the Acorn BBC Master 512's character
set (DOS Plus), from `FONTS/NON-PC/BBC512.F08` in VileR's collection of
bitmap fonts compatible with VGA text mode,
https://github.com/viler-int10h/vga-text-mode-fonts: 256 glyphs of 8x8 in
code page 437 order. The collection has no licence file; its `FONTS.TXT`
says that, as far as VileR could determine, including these plain bitmap
fonts violates no copyright, and asks a font's creator who objects to get
in touch. Changes: the glyphs drawn in rows 1-8 of an 8x10 cell, and
arranged as the terminal's 128 glyphs, with the DEC special graphics at 0x00-0x1F taken
from its own code page 437 glyphs where it has them (the box-drawing ones
carried into rows 0 and 9) and from the owner's `font-8x10.c` where it does
not, and code page 437's upper half dropped (design.md §4.1).

## The DOS flavour's font — VileR, CC BY-SA 4.0

`src/core/font-ibm-8x10.c` is made by `tools/mkfont-ibm.py` from
`Bm437_IBM_BIOS.otb` in **The Ultimate Oldschool PC Font Pack** v2.2,
(c) 2016-2020 VileR, https://int10h.org/oldschool-pc-fonts/, licensed under
the Creative Commons Attribution-ShareAlike 4.0 International License
(https://creativecommons.org/licenses/by-sa/4.0/). Changes: the IBM PC BIOS
8x8 glyphs are drawn in rows 1-8 of an 8x10 cell, the box-drawing glyphs
carried into rows 0 and 9, and arranged as the terminal's 128 glyphs, with
the owner's glyphs where code page 437 has none (design.md §4.7). This
adaptation is licensed under CC BY-SA 4.0 as well, which Creative Commons
declares one-way compatible with the GPL-3.0 the rest is under. Only the
DOS flavour's firmware contains it.

## FatFs — ChaN

The shell links FatFs R0.15 (with patch 1), in the firmware for the SD card
and on the host for image files. It is **not in the tree**: CMake
(`cmake/fatfs.cmake`) copies `ff.c`, `ff.h`, `ffunicode.c` and `diskio.h`
out of the Pico SDK's `lib/tinyusb/lib/fatfs/source/` into the build
directory at configure time, as the emulators do. The one FatFs file in the
repository is `src/fatfs/ffconf.h`, which started as the SDK's copy and
records in its header which values were changed.

> FatFs — Generic FAT Filesystem Module, Copyright (C) 2022, ChaN, all
> right reserved.
>
> FatFs module is an open source software. Redistribution and use of FatFs
> in source and binary forms, with or without modification, are permitted
> provided that the following condition is met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this condition and the following disclaimer.
>
> This software is provided by the copyright holder and contributors "AS
> IS" and any warranties related to this software are DISCLAIMED. The
> copyright owner or contributors be NOT LIABLE for any damages caused by
> use of this software.

## Pico SDK import script — Raspberry Pi

`cmake/pico_sdk_import.cmake` is the Raspberry Pi Pico SDK's own
`external/pico_sdk_import.cmake`, copied as the SDK's instructions direct,
under the SDK's BSD-3-Clause licence.

## ntp.uf2: the CYW43 driver, its firmware, and lwIP

`ntp.uf2` (design.md §3.8), built only in the Pico 2 W build, links three
things from the Pico SDK that the shell itself does not. None is in the
tree.

- **lwIP**, Copyright (c) 2001, 2002 Swedish Institute of Computer
  Science, under a BSD-3-Clause licence (`lib/lwip/COPYING`), compatible
  with the GPL-3.0. `src/ntp/lwipopts.h` is this project's, after the
  SDK examples' `lwipopts_examples_common.h` (BSD-3-Clause).
- **The CYW43 driver and the CYW43439's firmware** (`lib/cyw43-driver`),
  Copyright (C) 2019-2022 George Robotics Pty Ltd, offered under either
  of two licences: `LICENSE.RP`, which permits use and redistribution
  only with Raspberry Pi's chips, or `LICENSE`, which permits them only
  for personal, non-commercial use.

### Additional permission under GNU GPL version 3 section 7

*Settled 2026-10-24.* Each of the driver's two licences adds a
restriction the GPL-3.0 does not allow on a work it covers (its §10), so
the copyright holder of this project's own code, Blair Leduc, adds this
permission to the GPL-3.0 for that code:

> If you modify this Program, or any covered work, by linking or
> combining it with the CYW43 driver (`cyw43-driver`, Copyright (C)
> 2019-2022 George Robotics Pty Ltd), including the CYW43439 firmware it
> carries, as distributed with the Raspberry Pi Pico SDK, or a modified
> version of that driver, containing parts covered by the terms of the
> driver's `LICENSE.RP` or `LICENSE`, the licensors of this Program grant
> you additional permission to convey the resulting work.

It covers only code the owner holds the copyright to: not the files
taken from UF2 Loader or TinyUSB (above), whose authors have not given
it. `ntp.uf2` links none of them. It returns to the shell by writing
UF2 Loader's `BOOT_SD` command with `include/picocalc_shell.h`'s values,
not through `src/port/loader.c`, and the shell core's `uf2.c` is not
linked into it. A change to `ntp.uf2` must keep it that way.

A release of `ntp.uf2` uses the driver under `LICENSE.RP` (the RP2350 is
Raspberry Pi's chip), whose third condition asks for this notice with
the binary:

> Copyright (C) 2019-2022 George Robotics Pty Ltd
>
> Raspberry Pi Ltd (Licensor) hereby grants to you a non-exclusive license to
> use this software solely with the Licensor's microcontroller chip (RP2040) or
> any other semiconductor device produced by the Licensor. No other use is
> permitted under the terms of this licence.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. The software can only be used and redistributed in conjunction with RP2040
>    or any other semiconductor device produced by the Licensor.
> 2. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
> 3. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE LICENSOR AND COPYRIGHT OWNER "AS IS" AND ANY
> EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
> WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
> DISCLAIMED. IN NO EVENT SHALL THE LICENSOR OR COPYRIGHT OWNER BE LIABLE FOR
> ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
> DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
> SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
> CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
> OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
> OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
