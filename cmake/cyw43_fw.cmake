# cyw43_fw.cmake — the CYW43439's firmware read from the card instead of
# held in RAM (design.md §3.10).
#
# The Pico SDK's cyw43_ll.c includes the firmware as a C array, 232 KB of
# the program's RAM, and reads it by pointer in three places: the check of
# its tail, the download in blocks, and the CLM blob. This patches a copy
# of cyw43_ll.c at configure time, as picocalc-extras' bluetooth.cmake
# patches BTstack (the build stops with a message if the SDK's code there
# has changed), so that a program compiled with CYW43_FW_STREAM=1 reads
# those through cyw43_fw_read(), from a file on the card. Any other target
# that uses the driver compiles the original code from the same copy.
#
#   cyw43_fw_stream_setup()               once, after pico_sdk_init
#   target_link_libraries(prog cyw43_fw_stream)  per program that streams
#
# cyw43_fw_stream is an interface library: the definition, the include
# directory of the generated header, and the build of the file
# `cyw43-fw.bin` (in ${CMAKE_BINARY_DIR}/cyw43fw), which the user copies
# to /bluetooth/ on the card.

function(cyw43_fw_stream_setup)
    set(ORIG ${PICO_CYW43_DRIVER_PATH}/src/cyw43_ll.c)
    set(DIR ${CMAKE_BINARY_DIR}/cyw43fw)
    set(PATCHED ${DIR}/cyw43_ll.c)
    set(FWHDR ${PICO_CYW43_DRIVER_PATH}/firmware/wb43439A0_7_95_49_00_combined.h)
    file(MAKE_DIRECTORY ${DIR})

    file(READ ${ORIG} TEXT)

    # Replace OLD (once, exactly) with NEW in TEXT.
    macro(patch OLD NEW)
        string(FIND "${TEXT}" "${OLD}" AT)
        if (AT EQUAL -1)
            message(FATAL_ERROR "cyw43_fw.cmake: the SDK's cyw43_ll.c has changed; "
                    "check that its firmware reads are still these: ${OLD}")
        endif()
        string(REPLACE "${OLD}" "${NEW}" TEXT "${TEXT}")
    endmacro()

    # 1. The array becomes the generated header, and the one place the
    # firmware is read from: the card for the virtual address, memory for
    # anything else (the NVRAM).
    patch("#include CYW43_CHIPSET_FIRMWARE_INCLUDE_FILE\n"
"#if CYW43_FW_STREAM\n#include \"cyw43_fw_stream.h\"\n\n\
// (shell) The firmware is on the card: `src` is an address in the\n\
// virtual window fw_data starts, or ordinary memory.\n\
static int cyw43_fw_fetch(void *dst, uintptr_t src, size_t len) {\n\
    if (src >= CYW43_FW_VIRTUAL && src - CYW43_FW_VIRTUAL < CYW43_FW_FILE_LEN) {\n\
        return cyw43_fw_read((uint32_t)(src - CYW43_FW_VIRTUAL), dst, len);\n\
    }\n\
    memcpy(dst, (const void *)src, len);\n\
    return 0;\n\
}\n\
#else\n#include CYW43_CHIPSET_FIRMWARE_INCLUDE_FILE\n#endif\n")

    # 2. The check of the firmware's tail.
    patch("    const uint8_t *b = (const uint8_t *)source + len - fw_end;\n"
"#if CYW43_FW_STREAM\n\
    static uint8_t tail[800];\n\
    if (cyw43_fw_fetch(tail, source + len - fw_end, fw_end) != 0) {\n\
        return CYW43_FAIL_FAST_CHECK(-CYW43_EIO);\n\
    }\n\
    const uint8_t *b = tail;\n\
#else\n\
    const uint8_t *b = (const uint8_t *)source + len - fw_end;\n\
#endif\n")

    # 3. The download, a block at a time.
    patch("        const uint8_t *src = (const uint8_t *)source + offset;\n"
"#if CYW43_FW_STREAM\n\
        static uint8_t stage[CYW43_BUS_MAX_BLOCK_SIZE] __attribute__((aligned(4)));\n\
        if (cyw43_fw_fetch(stage, source + offset, sz) != 0) {\n\
            return CYW43_FAIL_FAST_CHECK(-CYW43_EIO);\n\
        }\n\
        const uint8_t *src = stage;\n\
#else\n\
        const uint8_t *src = (const uint8_t *)source + offset;\n\
#endif\n")

    # 4. The CLM blob. It returns nothing: with no data the status that
    # follows says so.
    patch("        memcpy(buf + 20, clm_ptr + off, len);\n"
"#if CYW43_FW_STREAM\n\
        if (cyw43_fw_fetch(buf + 20, (uintptr_t)clm_ptr + off, len) != 0) {\n\
            return;\n\
        }\n\
#else\n\
        memcpy(buf + 20, clm_ptr + off, len);\n\
#endif\n")

    # Written only if it changed, so a configure does not rebuild it.
    set(OLD "")
    if (EXISTS ${PATCHED})
        file(READ ${PATCHED} OLD)
    endif()
    if (NOT OLD STREQUAL TEXT)
        file(WRITE ${PATCHED} "${TEXT}")
    endif()
    get_target_property(SRCS cyw43_driver INTERFACE_SOURCES)
    list(REMOVE_ITEM SRCS ${ORIG})
    list(APPEND SRCS ${PATCHED})
    set_target_properties(cyw43_driver PROPERTIES INTERFACE_SOURCES "${SRCS}")

    # The file and its header, from the SDK's own array.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(
        OUTPUT ${DIR}/cyw43-fw.bin ${DIR}/cyw43_fw_stream.h
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/cyw43-fw.py
                ${FWHDR} ${DIR}/cyw43-fw.bin ${DIR}/cyw43_fw_stream.h
        DEPENDS ${CMAKE_SOURCE_DIR}/tools/cyw43-fw.py ${FWHDR}
        COMMENT "cyw43-fw.bin from the SDK's firmware"
        VERBATIM)
    add_custom_target(cyw43_fw_files DEPENDS ${DIR}/cyw43-fw.bin ${DIR}/cyw43_fw_stream.h)

    add_library(cyw43_fw_stream INTERFACE)
    add_dependencies(cyw43_fw_stream cyw43_fw_files)
    target_include_directories(cyw43_fw_stream INTERFACE ${DIR})
    target_compile_definitions(cyw43_fw_stream INTERFACE CYW43_FW_STREAM=1 CYW43_ENABLE_BLUETOOTH=1)
endfunction()
