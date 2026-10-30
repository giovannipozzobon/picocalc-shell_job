# Host tests (design.md §6). Every one builds src/core/ with the system
# compiler and no Pico SDK. One binary per area; exit 77 is a skip.

set(SHELL_HOST_TESTS
    test_clock
    test_editor_lines
    test_editor_pattern
    test_editor_undo
    test_editor_vi
    test_expand
    test_files
    test_httpd
    test_keys
    test_line
    test_parse
    test_path
    test_pipe
    test_select
    test_shell
    test_skeleton
    test_system
    test_term
    test_text
    test_uf2
    test_vi
    test_web
)

# The core's files need a disk: the host's image file, or none.
foreach(t ${SHELL_HOST_TESTS})
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE shell_core shell_host_disk)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${SHELL_WARNINGS})
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

# The DOS flavour's command line, over its own core (design.md §4.7).
add_executable(test_dos test_dos.c)
target_link_libraries(test_dos PRIVATE shell_core_dos shell_host_disk)
target_include_directories(test_dos PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_dos PRIVATE ${SHELL_WARNINGS})
add_test(NAME test_dos COMMAND test_dos)

# The DOS flavour's date and time (design.md §3.8).
add_executable(test_dos_time test_dos_time.c)
target_link_libraries(test_dos_time PRIVATE shell_core_dos shell_host_disk)
target_include_directories(test_dos_time PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_dos_time PRIVATE ${SHELL_WARNINGS})
add_test(NAME test_dos_time COMMAND test_dos_time)

# NTP's settings file and the W build's `ntp`, in each flavour (design.md
# §3.8).
add_executable(test_ntp test_ntp.c)
target_link_libraries(test_ntp PRIVATE shell_core shell_host_disk)
add_executable(test_ntp_dos test_ntp.c)
target_link_libraries(test_ntp_dos PRIVATE shell_core_dos shell_host_disk)
foreach(t test_ntp test_ntp_dos)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${SHELL_WARNINGS})
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# EDIT, the DOS flavour's editor (design.md §4.7).
add_executable(test_edit test_edit.c)
target_link_libraries(test_edit PRIVATE shell_core_dos shell_host_disk)
target_include_directories(test_edit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_edit PRIVATE ${SHELL_WARNINGS})
add_test(NAME test_edit COMMAND test_edit)

# vi and EDIT with the W builds' smaller buffers (design.md §3.10).
add_executable(test_vi_compact test_vi.c)
target_link_libraries(test_vi_compact PRIVATE shell_core_compact shell_host_disk)
add_executable(test_edit_compact test_edit.c)
target_link_libraries(test_edit_compact PRIVATE shell_core_dos_compact shell_host_disk)
foreach(t test_vi_compact test_edit_compact)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${SHELL_WARNINGS})
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The port's renderer has no SDK header, so it is tested here on its own
# source, against a model of the panel in place of lcd.c.
add_executable(test_screen test_screen.c fake_lcd.c ${CMAKE_SOURCE_DIR}/src/port/screen.c)
target_link_libraries(test_screen PRIVATE shell_core shell_host_disk)
target_include_directories(test_screen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/port)
target_compile_options(test_screen PRIVATE ${SHELL_WARNINGS})
add_test(NAME test_screen COMMAND test_screen)
set_tests_properties(test_screen PROPERTIES SKIP_RETURN_CODE 77)

# The real UF2s test_uf2 reads, if tools/fetch-suites.sh has fetched them.
target_compile_definitions(test_uf2 PRIVATE SHELL_SUITES_DIR="${CMAKE_SOURCE_DIR}/test/suites")

# The port's card driver, against a model of a card in SPI mode, through
# a fake of the few SDK calls it makes (fake_sdk/).
add_executable(test_sd test_sd.c fake_sd.c ${CMAKE_SOURCE_DIR}/src/port/sd.c)
target_include_directories(test_sd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_sdk ${CMAKE_SOURCE_DIR}/src/port)
target_compile_options(test_sd PRIVATE ${SHELL_WARNINGS})
add_test(NAME test_sd COMMAND test_sd)

# The port's DS3231 driver and the clock's record, against a model of the
# chip and of POWMAN, through the same fake SDK.
add_executable(test_ds3231 test_ds3231.c fake_ds3231.c ${CMAKE_SOURCE_DIR}/src/port/ds3231.c
    ${CMAKE_SOURCE_DIR}/src/port/clock.c)
target_link_libraries(test_ds3231 PRIVATE shell_core shell_host_disk)
target_include_directories(test_ds3231 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_sdk ${CMAKE_SOURCE_DIR}/src/port ${CMAKE_SOURCE_DIR}/include)
target_compile_options(test_ds3231 PRIVATE ${SHELL_WARNINGS})
add_test(NAME test_ds3231 COMMAND test_ds3231)
