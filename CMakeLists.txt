# shell — one source tree, two targets (design.md §6):
#   - BOOT2350.UF2, the shell as a no_flash RAM program (default), in the
#     flavour PICO_SHELL_FLAVOUR names: unix (default) or dos (§4.7)
#   - a host build of src/core/ and its tests with the system compiler and
#     no Pico SDK, behind -DPICO_SHELL_HOST=ON, run under CTest; it builds
#     both flavours.
#
# The core building clean under -Wall -Wextra -Werror on both toolchains
# is what keeps SDK dependencies out of it (EL §2.1).

cmake_minimum_required(VERSION 3.18)

option(PICO_SHELL_HOST "Build the portable core and its host tests, with no Pico SDK" OFF)

# Every fixed capacity lives in src/core/config.h; nothing here duplicates one.
# What both flavours share (design.md §4.7)...
set(SHELL_CORE_SOURCES
    src/core/bininfo.c
    src/core/btstore.c
    src/core/clock.c
    src/core/editor_file.c
    src/core/fat.c
    src/core/editor_pattern.c
    src/core/files.c
    src/core/history.c
    src/core/httpd.c
    src/core/input.c
    src/core/keys.c
    src/core/line.c
    src/core/ntp.c
    src/core/out.c
    src/core/path.c
    src/core/pipe.c
    src/core/run.c
    src/core/shell.c
    src/core/system.c
    src/core/term.c
    src/core/text.c
    src/core/uf2.c
    src/core/vars.c
    src/core/web.c
)
# ...the UNIX flavour's command language, vi and font...
set(SHELL_UNIX_SOURCES
    src/core/editor_lines.c
    src/core/editor_undo.c
    src/core/editor_vi.c
    src/core/expand.c
    src/core/font-master-512-8x10.c
    src/core/parse.c
    src/core/unix.c
    src/core/vi.c
)
# ...and the DOS flavour's.
set(SHELL_DOS_SOURCES
    src/core/dos.c
    src/core/dos_batch.c
    src/core/dos_files.c
    src/core/dos_parse.c
    src/core/dos_path.c
    src/core/dos_text.c
    src/core/dos_time.c
    src/core/edit.c
    src/core/editor_lines.c
    src/core/font-ibm-8x10.c
)

set(SHELL_WARNINGS -Wall -Wextra -Werror)

include(cmake/fatfs.cmake)

# The core in one flavour, unix or dos, as the library `name`, with the
# `ntp` builtin if ntp is 1 (design.md §3.8). It uses FatFs's API for
# files (design.md §5); the disk under it is the port's (the SD card) or
# the host's (an image file).
function(shell_core_library name flavour ntp compact)
    if (flavour STREQUAL "unix")
        add_library(${name} STATIC ${SHELL_CORE_SOURCES} ${SHELL_UNIX_SOURCES})
        target_compile_definitions(${name} PUBLIC SHELL_DOS=0)
    elseif (flavour STREQUAL "dos")
        add_library(${name} STATIC ${SHELL_CORE_SOURCES} ${SHELL_DOS_SOURCES})
        target_compile_definitions(${name} PUBLIC SHELL_DOS=1)
    else()
        message(FATAL_ERROR "shell: PICO_SHELL_FLAVOUR is unix or dos, not ${flavour}")
    endif()
    target_compile_definitions(${name} PUBLIC SHELL_NTP=${ntp})
    target_compile_definitions(${name} PUBLIC SHELL_COMPACT=${compact})
    target_include_directories(${name} PUBLIC src/core)
    target_compile_options(${name} PRIVATE ${SHELL_WARNINGS})
    # Pico Logo's key layer cuts its Ctrl-G report to the status row on
    # purpose (its limits.h says so); GCC's truncation warning is off for
    # that file alone.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        set_source_files_properties(src/core/editor_vi.c PROPERTIES
            COMPILE_OPTIONS -Wno-format-truncation)
    endif()
    target_link_libraries(${name} PUBLIC shell_fatfs)
endfunction()

