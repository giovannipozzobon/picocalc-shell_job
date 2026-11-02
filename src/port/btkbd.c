/* btkbd.c -- a Bluetooth keyboard for the PicoCalc (Pico 2 W): BTstack's
 * HID hosts, classic and Low Energy (HID over GATT), with the keys turned
 * into PicoCalc key events.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Chris Torkildson.
 *
 * Built on BTstack's hid_host_demo.c and hog_host_demo.c (BlueKitchen), free to use on
 * the Raspberry Pi Pico W and Pico 2 W under Raspberry Pi's licence.
 *
 * Taken from picocalc-extras' bluetooth/btkbd.c for the shell (design.md
 * §3.10; THIRD-PARTY.md). What is different here: the pairing file is
 * read and written by btstore.c, through FatFs, in the same format, so
 * btpair's pairing is the one used; the radio's firmware is read from the
 * card (cyw43_fw.h) and not held in RAM; and the keys are taken by
 * btkbd_read() alone, where the library wrapped sb_read_keyboard(). The
 * search and pairing code is as it was, and not used by the shell: pairing
 * is btpair's. Updated to picocalc-extras' btpair 2.1 (a keyboard and a
 * mouse, each in its own slot); the shell uses the keyboard, and the
 * mouse is connected but its movement is not read. */
#include "btkbd.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "hci_dump.h"
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/time.h"
#include "pico/btstack_cyw43.h"
#include "pico/btstack_run_loop_async_context.h"
#include "pico/btstack_hci_transport_cyw43.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "ble/le_device_db_tlv.h"
#include "btstore.h"
#include "cyw43_fw.h"
#include "keycodes.h"

/* ------------------------------------------------------------ storage */

/* A few tagged records (BTstack's link keys, and which keyboard is ours),
   kept in RAM and saved whole to the SD card when one changes (btstore.c:
   the file's format is btpair's). */
static bt_store_t store;
static bool store_upgraded;

static void store_load(void)
{
	store_upgraded = bt_store_load(&store) == BT_STORE_UPGRADED;
}

static bool store_dirty;
static absolute_time_t save_after;

/* saving can wait a moment: the card is slow, and a pairing in progress
   shouldn't be kept waiting for it */
static void store_changed(void)
{
	store_dirty = true;
	save_after = make_timeout_time_ms(3000);
}

static void store_save(void)
{
	store_dirty = false;
	bt_store_save(&store);
}

static int tlv_get(void *ctx, uint32_t tag, uint8_t *buf, uint32_t size)
{
	(void)ctx;
	return bt_store_get(&store, tag, buf, size);
}

static int tlv_store(void *ctx, uint32_t tag, const uint8_t *data, uint32_t size)
{
	(void)ctx;
	int failed = bt_store_put(&store, tag, data, size);
	if (!failed) store_changed();
	return failed;
}

static void tlv_delete(void *ctx, uint32_t tag)
{
	(void)ctx;
	if (bt_store_delete(&store, tag)) store_changed();
}

static const btstack_tlv_t tlv = { tlv_get, tlv_store, tlv_delete };

/* The SDK's own version of this keeps pairings in the last sectors of flash,
   which the PicoCalc's UF2 Loader and other programs may use; this one keeps
   them on the SD card instead. cyw43_arch_init() calls it. */
bool btstack_cyw43_init(async_context_t *context)
{
	btstack_memory_init();
	btstack_run_loop_init(btstack_run_loop_async_context_get_instance(context));
	hci_init(hci_transport_cyw43_instance(), NULL);
	store_load();
	btstack_tlv_set_instance(&tlv, NULL);
	hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(&tlv, NULL));
	le_device_db_tlv_configure(&tlv, NULL);		/* BLE bondings, the same way */
	return true;
}

void btstack_cyw43_deinit(async_context_t *context)
{
	(void)context;
	hci_power_control(HCI_POWER_OFF);
	hci_close();
	btstack_run_loop_async_context_deinit();
	btstack_run_loop_deinit();
	btstack_memory_deinit();
}

/* ------------------------------------------------------------ state */

/* Bluetooth as a whole: OFF, STARTING, IDLE (running: see each device for
   its own state), SEARCHING or PAIRING */
static btkbd_state_t state = BTKBD_OFF;
static char status[48] = "Bluetooth off";
static char code[12];			/* what to type while pairing */

/* A keyboard and a mouse, each paired, kept and connected on its own. */
typedef struct {
	uint32_t tag, tag_le;		/* its record in the store: classic, BLE */
	bool paired, le;
	bd_addr_t addr;
	bd_addr_type_t type;		/* (BLE) */
	char name[33];
	bool connected;
	/* classic: the HID connection, or 0 */
	uint16_t cid;
	bool boot_mode;			/* that connection is in the simple boot mode */
	bool descriptor_ok;
	int stuck;
	btstack_timer_source_t stall_timer;
	hci_con_handle_t stall_handle;
	/* BLE: the connection, and the HID service on it */
	hci_con_handle_t le_handle;
	uint16_t hids_cid;
} slot_t;

static slot_t slots[2] = {
	[BTKBD_KEYBOARD] = { .tag = BT_TAG_KEYBOARD, .tag_le = BT_TAG_KEYBOARD_LE, .le_handle = HCI_CON_HANDLE_INVALID },
	[BTKBD_MOUSE] = { .tag = BT_TAG_MOUSE, .tag_le = BT_TAG_MOUSE_LE, .le_handle = HCI_CON_HANDLE_INVALID },
};
#define KBD (&slots[BTKBD_KEYBOARD])
#define MOUSE (&slots[BTKBD_MOUSE])
static slot_t *other(slot_t *s) { return s == KBD ? MOUSE : KBD; }

static slot_t *slot_by_cid(uint16_t c)
{
	for (int i = 0; i < 2; i++) if (c && slots[i].cid == c) return &slots[i];
	return NULL;
}
static slot_t *slot_by_le(hci_con_handle_t h)
{
	for (int i = 0; i < 2; i++) if (h != HCI_CON_HANDLE_INVALID && slots[i].le_handle == h) return &slots[i];
	return NULL;
}
static slot_t *slot_by_hids(uint16_t c)
{
	for (int i = 0; i < 2; i++) if (c && slots[i].hids_cid == c) return &slots[i];
	return NULL;
}
static slot_t *slot_by_addr(const bd_addr_t a, bool le)
{
	for (int i = 0; i < 2; i++)
		if (slots[i].paired && slots[i].le == le && bd_addr_cmp(slots[i].addr, a) == 0) return &slots[i];
	return NULL;
}

static uint8_t descriptor[1024];		/* (classic: both devices' layouts) */
static btstack_packet_callback_registration_t hci_cb;
static btstack_timer_source_t retry_timer;
#define RETRY_MS 15000

/* BLE: one connection is made at a time */
static uint8_t le_descriptor[1024];
static enum { LE_IDLE, LE_SEARCHING, LE_WAITING, LE_CONNECTING } le_mode;
static slot_t *le_connecting;			/* the device that connection is for */
static btstack_timer_source_t le_timer;
static bd_addr_t le_ident;			/* while pairing: its identity address, once known */
static bd_addr_type_t le_ident_type;
static bool le_ident_known;
static bd_addr_t le_looked_up;			/* the last private address asked about */
static btstack_packet_callback_registration_t sm_cb;
static void le_wait(void);
static int classic_after_le = -1;	/* a pairing waiting for the old BLE device to let go */

/* BLE pairing: the PicoCalc can show a code, and offers Secure Connections;
   it takes the older kind of pairing too, which many keyboards still use
   (BTstack starts out accepting Secure Connections only). It doesn't insist
   on a code (MITM protection): a keyboard that wants one asks for it, and
   one paired without (the XK01 has no way to type one) would otherwise be
   paired again, rather than reconnected, every time it wakes up. */
