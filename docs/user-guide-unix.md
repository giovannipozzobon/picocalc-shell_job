# PicoCalc shell, UNIX flavour — user guide

The shell is a small UNIX-like command line for the ClockworkPi PicoCalc
with an RP2350 board. From it you manage the files on the SD card, edit
text files in vi, and run programs by name. This guide covers every
command and the command line itself.

The shell also comes in a DOS flavour, with `DIR`, `COPY`, batch files
and EDIT; it has [its own guide](user-guide-dos.md). Each flavour is a
separate `BOOT2350.UF2`. To install one, see the [README](../README.md).

## Contents

1. [Getting started](#1-getting-started)
2. [Keys](#2-keys)
3. [The command line](#3-the-command-line)
4. [Commands](#4-commands)
5. [Running programs](#5-running-programs)
6. [vi](#6-vi)
7. [The card](#7-the-card)
8. [Messages and limits](#8-messages-and-limits)

## 1. Getting started

Hold **Up** (or F1 or F5) while switching the PicoCalc on, and the shell
starts. Powering on with no key held starts the last program you ran.

With the shell's own build of UF2 Loader installed (see the README), it
is the other way round: switching on gives the shell, and holding Up (or
F1 or F5) starts the last program.

The prompt is the current directory followed by `>`:

```
/home>ls
notes/  todo.txt
/home>cat todo.txt
buy batteries
/home>cd /oric
/oric>
```

The shell starts in `/home` if the card if it exists, and otherwise in `/`.

`man` lists the commands, and `man name` provides detail:

```
/>man cp
usage: cp [-r] from... to
Copies files, -r directories
```

## 2. Keys

### At the prompt

| Key | Does |
|---|---|
| Left, Right | move along the line (also Ctrl-B, Ctrl-F) |
| Home, End | the start and end of the line (also Ctrl-A, Ctrl-E). Home is Shift+Tab, End is Shift+Del |
| Backspace, Del | delete before or under the cursor |
| Ctrl-U, Ctrl-K | delete to the start, or to the end, of the line |
| Up, Down | earlier and later lines from the history (also Ctrl-P, Ctrl-N) |
| Tab | complete a command or a path (below) |
| Ctrl-C | drop the line |
| Enter | run the line |

### While a command runs

**Esc**, **Break** (Shift+Esc) or **Ctrl-C** stops a long command, such
as a large `cp`, and drops the rest of the line. The shell prints `^C`.

### Tab completion

For the first word, Tab completes a command: a builtin, or a program in
`$PATH`, without its `.uf2`. On the rest of the line, it completes a path.

- One match goes in whole, followed by `/` for a directory or a space
  for anything else.
- With several matches, Tab adds the common start, and a second Tab
  displays the completions.
- Names are expanded as stored on the SD Card, with `\` before a
  space or any special character.

### The PicoCalc's own keys

Alt+`,`, Alt+`.`, Alt+Space and Alt+B are used by the PicoCalc for the
backlights and the battery. The `backlight` and `battery` commands
(section 4) do the same from the command line.

## 3. The command line

The shell is deliberately small. It has UNIX spelling but no `if`, `for`
or functions.

### Words and quoting

A line is split into words at spaces. To keep a space, or any special
character, in a word:

- `'single quotes'` keep everything as typed;
- `"double quotes"` keep everything but `$`, so variables still work;
- `\` keeps the next character: `my\ file.txt`.

`#` starts a comment, to the end of the line.

### Several commands

| Write | Runs |
|---|---|
| `a ; b` | `a`, then `b` |
| `a && b` | `b` only if `a` worked |
| `a \|\| b` | `b` only if `a` failed |
| `a \| b` | `a`, with its output given to `b` as input |

### Redirection

| Write | Does |
|---|---|
| `cmd > file` | the output into `file`, replacing it |
| `cmd >> file` | the output added to the end of `file` |
| `cmd < file` | `file` as the command's input |

A redirected file is written safely: if the command fails or is stopped,
the old file is left as it was. Error messages always go to the screen,
never into the file.

`cmd > /dev/null` throws the output away, to test a command without
seeing what it says: `date > /dev/null || ntp`. There is no `/dev`
directory on the card; only that one name, after `>` or `>>`, is
special.

### Pipes

`a | b` runs `a` to the end, then gives its output to `b`. A pipe can
have several stages:

```
/>ls /pico2-apps | grep oric
pico-oric.uf2
/>cat log.txt | grep -n error | more
```

Up to 16 KB is held in memory. Larger output goes through a temporary
file in `/.shell/`, which is removed afterwards, so a pipe is limited only
by space on the card.

`more`, `grep`, `head`, `tail`, `wc` and `cat` read a pipe or a `<` file
when they are given no file names.

### Globs

`*` matches any characters, `?` matches one character, and `[abc]` or
`[a-z]` matches one of a set (`[!a-z]` matches one not in it):

```
/>rm *.bak
/>ls /oric/keymaps/[a-m]*
```

- Letters match without regard to case, as FAT names do.
- A pattern that matches nothing stays as typed, so `ls *.xyz` says
  `no such file`.
- Hidden names match only a pattern that starts with `.`.
- Only the last part of a path is expanded: `*/x` stays as typed.

### Variables

```
/>NAME=world
/>echo hello $NAME
hello world
/>echo ${NAME}s
worlds
```

- `$NAME` or `${NAME}` is a variable's value, and `$?` is the last
  command's status: 0 if it worked.
- `NAME=value` on its own sets a variable. `NAME=value cmd` is not
  supported.
- An unset variable is empty.
- Variables last until a command is run or reboot. There is room for 1 KB of them.

Two variables matter to the shell:

| Variable | Default | Used for |
|---|---|---|
| `HOME` | `/home` | `cd` with no directory, and where the shell starts |
| `PATH` | `/pico2-apps` | where programs are looked for, directories separated by `:` |
| `TZ` | (none: UTC) | the time zone, as a POSIX string: `export TZ=EST5EDT,M3.2.0,M11.1.0` |

`TZ` names the standard zone and how many hours it is behind UTC, then
the summer zone, if there is one, and when summer time starts and ends:
`M3.2.0` is the second Sunday of March, at 2:00. Without the dates, the
United States' are used. Some others: `CST6CDT`, `MST7MDT`, `PST8PDT`,
`GMT0BST,M3.5.0/1,M10.5.0`, `CET-1CEST,M3.5.0,M10.5.0/3`,
`AEST-10AEDT,M10.1.0,M4.1.0/3`. Put it in `/etc/profile`. Names of
places, such as `America/Toronto`, are not understood, and give UTC.

### Statuses

Every command gives a status: 0 when it worked, and otherwise a number
that says how it failed.

- 1 is an ordinary failure.
- `grep` gives 1 for "nothing found" and 2 for an error.
- 127 means the command was not found.
- 130 means it was stopped with Esc, Break or Ctrl-C.

`&&`, `||` and `$?` use the status.

### History

The shell keeps about 2 KB of the lines you have typed. Up and Down navigates
through the history, and `history` displays the contents. A blank line, or one
the same as the one before, is not kept.

The history is saved to the card (`/.shell/history`) when you leave the
shell: for a program, `menu`, `usbdisk`, `reboot`, `poweroff` or
`bootsel`. Lines typed since the last save are lost if the power is
switched off.

### Scripts

`sh file` runs a file of commands, one line at a time. A mistake is
reported with the file's name and line number, and the script goes on.

`/etc/profile`, if exists, runs every time the shell starts.
You can use this file to store your own settings:

```
# /etc/profile
phosphor amber
PATH=/pico2-apps:/games
cd /home
```

Scripts can call other scripts, up to three deep.

To be reminded at power-on to set the clock, only when it is not set
(`date` says `date: clock not set` then, with status 1):

```
# /etc/profile
export TZ=EST5EDT
date > /dev/null || echo set the clock: date mmddHHMM
```

With the Pico 2 W build, `ntp` first sets it from the network when it
can:

```
# /etc/profile
export TZ=EST5EDT
ntp || echo set the clock: date mmddHHMM
```

With a DS3231 fitted too, the shell has read the chip before
`/etc/profile` runs, so the clock is already set unless the chip lost
its time. To use the network only then:

```
# /etc/profile
export TZ=EST5EDT
date > /dev/null || ntp || echo set the clock: date mmddHHMM
```

The time NTP gives is written to the chip, ready for the next power-on.
The chip drifts a little; `ntp -f` now and then puts it right.

`ntp` runs `ntp.uf2` only once each power-on, so these do not run it
again each time the shell comes back from a program.

### Menus

`select` shows a list of words, numbered, and asks you to choose one:

```
/>select p in pico-oric pico-atom vi break; do $p; done
1) pico-oric  3) vi
2) pico-atom  4) break
#? 2
```

It sets the variable (`p` here) to the word you chose, and runs the
commands between `do` and `done`. Then it asks again, until a command
runs `break`. Type a number and Enter. Enter on its own shows the list
again. A number not in the list sets the variable to nothing. Break,
Esc or Ctrl-C stops it.

There is no `if` or `case` in this shell, so make each word something
you can run, as above: `$p` runs the program chosen, and choosing
`break` runs `break`. A wrong number does nothing, because an empty
`$p` runs nothing.

What you typed is also in `$REPLY`, and the question `#? ` is `$PS3`:
set it to change it. The words can be a glob, such as `/home/*.txt`.

In a script, a menu can take several lines:

```
# /etc/profile: a menu at every start
PS3='Run which? '
select p in pico-oric pico-atom pico-ace
do
    echo Starting $p
    $p
done
```

Esc leaves this menu for the prompt. At the prompt, the whole menu
must fit on one line. A menu can hold another, and `break 2` leaves two
at once.

## 4. Commands

Every command is built in. Any other name runs a
program (section 5).

### Finding your way

| Command | Does |
|---|---|
| `pwd` | prints the current directory |
| `cd [dir]` | changes directory; to `$HOME` with none. `..` is the directory above |
| `ls [-l] [-a] [path...]` | lists a directory, sorted, with `/` after each directory's name. `-l` adds the size and date, and `d` for a directory. `-a` includes hidden names |

```
/>ls -l /home
-rw       24 Oct 22 14:30 a.txt
-rw      310 Mar  3  2025 old.txt
drw          Oct 20 09:12 notes/
```

`ls -l` shows the time for a date within six months of now, and the
year in place of the time for one further away. A name longer than 14
characters wraps to the next row.

### Files and directories

| Command | Does |
|---|---|
| `cat file...` | shows files one after another, or joins them: `cat a b > c` |
| `cp [-r] from... to` | copies a file, or several files into a directory. `-r` copies directories and everything in them |
| `mv from... to` | moves or renames files and directories |
| `rm [-r] path...` | removes files. `-r` removes directories and everything in them. **There is no undo** |
| `mkdir [-p] dir...` | makes directories. `-p` makes the ones above too, and does not mind if it is already there |
| `rmdir dir...` | removes empty directories |
| `touch file...` | makes empty files. A file that exists gets the time now, if the clock is set; otherwise it is left alone |
| `df` | the card's format, size, space used and space free |

Names on the card are case-insensitive but keep their case: `cat
TODO.TXT` reads `todo.txt`, and `ls` shows it as `todo.txt`.

A file written or made gets the date and time, in local time, as a
computer does. `cp` gives the copy the time now; `mv` keeps the file's.
With the clock not set (see `date`), every file gets the same fixed
date, 1 January 2026.

### Reading text

| Command | Does |
|---|---|
| `more file...` | shows text a screen at a time |
| `head [-n N] [file...]` | the first N lines, 10 if not given |
| `tail [-n N] [file...]` | the last N lines, 10 if not given |
| `grep [-v] [-n] [-c] [-i] pattern [file...]` | the lines that match a pattern |
| `wc [-l] [-w] [-c] [file...]` | counts lines, words and bytes |

Each of these reads the files named, or its input from `<` or a pipe.

**more** stops after each screenful at `--More--`. Then:

| Key | Does |
|---|---|
| Space | shows the next screenful |
| Enter or Down | shows one line more |
| q, Esc, Break or Ctrl-C | stops |

When its output goes to a file or a pipe, `more` just copies.

**head** and **tail** take `-n 5`, `-n5` or `-5`. With several files,
each one starts with `==> name <==`. `tail` keeps the last 4 KB of what
it reads. If the lines you ask for are longer than that, it shows the
whole lines that fit and says so.

**grep** uses vi's patterns, which always ignore case:

| Pattern | Matches |
|---|---|
| `.` | any character |
| `*` | any number of the one before |
| `^` and `$` | the start and the end of the line |
| `[abc]`, `[a-z]` | one character of a set |
| `\<` and `\>` | the start and the end of a word |

- `-v` gives the lines that do not match.
- `-n` adds line numbers.
- `-c` gives only how many lines matched.
- `-i` is accepted, and changes nothing.

With several files, each line starts with its file's name. The status is
0 if something matched, 1 if nothing did, and 2 on an error.

```
/>grep -n '^speed' /oric/pico-oric.cfg
4:speed=fast
```

**wc** shows lines, words and bytes, or just the ones asked for with
`-l`, `-w` or `-c`. Several files get a total line too.

```
/>wc notes.txt
     12      85     502 notes.txt
/>ls | wc -l
7
```

### Editing

| Command | Does |
|---|---|
| `vi [-R] file` | edits a text file. `-R` opens it read-only |
| `view file` | the same as `vi -R` |

See section 6.

### The screen

| Command | Does |
|---|---|
| `clear` | clears the screen |
| `phosphor [white\|green\|amber\|default]` | the colour of the text, like an old monitor's phosphor. `phosphor` alone shows the current one |
| `echo [-n] [word...]` | prints its words. `-n` leaves off the line break |

`phosphor` lasts until the shell is left. Put it in `/etc/profile` to
have it every time.

### The PicoCalc

| Command | Does |
|---|---|
| `battery` | the battery's charge, and `charging` when the charger is on |
| `backlight [lcd\|kbd] [0-255]` | shows or sets the screen (`lcd`) or keyboard (`kbd`) backlight |
| `info` | the shell's version, its build (Pico 2 or Pico 2 W, with `BT` if it has the Bluetooth keyboard and `hdmi` if it has the HDMI mirror), the chip, memory and PSRAM (0 MB on a board without it), UF2 Loader, the clock, the keyboard's firmware, the card and the battery |
| `date [-u]` | the date and time; `-u` in UTC. Says `clock not set`, with status 1, when it is not |
| `date [-u] mmddHHMM[[cc]yy]` | sets the date and time: month, day, hour, minute and the year, which may be left off. `-u` takes it as UTC |
| `ntp [-f]` | in the Pico 2 W build: sets the clock from the network, once each power-on; `-f` tries again |
| `web` | in the Pico 2 W build: manages the card from a browser, over Wi-Fi, until Esc |
| `bt` | in the Pico 2 W build: what the Bluetooth keyboard is doing |
| `hdmi` | in a build made for it: sends the screen to the HDMI mirror again |

`date`:

- The clock keeps going while a program runs, and through `reboot`.
  Switching the PicoCalc off loses it, unless a DS3231 clock chip is
  fitted, on GP4 (SDA) and GP5 (SCL): setting the time sets the chip
  too, and the shell reads it back at power-on. If the chip lost its
  time (it is new, or its battery is flat), the shell says
  `shell: the DS3231 lost its time` at power-on, but not again after a
  program, until the clock is set. The chip holds 2000 to 2199.
- The time is set to the minute, in local time, given by `TZ` (below).
  The year is 1980 to 2099; `yy` from 69 to 99 is the 1900s.
- `date` in `/etc/profile` with a test can remind you to set it
  (Scripts, section 3).

```
/>date 1022143026
Thu Oct 22 14:30:00 EDT 2026
/>date -u
Thu Oct 22 18:30:00 UTC 2026
```

`ntp`:

- The shell comes in two builds of each flavour: one for the Pico 2,
  and one for the Pico 2 W (`shell-unix-w.uf2`, copied to the card as
  `/BOOT2350.UF2`), which also has `ntp`. The Pimoroni Pico Plus 2 W
  takes the W build. `info` names the build.
- `ntp` sets the clock over Wi-Fi from an NTP server. It runs the
  program `ntp.uf2`, found in `$PATH` as any program is (put it in
  `/pico2-apps/`), which shows what it is doing, and comes back to the
  shell. Esc stops it. Without it, `ntp` says `ntp: not found`, with
  status 127.
- It runs once each power-on, and `ntp.uf2` shows how it went before it
  comes back. After that, `ntp` says nothing if NTP set the clock, with
  status 0, and why not if it did not (as below, `ntp: no time from the
  server`), with status 1. So `ntp` in `/etc/profile` does not run it
  again each time the shell comes back from a program, or loop when the
  network is down. `ntp -f` runs it again.
- With a DS3231 fitted, the shell writes the time NTP gave to the chip
  when it comes back.
- Its settings are in `/ntp/ntp.cfg`, which you write with vi:

```
# /ntp/ntp.cfg
ssid     = Home                 # the network's name
password = "my # password"      # left out for an open network
server   = pool.ntp.org         # the default
country  = CA                   # Wi-Fi's country; worldwide if left out
timeout  = 20                   # seconds to join and get the time, 5 to 60
```

- Only `ssid` is needed. A `#` after a space starts a comment, so a
  value holding one, or starting or ending with a space, goes in
  double quotes. The password is kept on the card as written: WPA2's 8
  to 63 characters, or 64 hex digits. Each key once.
- `ntp` in `/etc/profile` sets the clock at power-on, and with a
  DS3231 fitted, only when the chip has no time (Scripts, section 3).

| `ntp.uf2` and `ntp` say | Means |
|---|---|
| `no /ntp/ntp.cfg` | write the file, as above |
| `a mistake in /ntp/ntp.cfg` | `ntp -f` shows which line, and what is wrong with it |
| `no Wi-Fi on this board` | a Pico 2 W build on a Pico 2 |
| `the network was not joined` | no network by that name in range, or it would not take the PicoCalc |
| `the network refused the password` | check `password` |
| `the network gave no address` | joined, but its DHCP server did not answer in time |
| `the server was not found` | the server's name is wrong, or the network's DNS did not answer |
| `no time from the server` | the server did not answer in time |
| `stopped` | Esc, Break or Ctrl-C |
| `ntp.uf2 did not finish` | the program was cut off: power, or a reset |


`web`:

- `web` manages the card from a browser on the same network: list
  folders, upload (drag files onto the page, or paste them), download,
  rename, move, delete and make folders. It runs the program `web.uf2`,
  found as `ntp.uf2` is, which joins the network, shows its address on the
  screen and answers a browser there, and at `http://picocalc.local/`. Without it, `web` says `web: not found`.
- It joins the network `/ntp/ntp.cfg` names (the file `ntp` reads; only `ssid`,
  `password`, `country` and `timeout` matter to it), written with vi.
  It joins as a client of the network: it makes no network of its own.
- **There is no password.** While it runs, anyone on the network can read
  and change everything on the card. Use it on a network you trust.
- Esc, Break or Ctrl-C on the PicoCalc stops it and comes back to the
  shell.
- A file is written to `name.new` first and put in place when it is whole,
  so a transfer that breaks off leaves the old file as it was. Names are
  shown as UTF-8; a name from the browser that the card's character set
  (CP437) cannot spell is refused.
- Installing a program from the page is not there yet.
- It is the Pico 2 W build's; with `web` on a Pico 2 you get
  `web: not on this machine`.

| `web.uf2` says | Means |
|---|---|
| `The card cannot be read.` | no card, or one that is not FAT |
| `Line N: ...` | a mistake in `/ntp/ntp.cfg`; each key once |
| `It names no network.` | no `ssid` in the file |
| `The password was refused.` | check `password` |
| `The network was not joined.` | no network by that name in range |
| `No address from the network.` | joined, but DHCP did not answer in time |
| `No radio.` | a Pico 2 W build on a Pico 2 |

`bt`:

- A Bluetooth keyboard works at the prompt, in vi and in everything that
  reads keys, alongside the PicoCalc's own keyboard, in the Pico 2 W build.
  Classic Bluetooth keyboards, and Bluetooth Low Energy ones (btpair marks
  them `(BLE)`), are supported; the Low Energy ones have not been tried
  in the shell yet.
- It is **paired with `btpair`**, the program of
  [picocalc-extras](https://github.com/ctorkildson-netizen/picocalc-extras/tree/main/bluetooth),
  not here: copy `btpair.uf2` to `/pico2-apps/`, run it, and follow it. It
  keeps the pairing in `/bluetooth/pairing.dat`, which the shell reads.
- The radio's firmware is a file the shell reads from the card, so that it
  does not take the shell's memory: copy `cyw43-fw.bin` (made with the
  shell: the one of the same build) to `/bluetooth/`.
- With **both files on the card**, the shell starts the radio right after
  the first prompt, in about a second, and the keyboard connects when you
  press a key on it (or within a few seconds if it is awake). With no
  `pairing.dat` the radio is never started. If it cannot be, the shell says
  `bluetooth: why` in one line and carries on with the PicoCalc's keyboard.
- `bt` says what it is doing: `connected: K380`, `waiting for K380`, `no
  keyboard paired`, or why it is off, such as `off: no firmware file on
  the card`. `info` has the same line.
- Most Low Energy keyboards choose a new identity each time they are put
  into pairing mode, so the PicoCalc no longer knows them: if one stops
  connecting, pair it again in `btpair`. A Bluetooth mouse can be paired
  beside the keyboard (btpair 2.1 keeps one of each): it connects when
  the radio starts, but the shell does not use it, and `bt` speaks of the
  keyboard only.
- A program (`web`, an emulator) runs without it: the radio goes with the
  shell, and starts again, with the keyboard connecting again, when the
  program returns.
- The radio takes some of the processor's time: `usbdisk` reads about a
  fifth slower with it on (writing is as fast). On a Pico 2 there is no
  radio and no `bt`.

`hdmi`:

- A shell built with `-DPICO_SHELL_HDMI=ON` (a build of your own, with
  `-DPICO_SHELL_UART=OFF`) copies everything it draws to a Tang Nano 20K
  on the PicoCalc's J703 header, which shows the screen on HDMI. The
  gateware and the wiring are the project `pico_tang20k_hdmi`'s. In any
  other build `hdmi` says `hdmi: not on this machine`.
- Power the FPGA before the PicoCalc and nothing more is needed. If it
  came up later, or a cable moved, type `hdmi`: the shell sends the FPGA
  the screen's state again, draws every row again, and says
  `HDMI mirror on, QSPI 15000 kHz`.
- The copy takes some time at each redraw. The link uses GP2 to GP5,
  GP20, GP21 and GP28, so a build with it has no DS3231 clock chip (GP4
  and GP5), and the clock is lost at power-off as with none fitted.

`battery`:

- The value is updated approximately every 20 seconds.
- `charging` is displayed if the PicoCalc charging on USB power (top USB connector).

`backlight`:

- `backlight` with no parameters displays both backlights.
- `backlight 200` sets the screen backlight.
- `backlight kbd 0` switches the keyboard's backlight off.
- The command shows the level the PicoCalc reports after
  setting it. Some keyboard firmware rounds the level (the screen to steps
  of 16, from 16 to 240, the keyboard to steps of 32); others keep the
  value as given. At 0 the screen may go fully dark: type
  `backlight 128` to bring it back.

```
/>backlight
lcd 128
kbd 64
/>backlight 200
lcd 200
```

### Leaving the shell

| Command | Does |
|---|---|
| `poweroff` | switches the PicoCalc off |
| `reboot` | restarts the shell |
| `bootsel` | restarts the RP2350 in BOOTSEL mode, as a USB drive, to install firmware |
| `menu` | runs UF2 Loader's own menu (`/BOOT2350.ORG`) |
| `usbdisk` | offers the SD card to a computer over USB (below) |

Each of these saves the history first. None can be used with a
redirection or in a pipe.

- **poweroff**: the keyboard's controller switches the power off about
  6 seconds later, on USB power too. If it is still on after 10 seconds,
  the shell says so and carries on.
- **reboot**: comes back to the shell, not to the last program. It needs
  the card in and UF2 Loader in flash, and says so if either is missing.
- **bootsel**: the RP2350's own drive appears on the computer. Switch the
  PicoCalc off and on to come back.

### usbdisk

`usbdisk` makes the SD card a USB drive on a computer connected to the
PicoCalc's USB port. Copy files to and from it as to any drive.

To come back to the shell:

- **Eject** the drive on the computer. The shell comes back by itself.
- **Esc** comes back at once. If the computer was still writing, those
  files may be incomplete, so wait for copies to finish first. An iPad
  has no eject, so there Esc is the only way.

The shell reads the card afresh afterwards. If the current directory no longer exists,
it moves to `/`.

### Variables and scripts

| Command | Does |
|---|---|
| `set` | lists the variables |
| `export [NAME=value...]` | sets variables, or lists them. The same as `NAME=value` |
| `unset NAME...` | removes variables |
| `sh file` | runs a script |
| `select NAME in WORD...; do LIST; done` | shows the words as a numbered menu, and runs LIST with `NAME` set to the one chosen |
| `break [N]` | leaves a `select`, or N of them |
| `history` | lists the lines typed before |

## 5. Running programs

Type a program's name to run it. The shell looks for `name.uf2` in each
directory of `$PATH`, which is `/pico2-apps` unless you change it:

```
/>pico-oric
writing 112 of 112 sectors
```

The count increases as each sector is written. Only the sectors that
differ from what flash holds are written, so a program run before
writes few or none.

A name that includes a `/` is read as a path: `/games/chess.uf2`.

### What happens

1. The shell checks that the file is a UF2 for the RP2350 and that it
   fits the flash set aside for programs.
2. It writes the program to flash, but only the parts that differ from
   what is there. Running the same program again takes about a second.
3. It restarts the PicoCalc into the program.

If the power is lost while the program is being written, the PicoCalc comes
back to the shell next time.

A program that runs from RAM (a `no_flash` build) is loaded straight
from the card without writing flash.

### Coming back

- **Hold Up, F1 or F5 while switching on.** This works for every program.
  With the shell's build of the loader, just switch on.
- **The program's own exit command**, if it has one, such as an *Exit to
  shell* item in its menu.
- **A program that keeps restarting itself**, with the shell's build of
  the loader: hold Up, F1 or F5 until the shell appears, or switch off
  and on. For a program that runs from RAM, only switching off and on
  works.

### Limits

- **Programs take no arguments yet.** `pico-oric game.tap` is refused,
  and so is running a program with a redirection or in a pipe.
- **Programs that keep files in flash.** Running a program rewrites the
  program area of flash, so a program that keeps its own files there,
  after its code, loses them when another program runs.

## 6. vi

`vi file` edits a text file of up to 64 KB (32 KB in the Pico 2 W build), such as a program's settings
file:

```
/>vi /atom/pico-atom.cfg
```

vi has three modes.

- **Normal mode** is where it starts. Keys are commands.
- **Insert mode** is for typing text. Esc goes back to normal mode.
- **Visual mode** is for selecting text. `v` selects characters, `V`
  selects whole lines.

The bottom row shows the mode, messages, and the cursor's line and column.

### The commands most used

| Keys | Does |
|---|---|
| arrows, `h` `j` `k` `l` | move |
| `w` `b` `e` | next word, previous word, end of word |
| `0` `$` | start and end of the line |
| `gg` `G` | first and last line; `12G` goes to line 12 |
| PgUp, PgDn (Shift+Up, Shift+Down) | a screenful up or down |
| `i` `a` | insert before or after the cursor |
| `I` `A` | insert at the start or end of the line |
| `o` `O` | open a new line below or above |
| `x` | delete a character |
| `dd` `yy` | cut or copy a line; `3dd` cuts three |
| `dw` `cw` | delete or change a word |
| `p` `P` | paste after or before |
| `u` `Ctrl-R` | undo and redo |
| `/text` `?text` | search forward or back; `n` finds the next |
| `:%s/old/new/g` | replace in every line |
| `>>` `<<` | indent or unindent a line by 4 spaces |
| `.` | repeat the last change |

### Saving and leaving

| Keys | Does |
|---|---|
| `:w` | save |
| `:wq` | save and leave |
| `ZZ` or `:x` | save if changed, and leave |
| `:q` | leave, if nothing has changed |
| `:q!` or `ZQ` | leave without saving |
| Break (Shift+Esc) | leave at once, **without saving** |

vi saves safely: it writes `name.new` first, then puts it in place, so a
failed save leaves the old file as it was.

### Good to know

- vi keeps a file's line endings, LF or CR LF, as they were, and whether
  the last line ends in a line break.
- `view`, `vi -R`, and files marked read-only open read-only.
- A file larger than 64 KB (32 KB in the Pico 2 W build) is refused, never cut short.
- Long lines do not wrap: the view scrolls sideways to follow the cursor.
- A tab shows as up to 8 spaces. A control character shows as `^X`.

## 7. The card

```
/BOOT2350.UF2         the shell
/BOOT2350.ORG         UF2 Loader's own menu, run with `menu`
/pico2-apps/          programs, run by name
/pico2-apps/ntp.uf2   the Pico 2 W build's ntp
/ntp/ntp.cfg          ntp's settings, and the network web joins
/pico2-apps/web.uf2   the Pico 2 W build's web
/bluetooth/pairing.dat  the paired Bluetooth keyboard (btpair's)
/bluetooth/cyw43-fw.bin  the radio's firmware, read at its start
/etc/profile          run each time the shell starts
/home/                where the shell starts ($HOME)
/.shell/              the shell's history, and large pipes while they run
```

- **Hidden names.** `ls` and globs leave out names that start with `.`
  (such as macOS's `._` files) and names FAT marks hidden or system, such
  as Windows's `System Volume Information`. `ls -a` shows them.
- **Safe writes.** Every command that writes a file (`cp`, `>`, `>>`,
  vi's `:w`) writes `name.new` and then renames it into place. If you
  ever find a `name.new` left on the card, the power went at just the
  wrong moment. It holds the complete new version.
- **Taking the card out** at the prompt is fine. The shell reads the new
  card at the next command.

## 8. Messages and limits

Errors name the command, then the file, then the reason:

```
/>cat nope.txt
cat: nope.txt: no such file or directory
/>pico-ace
pico-ace: not found
```

| Message | Means |
|---|---|
| `name: not found` | no builtin, and no `name.uf2` in `$PATH` |
| `not for this device` | the UF2 is for another chip, such as an RP2040 |
| `not on this machine` | the command needs the PicoCalc's hardware, as in the shell built for a computer |
| `not with a redirection or a pipe` | the command leaves the shell, so it must run on its own |
| `cannot run programs: ...` | the shell was not started by UF2 Loader, or the loader is a version it does not know |

Limits:

| What | Limit |
|---|---|
| A typed line | 255 characters |
| Words on a line, after globs | 128 |
| Variables | 1 KB in all |
| History | about 2 KB of lines |
| A file in vi | 64 KB (32 KB in the Pico 2 W build) |
| Scripts within scripts | 3 deep |
| A `select` in a script, `select` to `done` | 1 KB |
| Words in a `select` and those inside it | 64, in 512 bytes |
| A path | 255 characters |

