# PicoCalc hardware notes

A standalone hardware reference for developers and agents writing software for
the **ClockworkPi PicoCalc**, primarily in C with the Raspberry Pi Pico SDK.
It covers wiring, peripheral protocols, timing, memory tradeoffs and observed
quirks. It can be copied into a new project without companion files.

**Scope, September–October 2026.** This is our current understanding of the platform.
Hardware observations cover Pico 2, Pico 2 W and Pico Plus 2 W configurations;
RP2040 guidance is based on documentation and SDK behavior, not equivalent
on-device validation. Measurements describe the stated board and workload,
not guaranteed limits for every unit. Distinguish requested/configured bus rates
from physical signal measurements, and successful listening/visual checks from
instrumented electrical validation. Official hardware and firmware references
are linked where relevant and collected in §11.

**Contents**

1. [The machine](#1-the-machine) · 2. [The processor board](#2-the-processor-board) ·
3. [Clocks and overclocking](#3-clocks-voltage-and-overclocking) ·
4. [Display](#4-the-display) · 5. [Audio](#5-audio) ·
6. [Keyboard and southbridge](#6-keyboard-battery-backlight-power) ·
7. [Storage](#7-storage) · 8. [On-chip odds and ends](#8-on-chip-odds-and-ends) ·
9. [Making it fast](#9-making-it-fast) · 10. [Bring-up checklist](#10-bring-up-order-and-trap-checklist) ·
11. [Official references](#11-official-references)

---

## 1. The machine

The PicoCalc is a **carrier board**, not a computer. It supplies a 320×320 SPI
LCD, a 4×N keyboard behind its own microcontroller, stereo PWM audio, an SD
slot, a battery and charger, and two backlights. The computer is a
Raspberry Pi Pico–form-factor module you plug into it, and everything on the
carrier reaches that module over **three buses and a pair of PWM pins**.

| Peripheral | Bus | Starting configuration |
|---|---|---|
| LCD (ST7789P / ST7365P class) | `spi1`, 4-wire | 25 MHz; 75 MHz has also been exercised (§4) |
| Southbridge — keyboard, battery, backlights, power | `i2c1`, addr `0x1F` | 10 kHz |
| SD card | `spi0` | 400 kHz init → 25 MHz requested |
| Audio | PWM, both channels of one slice | choose carrier and sample rate together (§5) |

### 1.1 GPIO map

Everything the carrier drives, plus the pins the processor board itself claims.
GP numbers are the Pico header numbering and are **the same on all five boards**
— the Pimoroni Plus 2 W maps its RP2350B pins so that GP0–GP29 land where a Pico
puts them. Use the module's board definition for its on-board peripherals.

| GP | Used by | Function |
|---:|---|---|
| 0–5 | — | GP2/GP3 carry UART0 debug through the Debug Probe (GP4/GP5, UART1, until 2026-10-22); GP0/GP1 and GP4/GP5 are free |
| 6 | carrier | `i2c1` SDA → southbridge |
| 7 | carrier | `i2c1` SCL → southbridge |
| 8–9 | — | free |
| 10 | carrier | `spi1` SCK → LCD SCL |
| 11 | carrier | `spi1` TX → LCD SDI |
| 12 | carrier | `spi1` RX ← LCD SDO |
| 13 | carrier | LCD CS — **plain GPIO, driven by hand** |
| 14 | carrier | LCD D/CX — plain GPIO |
| 15 | carrier | LCD RST — plain GPIO |
| 16 | carrier | `spi0` RX ← SD MISO |
| 17 | carrier | SD CS — plain GPIO |
| 18 | carrier | `spi0` SCK |
| 19 | carrier | `spi0` TX → SD MOSI |
| 20–21 | — | free |
| 22 | carrier | SD card detect, **active low**, needs an internal pull-up |
| 23 | *board* | SMPS mode (non-W) / `WL_REG_ON` (W) |
| 24 | *board* | VBUS sense (non-W) / CYW43 data (W) |
| 25 | *board* | status LED (non-W) / CYW43 CS (W) |
| 26 | carrier | PWM5 A → audio **left**, via an RC low-pass into the amplifier |
| 27 | carrier | PWM5 B → audio **right** |
| 28 | — | free (ADC2) |
| 29 | *board* | VSYS/3 on ADC3 (non-W) / CYW43 clock (W) |

So a PicoCalc application has roughly **eleven free GPIOs** (0–5, 8–9, 20–21,
28) plus, on a Plus 2 W, the RP2350B's extra pins that are not on the header.
"Free" here means *not assigned to the peripherals listed above*; confirm against the
PicoCalc schematic before you route a signal to one, because the carrier may
still connect it.

The pins the Debug Probe's UART uses are only free in a build that leaves
that UART alone, and it is how the probe gets your log (§2.7). Either keep a development build
with the UART and a release build without it, or hand the pins back and forth
at run time and say which is lost. The pads are 3.3 V at default drive
strength; a circuit built for a 5 V port may need a level shifter or a
buffer. For inputs, use the internal pull-up and active-low wiring (§2.2's
E9 erratum). Verified on a Plus 2 W on 2026-09-28: an LED driven push-pull
from a free pin, and GP2 grounded reading low against its pull-up.

### 1.2 Four things the hardware does not give you

Design around these from the start; each one has cost a project time.

- **No tearing-effect (TE) signal.** Six wires reach the panel and none of them
  is TE, so you cannot sync a present to the panel's own refresh. Tearing is
  managed by *when and in what order* you send, not by waiting for a strobe.
- **No keyboard interrupt.** The southbridge has no interrupt line to the
  processor (the pin exists in its firmware and is commented out). Input is
  polled, always.
- **No audio DAC.** PWM is the output hardware. Whatever you want to hear, you
  synthesize or decode in software.
- **No battery-backed real-time clock.** The RP2350's AON timer (RP2040's RTC)
  loses time at power-off. Wall-clock time comes from the network on a W board,
  or from the user, or from a DS3231 added on GP4 and GP5 (§8.5).

---

## 2. The processor board

### 2.1 The five boards

| Board | Part | Cores | SRAM | Flash | PSRAM | Radio |
|---|---|---|---:|---:|---:|---|
| Pico | RP2040 | 2 × M0+, 200 MHz max | 264 KB | 2 MB | — | — |
| Pico W | RP2040 | 2 × M0+, 200 MHz max | 264 KB | 2 MB | — | CYW43439 |
| Pico 2 | RP2350A | 2 × M33, 150 MHz | 520 KB | 4 MB | — | — |
| Pico 2 W | RP2350A | 2 × M33, 150 MHz | 520 KB | 4 MB | — | CYW43439 |
| Pimoroni Pico Plus 2 W | RP2350**B** | 2 × M33, 150 MHz | 520 KB | 16 MB | 8 MB | CYW43439 |

Device evidence covers the three RP2350 boards. The RP2040 rows describe
compatible module specifications, not an equivalent application test history.

**Record physical board identity separately from the SDK build target.** A
Pico Plus 2 W (RP2350 revision 2, 16 MiB Winbond flash) and a Pico 2 W
(revision 2, 4 MiB Winbond flash) both ran a `pico2` application build during
September 2026 tests. The Pico 2 W also ran a `pico2_w` build. A banner saying
`board=pico2` identifies compilation settings, not the installed module.
Successful common-peripheral operation does not qualify the wrong board
configuration for PSRAM, Wi-Fi, ADC channel selection or full flash capacity.

### 2.2 RP2040 versus RP2350, in the terms that change your code

| | RP2040 | RP2350 |
|---|---|---|
| Core | Cortex-M0+ | Cortex-M33 (or Hazard3 RISC-V, switchable at boot) |
| Float | **no FPU** — single precision is a bootrom software library | **single-precision FPU in hardware**; double is still software |
| SRAM | 264 KB | 520 KB |
| DMA channels | 12 | 16 |
| PWM slices | 8 | 12 |
| PIO blocks | 2 × 4 SMs | 3 × 4 SMs |
| ADC channels | 4 + temperature | 4 + temp (RP2350A) / 8 + temp (RP2350B) |
| Bank 0 GPIOs | 30 | 30 (A) / 48 (B) |
| External QSPI | flash only | flash **and** a second chip select for PSRAM |
| XIP cache | 16 KB | 16 KB |
| Also | — | HSTX, TrustZone, signed boot |

Two consequences worth stating plainly:

- **Prefer `float` when its precision is sufficient.** On RP2350 that is a hardware
  instruction and on RP2040 it is the cheaper of two software paths. A stray
  `double` literal (`2.0` instead of `2.0f`) silently promotes an entire
  expression.
- **An RP2040 build is a different memory problem, not a slower one.** A 320×320
  8-bit framebuffer is 100 KB: 19 % of an RP2350's SRAM and **39 % of an
  RP2040's**. If you intend to support a Pico, size the framebuffer question
  first and everything else after.

**Erratum RP2350-E9** affects GPIOs used as inputs with the internal *pull-down*
enabled: the pad can latch at an intermediate level instead of reading low.
Prefer active-low inputs with pull-ups (which is what the carrier does for card
detect), or fit an external pull-down, and read the current errata list in the
RP2350 datasheet before wiring a button to a spare pin.

### 2.3 SRAM is the scarce resource

On RP2350, 520 KiB total. Budget code placed in SRAM, static buffers, both
cores' stacks, DMA storage and the peak heap together. Static regions can fit
at link time while leaving too little space for initialization allocations.
Check the linker map and measure heap and stack high-water use under load;
allocator arena size and linker minimum-heap reservations may overlap.

Framebuffer arithmetic, since it dominates everything else:

| Buffer | Bytes | Verdict |
|---|---:|---|
| 320×320 @ 8 bpp indexed | 102,400 | fits, with room to work |
| 320×320 @ 16 bpp RGB565 | 204,800 | 39 % of SRAM; possible but crowds everything |
| Double-buffered 8 bpp | 204,800 | same cost, and see §4.10 |
| Double-buffered 16 bpp | 409,600 | fits within RP2350 total SRAM, but leaves only 122,880 bytes for everything else |
| Two DMA line buffers (320 px × 2 B × 2) | 1,280 | what a streaming blit needs |
| One 16 bpp tile row band (320 × 16 px) | 10,240 | a banded renderer's whole working set |

An 8-bit indexed canvas with a 256-entry RGB565 palette expanded at blit time
is a useful compromise. A band renderer or compact monochrome snapshot is
cheaper still (§4.10). Double buffering is an application memory-budget choice;
it is not categorically unavailable on RP2350, and it does not by itself
synchronize writes to the panel's scan.

Keep fixed capacities in one header rather than scattered `#define`s; you will
be trading them against each other repeatedly.

### 2.4 PSRAM — Plus 2 W only

8 MB, on QMI chip select 1 (GPIO 47). With a matching board definition and
PSRAM-enabled runtime, it can be initialized before `main()` and exposed as a
memory-mapped window. Do not assume a generic `pico2` build enables it. Three things to know:

- **Verify it before trusting it.** The SDK's init checks the chip ID but not
  that the window works at speed. Write distinct patterns at spread offsets,
  `xip_cache_clean_all()` + `xip_cache_invalidate_all()` to force a real round
  trip, then read back. A marginal QMI configuration then degrades to SRAM-only
  instead of handing your allocator a region that hardfaults on use.
- **PSRAM reads go through the same 16 KB XIP cache as instruction fetch from
  flash.** Cached reads are SRAM-speed; a cache-hostile access pattern is not,
  and it contends with your code. §9.3 has the measurement.
- **Its QMI timing is computed from `clk_sys` once, before `main`.** Change the
  system clock and it is wrong — see §3.

Use PSRAM for **capacity** (level data, sprite banks, audio samples, a text
buffer) and SRAM for the **per-frame working set**.

**Finding it without a board definition.** A generic `pico2` build can still
learn the size: on an RP2350B, set GPIO 47 to `GPIO_FUNC_XIP_CS1`, point
`flash_devinfo` CS1 at GPIO 47 (otherwise the bootrom drives GPIO 0 as CS1
for the XIP exit sequence), and call the SDK's `psram_detect_size()`, which
reads the chip's id. It reported 8 MB on the Plus 2 W (the shell's
development builds, both flavours, from SRAM over SWD, 2026-10-22). This
reads the id only; it is not the round-trip check above.

### 2.5 The status LED

Not on the PicoCalc's front panel — it is the small LED on the processor board,
visible through the case. It is a different piece of hardware on each board:

- **Pico / Pico 2**: `PICO_DEFAULT_LED_PIN`, GPIO 25, a plain `gpio_put`.
- **Any W board**: there is no such pin. The LED is `CYW43_WL_GPIO_LED_PIN`,
  WL_GPIO 0 **on the wireless module**, reached by `cyw43_gpio_set`.

SDK 2.1's `pico_status_led` hides that difference and is the reason no board
conditional is needed. But the consequence does not go away: **lighting the LED
on a W board powers the radio**, so the first call costs the CYW43 firmware
upload (about a second) and leaves the module up.

If you use both the LED and Wi-Fi, bring the driver up **once**:
`status_led_init()` with no argument builds its own `async_context` and calls
`cyw43_driver_init` itself, which a later `cyw43_arch_init` then trips over.
Pass `status_led_init_with_context(cyw43_arch_async_context())` after your own
Wi-Fi init instead — one context, one driver, one initialised flag. This costs
72 bytes of otherwise-dead SRAM on a W board and is still the right trade.

### 2.6 Wi-Fi

Present on Pico W, Pico 2 W and Plus 2 W (all CYW43439). Two hardware facts
matter more than the driver API:

- **The radio needs SRAM.** The driver plus lwIP is a large fixed cost on a board
  that has no PSRAM to move it to. On a Pico 2 W this is the difference between
  budgets, not a rounding error.
- **TLS needs a measured peak-memory budget.** Its handshake heap competes
  with framebuffers and application state. PSRAM can make larger configurations
  practical, but HTTPS is not inherently limited to boards with PSRAM.

The driver reaches the chip over a **PIO SPI whose divider is fixed against
`clk_sys` when the bus comes up** (§3).

### 2.7 Getting code onto the board, and getting output back

- **SWD is a practical development path.** A CMSIS-DAP Debug Probe and OpenOCD
  can flash, verify and reset the RP2350 using `target/rp2350.cfg`; 5 MHz SWD
  has worked on tested units. The target must be powered for the debug port
  to respond. Connect SWDIO, SWCLK and ground correctly.
- **UF2 over USB works** when BOOTSEL is accessible. Identify the exact image
  and its intended startup behavior before treating a blank screen as a boot
  failure: diagnostic firmware may deliberately start blank and silent.
- **UART is a separate connection from SWD.** UART1 at **115200, 8-N-1**, TX
  GP4 and RX GP5, was verified in both directions through the Debug Probe.
  So was **UART0 on TX GP2 and RX GP3**, through function 11 (`UART_AUX`),
  which the SDK picks itself for those pins on RP2350 (`UART_FUNCSEL_NUM`)
  from `PICO_DEFAULT_UART=0` and the two pin numbers: the shell's log, its
  screen mirrored, typed keys and the FS screen dump, on a Plus 2 W,
  2026-10-22 (`out/m12-uart0-1.log`). Not yet checked there: that the RX
  pull-up keeps a floating GP3 quiet with the probe unplugged.
  Cross TX/RX and share ground. Discover the serial endpoint rather than
  hardcoding a workstation path.
- **Keep a Mac's display awake while the probe is in use.** On a Mac, when
  the display sleeps the Debug Probe wedges: its serial port goes away
  ("Device not configured") or the UART goes silent, and it does not
  enumerate again until it is unplugged and plugged back in. `caffeinate -s`
  is not enough, since it prevents only system sleep and lets the display
  sleep; `caffeinate -d` (or `-dims`) keeps the display up. Found by the
  owner of the PicoCalc emulators, 2026-10-04, after two unexplained
  drop-outs: both cores were still running their normal loops each time.
- **Configure stdio explicitly.** Select UART, USB CDC or a custom LCD
  `stdio_driver_t` in the application. Keep CMake's UART definitions consistent
  with the pin header. Multiple enabled stdio drivers can receive output;
  synchronous logging can also consume the time needed by audio or simulation.

- **Reset both cores together from OpenOCD, or core 1 is lost.** The
  `rp2350.cfg` shipped with the Pico SDK's OpenOCD (0.12.0+dev) gives each core
  `reset_config sysresetreq` and asserts it per core, and on RP2350 that resets
  only the core that asks. `reset run` — including the reset in
  `program … reset` — restarts core 0 first; the firmware boots and launches
  core 1 within milliseconds, and then OpenOCD resets core 1 back into the
  bootrom, where it waits for a launch that has already happened. It looks
  like core 1 printing one line and hanging, intermittently, depending on how
  far it got. Found 2026-09-22 on a Plus 2 W; SWD showed core 1 at PC `0xda`
  with the bootrom's `0xf0000000` stack. Use `reset halt` then `resume`, so
  both cores are held until the per-core resets are done. A power-on reset is
  unaffected.

On macOS, keeping the serial descriptor open while setting baud and reading
avoids an adapter reverting to its defaults between a separate `stty` command
and the reader. A startup banner plus consecutive heartbeat messages is more
useful boot evidence than a single line.

Three more things about the UART, each found the hard way:

- **A blocking `printf` costs real time.** At 115200 baud a few-hundred-byte
  status line takes ~30 ms, longer than an audio queue's slack (§5.8). On a
  core with a deadline, format into a ring and let the other core move bytes
  into the UART FIFO as it has room; count dropped lines.
- **Let one process read the port.** Two readers split the byte stream and
  both logs come out scrambled. Make a capture script refuse a port that
  something else already has open, and start the capture before flashing so
  the banner is in it.
- **The logs may have CR line endings** and UTF-8 in them. Some `grep`
  replacements (ugrep) print nothing on such a file; read it with Python if a
  search comes back empty.

The UART also works the other way: firmware that turns received bytes into
key events can be driven with no one at the keyboard, which is how scripted
performance runs and soaks are done. Pace the characters to what the
application can absorb.

**Running an SRAM-only (`no_flash`) image over SWD, without touching
flash.** Load it with OpenOCD's `load_image` while the cores are halted,
then let the bootrom start it: set the watchdog's "boot to PC and SP"
words, as the SDK's `watchdog_reboot(pc, sp, 0)` does (`scratch[4]` =
`0xb007c0d3`, `scratch[5]` = `pc ^ -0xb007c0d3`, `scratch[6]` = SP,
`scratch[7]` = PC with the Thumb bit; `PSM_WDSEL` = `0x01fffff3`), then set
`WATCHDOG_CTRL.TRIGGER`. SRAM survives the watchdog reset, the bootrom
does its whole start-up, and it jumps to the image's entry point.
Starting the image directly instead, by setting PC, MSP and VTOR after a
`reset halt`, fails: the core is halted at the bootrom's first
instruction, the bootrom's enabling of the coprocessors never runs, and
the SDK's `runtime_init_per_core_bootrom_reset` takes a UsageFault (NOCP,
`CFSR` = `0x00080000`) on its first bootrom call. Found on a Plus 2 W,
2026-10-12, starting the PicoCalc shell's `BOOT2350.elf`. Three more
things seen in the same work:

- A watchdog trigger written over SWD resets the chip in the middle of
  the debug port's write, so OpenOCD reports `Failed to write memory at
  0x00000000` although the reset happened. Put the trigger last.
- `write_memory` (and `read_memory`) work while the target runs; the
  watchdog's scratch registers can be set with nothing halted.
- `TIMER0` is reset by a watchdog reset (it is in `PSM_WDSEL`) but **not**
  by OpenOCD's `reset halt`: after one, the timer kept counting from 91 s.
  A "time since reset" read with `time_us_64()` therefore counts from the
  last power-on or watchdog reset.
- **Flash is not set up after such a boot.** The bootrom set up neither
  XIP nor its partition table, because it booted from SRAM, not flash:
  reads through XIP returned garbage, and `rom_get_partition_table_info`
  reported no table (`PT_INFO_PT_INFO` gave a partition count of 0)
  even after `rom_load_partition_table(..., force_reload)`. Calling
  `rom_connect_internal_flash()`, `rom_flash_exit_xip()`,
  `rom_flash_flush_cache()` and `rom_flash_enter_cmd_xip()` first fixed
  both: the table then read correctly. A program the bootrom started
  from flash, such as UF2 Loader's stage3, and a RAM program stage3
  loads after it, find flash already set up. Found on a Plus 2 W,
  2026-10-13, with the PicoCalc shell started over SWD.

**A build with no UART can still be read over SWD**, with both cores
running. OpenOCD started with `init` and no `reset` or `halt` reads memory
through the debug port's memory access (`read_memory addr 32 n`), and the
target never stops. Keep the counters in one block at a fixed symbol, with a
magic word and a layout number, and find its address with `nm`; a debugger
that attaches as gdb does halts the cores, and a halted audio producer
underruns. **Measured** on a Plus 2 W, 2026-10-05: 30 such reads over 44 s,
each a fresh OpenOCD, left the audio's underrun and late-refill counts at 0
and dropped no video frames. Each read takes about a second, most of it
OpenOCD starting.

**A memory read over SWD is not atomic against the running core.** A block
of a few dozen words read while the firmware rewrites it can come back
half old and half new. In a 30-minute soak sampled every 10 s on a Plus 2 W
(2026-10-05), one such torn read made two windows of a field counter
read a tenth fast and a tenth slow. Read the block twice in one
OpenOCD session and keep it only when both reads agree, retrying a few
times; or have the writer bump a sequence word before and after the update
and discard a read whose two sequence words differ.

---

## 3. Clocks, voltage and overclocking

The SDK's default `clk_sys` is **150 MHz** on RP2350 and **125 MHz** on RP2040,
but neither default is the part's limit, and the two parts differ in whether
going faster is *supported* or merely *possible*.

**RP2040 is rated to 200 MHz**, not the 133 MHz its original datasheet
specified — Raspberry Pi re-rated the part, and the SDK carries the higher
figure as the fastest *supported* clock. It is not the default, for backward
compatibility; you opt in with `PICO_USE_FASTEST_SUPPORTED_CLOCK=1` (or by
setting `SYS_CLK_KHZ` yourself). Two things come with it, both visible in
`hardware/clocks.h`: the SDK switches to a 1200 MHz VCO ÷6 ÷1 for the system
PLL, and — because 200 MHz needs more than the stock rail —
`SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST` turns itself on and raises the core to
**1.15 V** during clock init, with a settle delay. That is a supported
configuration, not an overclock.

**RP2350 is rated to 150 MHz and the SDK offers nothing above it.** It will run
past that — applications have run at 300 MHz on tested Pico 2, Pico 2 W
and Plus 2 W units — but **everything above 150 MHz is outside the datasheet**, and you are
raising the rail yourself (the rail note at the end of this section).

Either way, the interesting part is not whether the core survives. It is that
five other things are derived from `clk_sys`, and each of them breaks
differently.

**Measured, Plus 2 W, 2026-08-23** (interpreter throughput, so pure CPU work):

| `clk_sys` | CPU work | speedup | clock ratio |
|---:|---:|---:|---:|
| 150 MHz | 52.5 µs/statement | 1.000 | 1.000 |
| 200 | 39.5 | 1.329 | 1.333 |
| 250 | 31.5 | 1.667 | 1.667 |
| 300 | 25.5 | 2.059 | 2.000 |

Inside 3 % at every point: **CPU-bound code is purely clock-bound here.** Die
temperature went 24.3 → 26.9 °C over a 200-frame run at 300 MHz. That short run does not establish a thermal bound for other workloads,
ambient temperatures or units.

What you must fix up when you move the clock:

1. **`clk_peri` does not follow `clk_sys` by default.** `set_sys_clock_pll`
   parks it on the USB PLL at 48 MHz, so an `spi_set_baudrate(75 MHz)` quietly
   delivers 24 MHz and your display gets *slower* the faster you clock the CPU —
   we measured a present going 19.6 → 58 ms, flat across every overclock,
   because 48 MHz does not care what `clk_sys` is doing. Build with
   `PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1`, then **re-apply your SPI
   baud rate after every clock change**.
2. **The SPI dividers are coarse, and it decides which overclocks are worth
   taking.** The SPI divides `clk_peri` by an even prescale times a post-divider,
   so only clocks that reach the panel's rate *exactly* keep the display at full
   speed:

   | `clk_sys` | LCD SPI actually gets |
   |---:|---:|
   | 150 MHz | **75 MHz** (÷2) |
   | 200 | 50 MHz |
   | 250 | 62.5 MHz |
   | 300 | **75 MHz** (÷4) |

   An overclock to 200 MHz makes your code 1.33× faster and your display
   **1.5× slower**. 300 MHz is the only overclock that is free on both.

   This table is about `clk_peri`, not about which part you are on, so it
   applies just as much to an RP2040 taking its supported 200 MHz: a Pico at
   200 MHz drives this panel at **50 MHz**, so every figure in §4.7 stretches by
   half again. Nothing here has measured that — but the wire term is arithmetic,
   and on an RP2040 the display, not the core, is what you would be trading
   away.
3. **PSRAM QMI timing is computed once, before `main`.** Divider, RX delay, max
   select and min deselect all come from `clk_sys` at runtime init. At 300 MHz
   three of them are wrong in the unsafe direction simultaneously, and the part
   does not fault — it returns occasional bad data. This was found as scattered
   corruption in a text buffer. Recompute and write `qmi_hw->m[1].timing`
   immediately after the PLL moves, using the PSRAM part's timing requirements
   and the SDK/QMI register definitions. Avoid tearing down XIP while code or
   data still depends on it.
4. **The CYW43 bus divider is fixed at bus bring-up.** It divides `clk_sys` by 2,
   so at 250 MHz the chip is clocked at 62 MHz instead of 37 and answers with
   garbage headers. Scale the divider with the clock
   (`cyw43_set_pio_clkdiv_int_frac8`). A *running* radio can be retuned by
   bringing only the bus down and up — `cyw43_spi_deinit` / `cyw43_spi_init` —
   which preserves the association, lwIP state and open sockets. Tearing down
   `cyw43_arch` is the wrong seam.
5. **A PWM audio carrier depends on `clk_sys`, divider and TOP + 1**, so the sample rate
   and every rate derived from it move with the clock. Recompute, do not
   hard-code (§5.2).

And the voltage: the core rail is 1.10 V, which is what 150 MHz is rated
against. Raise to `VREG_VOLTAGE_1_20` **before** the PLL goes up and lower it
**after** it comes back down, with a millisecond to settle, so the rail is never
the lower of the two while the clock is the higher.

Flash XIP also runs at the new clock, and there is little you can do about it —
but the bootrom's divider is conservative, 300 MHz has run test sessions
on the RP2350 boards above. Do not assume an excessive flash clock always
fails immediately or visibly; qualify data integrity as well as startup.

What the bootrom leaves, read off `qmi_hw->m[0].timing` on a Plus 2 W on
2026-09-27 in a build without an embedded boot stage 2 (the SDK's default on
RP2350): **clkdiv 3, RX delay 2**, so 50 MHz at 150 MHz and 100 MHz at 300 if
left alone. The divider is a register you can move. Double both fields
before raising the PLL and the flash sees at 300 MHz exactly what it saw at
150. The RX delay counts half `clk_sys` cycles, so it must scale too. Do it
from SRAM with interrupts off, since nothing may fetch from flash while the
timing changes. Do it once, at power-on, before stdio or any peripheral is
up, so each driver derives its rate from the clock it finds. To change the
clock later, save the choice and restart with the watchdog rather than retune
running peripherals.

**The core rail survives a reset.** A 150 MHz build flashed over SWD after a
300 MHz run, or restarted by the watchdog, comes back with the rail still at
1.20 V: the regulator is not in the domain a system reset clears. The QMI
timing is set again by the bootrom, but the voltage is not. So a firmware
that ever raises the rail must set it at 150 MHz too, after the clock.
Found on a Plus 2 W on 2026-09-27 by reading `vreg_get_voltage()` in the
banner.

---

## 4. The display

### 4.1 The panel

A **320×320 IPS panel** on an ST7789P/ST7365P-class controller with **480 rows
of frame memory**. RGB565, 16 bits per pixel on the wire. The extra 160 rows of
memory are not wasted — they back the controller's hardware vertical scroll
(§4.8).

### 4.2 Wiring and SPI setup

Four-wire SPI on `spi1`: SCK on GP10, MOSI on GP11, MISO on GP12, with **CS
(GP13), D/CX (GP14) and RST (GP15) driven as ordinary GPIOs**. The SDK's SPI
peripheral does not manage chip select here; you do, and that turns out to
matter (§4.3).

Start at **25 MHz**. A **75 MHz** configuration has also been exercised on
hardware and supplies the faster measurements in §4.7; it is above the cited
62.5 MHz ST7789P limit and is not a guarantee for every controller or unit.
Identify the panel and check its specification. Report the rate returned by
`spi_init`/`spi_set_baudrate`, and label it as configured rather than physically
measured unless SCK was checked with an instrument.

### 4.3 The 40 ns chip-select rule — the one that costs a week

The controller requires a minimum **40 ns CS-high pulse** before a RAM write.
At 75 MHz you cannot insert that with a delay loop without paying for it on
every transaction. The trick is to let the instructions you already need supply
the gap:

```c
// DO NOT MOVE. These two lines must precede the CS low, not follow it:
// they are what creates the required CS-high interval.
spi_set_format(LCD_SPI, 16, 0, 0, SPI_MSB_FIRST);
gpio_put(LCD_DCX, 1);          // data
gpio_put(LCD_CSX, 0);          // ...now assert CS
```

Keep that ordering in every 16-bit write path and recheck the gap when
changing CPU speed or driver implementation. Get it wrong and you see intermittent corrupt pixels that look like a
wiring or clock problem.

### 4.4 Initialisation

Use the panel-specific gamma and power settings from the
[ClockworkPi firmware examples](https://github.com/clockworkpi/PicoCalc/tree/master/Code)
and the matching controller specification. A known-working RGB565 setup uses
`MADCTL=0x48`, `COLMOD=0x55` and entry mode `0x06`. Confirmed again 2026-09-22 on a Plus 2 W:
with ClockworkPi's gamma/power block and those three values, a corner-coded
test pattern showed correct orientation and R/B order at 75 MHz. Some examples use 18-bit
pixels; their pixel format and transfer width must be adapted together.
The initialization includes gamma, power/VCOM, interface and frame-rate
controls, inversion, display-function controls and manufacturer commands.
Generic controller defaults may not suit this glass.

A conservative tested reset sequence holds hardware reset low for at least
10 µs, waits 120 ms after release, sends software reset and waits at least
5 ms. After configuration, send `SLPOUT` and wait 120 ms before normal display
use. Clear visible frame memory before `DISPON` so startup does not show
uninitialized pixels. Initialize scroll registers if using hardware scroll.
Check the controller's requirements before shortening any of these waits.

### 4.5 Addressing and colour

`CASET` (columns) and `RASET` (rows) set an inclusive window; `RAMWR` then
streams pixels into it, left to right, top to bottom, wrapping at the window
edge. Window setup is three commands and eight bytes of parameter — at 75 MHz,
under a microsecond of wire, which is why **splitting a present into many
windows is nearly free** (§4.7).

RGB565 on the wire, big-endian: high byte first. The controller expands to
18 bits internally; entry mode `0x06` sets the conversion so that r(0) = b(0) = 0.

### 4.6 Getting pixels out

Three approaches, in ascending order of effort and descending order of CPU cost:

**(a) Blocking writes.** `spi_write16_blocking` per row or per pixel. Simple,
correct, and the CPU is stalled on the wire the whole time: a full 320-px row is
68 µs of doing nothing, ~22 ms for a screen. Fine for setup and text, wrong for
a frame loop.

**(b) One DMA channel, two line buffers, ping-ponged.** The pattern that fits
this panel:

```
lcd_blit_begin(x, y, w, h)     set CASET/RASET/RAMWR, 16-bit format, CS low, hold it
  lcd_blit_row(row) × h        expand row N into line buffer A
                               while DMA streams line buffer B to the SPI TX FIFO
lcd_blit_end()                 wait out the last DMA, drain the wire, release CS
```

The channel is configured **once** — DREQ-paced by `spi_get_dreq(spi1, true)`,
`DMA_SIZE_16`, read increment on, write increment off. Per row you set only the
read address and the transfer count. The CPU's visible cost per row drops from
~68 µs to ~5–8 µs (whatever your row transform costs), and it is spent doing
useful work rather than waiting.

Four details that are not optional:

- **Poll for completion; do not take an interrupt.** You have useful work — the
  next row — while you wait, and leaving the LCD DMA interrupt-free means your
  audio engine owns its DMA IRQ uncontended (§5.4).
- **Drain properly at the end.** The DMA only fills the TX FIFO. Wait for
  `SSPSR_BSY` to clear, discard everything the full-duplex SPI clocked into the
  RX FIFO, and clear the overrun flag (`SSPICR_RORIC`), or the next transaction
  inherits garbage.
- **Restore 8-bit format** when the window closes; commands are 8-bit.
- **Leave interrupts enabled for the whole blit.** The controller tolerates SPI
  clock pauses mid-window, so an audio refill IRQ may preempt a blit freely.
  This is load-bearing — see §5.7.

**(c) PIO.** Usually unnecessary for this interface: the SPI peripheral plus one DMA
channel already saturates the wire at 75 MHz, and PIO would buy you a
different pixel format or a wider bus that the panel does not have. Spend the
state machines elsewhere.

### 4.7 What the wire costs

Theoretical ceiling at 75 MHz, 16 bpp: **4.69 Mpx/s**, i.e. 21.8 ms for
320×320.

Measured on a **Pico 2, 2026-08-01**, timing forced full-region presents twenty
times through an 8-bpp-indexed → RGB565 expanding pipeline:

| Region | Pixels | Wall time |
|---|---:|---:|
| Full screen 320×320 | 102,400 | **25.6 ms** |
| 256×320 (a 16-column viewport) | 81,920 | 21.1 ms |
| 64×320 strip | 20,480 | 5.45 ms |
| One 16-px column, full height (16×320) | 5,120 | 1.259 ms |
| Fixed cost per present | — | 0.41 ms |

**The effective rate is 4.0 Mpx/s — 17–21 % below the wire math.** The figures
are internally consistent (slope 1.259 ms per column, intercept 0.41 ms), so the
gap is real work, not noise: the source copy, the palette expansion, and the
fact that a full-height present with one dirty span per tile row is *twenty*
blit windows rather than one.

Two conclusions to design against:

- **Cost is per pixel, not per present.** The intercept is 0.41 ms for the whole
  screen. Narrowing a viewport buys back exactly its area share; splitting a
  present into several windows is nearly free. A HUD column that never changes
  is worth its area in milliseconds every frame.
- **A native RGB565 source streams closer to the 21.8 ms wire figure**, because
  it deletes the expansion. That is the trade against §2.3's memory arithmetic:
  8 bpp costs ~17 % of present time and saves 100 KB.

**With the source cost removed, the wire and DMA alone run ~9.5 % over the
wire math.** Measured on a **Plus 2 W (RP2350B), 2026-10-03**, at 75 MHz:
two RGB565 line buffers filled once, then sent row after row through the
polled-DMA ping-pong of §4.6 in one window, eight runs each:

| Region | Pixels | Wire math | Wall time |
|---|---:|---:|---:|
| Full screen 320×320 | 102,400 | 21.8 ms | **23.90–23.93 ms** |
| 256×192 | 49,152 | 10.5 ms | **11.475–11.476 ms** |

That is the floor any renderer adds its own cost to.

**Text rows, measured.** A character terminal drawing 320×10-pixel text
rows from RGB565 cell data through the same ping-pong DMA, one window per
changed span, with hardware scroll (§4.8): the PicoCalc shell's display
stress test, release build, Plus 2 W, 2026-10-12. Each scrolled line drew
two full rows (the new text, and the row exposed at the bottom).

| SPI configured | 2,000 scrolled lines | 60,000 single-cell rewrites |
|---|---:|---:|
| 25 MHz | 10.67 s, 187 lines/s | 7.60 s, 7,891 cells/s |
| 75 MHz | 3.91 s, 511 lines/s | 4.74 s, 12,666 cells/s |

At 25 MHz a scrolled line is 5.3 ms against 4.1 ms of wire for its two
rows. A one-cell rewrite (8×10 pixels, 1.4 µs of wire at 75 MHz) costs
79 µs at 75 MHz: the window setup, the escape-sequence parsing and the
dirty-span search, not the wire. The 75 MHz figures are timings only;
nobody looked at the panel during that run.

The same holds for a 240×224 rectangle (Plus 2 W, 2026-10-08, 75 MHz, one
run each, at boot): **12.58 ms** row by row from two line buffers and
12.55 ms as one colour with the DMA's read address held, against 11.47 ms
of wire math, 9.7 % over. One 240-pixel row in a window of its own took
**64 µs** against 51 µs on the wire: the window's three commands cost
about 13 µs.

Frame ceilings, if you present serially on one core:

| Strategy | Present cost | Ceiling |
|---|---:|---:|
| Full screen every frame | 25.6 ms | ~39 fps with nothing left over |
| 256×320 viewport every frame | 21.1 ms | ~47 fps |
| Dirty tiles only, a few sprites moving | 1.6–2.7 ms | not the constraint |

**Measured, Pico Plus 2 W, September 2026, 25 MHz configured SPI1:** full
320×320 monochrome-to-RGB565 diagnostic transfers took **about 71.7 ms**,
including row generation and DMA waits. With LCD and I2C on core 1 and a
CPU/audio workload on core 0, transfers took **about 90–100 ms**. These are
software transfer durations, not panel refresh rates or wire-only timing.
Physical SCK and CS/D-C waveforms were not measured in that test series.

Static borders, diagonals, checkerboards and black clears passed visual checks
through all four corners. Alternating full-screen patterns showed tearing.
A battery-powered 30-minute combined display/audio/input run on the Plus 2 W
recorded 1,800 heartbeats, all eight scripted physical key events, zero I2C
errors and zero late audio refills; the operator reported no display corruption
or audio interruptions. Panel revision identifiers were not recorded.

### 4.8 Hardware vertical scroll

`VSCRDEF` defines top-fixed / scrolling / bottom-fixed bands within the 480 rows
of frame memory; `VSCSAD` sets which memory line appears at the top of the
scrolling band. Bump the offset and the whole band moves with **no pixels sent
at all** — you then only draw the newly exposed line or band.

The price is that **every blit must remap its y through the scroll offset**,
including the wrap around 480 rows. Get it wrong and drawing lands
somewhere plausible-but-wrong, which is a miserable class of bug. If you use
hardware scroll, put the remap inside your single `blit_begin` and give no other
path direct access to `RASET`.

For a game this is the cheapest lever on the machine: **a vertical scroller gets
its background for the cost of one exposed band instead of 25 ms**. It buys
nothing for horizontal motion, and it fights any other use of the scroll offset
(text scrolling, split screens) — you get one scroll register, so pick one
customer for it.

### 4.9 Readback and tearing

The MISO line is wired and `RAMRD` exists, so the frame memory *can* be read
back — but the controller's read timing is far slower than its write timing, so
this is a diagnostic, not a data path. **Keep your own copy of anything you need
to read.**

Treat readback as a **development-only test facility**, for example to compare a
known test pattern or checksum against the controller's frame memory during LCD
bring-up. Do not use it in the production rendering path or as a substitute for
a software framebuffer.

There is no TE line (§1.2). If you present a moving object faster than the panel
scans, you will see tearing. The mitigations available are ordering ones: keep
presents small and localised so a tear affects one tile rather than a whole
sprite; present the moving band before the static one; or accept it — at 320×320
and typical panel refresh, most games do.

### 4.10 Choosing a framebuffer model

The hardware constrains this more than taste does. Four models that work, with
what each costs:

| Model | SRAM | Present cost | Notes |
|---|---:|---|---|
| 8 bpp indexed full frame + palette | 100 KB | 25.6 ms full, linear in area | software reads are cheap; see §2.3 |
| 16 bpp full frame | 200 KB | ~21.8 ms full | fastest wire, crowds everything else |
| Band renderer (N-row strip, regenerated) | 5–20 KB | wire-rate, but you redraw *everything* every frame | no canvas to read back; scrolling is free |
| Dirty-region 8 bpp | 100 KB + tracker | 1–3 ms typical | best for a static background with moving objects |

Double buffering protects the source image from changes during DMA, subject
to the SRAM budget in §2.3. It does not make the LCD update atomic: without a
TE connection, even an immutable full-frame source can tear during scanout.
Band buffering and hardware scrolling reduce transfer work but do not provide
a general scan-synchronization signal.

If you do go dirty-region: a **20×20 grid of 16×16 tiles, one inclusive
`[min..max]` column span per tile row**, is 40 bytes of state for the whole
screen, O(1) to mark and ≤20 windows to flush. Over-mark rather than under-mark
— a few extra tiles cost microseconds and a missed tile leaves a stale sprite on
screen forever. Snapshot the dirty state and clear it *before* sending, so
writes that happen during the blit are tracked for the next present. Marking
overhead is negligible and we measured it (~20 µs over 288 rows against a 7.6 ms
draw). Keep geometry and dirty tracking independent of SDK APIs so they can
be tested on a host before hardware bring-up.

A **256×192 1-bit snapshot needs only 6,144 bytes**. It can be expanded into a
single **640-byte RGB565 row**, reused after DMA completion, or two rows to
overlap generation and transmission. Keep the snapshot immutable until the
whole presentation finishes. DMA completion releases its source buffer, but
CS/window changes must also wait for the SPI shifter to become idle.

Tested layouts on the 320×320 panel are native 256×192 at **(32,64)** and
nearest-neighbor 320×240 at **(0,40)**. Both were readable and visually centered;
the expected uneven pixel doubling at 1.25× was visible. Exact black margins
were verified by geometry tests, not visual inspection. If a driver still
transmits all 320×320 pixels, a smaller source image saves memory but does not
save wire time. Restrict the transmitted window to gain that benefit.

### 4.11 Backlight

The LCD backlight is **not** a PWM pin on the processor — it is register `0x05`
on the southbridge (§6). The keyboard MCU's source steps levels to multiples of 16 and
clamps them to 16–240, so a smooth fade would be 15 steps, not 256. The
installed firmware does not report that: on a Plus 2 W's PicoCalc with
keyboard firmware `0x00` (2026-10-14, the shell's `backlight` over SWD),
register `0x05` read back exactly what was written, 100, 0 and 240, and
`0x0A` 255, 100 and 0, with no stepping or clamping; on a Pico 2 W's
PicoCalc (the owner, the same day) `0x05` kept 33, which the source would
make 32, and the light followed the levels set. Read the register back
rather than assume either behaviour: other firmware may step. Dimming on
pause is a cheap piece of polish and costs one I²C transaction. Read register
`0x05` before dimming and write that level back on resume: the level may be
one the user set with the MCU's own Alt chords (§6.3), not one your software
chose. Use the value only from a read that succeeded.

---

## 5. Audio

### 5.1 The output stage

There is no DAC. The PicoCalc routes **GP26 (left) and GP27 (right) through an
RC low-pass filter into an amplifier**. Two facts make the output stage almost
free:

- GP26 and GP27 are **channels A and B of the same PWM slice** (slice 5 on every
  board — `(gpio >> 1) & 7` — but ask `pwm_gpio_to_slice_num` and never hard-code
  it). One slice clocks both ears sample-locked, and the two compare registers
  share **one 32-bit word**: `left | (right << 16)`. A stereo frame is a single
  32-bit write, so you need one DMA stream, not two.
- The slice's wrap event is a **DMA pacing signal** (`pwm_get_dreq(slice)`), so
  a DMA channel can stream compare values from a buffer at exactly the sample
  rate with zero CPU per sample.

### 5.2 Choosing a carrier and a sample rate

In free-running, edge-aligned PWM, TOP and the divider set the carrier;
repeating each sample across wraps sets the logical sample rate:

```
period clocks = TOP + 1
carrier Hz    = clk_sys / (divider * (TOP + 1))
resolution    = log2(TOP + 1) bits
sample rate   = carrier / oversample
```

At 150 MHz and divider 1, **TOP=2047 gives a 73.2 kHz carrier at 11 bits**. That is the
sweet spot on this hardware: ultrasonic, so the RC filter and the speaker remove
it without any filtering of your own, and 11 bits is more dynamic range than the
speaker resolves. Writing each mixed frame twice (oversample 2) gives a
**36.6 kHz sample rate** — an 18.3 kHz Nyquist limit, comfortably above anything
the output stage passes.

Push the wrap up for resolution and the carrier drops into the audible band;
push it down for carrier headroom and you lose bits. If you want 12 bits, you
need the clock: at 300 MHz, TOP=4095 still leaves a 73 kHz carrier.

**Recompute these from `clock_get_hz(clk_sys)` at init and again on any clock
change** (§3), rather than baking 73,242 into a constant.

For accurate resampling, preserve the rational cadence: at 150 MHz, divider
1, TOP=2047 and two wraps per sample, it is **150,000,000 / 4096 samples/s**.
Integer reports of 73,242 Hz carrier and 36,621 Hz samples are truncated.
Do not feed those rounded reports back into long-running timing calculations.
Change clocks only through a coordinated playback stop/retune/restart path.

### 5.3 Streaming without gaps

Two DMA channels, each **chained to the other**, ping-ponging through a two-half
ring; each is paced by the slice's wrap DREQ and writes the 32-bit compare word.
When a half drains, that channel's IRQ refills it. Chaining is what makes
playback gapless — there is never a moment when no channel is armed.

Two hardware details that are not obvious and cost real debugging time:

- **Size and align the whole ring to a power of two, and use
  `channel_config_set_ring()` on the read address.** This is your safety net for
  IRQ starvation. If a refill is delayed past a half draining, a non-wrapping
  DMA marches into adjacent RAM and plays it as an audible burst of static; with
  the hardware wrap it cleanly replays the ring instead — silence at idle, a
  brief stutter under a note. You need this because **flash program/erase masks
  interrupts with XIP offline for tens of milliseconds** (§7.2), which no amount
  of careful IRQ design avoids.
- **When you re-arm a chained channel from its IRQ, reset both the read address
  and the transfer count.** A trigger, a chain's included, does not reload the
  read address: it carries on from where the last run ended. It does reload the
  count: writing `TRANS_COUNT` sets a RELOAD value, and every trigger copies it
  into the live counter, so a channel performs the same number of transfers each
  time it is started (the RP2350 datasheet's register description, as the
  `rp235x` PAC transcribes it; the RP2040's says the same). So a channel
  chained again before its IRQ has re-armed it plays a full half from where it
  stopped, which in a two-half ring is the *other* half, again; with the ring
  wrap that is a replay, not a march through SRAM. A refill handler that finds
  its channel already running has missed its deadline, and must leave it alone
  and count a late refill (§5.8). Earlier editions of this note said the count
  was not reloaded and that resetting only the address stormed the IRQ; that
  was never reproduced, and it does not fit the datasheet or the late-refill
  path forced on a board (EL §6.4: each 9 ms stall cost 2–3 late refills and
  about three halves of samples, with no storm, and playback carried on). A
  host simulation of the DMA with the count reloaded reproduces that figure, 3
  late refills a stall; without the reload it cannot recover. Set both anyway:
  it costs nothing, and the address must be set.

Ring size sets your latency and your deadline. 256 slots per half at 2×
oversample is 128 frames, i.e. **one refill every ~3.5 ms** — for 2,048 bytes of
SRAM. Halve it for latency, double it for slack.

### 5.4 The deadline, and IRQ priority

**Everything about audio on this machine reduces to one number: the refill
deadline.** At 3.5 ms, any handler that can run longer than that at the same
NVIC priority will produce audible clicks, because same-priority interrupts
cannot preempt each other.

A common source is a **southbridge transaction that blocks 4–5 ms on the
10 kHz I²C bus** (§6). Put that in a timer
IRQ at default priority and a sustained note clicks about ten times a second.

So: `irq_set_priority(DMA_IRQ_0, 0x40)` — above the default `0x80` — so the
audio refill preempts everything else. The I²C peripheral runs autonomously and
is unharmed by being preempted. And put **every function on the refill path**
behind `__not_in_flash_func`, so it survives flash writes and adds no XIP misses
to a latency-critical path.

Flash program/erase is the one accepted violator of the deadline. Everything
else is a bug.

### 5.5 What a mixer costs

A software PSG — 8 voices (three tone plus one noise per ear), phase-accumulator
oscillators, 16-bit LFSR noise, per-voice ADSR, integer throughout — costs about
**35 cycles per voice per frame**: 8 voices × 36.6 kHz ≈ **7 % of one core at
150 MHz**, in interrupt context, regardless of what the foreground is doing.
That is the budget for a synth. Sample playback is cheaper; anything with a
filter per voice is not.

Techniques that made that number achievable, all of them portable to whatever
you build:

- **Phase accumulators**: `phase += (freq << 32) / sample_rate`, take the
  waveform from the top bits. No trig, no tables, exact frequency.
- **Advance envelopes once per refill block, not per sample.** At 3.5 ms
  granularity this is inaudible for millisecond-scale envelopes and it keeps the
  inner loop to an add and a lookup.
- **A logarithmic volume ladder** (16 steps of 2 dB, the TI/Atari curve) scaled
  to 0..256. Linear volume sounds wrong and costs the same.
- **Fix your headroom by construction.** Four voices summed per ear, `>> 2`,
  saturate, bias to mid: four simultaneous full-scale voices cannot wrap. Wrap
  is not a soft failure — it is a full-scale discontinuity.
- **Crest factor is not loudness.** A square wave has a crest factor of 1 and
  speech has 4–8, so matching *peaks* leaves speech ~20 dB below a tone in the
  RMS the ear reads as loudness. If you mix synthesis and samples, tune by ear
  and expect the sample to need several times the "correct" gain.

For the foreground-to-IRQ handoff, a **lock-free SPSC queue per voice** (one
reserved slot so full and empty are distinguishable) removes almost all critical
sections; the few instructions that touch shared voice state go inside
`save_and_disable_interrupts()`.

### 5.6 Other things you could do with this output stage

- **Sample playback** — the DMA path is format-agnostic; anything you can turn
  into 11-bit stereo frames at your chosen rate plays.
- **Mono at double resolution** — not available: the two compare registers are
  physically separate outputs. You would sum in software and write both.
- **I²S to an external DAC over PIO** — possible in principle, but there is no
  DAC on the carrier and no free pins near the audio jack, so this means
  hardware work.
- **1-bit / PDM tricks** — the RC filter is sized for a ~73 kHz carrier; anything
  that puts energy lower will be heard.

### 5.7 Three bring-up findings

All three presented as "audible clicking" and all three had different causes.
The common thread is §5.4's deadline.

1. **The DMA ring wrap** (§5.3). Clicks at idle, static on a mode switch,
   because the IRQ reset the read address and a masked IRQ let the DMA run off
   the end of the buffer.
2. **The LCD driver was masking interrupts.** Stutter under any large screen
   update, because the driver wrapped every screen operation in
   `save_and_disable_interrupts()` — purely because a cursor-blink *timer* drew
   to the LCD from IRQ context. The fix was to make the timer set a flag and do
   the drawing in the foreground, after which all interrupt masking was deleted
   from the LCD driver. **The invariant that came out of it: the LCD is only ever
   touched from thread context, never from an interrupt handler.** Write that at
   the top of your display driver.
3. **IRQ priority** (§5.4), the keyboard poll case.

### 5.8 Count producer starvation separately

A software PCM queue can empty even while every DMA refill meets its deadline.
A stock-clock CPU workload initially missed approximately **14,000–18,000
samples/s with zero late DMA refills**, producing audible buzzing. Count both
PCM underrun samples and late DMA refills; one counter cannot replace the other.
Bound queues, define the silence value, and keep consuming samples when muted
so muting does not change application timing.

**The late path, forced.** A late refill rarely happens in normal running, so
its recovery goes unexercised. A scratch build that masked the producing
core's interrupts for 9 ms every 250 video fields (on a Plus 2 W,
2026-10-04, with §5.3's ring of two 128-frame halves at oversample 2)
counted 3 late refills per stall and lost three halves' worth of samples,
with no IRQ storm, no PCM underrun, and playback carrying on afterwards.
That is what the ring wrap and the both-registers re-arm of §5.3 buy.

A tested arrangement uses a 1,024-sample software queue (about 28 ms), starts
streaming after 768 samples (about 21 ms), and has two 128-sample DMA halves
adding up to about 7 ms. These are buffering calculations, not measured
end-to-end acoustic latency. Choose capacity and startup fill for the workload.

Left/right/stereo/mute operation passed physical listening checks on a Plus 2 W.
A phone app reported approximately **439 Hz** for a 440 Hz test tone; calibration
and resolution were not recorded. This does not measure the PWM carrier,
analog frequency response or waveform fidelity.

---

## 6. Keyboard, battery, backlight, power

None of this is wired to the processor. An **STM32F103R8T6 on the PicoCalc
mainboard** owns the key matrix, both backlights, the battery gauge and the
power rail, and exposes all of it as an **I²C slave at address `0x1F`, 10 kHz**.

Protocol: write the register number, then read two bytes. **OR `0x80` into the
register number to write it.** Always use the `*_timeout_us` variants — a wedged
bus must not hang the machine.

| Reg | Name | Read gives | Notes |
|---|---|---|---|
| `0x01` | `VER` | firmware version | |
| `0x04` | `KEY` | FIFO depth (low 5 bits), caps bit 5, num bit 6 | |
| `0x05` | `BKL` | LCD backlight | writable; the source steps to multiples of 16, clamped 16–240, but firmware `0x00` reads back the value written (§4.11) |
| `0x08` | `RST` | — | **a read resets the MCU** after 1 s; a write resets after *value* seconds |
| `0x09` | `FIF` | `[state, key]`, one event per read | `[0,0]` when empty |
| `0x0A` | `BK2` | keyboard backlight | writable, steps of 32 |
| `0x0B` | `BAT` | percent in bits 0–6, **bit 7 = charging** | refreshed by the MCU every 20 s, not on demand. Bit 7 is the charger, not USB power: on a Plus 2 W on 2026-09-27 it read `100%` with bit 7 for over half an hour on USB, then `100%` without it, still on USB, once the charge finished. So bit 7 set proves USB power, and clear proves nothing |
| `0x0C` | `C64_MTX` | 10 bytes of raw matrix bitmap | reply is longer than two bytes |
| `0x0D` | `C64_JS` | arrows + Enter as a C64-style joystick | |
| `0x0E` | `OFF` | — | write to power off; value is clamped to ≥6 and used as a **delay in seconds** |

Registers `0x02` (`CFG`), `0x03` (`INT`), `0x06` (`DEB`) and `0x07` (`FRQ`) are
**not in the firmware's dispatch** and reply `[0,0]`. That matters: `CFG` is
where you would ask for raw keys instead of MCU-resolved modifiers, and you
cannot.

The two `C64_` registers are the only raw view, and neither covers the whole
keyboard. This is read from `keyboard.ino` and `picocalc_keyboard.ino`. It has
not been checked against the installed firmware, which reports version `0x00`.

- **`C64_MTX` (`0x0C`)** replies with ten bytes: the register number, eight
  column bytes and a ninth that is always `0xFF`. Bits 0–6 of column byte *c*
  are the 7×8 main matrix, active low. Bit 7 of bytes 0–7 carries eight of the
  side buttons: Alt, Ctrl, left Shift, right Shift, `0`, `9`, `]` and `[`.
  **The arrow keys are not in it.** The state persists between scans, so the
  register reads what the last 16 ms scan saw. At 10 kHz a ten-byte reply is
  roughly three times the bus time of an ordinary read.
- **`C64_JS` (`0x0D`)** is meant to hold the four arrows (bits 0–3: right, up,
  down, left) and Enter (bit 4), active low. `keyboard_process()` sets it to
  `0xFF` on **every** call, before the check that returns early until 16 ms
  have passed. The main loop calls it far more often than it scans, so a read
  almost always finds `0xFF`, nothing pressed. As published, the register
  looks broken.

So the arrows are only visible through the event FIFO, with Shift resolved
(§6.3). A raw poll could replace the FIFO for the other keys, at extra bus cost,
but it cannot get around the swallowed Shift+arrow chords.

### 6.1 What it costs, and how to poll it

- **Each transaction costs 4–5 ms of wall time** at 10 kHz. That is the single
  most expensive routine operation on this machine, and it is why §5.4 exists.
  A FIFO read of an empty FIFO (a one-byte register write, then a two-byte
  read) measured **4,825–4,845 µs** over a 655 s run of 19,651 polls at 30 Hz,
  with **zero** failed transactions (Plus 2 W, 2026-10-03).
- **Poll from your frame loop, in thread context** — at 25–60 Hz — not from a
  timer IRQ. Same cost, but it lands somewhere you can account for it and it
  cannot preempt anything.
- **Guard the bus with a flag.** An atomic bool checked before starting a
  transfer keeps a foreground battery read and a background key poll from
  interleaving.
- **Drain more than one FIFO entry per poll.** The FIFO holds 31 entries and a
  held key produces an event every 100 ms on top of press/release pairs; a
  one-event-per-tick consumer falls behind and the machine feels laggy.
- **The MCU resets its own I²C bus** if it sees no transaction for 2.5 s (after
  the first 10 s of uptime). A game that stops polling for seconds will come
  back to a slave that has just re-initialised.

### 6.2 The event model, and what to build from it

Every FIFO entry is a `[state, keycode]` pair: pressed=1, held=2, released=3.
Modifier codes are defined in the `0xA1`–`0xA5` range; not every defined code
is necessarily emitted by the installed firmware. The MCU scans the
matrix every ~26 ms and a held key produces **nothing for 300 ms, then one event
per 100 ms**.

That cadence is a *typing* cadence, and it is wrong for a game. Build a
**held-key bitmap from press/release events** — 256 bits is 32 bytes — and ask
"is this key down now" instead of consuming a character stream. A frame loop
reading one buffered character per frame consumes slower than the repeat
produces, so the backlog grows and the ship keeps turning after the player lets
go; and one character per frame means two keys can never be held at once. Keep a
separate "pressed since last frame" latch for edge-triggered actions.

**Observed firmware quirks, September 2026:** the version register returned
`0x00` (nibble-decoded as `0.0`), which did not identify a specific release.
A 762-event physical capture showed printable auto-repeat as additional
**pressed** events, while modifiers produced **held** events. Holding `j`
produced an initial press plus 16 repeated presses before release. A pressed
event is therefore not necessarily a new physical down edge.

The MCU retranslates keys at each transition. Releasing Shift first can give
press `A` / release `a`, or press `!` / release `1`. Canonicalize letters and
shifted punctuation for held-state identity, while retaining the translated
press for text input. The captured stream left no keys stuck after this
normalization. For actions requiring a fresh key press, consult prior down
state and require release before rearming; do not rely on state 1 alone.

Retranslation goes further when Alt is the modifier let go. Alt+I sends Insert
(`0xd1`, §6.3). Let go of Alt while I is still down, and the MCU's
auto-repeat arrives as more **presses of `i`**, then the release as `i`; no
release of Insert is ever sent (captured on a Plus 2 W, 2026-10-03). So a
held-key set must decide which physical key an Insert is when it arrives:
under Alt it is I, otherwise Enter.

Backspace is `0x08` and Enter is `0x0a`. Alt `0xa1`, both Shift keys
`0xa2`/`0xa3`, and Ctrl `0xa5` were observed; Symbol `0xa4` was not.
`F1`–`F5` arrive as `0x81`–`0x85`, and `F10` as `0x90`, the MCU's Shift+`F5`
(verified on a Plus 2 W, and `F10` also on a Pico 2 W, September 2026).
`F6` arrives as `0x86`, the MCU's Shift+`F1` (checked on a Pico 2 W and
a Plus 2 W, October 2026). A function key
pressed with Alt still held arrives as itself, so match it whatever the Alt
state. Esc is `0xb1`, the arrows `0xb4`–`0xb7` (left, up, down, right), Del
`0xd4`, and **Caps Lock is an event too**, `0xc1` pressed and released,
while the MCU also applies it to the letters it sends (a Plus 2 W,
2026-10-08: after it, letters arrived as capitals with no Shift). A host
that does not bind it must still accept it.

**A release can arrive with no press before it.** In the same capture,
Alt pressed and released, then a release of Space with no press of Space
before it. The operator did not mean to press Space with Alt held. One
reading is a rolled press: Space went down a moment before Alt came up, the
MCU took it as Alt+Space, which it consumes (§6.3), and the release came
after Alt's. A press lost some other way would look the same. It was seen
once and is not explained. Ignore a release for a key that is not down.

### 6.3 The keymap decides which chords exist

**Shift is resolved in the MCU, not host-side, and it can swallow a key
entirely.** Each key has a character and an *alternate*; a shifted non-letter is
replaced by its alternate, and if the alternate is zero **nothing is sent at
all**. Shift + Left, Shift + Right, Shift + Space and Shift + Backspace are
therefore unreachable by any host code. The swallowing covers **release too**:
hold Left, press Shift, let go of Left, and no release event ever arrives, so
a held-key set built from events (§6.2) keeps Left down. Never design a
binding that needs Shift held together with an arrow, such as fire on Shift
while moving. (This follows from the keymap source; a design was changed for
it in September 2026 rather than tested to failure.) Ctrl and Alt are different — they pass
the key through unchanged alongside a modifier event, so Ctrl chords are yours
to define.

Several keys exist *only* as shifted alternates: Home is Shift+Tab, End is
Shift+Del, PgUp/PgDn are Shift+Up/Down, Break is Shift+Esc, Insert is
Shift+Enter or Alt+I. And Alt+`,`/`.`/Space/`B` are consumed by the MCU itself
for the backlights and battery display.

**Alt chords, measured** (a Plus 2 W, keyboard firmware version `0x00`,
2026-10-14, 495 events captured over UART1 from the keys pressed on the
panel; the shell's ring and the bus reported no overflow and no I²C
error):

- **A tap of Alt is a press and a release** of `0xa1`, with nothing
  between. Held past about 300 ms, Alt sends **hold** events (`2 0xa1`)
  about every 100 ms until it is let go, so a host can tell a tap from a
  hold: a tap has no hold between its press and its release.
- **Alt with a letter sends the capital** (`0x46` for Alt+F) beside
  Alt's own events: in the source, the MCU's Alt branch comes before its
  lower-casing. Alt+I is Insert, `0xd1`, pressed and
  released. Alt+B, Alt+`,`, Alt+`.` and Alt+Space are the MCU's own
  (backlights, battery) and send nothing. The source suggests a quick
  one would then look like a tap of Alt to the host, but on a Pico 2 W
  (2026-10-14, the owner) Alt+`,` and Alt+`.` did not open EDIT's menu,
  which opens on a tap: why is not known (the MCU may send something, or
  the chord last long enough for Alt's hold events).
- **Alt with an arrow sends the arrow's own code** (`0xb4`-`0xb7`)
  beside Alt's hold events. Alt+digit sends the digit.
- **Ctrl with a letter sends the lower-case letter** beside Ctrl's
  events (`0xa5`): Ctrl+X, C and V arrived as `0x78`, `0x63`, `0x76`.
- In the same capture, Alt+C and Alt+2 sent nothing, and some keys
  pressed under Alt (1, 3, 5, 8, Z, D) had no release. The MCU's source
  has no reason for either; the operator may have rolled the keys. It
  was seen once and is not explained. Accept a press with no release
  (§6.2).
- Not captured: Ctrl with the arrows, Home and End, and Shift with the
  arrows (which the source says are swallowed or turned into PgUp and
  PgDn, above).

**Read the keymap before designing a binding.** The full register map, event
cadence, matrix tables and the list of swallowed chords are in
[official keyboard MCU source](https://github.com/clockworkpi/PicoCalc/blob/master/Code/picocalc_keyboard/keyboard.ino).
Match it to the installed firmware; a header constant does not prove that the
firmware dispatches that register or emits that event.

---

## 7. Storage

### 7.1 SD card

Use `spi0` at **400 kHz for initialization**, then request **25 MHz** as a
conservative operating point. Card detect is **GP22, active low**, with an
internal pull-up. Transfers use 512-byte blocks. Distinguish SDSC byte
addressing from SDHC block addressing, bound command/token/busy waits and
retries, and report the configured rate.

The card uses a separate SPI instance from the LCD, so there is no shared
chip-select bus arbitration between them. They still compete for CPU time,
SRAM bandwidth and, if used, DMA channels. Card write latency can exceed a
frame or audio-buffer deadline. Pause at a defined application boundary or
provide an independently serviced audio path during storage operations.

Filesystem support is software policy: FAT32 support does not imply exFAT
support. Check missing/full media, removal, corrupt files and interrupted writes
on physical cards. Host fault tests and successful firmware builds do not
qualify card compatibility or power-loss recovery. The 400 kHz/25 MHz settings here are driver configuration guidance, not a
measured physical-card reliability result.
Use a temporary file and an explicit publish/recovery policy to preserve prior
valid data where possible; filesystem rename alone is not proof of power-loss
atomicity. A pattern that works: write `name.new` and close it, unlink the
old file, rename the new one into place, and on load, fall back to a whole
`name.new` when `name` is missing or fails its check, which is what an
interrupted publish leaves. FAT's read-only attribute is a convenient
user-visible write-protect flag.

**Measured** on a Plus 2 W, 2026-09-23, FatFs over polled SPI at 25 MHz
requested: reading a 2.5 KiB disc-image track (ten 256-byte sectors) took
17–19 ms and writing one 25–27 ms. Both are longer
than a 16.7 ms video frame, which is why card work belongs at a boundary
where the application can stop and feed audio silence.

**Measured** on a Plus 2 W, 2026-10-04, the same driver: the first
initialisation and FAT mount after a card is powered or inserted took
192–211 ms, most of it the card answering `ACMD41` busy; a mount of a card
already initialised, after a reset that left it powered, took 15 ms.
Opening and reading a 231-byte file took 9.7 ms, and finding that a file
is not there 7.4–7.6 ms. **Card detect is not a clean edge.** The slot is
push-to-eject, and pulling and reinserting a card read as out, in, out, in:
the contact closed for longer than a 250 ms debounce during the card's
travel, and the mount attempted then failed with FatFs's `FR_NOT_READY`
before the card was really in. Debounce card detect, and treat a failed
mount after an insertion as a state to retry on the next change rather than
an error, since nothing about it hangs.

**Measured** on a Plus 2 W, 2026-10-08, the same driver: opening a 16 KiB
file, reading it in 512-byte pieces and hashing it with SHA-1 took
20.2–20.6 ms (two files), and an 8 KiB file 11.7 ms. A cold mount took
231–238 ms and a warm one 14.6 ms, as before.

**Measured** on a Plus 2 W, 2026-10-08 to 2026-10-10, the same driver,
larger transfers, each with nothing else using the card:

| Transfer | Time |
|---|---|
| a 59–99-byte text file opened, read and parsed | 7.1–8.1 ms |
| a 6,400-byte track read from a 538 KB image | 22.9–23.4 ms |
| the same track written back into the image | 39–45 ms |
| a 65,876-byte file written to `name.new`, the old file unlinked, the new renamed | 281 ms |
| the same file read twice, start to end | 142 ms |
| the same file read once, its CRC checked | 78 ms |
| a 307,254-byte file written in 7,680-byte pieces, through `.new` and a rename | 938 ms |

The last includes drawing each piece's rows and a keyboard poll every
33 ms between pieces, about 0.13 s of it at §6.1's cost. So a large file
reads at about 0.85–0.93 MB/s and writes at about 0.23–0.38 MB/s, on top
of the ~10 ms an open costs (above): a quarter-second write is ordinary,
and holds up everything that waits for the card.

**Measured** on a Plus 2 W, 2026-10-13, the same driver with multi-block
transfers added (`CMD18` stopped by `CMD12`, `CMD25` stopped by its stop
token), SPI at 25 MHz requested, on a 32 GB SDHC card: copying a 20 MB
file on the card through FatFs, in 4 KB pieces, took 37.2 s, where one
command per 512-byte block took 93.2 s. Over USB mass storage at full
speed, in 16 KB pieces that alternate with the USB transfers, a Mac
wrote 32 MB at 603–615 KB/s and read it at 696–697 KB/s, where 512-byte
pieces gave 219 and 432 KB/s. After `CMD12`, skip one byte before
looking for R1: the card may still be shifting out the data it was
stopped in, and a data byte can look like an R1.

**Poll for a block's data token without sleeping.** UF2 Loader's
stage3 (Petit FatFs, `common/petit_fatfs/diskio.c`, `rcvr_datablock`)
waits for a read's data token by reading one byte, then sleeping 100 µs,
up to 1,000 times; `wait_ready()` does the same. Measured on the Plus 2 W
with its 32 GB SDHC test card, 2026-10-25, from a watchdog reboot to the
program's first UART line on the Mac: stage3 loading the shell's 365 KB
UF2 (`BOOT_SD`, then the shell's own 0.43 s to its first line) took
1.044–1.046 s, and 0.852–0.853 s with both loops reading back to back
until the token, ready, or the timeout; a 118 KB RAM program
(`BOOT_RAM`) took 0.319–0.320 s, and 0.227–0.229 s. That is about
0.27 ms a sector, the polls' granularity; the SDK (2.2.0 or 2.3.1) made
no difference. The shell's `sd.c` polls without sleeping.

**Do not time a load by halting the core.** Sampling the program counter
over SWD every ~40 ms (halt, read `pc` and `lr`, resume) showed where
stage3 spent its time, mostly in that wait, but made the same loads
2.4–4.8 s instead of 0.3–1.0 s, the sleeping loop far more than the
other. The likely cause, not checked, is that the timer pauses while a
core is halted for debug, so each sleep outlasts its halt. Time with
the UART on the Mac; sample only for where, not how long.

Three more things from the same driver on the same board:

- **Bound every wait below the southbridge's 2.5 s watchdog** (§6.1) when one
  core does both card work and keyboard polling. A card that stays
  `ACMD41` busy is the longest case; a 1 s limit kept every job inside it.
  Jobs measured 7–221 ms.
- **A transfer that fails marks the card uninitialised**, so the next mount
  runs the full SPI initialisation again rather than talking to a card in
  an unknown state.
- **macOS writes a `._name` file beside every file it copies** to a FAT card
  (AppleDouble metadata). A file list on the device should skip names
  starting `._`; they are not the user's files.

**Prefer the card to internal flash for anything the application writes**,
such as settings. A text file on the card is one source of truth the user can
read and edit on any computer, it survives reflashing, and writing it opens no
interrupt hole (§7.2).

### 7.2 Internal flash, XIP, and the interrupt hole

Code executes from flash through the QMI in execute-in-place mode, cached by a
**16 KB XIP cache** shared by both cores (and, on a Plus 2 W, by PSRAM reads).
Reserve a non-overlapping region explicitly in the flash layout before using
spare flash for a filesystem such as LittleFS. Size it against the physical
flash and the firmware/update scheme. Typical SDK operation geometry is
**4096-byte erase sectors and 256-byte program pages**.

While flash is outside XIP mode, neither core, an ISR nor DMA may read code
or data from it. Use the SDK's
[`flash_safe_execute`](https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2_common/pico_flash/include/pico/flash.h)
with the appropriate safety setup for the runtime. With `pico_multicore`,
initialize participation using `flash_safe_execute_core_init()` on the core
that must be locked out. Check return values; a coordination timeout is not
proof that the callback never ran. Masking only the calling core's interrupts
is insufficient when the other core can access flash.

Additional constraints:

- **Program from SRAM.** The source buffer must remain accessible while XIP is
  unavailable. Quiesce DMA that reads flash or a QMI-backed memory window.
- **Account for PSRAM cache state.** On RP2350, preserve dirty PSRAM data and
  follow the SDK's cache-maintenance requirements around QMI/flash operations.
- **Bound each operation.** One erase sector or program page per critical
  interval limits the stall, but an erase can still exceed the ~3.5 ms audio
  refill deadline. Ring wrapping prevents out-of-bounds reads; it does not
  guarantee uninterrupted new audio while software refills are stopped.
- **Choose the visible behavior.** Pause/mute explicitly or provision a path
  that can continue safely without flash. Do storage at defined application
  boundaries rather than inside a latency-sensitive frame loop.

**What a write costs.** Erasing one 4 KB sector and programming its 16
pages through the bootrom's `rom_flash_op`, then reading it back through
XIP, took **about 61 ms a sector** on a Plus 2 W (16 MiB Winbond) at
150 MHz: 39 sectors of pico-atom in 2.66 s, less 0.28 s spent reading
the UF2 from the card in the same loop (PicoCalc shell, 2026-10-13). This
is the time from the shell's own clock, not a datasheet figure.

**`time_us_64()` at `main` does not count a bootloader's time.** A
program started by UF2 Loader's stage3 read about 490 µs at its first
line, though stage3 had waited 0.5 s for a key before starting it, and
the UART on the host saw 0.53 s from the reboot to that line. Something
between, stage3 or the bootrom's `rom_chain_image`, resets the timer.
Time a whole boot from outside, as with timestamps on the host. Seen on a
Plus 2 W, 2026-10-13.

**Telling a program started from a partition.** When the RP2350's boot ROM
starts an image in a partition (at boot, or through `rom_chain_image`, as
UF2 Loader's stage3 does), it sets the QMI's address translation so the
partition appears at `0x10000000`. The image's own code can read
`(qmi_hw->atrans[0] & QMI_ATRANS0_BASE_BITS) >> QMI_ATRANS0_BASE_LSB`: the
partition's offset in 4 KB sectors, 0 for an image that runs from the
start of flash, as one copied on in BOOTSEL with no partition table does.
UF2 Loader's app partition starts after stage3, so a program it started
reads nonzero. From UF2 Loader 2.5's source (`common/bootloader/
proginfo.c`, its `flash_size()` example for programs) and the RP2350
datasheet's address translation; not yet read back on a board.

---

## 8. On-chip odds and ends

### 8.1 Die temperature

The RP2350's own sensor, so it costs no bus time and cannot be blocked by a
wedged southbridge. **The ADC channel differs between parts and the SDK already
hides it**: input 4 on RP2350A (Pico 2, Pico 2 W, QFN-60) and input 8 on
RP2350B (Plus 2 W, QFN-80), because the bigger package adds four user inputs
below it. `ADC_TEMPERATURE_CHANNEL_NUM` is `NUM_ADC_CHANNELS - 1` and
`NUM_ADC_CHANNELS` comes from `PICO_RP2350A` in the board header, so **no board
conditional belongs in your code**.

Two things the sensor's accuracy decides:

- **Average a burst.** A conversion is 12 bits over 3.3 V — about 0.24 °C per
  LSB, and noisy at that. Sixteen conversions cost ~32 µs total, which is
  nothing beside a 4.6 ms southbridge read.
- **Round the answer.** The datasheet formula (`27 - (V - 0.706) / 0.001721`) is
  *uncalibrated*, with several degrees of part-to-part offset, and it reads the
  die, which sits above the room by however hard the chip is working. A tenth of
  a degree is already generous.

Test it by *loading* the processor and requiring the reading to rise: a wrong
channel returns a floating GPIO, which reads as a plausible number.

The ADC block can be initialised lazily and the bias left on; `adc_init()`
touches no GPIO function, so the audio PWM on GP26/27 is unaffected.

**Unless the build's board is not the board.** One `pico2` image (RP2350A)
is a convenient way to run on all three RP2350 boards, but on a Plus 2 W, an
RP2350B, `ADC_TEMPERATURE_CHANNEL_NUM` is then 4, which on the QFN-80 part is
a GPIO. So read the package off `SYSINFO_PACKAGE_SEL` (1 = QFN-60), choose
input 4 or 8 yourself, and write `AINSEL` directly, since
`adc_select_input()`'s check uses the header's channel count. On a Plus 2 W
on 2026-09-27 the banner said QFN-80, input 8, and the reading passed the
load test: 23 °C idle at 150 MHz, 25 °C at 300 MHz and a 4 MHz guest on
boot, 27 °C after two and a half minutes of a compute loop.

On battery, at 150 MHz with one core about 21 % busy and the
other presenting and polling, a Plus 2 W read **20–21 °C throughout two
30-minute runs** (2026-10-05, uncalibrated, room temperature not recorded).
At this clock the die is not a constraint.

### 8.2 The remaining ADC inputs

GP26 and GP27 are ADC0/ADC1 on the silicon but are the audio outputs here, so
they are gone. **GP28 (ADC2) is the one free analogue input** on a 30-pin board.
GP29 is VSYS/3 on non-W boards and the radio clock on W boards — on a W board,
reading VSYS means wrapping the access in `cyw43_thread_enter`/`exit`, and the
battery gauge on the southbridge (§6) is a better source anyway.

### 8.3 USB

The module's USB port is reachable through the case. It is available for CDC
stdio, for mass storage, or for a USB host stack — but see §2.7: nothing routes
`printf` there unless you ask.

### 8.4 The AON timer and POWMAN's scratch registers across a reset

The RP2350's always-on timer (POWMAN's 64-bit millisecond count) and
POWMAN's eight scratch registers (`powman_hw->scratch[]`, at `0x401000b0`,
a bank separate from the watchdog's) are in the always-on domain. A
program that does not touch them leaves them as they were. The scratch
registers take a plain 32-bit write; unlike POWMAN's other registers,
they need no password in the top half. The SDK's `pico_low_power` uses
scratch 6 and 7.

Measured on a Plus 2 W, 2026-10-22, with the shell's release build run
from SRAM: the timer was started from the crystal at a set time, and the
shell wrote a record to scratch 0-2. Then `tools/swd-ram.sh` loaded
another image and rebooted by the watchdog, with `PSM_WDSEL` at
`0x01fffff3`. That value leaves bits 2 and 3, ROSC and XOSC, out of the
reset. The new image found the record whole, and the timer still
counting from the time it was given.

Measured on the same Plus 2 W, 2026-10-22, with the DOS flavour's
development build on the card as `/BOOT2350.UF2` under UF2 Loader 2.5:
the clock was set with `DATE` and `TIME`, then kept its time through each
of these, read back with `TIME /T` against the Mac's clock to the
second, with no loss seen at that resolution:

- the loader's `BOOT_SD` and a watchdog reboot (`tools/swd-shell.sh`),
  stage3 loading the shell from the card (`out/m12-card-roundtrip-1.log`);
- a program round trip: the shell flashed `exit-test.uf2` and rebooted,
  stage3 started it, and a key typed on UART0 sent it back through
  `picocalc_shell_exit()` (`out/m12-card-program-2.log`);
- the shell's `REBOOT` (`out/m12-card-reboot-1.log`).

Checked by the owner on the same Plus 2 W, 2026-10-23, with the DOS
flavour's W release build on the card and `TZ=EST5EDT` set and `NTP` run
in `AUTOEXEC.BAT`: `clock-test.uf2`, a flash program, read the shell's
time and offset through `picocalc_shell_time()` and showed the same time
as `TIME /T` before it; it set the clock a minute forward with
`picocalc_shell_set_time()` and returned, and the shell showed the minute
added, and wrote it to the DS3231 (design.md §3.8, M12 step 5).

Measured on the same Plus 2 W, 2026-10-24, with the DOS flavour's W
development build on the card under UF2 Loader 2.5, the clock set by
NTP: the AON timer read over SWD while the board ran (POWMAN
`READ_TIME_UPPER` and `LOWER`, 0x40100070), each read between two
readings of the Mac's NTP-disciplined clock, which bound it to 1 ms; 20
reads a sample, their median taken. **Each watchdog reboot loses 2.0 ms**
(±0.5 ms), the same every time:

- the loader's `BOOT_SD` and a watchdog reboot, stage3 loading the shell
  from the card: -1.0 to -3.0 ms against the Mac;
- the shell flashing `exit-test.uf2` and rebooting into it: -3.0 to
  -5.0 ms; `exit-test` returning through `picocalc_shell_exit()`: -5.0
  to -7.0 ms. A program round trip loses 4 ms;
- the shell's `REBOOT`: -7.0 to -9.0 ms.

Between reboots the offset held to 0.5 ms over 65 s and over 68 s, so
the crystal's drift does not show at this resolution. The cause of the
2 ms was not looked into; that the timer's 1 kHz tick stops while the
boot code restarts the crystal is a guess. Just after NTP set it, the
clock was 1 ms from the Mac's (`out/m12-roundtrip-ms-samples.txt`,
`out/m12-aon-sample.sh`).

**Which reset was it.** POWMAN's `CHIP_RESET` (`0x4010002c`) says
"last reset was" in the datasheet, but a watchdog reboot does not clear
it: on a Plus 2 W, 2026-10-25, it read `0x00010000` (`HAD_POR`) after
OpenOCD's `reset run`, and still `0x00010000` after a later watchdog
reboot with `PSM_WDSEL` at `0x01fffff3`, which leaves POWMAN alone. The
watchdog's `REASON` (`0x400d8008`) does tell them apart: `0x0` after
OpenOCD's reset and `0x2` (`FORCE`) after the watchdog reboot, read
once the next image was running. The datasheet gives zero for any
hardware reset. So a boot stage that must know whether it follows a
power-on reads `REASON`, as M7's stage3 does (design.md §10). A real
power-on and the RUN pin were not read directly, but both behave as
zero under M7's stage3: on a Pico Plus 2 and Plus 2 W, 2026-10-25 (the
owner), switching on and pressing the board's RESET button (which
grounds RUN) each gave the shell with no key held, and the program with
Up held.

Not yet checked: the cause of the 2 ms; power-off; the RUN pin;
BOOTSEL; OpenOCD's `reset halt`; and what the timer does if XOSC stops.

### 8.5 A DS3231 on i2c0

The PicoCalc has no clock chip of its own (§1.2). A DS3231 can be added
on `i2c0`, SDA on GP4 and SCL on GP5, which were the development UART's
before it moved to GP2 and GP3. It answers at `0x68`. The time is in
BCD in registers `0x00`-`0x06`, with the century in bit 7 of the month;
`0x0E` is control, where bit 7, `EOSC`, stops the oscillator on the
battery; `0x0F` is status, where bit 7, `OSF`, says the oscillator
stopped and the time is lost. `OSF` is cleared only by writing it 0.
Writing the seconds starts a new second (Maxim's data sheet, 19-5170).

Measured on the Plus 2 W, 2026-10-22, with the UNIX flavour's
development build run from SRAM (`tools/swd-ram.sh`), at 100 kHz with
the RP2350's pull-ups:

- with POWMAN's scratch 0 cleared over SWD, as a power-on clears it, a
  chip answered at `0x68` with `OSF` set, so it had not kept a time
  (`out/m12-ds3231-absent-2.log`);
- `date` set the clock, and the shell wrote the chip and cleared `OSF`
  (scratch 1 then read `0x890000`: *the RTC needs writing* clear);
- with scratch 0 cleared again and the shell restarted, the clock came
  from the chip, 1 s from the Mac's, less the minute it was set
  behind, at the readings' one-second resolution (`out/m12-ds3231-set-2.log`);
- reading it added about 1 ms to the bring-up: the panel was up at
  438.2 ms from reset, against 436.8-437.2 ms with the clock already
  set.

Checked by the owner on the same Plus 2 W, 2026-10-22, with the DOS
flavour's development build on the card as `/BOOT2350.UF2` under UF2
Loader 2.5: the time set with `DATE` and `TIME` was still right after a
power cycle, read from the chip. (The build before M12 step 3, which
had no DS3231 code, lost it, as it should.)

Checked by the owner on a Pico 2 W with no DS3231, 2026-10-22, with the
release build: setting the time returned to the prompt with no pause,
once the write no longer waited for the next second (design.md §3.8),
and `info` said the clock was set by hand.

The southbridge did not answer in the SRAM runs (`version read failed
(-2)`); the cause was not looked into, and the DS3231 is on the other
bus.

Checked by the owner on the same Plus 2 W, 2026-10-23, with the DOS
flavour's W release build: the time was still right after `POWEROFF`
(the southbridge cutting the power) and a power-on with Up held, read
from the chip.

Checked by the owner on a Pimoroni Pico Plus 2 PicoCalc (no radio),
2026-10-24, with the DOS flavour's W release build: `rtc-stop.uf2` set
`EOSC`, and after `POWEROFF` and a power-on the chip had stopped on its
battery, as the data sheet says: the shell said the DS3231 lost its
time, and the clock was not set. The chip is sealed in the case, so this
is the only way to take it away there.

Not yet checked: the time lost per power cycle, below a second; the module's own pull-ups; its drift; and a
board with no chip, which only the host tests cover.

### 8.6 Wi-Fi and NTP on the Plus 2 W

Measured by Claude on the Pimoroni Pico Plus 2 W (RP2350B, its CYW43439
on GP23, 24, 25 and 29, as the Pico 2 W's), 2026-10-22, with the DOS
flavour's W development build (`-DPICO_BOARD=pico2_w`) on the test card
as `/BOOT2350.UF2` and `ntp.uf2` (design.md §3.8) in `/pico2-apps/`,
under UF2 Loader 2.5, logged over UART0 (`out/m12-step4-a.log`, `-b.log`):

- The `pico2_w` build runs on the RP2350B as the `pico2` build does, and
  `cyw43_arch_init_with_country()` brings the radio up.
- stage3 loads `ntp.uf2` (1,363 blocks) with `BOOT_RAM`; the shell's
  check of it took 597-598 ms.
- **The first join attempt can report `CYW43_LINK_NONET` with the
  network in range**, about 1.4 s after `cyw43_arch_wifi_connect_async()`:
  the scan missed it. The SDK's own `cyw43_arch_wifi_connect_timeout_ms()`
  asks again on `NONET` until its timeout; a loop that takes `NONET` as
  final fails every time. With a name that is not in range, the same
  answer came after 1.4 s.
- Joining a WPA2 home network, DHCP, a DNS lookup of `pool.ntp.org` and
  one SNTP exchange: 7.6 s from `ntp.uf2`'s start to the clock set, with
  a half round trip of 23 ms. From Enter on `NTP /F` to the shell's
  prompt: 13.8 s, which includes the check, stage3's two loads, and the
  1.5 s the result stays on the panel. The design's estimate was 5-10 s.
- The time set agreed with the Mac's (NTP-synchronised) to the second
  shown by `TIME /T`, 11 s later.
- The development build mirrors the screen to UART0, so anything typed
  in EDIT or vi, such as `ntp.cfg`'s password, is in the UART log.

Checked by the owner on a Pico 2 W with no DS3231, 2026-10-22, with
the DOS flavour's W release build: `NTP` set the clock, with `ntp.uf2`
in `C:\BIN`, found through `PATH C:\BIN;C:\pico2-apps`. A first
attempt said `no /ntp/ntp.cfg` with the file there: macOS had saved it
as ` ntp.cfg`, with a leading space, which FAT keeps in a long name and
`DIR` does not show in the 8.3 one.

Checked by the owner on the Plus 2 W, reported 2026-10-23: after NTP
set the clock, the shell wrote the DS3231 when it came back (*the RTC
needs writing*, design.md §3.8), and the chip held NTP's time.

Checked by the owner on a Pimoroni Pico Plus 2 (RP2350B, no radio)
with a DS3231, reported 2026-10-23:

- with the DOS flavour's W release build and `NTP` in `AUTOEXEC.BAT`,
  power-on ran `ntp.uf2`, which reported no radio
  (`cyw43_arch_init_with_country()` failed), after about 392 ms as the
  owner recalls the panel, and returned to the shell;
  `AUTOEXEC.BAT` ran on to the prompt without starting it again, and
  `INFO` said the clock was not set;
- with the Pico 2 release build, the date and time set by hand were
  kept through a power cycle, read back from the DS3231.

Checked by the owner on the Plus 2 W with a W release build, reported
2026-10-23: with a wrong password in `ntp.cfg`, the join reported
`CYW43_LINK_BADAUTH`, and `ntp.uf2` gave "the network refused the
password" after 3,862 ms (as its panel said) and returned to the shell.

Not yet checked: a Pico 2 (RP2350A) with the W build, and what driving
its GP23-25 and GP29 does there (the owner has no Pico 2; the Plus 2 is
the nearest board checked); WPA3-only networks, which the WPA2-AES join
does not support.

### 8.7 A copy of the panel to a Tang Nano 20K, on J703

From the owner's project `pico_tang20k_hdmi` (its README and
`docs/PicoCalc_HDMI_Mirror.md`; its measurements, not made here). The
PicoCalc's J703 header carries the pins of the PSRAM chip, which the
board does not need when its PSRAM is unused:

| J703 | GP | Signal |
|---:|---:|---|
| 2-5 | GP2-GP5 | D0-D3, a nibble per clock, high nibble first |
| 6 | GP21 | SCK |
| 7 | GP28 | CS_N, low for a packet |
| 8 | — | GND |
| 1 | — | 3V3_OUT, **not connected** |

GP20 is the PSRAM's chip select, not on J703, and is held high. A packet
is a type byte, `0x00` command, `0x01` data or `0x80` control, and its
bytes; the data are the panel's own, RGB565 high byte first. The FPGA
decodes CASET, RASET, RAMWR, VSCRDEF, VSCSAD, INVOFF/ON and DISPOFF/ON;
the data of a command go on across packets; control `0x01` resets it.

On loose wires with no series resistors and slow edges at 2 mA, 1 MiB went
over with no errors at 1, 5, 10 and 15 MHz (SCK up to about 15 MHz with
the FPGA's glitch filter set to 3) and failed at 20 MHz. Copying costs time at
each redraw: +11.7% for a strip of pixels and +44.3% for a glyph, at
15 MHz, in that project's starter. An FPGA that is powered after the
Pico has missed what was drawn: the state is sent again and the picture
is redrawn. GP4 and GP5 are the DS3231's `i2c0` (§8.5), so the two do not
share a board; GP2 and GP3 are the debug UART's (§1.1).

---

## 9. Making it fast

This section is mostly about **how to measure on this machine**, which turned out
to matter more than any individual optimisation.

### 9.1 Measure on hardware, in the mode you ship

Three separate times a number that looked authoritative turned out to be an
artefact of the mode the harness ran in:

- **Presents can be free by accident.** A blit that returns early in text mode
  reported a present of 0.25 ms. Every "frame time" in a months-long series was
  a *body* time with no present in it.
- **The same bug hid a 25 ms cost.** Clearing the screen in text mode is a bare
  `memset`; in graphics mode it is a full-panel fill. A setup routine
  "regressed" from 20 ms to 66 ms purely because it was finally being measured in
  the mode it runs in.
- **Boundary and clipping modes interact with dirty tracking.** In one wrapping
  mode, a stroke's round cap spilled off the left edge onto the right and
  dirtied the entire tile row, making every measurement read as a full screen.

Corollaries we now follow:

- **Always carry a control** — a quantity in each run that should *not* change.
  When it finally moved 2 %, that told us we had reached the noise floor.
- **Expect ~2 % run-to-run spread** on this machine. Anything smaller is not a
  result. (A 150 MHz baseline read 65.5, 68.8 and 68.1 ms across three sittings
  on the same board.)
- **One board per comparison.** Different flash parts mean different
  instruction-fetch behaviour; we burned real time drawing a conclusion from the
  difference between a Pico 2 measurement and a Plus 2 W one.
- **Write measurements to a file.** A number on a 320×320 screen cannot be
  copied off it.

### 9.2 Instruction fetch through the XIP cache is a first-order cost

For large, branchy workloads, instruction fetch can dominate. Flash code uses a
16 KB cache shared by both cores and by PSRAM data. Moving hot functions into
SRAM with `__not_in_flash_func` produced, on a Plus 2 W:

| Tier | What moved | Frame | Speedup | SRAM cost |
|---|---|---:|---:|---:|
| — | baseline | 81.0 ms | | |
| 1 | 4 hot functions (3.1 KB of expression evaluator) | 65.5 | **1.24×** | 6.0 KB |
| 2 | 8 more (call path, variable lookup) | 53.2 | **1.23×** | 3.6 KB |
| 3 | loop and run-list path | 48.1 | 1.105× | 1.7 KB (fit in existing padding) |
| 4 | frames and bindings | 47.0 | 1.024× | 2.0 KB |

**1.72× cumulative for 13.6 KB of SRAM**, verified each time by checking that
the symbols moved from `0x1…` to `0x2…` in the image.

Three lessons that generalise well beyond an interpreter:

1. **Returns halve every tier.** Stop when the curve says so, not when the
   budget runs out.
2. **Every move relays the flash residue against the cache, and whatever is left
   adjacent to the boundary gets worse.** Tier 1 improved the evaluator and
   regressed procedure calls 29 %; tier 2 fixed calls and regressed the loop
   67 %; tier 3 fixed the loop and regressed calls again. It is self-limiting —
   the regressing line is always smaller than the set that improved — but **you
   must re-measure the whole profile after every move**, not just the thing you
   moved.
3. **It is not universal.** The same technique applied to a tight tile-copy loop
   produced *nothing* (§9.3). Instruction fetch dominates code that is large and
   branchy; a copy loop is data-bound and does not care. Measure before spending
   SRAM.

The functions worth moving were the big branchy ones **and** the tiny
constantly-called accessors (120–164 bytes, the best value per byte). Whole
modules are the wrong unit; individual hot functions are the right one.

### 9.3 Where the data lives matters as much as where the code lives

A bulk copy from PSRAM into an SRAM canvas, in 8-byte runs, measured **7.45–7.6
ms for 64,512 pixels** on a Plus 2 W — about 118 ns per pixel, only ~2× faster
than the SPI wire, on a 150 MHz core. We chased it:

| Suspect | Verdict |
|---|---|
| Per-row bookkeeping | **Red herring** — ~20 µs total. |
| Code not RAM-resident | **Disproved by experiment.** Moving the loop and its helpers to SRAM (+2 KB) gave 7.45 → 7.6 ms: no change, or slightly worse. Reverted. |
| **The data path** | **The leading explanation.** Source in PSRAM, destination in SRAM, small runs. That fits 118 ns/px far better than instruction fetch does. |

So: **keep per-frame working sets in SRAM.** Use PSRAM for capacity, not for the
hot path — or at least measure before assuming the XIP cache hides it.

### 9.4 Small `memcpy` calls are call overhead

A sampler that copies one 8-pixel tile run at a time makes 28 calls per row and
8,000 for a screen. At that size the call *is* the cost. Hand-unroll, or
specialise for the two or three run lengths you actually support.

### 9.5 Core 1 can own slow peripherals

A working split at **150 MHz** gives core 0 CPU-intensive simulation and audio,
and core 1 the LCD and southbridge. Use an immutable frame handoff with explicit
ownership and a bounded input-event queue. Polled LCD DMA needs no interrupt;
the audio DMA IRQ can remain on core 0. Local interrupt masking is not a
multicore lock.

In a measured emulator workload on a Plus 2 W, initial flash execution managed
**0.55–0.60× real time**, with about 84% of wall time in guest execution and
12% in 10 kHz I2C polling. Moving peripheral work to core 1, executing code from
SRAM, scoped LTO and CPU/audio optimizations together restored short-run
real-time operation. This does not isolate a speedup for any single change.
A **20 µs hardware-timer wait** between peripheral service iterations avoided
continuous shared-state polling while DMA was active.

Short captures had zero remaining lag, PCM underruns, late DMA refills, input
loss and device errors. More than an hour of clean visual/audio operation was
subsequently reported on each of a Pico 2 W and Plus 2 W; those long observations
had no contemporaneous UART capture. They demonstrate a viable ownership split,
not a universal compute-headroom guarantee.

Use compact source snapshots or generated bands when full-frame double
buffering would consume too much SRAM. Keep producer writes away from any
buffer still owned by DMA. Both cores share the XIP cache, so measure the main
workload with and without peripheral activity before assuming ideal overlap.
Coordinate flash writes with both cores (§7.2).

### 9.6 Levers, in order

If you need to reduce frame time:

1. **Transmit less area.** A smaller source only helps wire time if the driver
   also narrows the window; unchanged HUD regions need not be resent.
2. **Present less often than you simulate.** Retain simulation timing while
   dropping superseded presentation snapshots.
3. **Use hardware vertical scroll where appropriate** (§4.8).
4. **Overlap peripheral work on core 1** with explicit ownership (§9.5).
5. **Measure SRAM placement of hot code** before spending more memory (§9.2).
6. **Evaluate clock changes only with all peripheral clocks accounted for**
   (§3). At a requested 75 MHz SPI rate, 300 MHz has favorable divider arithmetic
   compared with 200/250 MHz, but it remains an RP2350 overclock.

### 9.7 `sleep_us` on core 1 interrupts core 0

`sleep_us`, `sleep_until` and `best_effort_wfe_or_timeout` set an alarm in
the SDK's **default alarm pool**, and that pool's IRQ belongs to the core
that initialised it — core 0, in `runtime_init`. A core 1 loop that sleeps
for 20 µs between iterations (§9.5's pattern) therefore takes an interrupt
**on core 0** every iteration, tens of thousands a second, in the middle of
whatever core 0 is doing.

In a 6502 emulator, on a Plus 2 W, switching core 1's wait to
`busy_wait_us_32(20)`, which polls the timer and sets no alarm, cut core 0's
cost per emulated instruction by 1.11×. Run-to-run spread fell from ±4 to
±0.1 cycles per instruction. It is still a hardware-timer wait rather than
spinning on shared state, which is what §9.5 asked for.

So measure core 0 with core 1 idle as well as busy. If core 0's timing
wanders while core 1 has nothing to do, look for sleeps on core 1. Use
`busy_wait_*` there, or give core 1 its own alarm pool.

**Check drivers when they change cores.** An SD driver brought over from
another project had one `sleep_us` in its initialisation; on core 1, it
would have interrupted core 0. Search any code bound for core 1 for `sleep_` and
`best_effort_wfe_or_timeout`.

### 9.8 SRAM placement, measured on an interpreter

§9.2's tiers, applied to a 6502 interpreter, gave
**1.12–1.19× for 25 KB**, not 1.7×. The same method, on a different kind
of code, gave a third of the gain:

- `-O3` inlined the bus into every opcode and made the interpreter 20 KB.
  But a real program uses a few dozen opcodes, and that part fits the
  16 KB cache.
- Moving the small callees first (4.4 KB) was worth 1.04–1.11×, and it
  regressed one workload 4 % (§9.2's relayout effect). The interpreter on
  top was worth another 1.06–1.08× on every workload, and it recovered
  the regression.
- A 256-byte const table moved to SRAM measured nothing.

Two build traps cost a pass each. **Build each tier in its own build
directory.** Make judges freshness by mtime, and a reconfigure that lands in
the same second as the last compile leaves stale objects: an image with half
its tier. **Check every symbol's address in each image**, not just the
first build's. Whatever an SRAM function calls in flash goes through a
long-branch veneer (`__name_veneer` in `nm`), so the veneer list is the
quick check that a tier is whole.

A third trap: **`static inline` is not a promise.** At `-O3`, GCC kept
out-of-line copies of a Z80 interpreter's small memory helpers (read,
16-bit immediate, push, pop) in ordinary `.text`, and the SRAM tier called
them in flash through veneers. Give such helpers the same section
attribute as their callers. It costs nothing where they are inlined.

The converse trap: **a hot function inlined into a cold caller runs where
the caller is.** A 6502 emulator (2026-10-08, gcc 15.2 `-O3`) marked its
run loop for SRAM, and its benchmark, which called the loop directly,
found it there. The emulator called it only from a once-a-field wrapper
in ordinary `.text`; GCC inlined the loop into the wrapper and dropped the
SRAM copy, so `nm` showed no symbol for it, and the loop ran from flash
and called the interpreter in SRAM through a veneer every instruction.
Mark the caller too, and look for every hot name in `nm`, not only the
ones that are there.

The same method on a Z80 interpreter (2026-10-03, Plus 2 W at 150 MHz, 30 KB
in SRAM) gave the same shape: **1.20×** on ZEXDOC's wide instruction mix,
and nothing (−1.2 %) on a small Forth inner-interpreter loop whose opcodes
already fit the cache.

The whole emulator around that Z80 interpreter (2026-10-04, same board and
clock, its real ROM, core 1 presenting) showed what the benchmark could
not, because the benchmark had the XIP cache to itself:

- **With the interpreter in flash, layout outside it decided the result.**
  Four builds whose differences were all outside the interpreter read
  37.9, 41.1, 34.2 and 31.8 % of core 0 on one compute loop, each steady
  within its run. With the interpreter in SRAM (33 KB), the same loop read
  23.3 % in three sittings, to 0.1 point. So measure placement before any
  other change, and compare other changes only at the placement you ship.
- **Once the interpreter left the cache, 8 KB of guest ROM data fitted it.**
  Copying the ROM into SRAM at boot changed nothing at that tier (to 0.1
  point), against 0.1–1.3 points with the interpreter still in flash.
- **Audio's cost fell with it.** The audio code cost the foreground 0.4–2.0 points with the interpreter in flash, mostly as extra
  cycles per guest instruction rather than time in audio code, and 0.4–0.8
  with it in SRAM.

A 6502 interpreter's benchmark agreed (2026-10-07, Plus 2 W at 150 MHz,
gcc 15.2 `-O3`, one core, the cache to itself). From flash it took 142.3
host cycles per instruction on a BASIC-shaped loop and 140.9 on Dormann's
functional test. **Moving only the small callees** (bus slow path, device
tick, ADC/SBC, 1.8 KB) **made it slower**, 154.7 and 147.6: the interpreter
stayed in flash and now called those functions through veneers. **Moving the
interpreter as well** (24 KB) gave 144.7 and 138.8, within 2 % of flash
either way. Each figure held to 0.02 % over seven to nine runs. So a tier of
callees alone is not a step on the way. Measure it, or skip it.

A fourth build trap: **CMake caches options in the build directory.** When a
project changes an option's default (here the SRAM tier), a directory
configured earlier keeps its cached value until the option is passed once.
A fresh directory per tier (above) avoids it.

---

## 10. Bring-up order and trap checklist

A practical bring-up sequence for `main()`; reserve DMA channels and IRQ
ownership explicitly rather than relying on initialization order:

1. **Southbridge I²C** — nothing else needs it, but the backlight does, and a
   dead bus is the first thing you want to know about.
2. **LCD** — reset, init sequence, clear frame memory, *then* `DISPON`.
3. **Keyboard** — on top of the southbridge.
4. **Audio** — the chained-buffer recipe needs two DMA channels and an IRQ;
   confirm that they do not conflict with display or storage allocations.
5. **PSRAM verification** (Plus 2 W), *before* handing the region to an
   allocator and before any flash mount, since flash writes drive the same QMI.
6. **Filesystems** — internal flash, then SD.
7. **Wi-Fi** — lazily, on first use; it costs a second and a lot of SRAM.

Checks to retain in each new driver/application:

- [ ] `spi_set_format()` and the `D/CX` write go **before** CS low — the 40 ns
      CS-high rule (§4.3).
- [ ] Drain the SPI RX FIFO and clear the overrun flag at the end of every DMA
      blit; restore 8-bit format.
- [ ] Never touch the LCD from an interrupt handler. Timers set flags.
- [ ] Never mask interrupts around a blit — audio has a 3.5 ms deadline.
- [ ] Audio DMA ring: power-of-two **and aligned**, with the hardware read wrap.
- [ ] Re-arm both read address **and** transfer count when re-arming a chained
      DMA channel from its IRQ.
- [ ] Audio IRQ priority above default (`0x40`); nothing else may run long at
      default priority.
- [ ] Every function on the audio IRQ path `__not_in_flash_func`.
- [ ] Verify PSRAM with a real round trip before handing it to an allocator.
- [ ] Re-apply the SPI baud rate, the PSRAM QMI timing, the CYW43 divider and
      the audio carrier after **any** change to `clk_sys` (§3).
- [ ] A firmware that ever raises the core rail sets it explicitly at 150 MHz
      too; the regulator survives a reset (§3).
- [ ] No `sleep_us`/`sleep_ms` in core 1's loop; `busy_wait_us_32` (§9.7).
- [ ] No blocking `printf` on a core with a deadline (§2.7).
- [ ] SD waits bounded below the southbridge's 2.5 s watchdog when one core
      does card work and keyboard polling (§7.1).
- [ ] Counters read over SWD read twice and kept only when they agree (§2.7).
- [ ] A Mac's display kept awake (`caffeinate -d`) while the Debug Probe is
      in use (§2.7).
- [ ] Each SRAM-placement tier built in its own build directory, and its
      symbols checked with `nm` (§9.8).
- [ ] Remap y through the vertical-scroll offset in *every* blit path, or do not
      use hardware scroll at all.
- [ ] Over-mark dirty regions; snapshot and clear before sending.
- [ ] No flash writes inside a frame loop; coordinate both cores and DMA
      before taking flash out of XIP mode (§7.2).
- [ ] Poll the keyboard from the frame loop, not a timer IRQ; guard the I²C bus
      with a busy flag; build held-key state from press/release events.
- [ ] Budget SRAM for frame storage, code, queues, heap and both-core stacks.
- [ ] Count PCM underruns separately from late DMA refills.
- [ ] Normalize shifted key releases and distinguish auto-repeat from fresh presses.
- [ ] Log physical board identity separately from the SDK build target.
- [ ] Profile in the mode you ship, with a control quantity, expecting 2 %
      spread, on one board.

---

## 11. Official references

- [ClockworkPi PicoCalc repository](https://github.com/clockworkpi/PicoCalc) —
  hardware, firmware, assembly instructions and controller documentation.
- [PicoCalc mainboard V2.0 schematic](https://github.com/clockworkpi/PicoCalc/blob/master/clockwork_Mainboard_V2.0_Schematic.pdf) —
  check carrier wiring and revision-specific details before using spare pins.
- [ST7365P controller specification](https://github.com/clockworkpi/PicoCalc/blob/master/ST7365P_SPEC_V1.0.pdf) —
  command, memory and electrical timing reference for that controller.
- [ClockworkPi firmware examples](https://github.com/clockworkpi/PicoCalc/tree/master/Code) —
  panel initialization and platform integration examples.
- [Keyboard MCU implementation](https://github.com/clockworkpi/PicoCalc/blob/master/Code/picocalc_keyboard/keyboard.ino),
  [register definitions](https://github.com/clockworkpi/PicoCalc/blob/master/Code/picocalc_keyboard/reg.h),
  and [key definitions](https://github.com/clockworkpi/PicoCalc/blob/master/Code/picocalc_keyboard/keyboard.h) —
  protocol dispatch, translated events and reserved chords.
- [Raspberry Pi board documentation](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html)
  and [Pico SDK documentation](https://www.raspberrypi.com/documentation/pico-sdk/) —
  board selection, clocks, peripherals and SDK APIs.
- [RP2040 datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf)
  and [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf) —
  silicon limits, peripheral semantics and errata.

Match the schematic, controller and keyboard firmware to the actual device.
Upstream source can change; record the revision used when qualifying a driver.