static void le_set_method(void)
{
	sm_set_io_capabilities(IO_CAPABILITY_KEYBOARD_DISPLAY);
	sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
	sm_set_secure_connections_only_mode(false);
	sm_set_encryption_key_size_range(7, 16);
}

#define FOUND_MAX 8
static struct {
	bd_addr_t addr;
	uint8_t psrm;
	uint16_t clock;
	bool named;
	bool le;
	int kind;			/* BTKBD_KEYBOARD, BTKBD_MOUSE, or -1 if it doesn't say */
	bd_addr_type_t type;
	char name[32];
} found[FOUND_MAX];
static int n_found, naming = -1, pairing_with = -1, pair_after_search = -1;
static slot_t *pairing_slot = &slots[BTKBD_KEYBOARD];	/* what's being paired, or was last */
static bool pair_saved;				/* the pairing got as far as being saved */
static btstack_timer_source_t name_timer;
#define NAME_MS 5000

/* for the screen, while this is being tried out */
static uint8_t last_event;
static int d_conn = -1, d_pair = -1, d_auth = -1, d_enc = -1, d_disc = -1, d_io = -1, d_hid = -1;
#define TRACE 24
static uint8_t trace[TRACE];
static int n_trace;
static void note_err(const char *where, uint8_t err) { (void)where; d_hid = err; }
static void hx(char *b, size_t n, const char *k, int v)
{
	size_t l = strlen(b);
	if (v < 0) snprintf(b + l, n - l, "%s-- ", k);
	else snprintf(b + l, n - l, "%s%02X ", k, v);
}
const char *btkbd_diag(void)
{
	static char b[64];
	snprintf(b, sizeof b, "st%d ", (int)state);
	hx(b, sizeof b, "con", d_conn); hx(b, sizeof b, "io", d_io); hx(b, sizeof b, "pr", d_pair);
	hx(b, sizeof b, "au", d_auth); hx(b, sizeof b, "en", d_enc); hx(b, sizeof b, "dc", d_disc); hx(b, sizeof b, "hid", d_hid);
	return b;
}
/* the last Bluetooth events, 12 to a line: line 0 the older, 1 the newer */
const char *btkbd_trace_line(int line)
{
	static char b[2][40];
	char *o = b[line & 1];
	o[0] = 0;
	int from = n_trace > 12 ? n_trace - 12 : 0;
	if (line == 0) { int to = from; from = to > 12 ? to - 12 : 0; for (int i = from; i < to; i++) { size_t l = strlen(o); snprintf(o + l, 40 - l, "%02X ", trace[i]); } return o; }
	for (int i = from; i < n_trace; i++) { size_t l = strlen(o); snprintf(o + l, 40 - l, "%02X ", trace[i]); }
	return o;
}
const char *btkbd_trace(void) { return btkbd_trace_line(1); }
/* the BLE pairing messages each way (L2CAP channel 6), for the screen:
   BTstack hands every packet to an "HCI dump", and this one keeps those */
#define SMP_LINES 10
static char smp[SMP_LINES][40];
static int n_smp;
static void lock(void);
static void unlock(void);
#ifdef BTKBD_LOG
/* everything BTstack sends and receives, and what it says (log_info), kept
   in memory in Apple's PacketLogger format, which Wireshark reads: a
   program saves it with btkbd_log() */
#define LOG_SIZE (256 * 1024)
static uint8_t log_buf[LOG_SIZE];
static size_t log_len;
static uint32_t log_lost;
static void log_record(uint8_t type, uint8_t in, const uint8_t *p, uint16_t len)
{
	if (len > 1100) len = 1100;	/* (a long packet keeps its start) */
	if (log_len + HCI_DUMP_HEADER_SIZE_PACKETLOGGER + len > LOG_SIZE) { log_lost++; return; }
	uint64_t us = time_us_64();
	hci_dump_setup_header_packetlogger(log_buf + log_len, (uint32_t)(us / 1000000), (uint32_t)(us % 1000000), type, in, len);
	memcpy(log_buf + log_len + HCI_DUMP_HEADER_SIZE_PACKETLOGGER, p, len);
	log_len += HCI_DUMP_HEADER_SIZE_PACKETLOGGER + len;
}
const uint8_t *btkbd_log(size_t *len, uint32_t *lost)
{
	lock();
	*len = log_len;		/* (what's there now stays put: records only go after it) */
	if (lost) *lost = log_lost;
	unlock();
	return log_buf;
}
#else
const uint8_t *btkbd_log(size_t *len, uint32_t *lost) { *len = 0; if (lost) *lost = 0; return NULL; }
#endif

static void dump_packet(uint8_t type, uint8_t in, uint8_t *p, uint16_t len)
{
#ifdef BTKBD_LOG
	log_record(type, in, p, len);
#endif
	/* HCI commands, and the controller's answers to them (to see what a
	   stalled connection is waiting for) */
	if (type == HCI_COMMAND_DATA_PACKET && len >= 2) {
		uint16_t op = little_endian_read_16(p, 0);
		if (op == 0x0C35 || op == 0x200C || op == 0x200B) return;	/* (routine: flow control, BLE scanning) */
		if (n_smp == SMP_LINES) { memmove(smp[0], smp[1], sizeof smp[0] * (SMP_LINES - 1)); n_smp--; }
		snprintf(smp[n_smp++], sizeof smp[0], "cmd %04X", op);
		return;
	}
	if (type == HCI_EVENT_PACKET && len >= 6 && (p[0] == 0x0E || p[0] == 0x0F)) {
		uint16_t op = little_endian_read_16(p, p[0] == 0x0E ? 3 : 4);
		uint8_t st = p[0] == 0x0E ? (len > 5 ? p[5] : 0) : p[2];
		if (op == 0 || op == 0x0C35 || op == 0x200C || op == 0x200B) return;
		if (n_smp == SMP_LINES) { memmove(smp[0], smp[1], sizeof smp[0] * (SMP_LINES - 1)); n_smp--; }
		snprintf(smp[n_smp++], sizeof smp[0], "  %s %04X st %02X", p[0] == 0x0E ? "done" : "stat", op, st);
		return;
	}
	if (type != HCI_ACL_DATA_PACKET || len < 9) return;
	/* the Security Manager's (6, BLE pairing), and on classic links the
	   L2CAP signalling channel (1): connection requests and their answers */
	uint16_t ch = little_endian_read_16(p, 6);
	if (ch != 0x0006 && ch != 0x0001) return;
	if (ch == 0x0001 && (p[8] < 0x02 || p[8] > 0x0B)) return;	/* connect, configure, disconnect, information */
	if (n_smp == SMP_LINES) { memmove(smp[0], smp[1], sizeof smp[0] * (SMP_LINES - 1)); n_smp--; }
	char *b = smp[n_smp++];
	snprintf(b, sizeof smp[0], "%s", in ? "in " : "out");
	for (int i = 8; i < len && i < (ch == 0x0001 ? 20 : 15); i++) {
		size_t l = strlen(b);
		snprintf(b + l, sizeof smp[0] - l, " %02X", p[i]);
	}
}
static void dump_reset(void) {}
static void dump_message(int level, const char *f, va_list a)
{
	(void)level;
#ifdef BTKBD_LOG
	char m[HCI_DUMP_MAX_MESSAGE_LEN];
	int n = vsnprintf(m, sizeof m, f, a);
	if (n < 0) return;
	if (n >= (int)sizeof m) n = sizeof m - 1;
	log_record(LOG_MESSAGE_PACKET, 0, (const uint8_t *)m, (uint16_t)(n + 1));
#else
	(void)f; (void)a;
#endif
}
static const hci_dump_t smp_dump = { dump_reset, dump_packet, dump_message };
const char *btkbd_smp(int i) { return i >= 0 && i < n_smp ? smp[i] : ""; }