if (PICO_SHELL_HOST)
    project(shell_host C)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
    # The FatFs/disk cycle below repeats two archives on the link line,
    # which GNU ld needs and Apple's linker warns about.
    if (APPLE)
        add_link_options(LINKER:-no_warn_duplicate_libraries)
    endif()

    shell_fatfs_library()
    # The host has the W build's `ntp`, so its tests run (design.md §3.8).
    shell_core_library(shell_core unix 1 0)
    shell_core_library(shell_core_dos dos 1 0)
    # The W builds' smaller vi and EDIT buffers, tested as they are built
    # (design.md §3.10).
    shell_core_library(shell_core_compact unix 1 1)
    shell_core_library(shell_core_dos_compact dos 1 1)
    # Only the host formats disks: the tests' FAT16 and FAT32 images.
    target_compile_definitions(shell_fatfs PUBLIC SHELL_FATFS_MKFS=1)

    # The host's disk, an image file, and the host shell over it.
    add_library(shell_host_disk STATIC src/host/host_disk.c)
    target_include_directories(shell_host_disk PUBLIC src/host)
    target_compile_options(shell_host_disk PRIVATE ${SHELL_WARNINGS})
    target_link_libraries(shell_host_disk PUBLIC shell_fatfs)
    # FatFs calls the disk, and the disk FatFs's types: a cycle, which
    # CMake writes out so that GNU ld, which scans each archive once in
    # order, finds the disk for a program that names only the core.
    target_link_libraries(shell_fatfs PUBLIC shell_host_disk)

    add_executable(shell src/host/main.c)
    target_compile_options(shell PRIVATE ${SHELL_WARNINGS})
    target_link_libraries(shell PRIVATE shell_core shell_host_disk)
    add_executable(shell-dos src/host/main.c)
    target_compile_options(shell-dos PRIVATE ${SHELL_WARNINGS})
    target_link_libraries(shell-dos PRIVATE shell_core_dos shell_host_disk)

    enable_testing()
    add_subdirectory(test/host)
