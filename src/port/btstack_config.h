/* btstack_config.h -- BTstack settings for a Bluetooth keyboard on the
 * PicoCalc (Pico 2 W): the HID hosts, classic and BLE.
 * Based on the Pico SDK's test/kitchen_sink/btstack_config.h. */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* (ENABLE_CLASSIC and ENABLE_BLE come from linking pico_btstack_classic
   and pico_btstack_ble) */
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_SECURE_CONNECTIONS
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 1
#define ENABLE_LOG_ERROR
#ifdef BTKBD_LOG
#define ENABLE_LOG_INFO	/* (into btpair's log, not printed) */
#endif
#define ENABLE_PRINTF_HEXDUMP	/* (the SDK always builds its hci dump; nothing is printed) */

/* buffers and sizes */
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

/* BTstack takes its memory from malloc, so no pool sizes are needed */
#define HAVE_MALLOC

/* keep the CYW43's shared bus from overrunning (as the SDK's sample does) */
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

/* pairings, kept on the SD card (see btkbd.c) */
#define NVM_NUM_LINK_KEYS 8
#define NVM_NUM_DEVICE_DB_ENTRIES 6	/* (BLE bondings) */

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#endif