static void reset_diag(void) { n_smp = 0; d_conn = d_pair = d_auth = d_enc = d_disc = d_io = d_hid = -1; n_trace = 0; }
static int pair_again = -1, pair_tries;	/* a pairing waiting for an old connection to go */
static btstack_timer_source_t pair_timer;
static void start_pairing(int i);

static void set_status(const char *s) { snprintf(status, sizeof status, "%s", s); }

static void load_slot(slot_t *s)
{
	bt_record_t *r = bt_store_find(&store, s->tag), *l = bt_store_find(&store, s->tag_le);
	s->le = !(r && r->len >= 6) && l && l->len >= 7;
	if (s->le) r = l;
	s->paired = r && r->len >= 6;
	memset(s->name, 0, sizeof s->name);
	if (!s->paired) return;
	memcpy(s->addr, r->data, 6);
	int at = 6;
	if (s->le) s->type = (bd_addr_type_t)r->data[at++];
	int n = (int)r->len - at;
	if (n > (int)sizeof s->name - 1) n = sizeof s->name - 1;
	memcpy(s->name, r->data + at, n);
}

static void load_paired(void) { load_slot(KBD); load_slot(MOUSE); }

/* one keyboard and one mouse: a new one replaces the old */
static void save_paired(slot_t *s, const bd_addr_t addr, const char *name, bool le, bd_addr_type_t type)
{
	uint8_t d[BT_TLV_DATA];
	size_t n = strlen(name), at = 6;
	if (n > 32) n = 32;
	memcpy(d, addr, 6);
	if (le) d[at++] = (uint8_t)type;
	memcpy(d + at, name, n);
	tlv_delete(NULL, le ? s->tag : s->tag_le);
	tlv_store(NULL, le ? s->tag_le : s->tag, d, (uint32_t)(at + n));
	/* (the same device paired as the other one too: it's only this now) */
	slot_t *o = other(s);
	if (o->paired && o->le == le && bd_addr_cmp(o->addr, addr) == 0) {
		tlv_delete(NULL, o->tag);
		tlv_delete(NULL, o->tag_le);
	}
	pair_saved = true;
	load_paired();
}

/* ------------------------------------------------------------ key events */

#define EQ 64
static volatile uint16_t eq[EQ];		/* filled in the background, emptied by the program */
static volatile unsigned eq_head, eq_tail;

static void put_event(uint8_t st, uint8_t c)
{
	unsigned next = (eq_head + 1) % EQ;
	if (next == eq_tail) return;		/* full: drop it */
	eq[eq_head] = (uint16_t)(st << 8 | c);
	eq_head = next;
}

uint16_t btkbd_read(void)
{
	unsigned t = eq_tail;
	if (t == eq_head) return 0;
	uint16_t e = eq[t];
	eq_tail = (t + 1) % EQ;
	return e;
}

/* HID usage (page 7) to PicoCalc code: letters lower case, the rest as the
   PicoCalc's keyboard sends them; 0 for keys it doesn't have */
static uint8_t code_of(uint8_t u, bool shift)
{
	static const char plain[] = "1234567890", shifted[] = "!@#$%^&*()";
	static const char sym[] = "-=[]\\#;'`,./", sym_s[] = "_+{}|~:\"~<>?";
	if (u >= 0x04 && u <= 0x1D) return (uint8_t)('a' + u - 0x04);
	if (u >= 0x1E && u <= 0x27) return (uint8_t)(shift ? shifted[u - 0x1E] : plain[u - 0x1E]);
	if (u >= 0x2D && u <= 0x38) return (uint8_t)(shift ? sym_s[u - 0x2D] : sym[u - 0x2D]);
	if (u >= 0x3A && u <= 0x43) {		/* F1-F10 */
		static const uint8_t f[] = { KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10 };
		return f[u - 0x3A];
	}
	if (u >= 0x59 && u <= 0x62) return (uint8_t)("1234567890"[u - 0x59]);	/* keypad digits */
	switch (u) {
	case 0x28: case 0x58: return KEY_ENTER;
	case 0x29: return KEY_ESC;
	case 0x2A: return KEY_BACKSPACE;
	case 0x2B: return KEY_TAB;
	case 0x2C: return ' ';
	case 0x39: return KEY_CAPS_LOCK;
	case 0x48: return KEY_BREAK;
	case 0x49: return KEY_INSERT;
	case 0x4A: return KEY_HOME;
	case 0x4B: return KEY_PAGE_UP;
	case 0x4C: return KEY_DEL;
	case 0x4D: return KEY_END;
	case 0x4E: return KEY_PAGE_DOWN;
	case 0x4F: return KEY_RIGHT;
	case 0x50: return KEY_LEFT;
	case 0x51: return KEY_DOWN;
	case 0x52: return KEY_UP;
	case 0x54: return '/';
	case 0x55: return '*';
	case 0x56: return '-';
	case 0x57: return '+';
	case 0x63: return '.';
	}
	return 0;
}

/* what's held down, as the PicoCalc codes sent for it */
#define HELD_MAX 6
static uint8_t held_u[HELD_MAX], held_c[HELD_MAX];
static uint8_t mods;			/* HID modifier bits: 0x11 Ctrl, 0x02 left Shift, 0x20 right Shift, 0x44 Alt */

static void set_mods(uint8_t m)
{
	struct { uint8_t bits, code; } map[] = {
		{ 0x11, KEY_MOD_CTRL }, { 0x02, KEY_MOD_SHL }, { 0x20, KEY_MOD_SHR }, { 0x44, KEY_MOD_ALT },
	};
	for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
		bool was = mods & map[i].bits, now = m & map[i].bits;
		if (now != was) put_event(now ? KEY_STATE_PRESSED : KEY_STATE_RELEASED, map[i].code);
	}
	mods = m;
}

static void release_all(void)
{
	for (int i = 0; i < HELD_MAX; i++)
		if (held_u[i]) { if (held_c[i]) put_event(KEY_STATE_RELEASED, held_c[i]); held_u[i] = held_c[i] = 0; }
	set_mods(0);
}

/* a new set of keys held down: releases for the ones let go, presses for new ones */
static void keys_now(uint8_t m, const uint8_t *keys, int n)
{
	set_mods(m);
	bool shift = m & 0x22;
	for (int i = 0; i < HELD_MAX; i++) {
		if (!held_u[i]) continue;
		bool still = false;
		for (int k = 0; k < n; k++) still |= keys[k] == held_u[i];
		if (!still) {
			if (held_c[i]) put_event(KEY_STATE_RELEASED, held_c[i]);
			held_u[i] = held_c[i] = 0;
		}
	}
	for (int k = 0; k < n; k++) {
		uint8_t u = keys[k];
		if (u < 0x04) continue;			/* none, or "too many keys" */
		bool known = false;
		for (int i = 0; i < HELD_MAX; i++) known |= held_u[i] == u;
		if (known) continue;
		for (int i = 0; i < HELD_MAX; i++) {
			if (held_u[i]) continue;
			held_u[i] = u;
			held_c[i] = code_of(u, shift);
			if (held_c[i]) put_event(KEY_STATE_PRESSED, held_c[i]);
			break;
		}
	}
}

static void parse_report(const uint8_t *desc, uint16_t desc_len, const uint8_t *report, uint16_t len);
static volatile unsigned ptr_reports;
static volatile int ptr_x, ptr_y, ptr_wheel, ptr_buttons;	/* added up, since the start */

/* a device let go: nothing it held is held any more */
static void let_go(slot_t *s)
{
	s->connected = false;
	if (s == KBD) release_all();
	else ptr_buttons = 0;
}