else()
    # RP2350 only (design.md §1). pico2 is the Pico 2 build, for every
    # RP2350 board; pico2_w is the W build, which adds `ntp` and builds
    # ntp.uf2 (design.md §3.8). Each is built in its own directory.
    if (NOT DEFINED PICO_BOARD)
        set(PICO_BOARD pico2)
    endif()
    if (PICO_BOARD STREQUAL "pico2")
        set(SHELL_W 0)
    elseif (PICO_BOARD STREQUAL "pico2_w")
        set(SHELL_W 1)
    else()
        message(FATAL_ERROR "shell: PICO_BOARD is pico2 or pico2_w, not ${PICO_BOARD}")
    endif()
    if (NOT DEFINED PICO_PLATFORM)
        set(PICO_PLATFORM rp2350)
    endif()

    include(cmake/pico_sdk_import.cmake)

    project(shell C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    pico_sdk_init()

    if (NOT PICO_RP2350)
        message(FATAL_ERROR "shell: RP2350 boards only (design.md §1), not ${PICO_PLATFORM}")
    endif()

    # The flavour (design.md §4.7): each is its own BOOT2350.UF2, built in
    # its own directory.
    set(PICO_SHELL_FLAVOUR unix CACHE STRING "The command line's flavour: unix or dos")
    shell_fatfs_library()
    shell_core_library(shell_core ${PICO_SHELL_FLAVOUR} ${SHELL_W} ${SHELL_W})
    # The W builds are built for size (-Os, the SDK's and TinyUSB's code
    # too), except the drivers whose loops are hot: the panel and the card
    # stay at -O3. The room is what Bluetooth takes (design.md §3.10).
    if (SHELL_W)
        set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG")
        set_source_files_properties(
            src/port/diskio.c src/port/lcd.c src/port/screen.c src/port/sd.c
            PROPERTIES COMPILE_OPTIONS -O3)
    endif()

    # The version for the banner, git describe --always --dirty, taken at
    # build time; the header only changes when the text does.
    set(SHELL_GENERATED ${CMAKE_BINARY_DIR}/generated)
    add_custom_target(shell_version ALL
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -DOUT=${SHELL_GENERATED}/shell_version.h
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
        BYPRODUCTS ${SHELL_GENERATED}/shell_version.h
        COMMENT "shell: version from git describe"
        VERBATIM)

    # PICO_SHELL_UART=OFF is the build that ships: nothing on UART0, so GP2
    # and GP3 are free. ON, the default, is the development build: the log,
    # the screen mirrored and keys typed over UART0 (design.md §3.8, §6,
    # EL §13.2). GP4 and GP5 are i2c0's, for a DS3231 (design.md §3.8).
    option(PICO_SHELL_UART "Log, mirror the screen and take keys over UART0 on GP2/GP3" ON)
    if (PICO_SHELL_UART)
        set(PICO_SHELL_UART_ON 1)
    else()
        set(PICO_SHELL_UART_ON 0)
    endif()

    # The panel's SPI rate: 75 MHz, the owner's decision after M0's
    # measurements (design.md §4.1; hardware-notes.md §4.2, §4.7).
    # 25000000 is the conservative fallback. A build directory configured
    # before keeps its cached value until this is passed once.
    set(PICO_SHELL_LCD_SPI_HZ 75000000 CACHE STRING "LCD SPI rate in Hz")

    get_filename_component(SHELL_TOOLCHAIN_BIN ${CMAKE_C_COMPILER} DIRECTORY)
    find_program(SHELL_SIZE arm-none-eabi-size HINTS ${SHELL_TOOLCHAIN_BIN} REQUIRED)

    add_executable(shell
        src/port/board.c
        src/port/clock.c
        src/port/diskio.c
        src/port/ds3231.c
        src/port/kbd.c
        src/port/lcd.c
        src/port/loader.c
        src/port/main.c
        src/port/screen.c
        src/port/sd.c
        src/port/southbridge.c
        src/port/usb_descriptors.c
        src/port/usbdisk.c
    )
    # The loader's stage3 loads /BOOT2350.UF2 into SRAM and runs it
    # (design.md §3.2), as it does the loader's own menu.
    set_target_properties(shell PROPERTIES OUTPUT_NAME BOOT2350)
    pico_set_binary_type(shell no_flash)
    pico_set_program_name(shell "shell")
    if (PICO_SHELL_FLAVOUR STREQUAL "dos")
        pico_set_program_description(shell "DOS-like command line for the PicoCalc")
    else()
        pico_set_program_description(shell "UNIX-like command line for the PicoCalc")
    endif()

    # include/: the header programs use to return to the shell, which
    # loader.c checks against its own idea of the protocol. src/port has
    # tusb_config.h, which TinyUSB looks for on the include path.
    target_include_directories(shell PRIVATE src/port include ${SHELL_GENERATED})
    target_compile_options(shell PRIVATE ${SHELL_WARNINGS})
    add_dependencies(shell shell_version)
    target_compile_definitions(shell PRIVATE
        PICO_SHELL_UART=${PICO_SHELL_UART_ON}
        PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
        # clk_peri does not follow clk_sys by default (hardware-notes.md §3).
        PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
        # UART0 at 115200 8-N-1, TX GP2 / RX GP3, through UART_AUX
        # (design.md §3.8; hardware-notes.md §2.7).
        PICO_DEFAULT_UART=0
        PICO_DEFAULT_UART_TX_PIN=2
        PICO_DEFAULT_UART_RX_PIN=3
        # board.c only reads the PSRAM's id, for info and MEM; nothing
        # sets it up or uses it (hardware-notes.md §2.4).
        PICO_RUNTIME_NO_INIT_PSRAM=1
    )
    target_link_libraries(shell
        shell_core
        pico_stdlib
        pico_unique_id
        hardware_clocks
        hardware_dma
        hardware_flash
        hardware_i2c
        hardware_powman
        hardware_psram
        hardware_spi
        hardware_vreg
        tinyusb_device
    )
    pico_enable_stdio_uart(shell ${PICO_SHELL_UART_ON})
    pico_enable_stdio_usb(shell 0)
    pico_add_extra_outputs(shell)

    # A flash program that returns to the shell when a key is pressed: the
    # board check of include/picocalc_shell.h (design.md §3.4). It shows
    # what it does with the shell's own panel, terminal and keyboard code.
    add_executable(exit-test
        test/pico/exit_test.c
        src/port/kbd.c
        src/port/lcd.c
        src/port/screen.c
        src/port/southbridge.c
    )
    target_include_directories(exit-test PRIVATE include src/port)
    target_compile_options(exit-test PRIVATE ${SHELL_WARNINGS})
    target_compile_definitions(exit-test PRIVATE
        PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
        PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
        PICO_DEFAULT_UART=0
        PICO_DEFAULT_UART_TX_PIN=2
        PICO_DEFAULT_UART_RX_PIN=3
    )
    target_link_libraries(exit-test shell_core pico_stdlib hardware_dma hardware_i2c hardware_spi)
    pico_enable_stdio_uart(exit-test 1)
    pico_enable_stdio_usb(exit-test 0)
    pico_add_extra_outputs(exit-test)

    # A flash program that restarts itself at every start: the board
    # check of M7's way out of a restart loop (design.md §10).
    add_executable(restart-test
        test/pico/restart_test.c
        src/port/kbd.c
        src/port/lcd.c
        src/port/screen.c
        src/port/southbridge.c
    )
    target_include_directories(restart-test PRIVATE include src/port)
    target_compile_options(restart-test PRIVATE ${SHELL_WARNINGS})
    target_compile_definitions(restart-test PRIVATE
        PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
        PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
        PICO_DEFAULT_UART=0
        PICO_DEFAULT_UART_TX_PIN=2
        PICO_DEFAULT_UART_RX_PIN=3
    )
    target_link_libraries(restart-test shell_core pico_stdlib hardware_dma hardware_i2c hardware_spi)
    pico_enable_stdio_uart(restart-test 1)
    pico_enable_stdio_usb(restart-test 0)
    pico_add_extra_outputs(restart-test)

    # restart-test as a RAM program, which restarts itself with BOOT_RAM
    # and its own 8.3 path, read from the card (design.md §10, M7).
    add_executable(restart-ram
        test/pico/restart_ram.c
        src/port/diskio.c
        src/port/kbd.c
        src/port/lcd.c
        src/port/screen.c
        src/port/sd.c
        src/port/southbridge.c
    )
    pico_set_binary_type(restart-ram no_flash)
    target_include_directories(restart-ram PRIVATE include src/port)
    target_compile_options(restart-ram PRIVATE ${SHELL_WARNINGS})
    target_compile_definitions(restart-ram PRIVATE
        PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
        PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
        PICO_DEFAULT_UART=0
        PICO_DEFAULT_UART_TX_PIN=2
        PICO_DEFAULT_UART_RX_PIN=3
    )
    target_link_libraries(restart-ram shell_core pico_stdlib hardware_dma hardware_i2c hardware_spi)
    pico_enable_stdio_uart(restart-ram 1)
    pico_enable_stdio_usb(restart-ram 0)
    pico_add_extra_outputs(restart-ram)

    # A flash program that shows the time the shell gave it, sets it a
    # minute forward and returns: the board check of the header's clock
    # (design.md §3.8). exit-test, which does not link hardware_powman,
    # checks the header still builds without it.
    add_executable(clock-test
        test/pico/clock_test.c
        src/port/kbd.c
        src/port/lcd.c
        src/port/screen.c
        src/port/southbridge.c
    )
    target_include_directories(clock-test PRIVATE include src/port)
    target_compile_options(clock-test PRIVATE ${SHELL_WARNINGS})
    target_compile_definitions(clock-test PRIVATE
        PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
        PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
        PICO_DEFAULT_UART=0
        PICO_DEFAULT_UART_TX_PIN=2
        PICO_DEFAULT_UART_RX_PIN=3
    )
    target_link_libraries(clock-test shell_core pico_stdlib hardware_dma hardware_i2c hardware_powman
        hardware_spi)
    pico_enable_stdio_uart(clock-test 1)
    pico_enable_stdio_usb(clock-test 0)
    pico_add_extra_outputs(clock-test)

    # A flash program that sets the DS3231's EOSC, so the chip loses its
    # time at the next power-off: the board check of a PicoCalc with no
    # clock source, where the chip cannot be taken out (design.md §3.8).
    add_executable(rtc-stop
        test/pico/rtc_stop.c
        src/port/ds3231.c
        src/port/kbd.c
        src/port/lcd.c
        src/port/screen.c
        src/port/southbridge.c
    )
    target_include_directories(rtc-stop PRIVATE include src/port)
    target_compile_options(rtc-stop PRIVATE ${SHELL_WARNINGS})
    target_compile_definitions(rtc-stop PRIVATE
        PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
        PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
        PICO_DEFAULT_UART=0
        PICO_DEFAULT_UART_TX_PIN=2
        PICO_DEFAULT_UART_RX_PIN=3
    )
    target_link_libraries(rtc-stop shell_core pico_stdlib hardware_dma hardware_i2c hardware_spi)
    pico_enable_stdio_uart(rtc-stop 1)
    pico_enable_stdio_usb(rtc-stop 0)
    pico_add_extra_outputs(rtc-stop)

    # The CYW43's firmware read from the card, not held in RAM, and a
    # program that checks it on the board (design.md §3.10).
    if (SHELL_W)
        include(cmake/cyw43_fw.cmake)
        cyw43_fw_stream_setup()
        add_executable(cyw43fw-test
            test/pico/cyw43fw_test.c
            src/port/cyw43_fw.c
            src/port/diskio.c
            src/port/kbd.c
            src/port/lcd.c
            src/port/screen.c
            src/port/sd.c
            src/port/southbridge.c
        )
        pico_set_binary_type(cyw43fw-test no_flash)
        pico_set_program_name(cyw43fw-test "cyw43fw-test")
        target_include_directories(cyw43fw-test PRIVATE include src/port)
        target_compile_options(cyw43fw-test PRIVATE ${SHELL_WARNINGS})
        target_compile_definitions(cyw43fw-test PRIVATE
            PICO_SHELL_UART=${PICO_SHELL_UART_ON}
            PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
            PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
            PICO_DEFAULT_UART=0
            PICO_DEFAULT_UART_TX_PIN=2
            PICO_DEFAULT_UART_RX_PIN=3
        )
        target_link_libraries(cyw43fw-test
            shell_core
            cyw43_fw_stream
            pico_stdlib
            pico_cyw43_arch_none
            hardware_dma
            hardware_i2c
            hardware_spi
        )
        pico_enable_stdio_uart(cyw43fw-test ${PICO_SHELL_UART_ON})
        pico_enable_stdio_usb(cyw43fw-test 0)
        pico_add_extra_outputs(cyw43fw-test)
        target_link_options(cyw43fw-test PRIVATE -Wl,--print-memory-usage)
    endif()

    # The Bluetooth keyboard (design.md §3.10): btkbd.c and BTstack, and a
    # program that shows its keys on the board.
    if (SHELL_W)
        include(cmake/bluetooth.cmake)
        add_executable(btkbd-test
            test/pico/btkbd_test.c
            src/port/cyw43_fw.c
            src/port/diskio.c
            src/port/kbd.c
            src/port/lcd.c
            src/port/screen.c
            src/port/sd.c
            src/port/southbridge.c
        )
        pico_set_binary_type(btkbd-test no_flash)
        pico_set_program_name(btkbd-test "btkbd-test")
        target_include_directories(btkbd-test PRIVATE include src/port)
        target_compile_definitions(btkbd-test PRIVATE
            PICO_SHELL_UART=${PICO_SHELL_UART_ON}
            PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
            PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
            PICO_DEFAULT_UART=0
            PICO_DEFAULT_UART_TX_PIN=2
            PICO_DEFAULT_UART_RX_PIN=3
        )
        target_link_libraries(btkbd-test
            shell_core
            shell_btkbd
            cyw43_fw_stream
            pico_stdlib
            hardware_dma
            hardware_i2c
            hardware_spi
        )
        pico_enable_stdio_uart(btkbd-test ${PICO_SHELL_UART_ON})
        pico_enable_stdio_usb(btkbd-test 0)
        pico_add_extra_outputs(btkbd-test)
        target_link_options(btkbd-test PRIVATE -Wl,--print-memory-usage)
    endif()

    # ntp.uf2, for /pico2-apps/ in the W build: Wi-Fi and NTP in a RAM
    # program of its own, which the shell's `ntp` runs (design.md §3.8).
    # It uses the shell's card, panel, keyboard and clock code. It links
    # the CYW43 driver under the owner's GPL-3.0 section 7 permission
    # (THIRD-PARTY.md), so it must link nothing from UF2 Loader (loader.c,
    # core/uf2.c, usbdisk.c) or another GPL-3.0 author.
    if (SHELL_W)
        add_executable(ntp
            src/ntp/main.c
            src/port/clock.c
            src/port/diskio.c
            src/port/ds3231.c
            src/port/kbd.c
            src/port/lcd.c
            src/port/screen.c
            src/port/sd.c
            src/port/southbridge.c
        )
        pico_set_binary_type(ntp no_flash)
        pico_set_program_name(ntp "ntp")
        pico_set_program_description(ntp "Sets the PicoCalc shell's clock from NTP")
        target_include_directories(ntp PRIVATE src/ntp src/port include)
        target_compile_options(ntp PRIVATE ${SHELL_WARNINGS})
        target_compile_definitions(ntp PRIVATE
            PICO_SHELL_UART=${PICO_SHELL_UART_ON}
            PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
            PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
            PICO_DEFAULT_UART=0
            PICO_DEFAULT_UART_TX_PIN=2
            PICO_DEFAULT_UART_RX_PIN=3
        )
        target_link_libraries(ntp
            shell_core
            pico_stdlib
            pico_rand
            pico_cyw43_arch_lwip_poll
            hardware_dma
            hardware_i2c
            hardware_powman
            hardware_spi
        )
        pico_enable_stdio_uart(ntp ${PICO_SHELL_UART_ON})
        pico_enable_stdio_usb(ntp 0)
        pico_add_extra_outputs(ntp)
        target_link_options(ntp PRIVATE -Wl,--print-memory-usage)

        # web.uf2: the card on the network, from a browser, in a RAM
        # program of its own, which the shell's `web` runs (design.md
        # §3.9). The same rules as ntp.uf2: it links the CYW43 driver
        # under the owner's section 7 permission, so nothing from UF2
        # Loader or another GPL-3.0 author.
        # The page, gzipped into a header (src/web/embed.cmake).
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/web/web_page.h
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/web
            COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html
                    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/web/web_page.h
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/src/web/embed.cmake
            DEPENDS src/web/index.html src/web/embed.cmake
            COMMENT "Embedding the web page"
        )
        add_executable(web
            src/web/main.c
            ${CMAKE_CURRENT_BINARY_DIR}/web/web_page.h
            src/port/clock.c
            src/port/diskio.c
            src/port/ds3231.c
            src/port/kbd.c
            src/port/lcd.c
            src/port/screen.c
            src/port/sd.c
            src/port/southbridge.c
        )
        pico_set_binary_type(web no_flash)
        pico_set_program_name(web "web")
        pico_set_program_description(web "A web file manager for the PicoCalc's card")
        target_include_directories(web PRIVATE src/web src/port include ${CMAKE_CURRENT_BINARY_DIR}/web)
        target_compile_options(web PRIVATE ${SHELL_WARNINGS})
        target_compile_definitions(web PRIVATE
            PICO_SHELL_UART=${PICO_SHELL_UART_ON}
            PICO_SHELL_LCD_SPI_HZ=${PICO_SHELL_LCD_SPI_HZ}
            PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
            PICO_DEFAULT_UART=0
            PICO_DEFAULT_UART_TX_PIN=2
            PICO_DEFAULT_UART_RX_PIN=3
            CYW43_HOST_NAME="picocalc"
        )
        target_link_libraries(web
            shell_core
            pico_stdlib
            pico_rand
            pico_cyw43_arch_lwip_poll
            pico_lwip_mdns
            hardware_dma
            hardware_i2c
            hardware_powman
            hardware_spi
        )
        pico_enable_stdio_uart(web ${PICO_SHELL_UART_ON})
        pico_enable_stdio_usb(web 0)
        pico_add_extra_outputs(web)
        target_link_options(web PRIVATE -Wl,--print-memory-usage)
    endif()

    # Memory use on every build (design.md §7, EL §2.1).
    target_link_options(shell PRIVATE -Wl,--print-memory-usage)
    add_custom_command(TARGET shell POST_BUILD
        COMMAND ${SHELL_SIZE} $<TARGET_FILE:shell>
        VERBATIM)
endif()
