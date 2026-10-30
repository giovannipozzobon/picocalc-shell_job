# bluetooth.cmake — a Bluetooth keyboard for the shell (design.md §3.10):
# BTstack's HID hosts, classic and BLE, on the Pico 2 W's CYW43439, with
# src/port/btkbd.c.
#
# Taken from picocalc-extras' bluetooth/bluetooth.cmake (MIT, Chris
# Torkildson; THIRD-PARTY.md): the BTstack libraries it links, and the two
# fixes to BTstack below, which are its own, unchanged. What is different:
# the sources are the shell's, the firmware is read from the card (the
# radio's firmware is not linked: cmake/cyw43_fw.cmake), and nothing
# wraps sb_read_keyboard().
#
#   include(cmake/cyw43_fw.cmake)   cyw43_fw_stream_setup()   # first
#   include(cmake/bluetooth.cmake)
#   target_link_libraries(prog shell_btkbd cyw43_fw_stream ...)
#
# The program's include path needs src/core (btstore.h, keycodes.h) and
# src/port. BTstack's settings (src/port/btstack_config.h) come with it.
get_filename_component(SHELL_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
add_library(shell_btkbd INTERFACE)
target_sources(shell_btkbd INTERFACE ${SHELL_ROOT}/src/port/btkbd.c)
target_include_directories(shell_btkbd INTERFACE ${SHELL_ROOT}/src/port ${SHELL_ROOT}/src/core)
target_compile_definitions(shell_btkbd INTERFACE CYW43_LWIP=0)
# classic Bluetooth and BLE over the CYW43, run in the background (an
# interrupt); btkbd.c supplies btstack_cyw43_init() (pairings on the SD
# card, not in flash), so pico_btstack_cyw43 isn't linked
target_link_libraries(shell_btkbd INTERFACE
    pico_rand pico_cyw43_arch_threadsafe_background
    pico_btstack_base pico_btstack_classic pico_btstack_ble
    pico_btstack_hci_transport_cyw43 pico_btstack_run_loop_async_context)

# Two fixes to BTstack, made at configure time from the SDK's own copies (so
# they follow the SDK; the build stops if the code they change has changed).
# btkbd_fix(file old new) swaps the SDK's file for the fixed one in the build.
# BTKBD_FIX_HCI and BTKBD_FIX_L2CAP turn each off, for a BTstack that has it
# fixed already (BlueKitchen took the l2cap.c one for their next release).
option(BTKBD_FIX_HCI "btkbd's fix to BTstack's hci.c" ON)
option(BTKBD_FIX_L2CAP "btkbd's fix to BTstack's l2cap.c" ON)
function(btkbd_fix FILE OLD NEW)
    set(ORIG ${PICO_BTSTACK_PATH}/src/${FILE})
    string(MAKE_C_IDENTIFIER ${FILE} NAME)
    set(FIXED ${CMAKE_BINARY_DIR}/btkbd_${NAME}.c)
    file(READ ${ORIG} TEXT)
    string(FIND "${TEXT}" "${OLD}" AT)
    if (AT EQUAL -1)
        message(FATAL_ERROR "bluetooth.cmake: BTstack's ${FILE} has changed; check whether it still needs btkbd's fix")
    endif()
    string(REPLACE "${OLD}" "${NEW}" TEXT "${TEXT}")
    file(CONFIGURE OUTPUT ${FIXED} CONTENT "${TEXT}" @ONLY)
    get_target_property(SRCS pico_btstack_base INTERFACE_SOURCES)
    list(REMOVE_ITEM SRCS ${ORIG})
    list(APPEND SRCS ${FIXED})
    set_target_properties(pico_btstack_base PROPERTIES INTERFACE_SOURCES "${SRCS}")
endfunction()

# 1. hci.c. With controller-to-host flow control on, when BTstack has to tell
# the controller "packets done" it sends that and stops, expecting to be run
# again when the transport has sent it. The CYW43's transport sends at once
# and says nothing, so a command still waiting is left until the next event.
# A Logitech K380 sends empty ACL fragments just as encryption starts, so the
# command that checks the key size (which opens the keyboard's connection)
# waited for an event that never came, until the keyboard gave up 20 s later.
# The fix: carry on when the transport is synchronous.
if (BTKBD_FIX_HCI)
btkbd_fix(hci.c
    "        hci_host_num_completed_packets();        \n        return;\n"
    "        hci_host_num_completed_packets();\n        // (btkbd: a synchronous transport is free again now, and nothing else\n        // would run this for commands still waiting)\n        if (!hci_transport_synchronous()) return;\n        if (!hci_can_send_command_packet_transport()) return;\n")
endif()

# 2. l2cap.c. Closing a channel that is still waiting for its connection to
# the keyboard (hid_host_disconnect() while it pages) sent a disconnect request
# down no connection, and the channel then waited for an answer for ever, so
# hid_host kept its record and refused to connect to that keyboard again
# (COMMAND_DISALLOWED) until a restart. The fix: such a channel fails at once,
# as it would if the connection had failed, which hid_host tidies up after.
if (BTKBD_FIX_L2CAP)
btkbd_fix(l2cap.c
    "    channel->state = L2CAP_STATE_WILL_SEND_DISCONNECT_REQUEST;\n    l2cap_run();\n    return ERROR_CODE_SUCCESS;\n}\n"
    "    // (btkbd: a classic channel not connected yet has no one to ask: fail it now)\n    if ((channel->channel_type == L2CAP_CHANNEL_TYPE_CLASSIC) &&\n        ((channel->state == L2CAP_STATE_WILL_SEND_CREATE_CONNECTION) || (channel->state == L2CAP_STATE_WAIT_CONNECTION_COMPLETE))){\n        btstack_linked_list_remove(&l2cap_channels, (btstack_linked_item_t *) channel);\n        l2cap_handle_channel_open_failed(channel, ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);\n        l2cap_free_channel_entry(channel);\n        return ERROR_CODE_SUCCESS;\n    }\n    channel->state = L2CAP_STATE_WILL_SEND_DISCONNECT_REQUEST;\n    l2cap_run();\n    return ERROR_CODE_SUCCESS;\n}\n")
endif()