static void handle_report(slot_t *s, const uint8_t *report, uint16_t len)
{
	if (len < 1 || report[0] != 0xA1) return;		/* an input report */
	if (s->boot_mode)	/* the layout every keyboard has in boot mode */
		parse_report(btstack_hid_get_boot_descriptor_data(), btstack_hid_get_boot_descriptor_len(), report + 1, len - 1);
	else if (s->descriptor_ok)	/* the device's own layout, once it has given it */
		parse_report(hid_descriptor_storage_get_descriptor_data(s->cid), hid_descriptor_storage_get_descriptor_len(s->cid),
		             report + 1, len - 1);
}

/* a report from a BLE device: as from a classic one, without the A1 */
static void handle_le_report(slot_t *s, uint8_t service, const uint8_t *report, uint16_t len)
{
	parse_report(hids_host_descriptor_storage_get_descriptor_data(s->hids_cid, service),
	             hids_host_descriptor_storage_get_descriptor_len(s->hids_cid, service), report, len);
}

static void parse_report(const uint8_t *desc, uint16_t desc_len, const uint8_t *report, uint16_t len)
{
	if (!desc || !desc_len || !len) return;
	btstack_hid_parser_t parser;
	btstack_hid_parser_init(&parser, desc, desc_len, HID_REPORT_TYPE_INPUT, report, len);
	uint8_t m = 0, keys[HELD_MAX];
	int n = 0;
	bool any = false, pointer = false;
	int32_t px = 0, py = 0, pw = 0, pb = 0;
	while (btstack_hid_parser_has_more(&parser)) {
		uint16_t page, usage;
		int32_t value;
		btstack_hid_parser_get_field(&parser, &page, &usage, &value);
		/* a mouse, or a keyboard's trackpad */
		if (page == 0x01 && (usage == 0x30 || usage == 0x31 || usage == 0x38)) {
			pointer = true;
			if (usage == 0x30) px = value; else if (usage == 0x31) py = value; else pw = value;
			continue;
		}
		if (page == 0x09 && usage >= 1 && usage <= 8) { pointer = true; if (value) pb |= 1 << (usage - 1); continue; }
		if (page != 0x07) continue;		/* only the keyboard page */
		any = true;
		if (usage >= 0xE0 && usage <= 0xE7) { if (value) m |= (uint8_t)(1 << (usage - 0xE0)); continue; }
		if (usage && n < HELD_MAX) keys[n++] = (uint8_t)usage;
	}
	if (any) keys_now(m, keys, n);		/* (a report from another page, e.g. media keys, changes nothing) */
	if (pointer) {
		ptr_reports++;
		ptr_x += px, ptr_y += py, ptr_wheel += pw, ptr_buttons = pb;
	}
}

void btkbd_pointer(unsigned *reports, int *x, int *y, int *wheel, int *buttons)
{
	*reports = ptr_reports, *x = ptr_x, *y = ptr_y, *wheel = ptr_wheel, *buttons = ptr_buttons;
}

static void lock(void);
static void unlock(void);
bool btkbd_mouse(int *dx, int *dy, int *wheel, int *buttons)
{
	static int took_x, took_y, took_w, took_b;
	*dx = *dy = *wheel = *buttons = 0;
	if (state == BTKBD_OFF) return false;
	lock();
	int x = ptr_x, y = ptr_y, w = ptr_wheel, b = ptr_buttons;
	unlock();
	*dx = x - took_x, *dy = y - took_y, *wheel = w - took_w, *buttons = b;
	bool any = *dx || *dy || *wheel || b != took_b;
	took_x = x, took_y = y, took_w = w, took_b = b;
	return any;
}

/* ------------------------------------------------------------ connecting */

static void trace_mark(uint8_t t)
{
	if (n_trace == TRACE) { memmove(trace, trace + 1, TRACE - 1); n_trace--; }
	trace[n_trace++] = t;
}

static void try_paired(slot_t *s)
{
	if (!s->paired || s->le || s->cid || state != BTKBD_IDLE) return;
	/* A keyboard in boot mode: it goes straight to the keyboard connection,
	   without first asking the keyboard for its layout -- which some
	   keyboards (the K380, after pairing) don't answer, as Linux and others
	   have found. Boot mode has all the ordinary keys. A mouse in its own
	   layout, which has the wheel. */
	hid_protocol_mode_t mode = s == KBD ? HID_PROTOCOL_MODE_BOOT : HID_PROTOCOL_MODE_REPORT;
	uint8_t err = hid_host_connect(s->addr, mode, &s->cid);
	s->boot_mode = err == ERROR_CODE_SUCCESS && mode == HID_PROTOCOL_MODE_BOOT;
	trace_mark(0xAA);		/* (in the trace: AA, we asked for the connection) */
	if (err != ERROR_CODE_SUCCESS) { s->cid = 0; note_err("retry", err); }
}

/* now and then: give up an attempt that never finished, and try a classic
   device that isn't connected (one at a time, taking turns) */
static void on_retry(btstack_timer_source_t *t)
{
	(void)t;
	static int turn;
	bool trying = false;
	for (int i = 0; i < 2; i++) {
		slot_t *s = &slots[i];
		if (state == BTKBD_IDLE && s->paired && !s->le && s->cid && !s->connected) {
			if (++s->stuck >= 2) { hid_host_disconnect(s->cid); s->cid = 0; s->stuck = 0; }
			else trying = true;
		} else {
			s->stuck = 0;
		}
	}
	if (!trying && state == BTKBD_IDLE)
		for (int k = 0; k < 2; k++) {
			slot_t *s = &slots[(turn + k) % 2];
			if (s->paired && !s->le && !s->cid) { try_paired(s); turn = (turn + k + 1) % 2; break; }
		}
	btstack_run_loop_set_timer(&retry_timer, RETRY_MS);
	btstack_run_loop_add_timer(&retry_timer);
}

/* running, and not searching or pairing: the status line says how the
   keyboard is (or the mouse, if there's no keyboard) */
static void idle_status(void)
{
	state = BTKBD_IDLE;
	slot_t *s = KBD->paired ? KBD : MOUSE->paired ? MOUSE : NULL;
	if (!s) set_status("No keyboard paired");
	else if (s->connected) snprintf(status, sizeof status, "Connected: %.30s", s->name);
	else snprintf(status, sizeof status, "Waiting for %.24s", s->name[0] ? s->name : s == KBD ? "the keyboard" : "the mouse");
}

/* a pairing is over, one way or the other */
static void pairing_over(const char *msg)
{
	pairing_with = -1;
	idle_status();
	if (msg) set_status(msg);
	le_wait();
}

static void on_name_timeout(btstack_timer_source_t *t);

static void name_next(void)
{
	btstack_run_loop_remove_timer(&name_timer);
	if (pair_after_search < 0)
		for (int i = 0; i < n_found; i++)
			if (!found[i].named) {
				naming = i;
				gap_remote_name_request(found[i].addr, found[i].psrm, found[i].clock | 0x8000);
				btstack_run_loop_set_timer_handler(&name_timer, on_name_timeout);
				btstack_run_loop_set_timer(&name_timer, NAME_MS);
				btstack_run_loop_add_timer(&name_timer);
				return;
			}
	naming = -1;
	/* A keyboard that speaks both (the K380 does) is found twice: keep it
	   as classic, which pairs and reconnects more reliably */
	for (int i = 0; i < n_found; i++) {
		if (!found[i].le) continue;
		bool twin = false;
		for (int j = 0; j < n_found; j++)
			twin |= !found[j].le && found[j].named && strcmp(found[j].name, found[i].name) == 0;
		if (!twin) continue;
		if (pair_after_search == i) pair_after_search = -1;
		else if (pair_after_search > i) pair_after_search--;
		memmove(&found[i], &found[i + 1], sizeof found[0] * (n_found - i - 1));
		n_found--, i--;
	}
	idle_status();
	snprintf(status, sizeof status, n_found ? "Found %d: choose one" : "Nothing found", n_found);
	if (pair_after_search < 0) le_wait();
	if (pair_after_search >= 0) { int i = pair_after_search; pair_after_search = -1; start_pairing(i); }
}

static void on_name_timeout(btstack_timer_source_t *t)
{
	(void)t;
	if (naming >= 0) {		/* no answer: call it by its address */
		snprintf(found[naming].name, sizeof found[naming].name, "%s", bd_addr_to_str(found[naming].addr));
		found[naming].named = true;
		name_next();
	}
}

/* ------------------------------------------------------------ BLE */

static void le_stop(void)
{
	if (le_mode == LE_SEARCHING || le_mode == LE_WAITING) gap_stop_scan();
	if (le_mode == LE_CONNECTING) gap_connect_cancel();
	btstack_run_loop_remove_timer(&le_timer);
	le_mode = LE_IDLE;
	le_connecting = NULL;
}

/* a BLE device paired and not connected: listen for it (it advertises when
   it wakes up), a slice of the time, to spare the battery. Not while a BLE
   connection is still being set up. */
static void le_wait(void)
{
	if (le_mode != LE_IDLE || state != BTKBD_IDLE) return;
	bool want = false;
	for (int i = 0; i < 2; i++) {
		slot_t *s = &slots[i];
		if (s->le_handle != HCI_CON_HANDLE_INVALID && !s->connected) return;
		want |= s->paired && s->le && s->le_handle == HCI_CON_HANDLE_INVALID;
	}
	if (!want) return;
	memset(le_looked_up, 0, sizeof le_looked_up);
	gap_set_scan_parameters(0, 0x0140, 0x0030);	/* passive: 30 ms in every 200 */
	gap_start_scan();
	le_mode = LE_WAITING;
}

static void on_le_timeout(btstack_timer_source_t *t)
{
	(void)t;
	if (le_mode != LE_CONNECTING) return;
	gap_connect_cancel();
	le_mode = LE_IDLE;
	slot_t *s = le_connecting;
	le_connecting = NULL;
	if (state == BTKBD_PAIRING && s == pairing_slot) pairing_over("Pairing failed: try again");
	le_wait();
}

static void le_connect(slot_t *s, const bd_addr_t addr, bd_addr_type_t type)
{
	if (le_mode == LE_SEARCHING || le_mode == LE_WAITING) gap_stop_scan();
	le_mode = LE_CONNECTING;
	le_connecting = s;
	btstack_run_loop_remove_timer(&le_timer);
	btstack_run_loop_set_timer_handler(&le_timer, on_le_timeout);
	btstack_run_loop_set_timer(&le_timer, 15000);
	btstack_run_loop_add_timer(&le_timer);
	bd_addr_t a;
	bd_addr_copy(a, addr);
	if (gap_connect(a, type) != ERROR_CODE_SUCCESS) {
		le_mode = LE_IDLE;
		le_connecting = NULL;
		btstack_run_loop_remove_timer(&le_timer);
	}
}

static bool ad_name(const uint8_t *ad, uint8_t len, char *out, size_t size)
{
	ad_context_t c;
	for (ad_iterator_init(&c, len, ad); ad_iterator_has_more(&c); ad_iterator_next(&c)) {
		uint8_t t = ad_iterator_get_data_type(&c);
		if (t != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME && t != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) continue;
		int n = ad_iterator_get_data_len(&c);
		if (n > (int)size - 1) n = (int)size - 1;
		if (n <= 0) return false;
		memcpy(out, ad_iterator_get_data(&c), n);
		out[n] = 0;
		return true;
	}
	return false;
}

/* what it says it looks like: 0x03C1 a keyboard, 0x03C2 a mouse, 0x03C0 a
   HID device of some sort; or 0 */
static uint16_t ad_appearance(const uint8_t *ad, uint8_t len)
{
	ad_context_t c;
	for (ad_iterator_init(&c, len, ad); ad_iterator_has_more(&c); ad_iterator_next(&c))
		if (ad_iterator_get_data_type(&c) == BLUETOOTH_DATA_TYPE_APPEARANCE && ad_iterator_get_data_len(&c) >= 2)
			return little_endian_read_16(ad_iterator_get_data(&c), 0);
	return 0;
}

/* a keyboard, a mouse, or something with the HID service */
static bool ad_is_hid(const uint8_t *ad, uint8_t len)
{
	if (ad_data_contains_uuid16(len, ad, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE)) return true;
	uint16_t a = ad_appearance(ad, len);
	return a == 0x03C0 || a == 0x03C1 || a == 0x03C2;
}

static void le_advert(const uint8_t *packet)
{
	bd_addr_t addr;
	gap_event_advertising_report_get_address(packet, addr);
	bd_addr_type_t type = (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet);
	const uint8_t *ad = gap_event_advertising_report_get_data(packet);
	uint8_t len = gap_event_advertising_report_get_data_length(packet);
	uint8_t kind = gap_event_advertising_report_get_advertising_event_type(packet);

	if (le_mode == LE_SEARCHING) {
		int i;
		for (i = 0; i < n_found; i++) if (found[i].le && bd_addr_cmp(found[i].addr, addr) == 0) break;
		if (i == n_found) {			/* new: a keyboard or mouse in pairing mode? */
			if (kind == 4 || n_found >= FOUND_MAX || !ad_is_hid(ad, len)) return;
			i = n_found++;
			memset(&found[i], 0, sizeof found[i]);
			bd_addr_copy(found[i].addr, addr);
			found[i].le = true;
			found[i].named = true;		/* (called by its address until it says) */
			found[i].type = type;
			found[i].kind = -1;
			snprintf(found[i].name, sizeof found[i].name, "%s", bd_addr_to_str(addr));
			snprintf(status, sizeof status, "Searching... found %d", n_found);
		}
		uint16_t a = ad_appearance(ad, len);	/* (it may be in the scan response) */
		if (a == 0x03C1) found[i].kind = BTKBD_KEYBOARD;
		if (a == 0x03C2) found[i].kind = BTKBD_MOUSE;
		ad_name(ad, len, found[i].name, sizeof found[i].name);
		return;
	}
	if (le_mode != LE_WAITING) return;
	if (kind != 0 && kind != 1) return;		/* only advertising that takes a connection */
	slot_t *s = slot_by_addr(addr, true);
	if (s && s->le_handle == HCI_CON_HANDLE_INVALID) { le_connect(s, addr, type); return; }
	/* a private address, which changes: ask the Security Manager whose it is */
	if (type == BD_ADDR_TYPE_LE_RANDOM && (addr[0] & 0xC0) == 0x40 && bd_addr_cmp(addr, le_looked_up) != 0) {
		bd_addr_copy(le_looked_up, addr);
		sm_address_resolution_lookup((uint8_t)type, addr);
	}
}

static void le_connected(const uint8_t *packet)
{
	if (le_mode != LE_CONNECTING) return;		/* (not ours) */
	btstack_run_loop_remove_timer(&le_timer);
	le_mode = LE_IDLE;
	slot_t *s = le_connecting;
	le_connecting = NULL;
	d_conn = gap_subevent_le_connection_complete_get_status(packet);
	if (d_conn != ERROR_CODE_SUCCESS || !s) {
		if (state == BTKBD_PAIRING && s == pairing_slot) pairing_over("Pairing failed: try again");
		le_wait();
		return;
	}
	s->le_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
	sm_request_pairing(s->le_handle);	/* pairs, or (paired before) turns encryption on */
}

static void le_hid_event(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size);

static void le_open_hid(slot_t *s)
{
	if (s->le_handle == HCI_CON_HANDLE_INVALID || s->hids_cid) return;
	if (hids_host_connect(s->le_handle, le_hid_event, HID_PROTOCOL_MODE_REPORT, &s->hids_cid) != ERROR_CODE_SUCCESS) {
		s->hids_cid = 0;
		gap_disconnect(s->le_handle);
	}
}

static void le_disconnected(slot_t *s)
{
	s->le_handle = HCI_CON_HANDLE_INVALID;
	s->hids_cid = 0;
	let_go(s);
	if (classic_after_le >= 0 && s == pairing_slot) {	/* the radio is free now: pair the new one */
		int i = classic_after_le;
		classic_after_le = -1;
		start_pairing(i);
		return;
	}
	if (state == BTKBD_PAIRING && s == pairing_slot) {
		code[0] = 0;
		pairing_over(pair_saved ? (s == KBD ? "Paired: press a key on the keyboard" : "Paired: move the mouse")
		                        : "Pairing failed: pairing mode, then try again");
		return;
	}
	if (state == BTKBD_IDLE) idle_status();
	le_wait();
}

static void le_hid_event(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
	(void)type; (void)channel; (void)size;
	if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;
	slot_t *s;
	switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
	case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
		s = slot_by_hids(gattservice_subevent_hid_service_connected_get_hids_cid(packet));
		if (!s) break;
		d_hid = gattservice_subevent_hid_service_connected_get_status(packet);
		if (d_hid != ERROR_CODE_SUCCESS) { s->hids_cid = 0; gap_disconnect(s->le_handle); break; }
		s->connected = true;
		if (state == BTKBD_PAIRING && s == pairing_slot) pairing_over(NULL);
		else if (state == BTKBD_IDLE) idle_status();
		le_wait();			/* (for the other device, if it's BLE) */
		break;
	case GATTSERVICE_SUBEVENT_HID_REPORT:
		s = slot_by_hids(gattservice_subevent_hid_report_get_hids_cid(packet));
		if (s)
			handle_le_report(s, gattservice_subevent_hid_report_get_service_index(packet),
			                 gattservice_subevent_hid_report_get_report(packet),
			                 gattservice_subevent_hid_report_get_report_len(packet));
		break;
	case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
		s = slot_by_hids(gattservice_subevent_hid_service_disconnected_get_hids_cid(packet));
		if (s) s->hids_cid = 0;
		break;
	}
}

/* The paired device is connected and secure. It should open the HID
   connection now (or let us open ours); the K380, now and then, does
   neither, and sits there. If nothing has come of it in a few seconds,
   hang up: on its next try it usually gets it right. */
static void on_stall(btstack_timer_source_t *t)
{
	slot_t *s = btstack_run_loop_get_timer_context(t);
	if (s->connected || s->le) return;	/* (it made it) */
	trace_mark(0xBB);			/* (in the trace: BB, we hung up on a stall) */
	if (s->cid) { hid_host_disconnect(s->cid); s->cid = 0; }
	gap_disconnect(s->stall_handle);
	set_status(s == KBD ? "Press a key on the keyboard again" : "Move the mouse again");
}

static void watch_for_stall(hci_con_handle_t h)
{
	hci_connection_t *c = hci_connection_for_handle(h);
	slot_t *s = c ? slot_by_addr(c->address, false) : NULL;
	if (!s || s->connected || state != BTKBD_IDLE) return;
	s->stall_handle = h;
	btstack_run_loop_remove_timer(&s->stall_timer);
	btstack_run_loop_set_timer_handler(&s->stall_timer, on_stall);
	btstack_run_loop_set_timer_context(&s->stall_timer, s);
	btstack_run_loop_set_timer(&s->stall_timer, 4000);
	btstack_run_loop_add_timer(&s->stall_timer);
}

static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
	(void)channel; (void)size;
	if (type != HCI_EVENT_PACKET) return;
	bd_addr_t addr;
	slot_t *s;
	uint8_t ev = hci_event_packet_get_type(packet);
	if (ev != HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS && ev != HCI_EVENT_TRANSPORT_PACKET_SENT
	    && ev != HCI_EVENT_COMMAND_COMPLETE && ev != HCI_EVENT_COMMAND_STATUS) {
		last_event = ev;
		trace_mark(ev == HCI_EVENT_HID_META ? (uint8_t)(0xF0 | (hci_event_hid_meta_get_subevent_code(packet) & 0x0F)) : ev);
	}
	switch (ev) {
	case HCI_EVENT_CONNECTION_COMPLETE: d_conn = hci_event_connection_complete_get_status(packet); break;
	case HCI_EVENT_DISCONNECTION_COMPLETE: d_disc = hci_event_disconnection_complete_get_reason(packet); break;
	case HCI_EVENT_ENCRYPTION_CHANGE:
		d_enc = hci_event_encryption_change_get_status(packet);
		if (d_enc == 0) watch_for_stall(hci_event_encryption_change_get_connection_handle(packet));
		break;
	case HCI_EVENT_IO_CAPABILITY_RESPONSE: d_io = hci_event_io_capability_response_get_io_capability(packet); break;
	default: break;
	}
	switch (ev) {
	case BTSTACK_EVENT_STATE:
		if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
		idle_status();
		try_paired(KBD);
		if (!KBD->cid) try_paired(MOUSE);	/* (else on the next retry: one at a time) */
		le_wait();
		break;

	case GAP_EVENT_INQUIRY_RESULT: {
		if (state != BTKBD_SEARCHING || n_found >= FOUND_MAX) break;
		uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
		if (((cod >> 8) & 0x1F) != 0x05) break;		/* not a peripheral */
		gap_event_inquiry_result_get_bd_addr(packet, addr);
		for (int i = 0; i < n_found; i++) if (bd_addr_cmp(found[i].addr, addr) == 0) goto done;
		int i = n_found++;
		memset(&found[i], 0, sizeof found[i]);
		bd_addr_copy(found[i].addr, addr);
		found[i].psrm = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
		found[i].clock = gap_event_inquiry_result_get_clock_offset(packet);
		/* a keyboard (or one with a trackpad too), a mouse, or neither */
		found[i].kind = (cod & 0xC0) == 0x80 ? BTKBD_MOUSE : (cod & 0x40) ? BTKBD_KEYBOARD : -1;
		found[i].named = gap_event_inquiry_result_get_name_available(packet);
		if (found[i].named) {
			int n = gap_event_inquiry_result_get_name_len(packet);
			if (n > (int)sizeof found[i].name - 1) n = sizeof found[i].name - 1;
			memcpy(found[i].name, gap_event_inquiry_result_get_name(packet), n);
			found[i].name[n] = 0;
		} else {
			snprintf(found[i].name, sizeof found[i].name, "(asking its name...)");
		}
		snprintf(status, sizeof status, "Searching... found %d", n_found);
	done:
		break;
	}
	case GAP_EVENT_INQUIRY_COMPLETE:
		if (le_mode == LE_SEARCHING) { gap_stop_scan(); le_mode = LE_IDLE; }
		if (state == BTKBD_SEARCHING && naming < 0) name_next();
		break;

	case GAP_EVENT_ADVERTISING_REPORT:
		le_advert(packet);
		break;
	case HCI_EVENT_META_GAP:
		if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
			le_connected(packet);
		break;
	case HCI_EVENT_DISCONNECTION_COMPLETE:
		s = slot_by_le(hci_event_disconnection_complete_get_connection_handle(packet));
		if (s) le_disconnected(s);
		break;

	/* BLE pairing (the Security Manager's events come here too) */
	case SM_EVENT_JUST_WORKS_REQUEST:
		sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
		break;
	case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
		sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
		break;
	case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
		snprintf(code, sizeof code, "%06u", (unsigned)sm_event_passkey_display_number_get_passkey(packet));
		set_status("Type the code on the keyboard");
		break;
	case SM_EVENT_IDENTITY_CREATED:
		sm_event_identity_created_get_identity_address(packet, le_ident);
		le_ident_type = (bd_addr_type_t)sm_event_identity_created_get_identity_addr_type(packet);
		le_ident_known = true;
		break;
	case SM_EVENT_PAIRING_COMPLETE:
		s = slot_by_le(sm_event_pairing_complete_get_handle(packet));
		if (!s) break;
		code[0] = 0;
		d_pair = sm_event_pairing_complete_get_status(packet);
		if (d_pair == ERROR_CODE_SUCCESS) {
			if (state == BTKBD_PAIRING && s == pairing_slot && pairing_with >= 0) {
				if (le_ident_known) save_paired(s, le_ident, found[pairing_with].name, true, le_ident_type);
				else save_paired(s, found[pairing_with].addr, found[pairing_with].name, true, found[pairing_with].type);
			}
			le_open_hid(s);
		} else {
			d_auth = sm_event_pairing_complete_get_reason(packet);
			gap_disconnect(s->le_handle);
		}
		break;
	case SM_EVENT_REENCRYPTION_COMPLETE:
		s = slot_by_le(sm_event_reencryption_complete_get_handle(packet));
		if (!s) break;
		d_enc = sm_event_reencryption_complete_get_status(packet);
		if (d_enc == ERROR_CODE_SUCCESS) le_open_hid(s);
		else gap_disconnect(s->le_handle);	/* it has forgotten us: pair it again */
		break;
	case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
		bd_addr_t id, a;
		sm_event_identity_resolving_succeeded_get_identity_address(packet, id);
		s = slot_by_addr(id, true);
		if (le_mode != LE_WAITING || !s || s->le_handle != HCI_CON_HANDLE_INVALID) break;
		sm_event_identity_resolving_succeeded_get_address(packet, a);
		le_connect(s, a, (bd_addr_type_t)sm_event_identity_resolving_succeeded_get_addr_type(packet));
		break;
	}
	case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
		if (naming >= 0) {
			if (hci_event_remote_name_request_complete_get_status(packet) == 0)
				snprintf(found[naming].name, sizeof found[naming].name, "%s",
				         hci_event_remote_name_request_complete_get_remote_name(packet));
			else
				snprintf(found[naming].name, sizeof found[naming].name, "%s", bd_addr_to_str(found[naming].addr));
			found[naming].named = true;
			name_next();
		}
		break;

	/* pairing */
	case HCI_EVENT_PIN_CODE_REQUEST:		/* an older keyboard: type the PIN on it */
		hci_event_pin_code_request_get_bd_addr(packet, addr);
		if (state == BTKBD_PAIRING && pairing_slot == MOUSE) {
			gap_pin_code_response(addr, "0000");	/* an older mouse: always this */
			break;
		}
		snprintf(code, sizeof code, "%04u", (unsigned)(get_rand_32() % 10000));
		gap_pin_code_response(addr, code);
		set_status("Type the code on the keyboard");
		break;
	case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
		snprintf(code, sizeof code, "%06u", (unsigned)hci_event_user_passkey_notification_get_numeric_value(packet));
		set_status("Type the code on the keyboard");
		break;
	case HCI_EVENT_USER_CONFIRMATION_REQUEST:	/* "just works" */
		hci_event_user_confirmation_request_get_bd_addr(packet, addr);
		gap_ssp_confirmation_response(addr);
		break;
	case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
		d_pair = packet[2];
		code[0] = 0;
		break;
	case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT:
		d_auth = packet[2];
		code[0] = 0;
		/* paired: remember it now. Keyboards often hang up straight after
		   pairing and then connect back by themselves, and that has to be
		   let in. */
		if (packet[2] == 0 && state == BTKBD_PAIRING && pairing_with >= 0 && !found[pairing_with].le)
			save_paired(pairing_slot, found[pairing_with].addr, found[pairing_with].name, false, 0);
		break;

	case HCI_EVENT_HID_META:
		switch (hci_event_hid_meta_get_subevent_code(packet)) {
		case HID_SUBEVENT_INCOMING_CONNECTION: {
			hid_subevent_incoming_connection_get_address(packet, addr);
			uint16_t c = hid_subevent_incoming_connection_get_hid_cid(packet);
			s = slot_by_addr(addr, false);
			if (!s && state == BTKBD_PAIRING) s = pairing_slot;
			if (s) {
				hid_host_accept_connection(c, HID_PROTOCOL_MODE_REPORT);
				s->boot_mode = false;	/* (it opened the connection: its own layout, as usual) */
			} else {
				hid_host_decline_connection(c);
			}
			break;
		}
		case HID_SUBEVENT_CONNECTION_OPENED: {
			uint16_t c = hid_subevent_connection_opened_get_hid_cid(packet);
			hid_subevent_connection_opened_get_bd_addr(packet, addr);
			s = slot_by_cid(c);
			if (!s) s = slot_by_addr(addr, false);
			if (!s && state == BTKBD_PAIRING) s = pairing_slot;
			if (!s) break;
			bool pairing = state == BTKBD_PAIRING && s == pairing_slot;
			if (hid_subevent_connection_opened_get_status(packet) != ERROR_CODE_SUCCESS) {
				/* (an attempt we gave up, as Forget does, ends here too: not ours now) */
				if (s->cid != c) break;
				note_err("connect", hid_subevent_connection_opened_get_status(packet));
				s->cid = 0;
				code[0] = 0;
				if (pairing) {
					if (pair_saved) {	/* paired, but not connected yet: it will come back, or we'll try again */
						pairing_over(s == KBD ? "Paired: press a key on the keyboard" : "Paired: move the mouse");
						btstack_run_loop_remove_timer(&retry_timer);
						btstack_run_loop_set_timer(&retry_timer, 8000);	/* give it time to call back first */
						btstack_run_loop_add_timer(&retry_timer);
					} else {
						pairing_over("Pairing failed: try again");
					}
				}
				break;
			}
			s->cid = c;
			s->descriptor_ok = false;
			s->stuck = 0;
			code[0] = 0;
			if (pairing && pairing_with >= 0) save_paired(s, addr, found[pairing_with].name, false, 0);
			s->connected = true;
			if (pairing) pairing_over(NULL);
			else if (state == BTKBD_IDLE) idle_status();
			break;
		}
		case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
			s = slot_by_cid(hid_subevent_descriptor_available_get_hid_cid(packet));
			if (s) s->descriptor_ok = hid_subevent_descriptor_available_get_status(packet) == ERROR_CODE_SUCCESS;
			break;
		case HID_SUBEVENT_REPORT:
			s = slot_by_cid(hid_subevent_report_get_hid_cid(packet));
			if (s) handle_report(s, hid_subevent_report_get_report(packet), hid_subevent_report_get_report_len(packet));
			break;
		case HID_SUBEVENT_CONNECTION_CLOSED:
			s = slot_by_cid(hid_subevent_connection_closed_get_hid_cid(packet));
			if (!s) break;
			s->cid = 0;
			s->descriptor_ok = false;
			let_go(s);
			if (state == BTKBD_IDLE) idle_status();
			break;
		}
		break;
	}
}

/* ------------------------------------------------------------ the API */

/* BTstack runs in the background (an interrupt); calls into it from the
   program take its lock */
static void lock(void) { async_context_acquire_lock_blocking(cyw43_arch_async_context()); }
static void unlock(void) { async_context_release_lock(cyw43_arch_async_context()); }

bool btkbd_init(void)
{
	/* The firmware is read from the card while the chip comes up: by the
	   first thing done to it, which is here, in the program's own thread
	   (not by BTstack in the background, where the card must not be
	   used), and the file is closed once that has returned. */
	const char *why = cyw43_fw_open();
	if (why) { state = BTKBD_OFF; set_status(why); return false; }
	if (cyw43_arch_init()) {
		cyw43_fw_close();
		state = BTKBD_OFF;
		set_status("Bluetooth didn't start");
		return false;
	}
	cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);	/* brings the chip up: loads the firmware */
	cyw43_fw_close();
	lock();
	hci_dump_init(&smp_dump);
	hci_dump_enable_packet_log(true);
	load_paired();
	l2cap_init();
	hid_host_init(descriptor, sizeof descriptor);
	/* BLE: pairing (we show a code, the keyboard types it), and a small
	   GATT server of our own, as some keyboards ask the PicoCalc its name */
	sm_init();
	le_set_method();
	sm_cb.callback = packet_handler;
	sm_add_event_handler(&sm_cb);
	gatt_client_init();
	att_db_util_init();
	att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS);
	att_db_util_add_characteristic_uuid16(ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME,
	                                      ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE,
	                                      (uint8_t *)"PicoCalc", 8);
	att_server_init(att_db_util_get_address(), NULL, NULL);
	hids_host_init(le_descriptor, sizeof le_descriptor);
	hid_host_register_packet_handler(packet_handler);
	hci_cb.callback = packet_handler;
	hci_add_event_handler(&hci_cb);
	gap_set_local_name("PicoCalc");
	gap_set_class_of_device(0x00010C);		/* a computer (laptop) */
	gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
	hci_set_master_slave_policy(HCI_ROLE_MASTER);
	gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
	gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_REQUIRED_GENERAL_BONDING);
	gap_set_bondable_mode(1);
	gap_connectable_control(1);			/* so a device can reconnect by itself */
	btstack_run_loop_set_timer_handler(&retry_timer, on_retry);
	btstack_run_loop_set_timer(&retry_timer, RETRY_MS);
	btstack_run_loop_add_timer(&retry_timer);
	state = BTKBD_STARTING;
	set_status("Starting Bluetooth");
	hci_power_control(HCI_POWER_ON);
	unlock();
	return true;
}

void btkbd_poll(void)
{
	/* (Bluetooth itself runs in the background; this only saves a new
	   pairing to the card, which has to be done from the program) */
	if (state == BTKBD_OFF) return;
	if (store_upgraded) { store_upgraded = false; store_changed(); }
	if (store_dirty && time_reached(save_after) && state != BTKBD_PAIRING && state != BTKBD_SEARCHING) {
		lock();
		store_save();
		unlock();
	}
}

btkbd_state_t btkbd_device_state(btkbd_device_t d)
{
	if (state != BTKBD_IDLE) return state;
	slot_t *s = &slots[d & 1];
	return s->connected ? BTKBD_CONNECTED : s->paired ? BTKBD_WAITING : BTKBD_IDLE;
}
const char *btkbd_device_name(btkbd_device_t d) { slot_t *s = &slots[d & 1]; return s->paired ? s->name : ""; }
btkbd_state_t btkbd_state(void) { return btkbd_device_state(BTKBD_KEYBOARD); }
const char *btkbd_status(void) { return status; }
const char *btkbd_name(void) { return btkbd_device_name(BTKBD_KEYBOARD); }
const char *btkbd_code(void) { return code[0] ? code : NULL; }

void btkbd_search(void)
{
	if (state == BTKBD_OFF || state == BTKBD_STARTING || state == BTKBD_SEARCHING) return;
	lock();
	n_found = 0;
	naming = -1;
	le_stop();
	state = BTKBD_SEARCHING;
	set_status("Searching...");
	gap_inquiry_start(8);				/* 8 x 1.28 s */
	gap_set_scan_parameters(1, 0x0030, 0x0030);	/* BLE too, asking for names */
	gap_start_scan();
	le_mode = LE_SEARCHING;
	unlock();
}

int btkbd_found(void) { return n_found; }
const char *btkbd_found_name(int i) { return i >= 0 && i < n_found ? found[i].name : ""; }
bool btkbd_found_le(int i) { return i >= 0 && i < n_found && found[i].le; }
int btkbd_found_kind(int i) { return i >= 0 && i < n_found ? found[i].kind : -1; }

void btkbd_pair_as(int i, btkbd_device_t d)
{
	if (i < 0 || i >= n_found || state == BTKBD_PAIRING) return;
	lock();
	pairing_slot = &slots[d & 1];
	pair_tries = 0;
	if (state == BTKBD_SEARCHING) {		/* finish the search first, then pair */
		pair_after_search = i;
		snprintf(status, sizeof status, "Stopping the search...");
		if (naming < 0) gap_inquiry_stop();
	} else {
		start_pairing(i);
	}
	unlock();
}

void btkbd_pair(int i)
{
	btkbd_pair_as(i, btkbd_found_kind(i) == BTKBD_MOUSE ? BTKBD_MOUSE : BTKBD_KEYBOARD);
}

static void on_pair_again(btstack_timer_source_t *t)
{
	(void)t;
	int i = pair_again;
	pair_again = -1;
	if (i >= 0 && state == BTKBD_PAIRING) start_pairing(i);
}

static void start_pairing(int i)
{
	slot_t *s = pairing_slot;
	reset_diag();
	pair_saved = false;
	if (s->cid) { hid_host_disconnect(s->cid); s->cid = 0; let_go(s); }
	le_stop();
	pairing_with = i;
	state = BTKBD_PAIRING;
	snprintf(status, sizeof status, "Pairing with %.28s", found[i].name);
	if (s->le_handle != HCI_CON_HANDLE_INVALID) {
		/* the old BLE one is connected: let it go first (a new connection
		   can't start while it's being dropped); this goes on from
		   le_disconnected() */
		classic_after_le = i;
		gap_disconnect(s->le_handle);
		return;
	}
	if (found[i].le) {
		gap_delete_bonding(found[i].type, found[i].addr);	/* start fresh */
		le_ident_known = false;
		le_connect(s, found[i].addr, found[i].type);
		if (le_mode != LE_CONNECTING) pairing_over("Couldn't connect: try again");
		return;
	}
	gap_drop_link_key_for_bd_addr(found[i].addr);	/* start fresh */
	s->boot_mode = false;
	uint8_t err = hid_host_connect(found[i].addr, HID_PROTOCOL_MODE_REPORT, &s->cid);
	if (err == ERROR_CODE_COMMAND_DISALLOWED && pair_tries < 3) {
		/* still connected to it from before (forgotten a moment ago, say):
		   hang up, and try again when that's done */
		s->cid = 0;
		pair_tries++;
		hci_connection_t *c = hci_connection_for_bd_addr_and_type(found[i].addr, BD_ADDR_TYPE_ACL);
		if (c) gap_disconnect(c->con_handle);
		pair_again = i;
		btstack_run_loop_remove_timer(&pair_timer);
		btstack_run_loop_set_timer_handler(&pair_timer, on_pair_again);
		btstack_run_loop_set_timer(&pair_timer, 2000);
		btstack_run_loop_add_timer(&pair_timer);
		return;
	}
	pair_tries = 0;
	if (err != ERROR_CODE_SUCCESS) {
		note_err("start", err);
		s->cid = 0;
		pairing_over("Couldn't connect: try again");
	}
}

void btkbd_forget_device(btkbd_device_t d)
{
	slot_t *s = &slots[d & 1];
	lock();
	if (s->cid) { hid_host_disconnect(s->cid); s->cid = 0; }
	if (le_connecting == s || le_mode == LE_WAITING) le_stop();	/* (le_wait() starts again if the other wants it) */
	if (s->le_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(s->le_handle);	/* (le_disconnected() tidies up) */
	if (s->paired && !s->le) gap_drop_link_key_for_bd_addr(s->addr);
	if (s->paired && s->le) gap_delete_bonding(s->type, s->addr);
	tlv_delete(NULL, s->tag);
	tlv_delete(NULL, s->tag_le);
	s->paired = s->le = false;
	s->name[0] = 0;
	let_go(s);
	if (state == BTKBD_IDLE) { idle_status(); le_wait(); }
	unlock();
}

void btkbd_forget(void) { btkbd_forget_device(BTKBD_KEYBOARD); }
