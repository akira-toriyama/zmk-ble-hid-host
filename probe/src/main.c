/*
 * XIAO HOGP probe — connect to a BLE HID (HOGP) pointer (e.g. Elecom IST PRO),
 * bond (Just Works / NoInputNoOutput), subscribe to every notifiable HID input
 * report, and hexdump each report over USB CDC ACM serial. Reconnects to the
 * bonded peer automatically after a disconnect.
 *
 * This is a diagnostic probe for the zmk-ble-hid-host project (milestone M1:
 * "receive only"). It is NOT ZMK; the HOGP discovery logic ports into the ZMK
 * module later.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hogp_probe, LOG_LEVEL_INF);

/* Sanity: console must be the USB CDC ACM UART so logs come out over USB. */
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart),
	     "console is not routed to a CDC ACM UART");

#define MAX_REPORTS 12
#define APPEARANCE_MOUSE 0x03C2

static struct bt_conn *default_conn;

/* Discovery is sequential, so one shared params struct is fine. */
static struct bt_gatt_discover_params discover_params;
static struct bt_uuid_16 disc_uuid = BT_UUID_INIT_16(0);

enum disc_state { DISC_IDLE, DISC_HIDS_PRIMARY, DISC_REPORT_CHRC, DISC_CCC };
static enum disc_state disc_state;

static uint16_t hids_end_handle;

/* Report value handles found in the CHARACTERISTIC pass, awaiting CCC wiring. */
static uint16_t pending_reports[MAX_REPORTS];
static size_t pending_count;
static size_t pending_idx;

/* Each subscription needs its OWN long-lived params (never one shared global). */
static struct bt_gatt_subscribe_params sub_params[MAX_REPORTS];
static size_t sub_count;

/* Diagnostics: log each distinct advertiser (and its scan-response name) once,
 * so the IST PRO can be told apart from other BLE HID devices nearby. */
#define MAX_SEEN 24
struct seen_dev {
	bt_addr_le_t addr;
	bool named; /* have we already logged a non-empty name for this addr? */
};
static struct seen_dev seen[MAX_SEEN];
static size_t seen_count;

/* Cooldown: after a pairing failure, skip the same device for a while so the
 * scanner cycles to other advertisers (e.g. the IST PRO in pairing mode)
 * instead of hammering a device that refuses to bond. */
#define FAIL_COOLDOWN_MS 8000
static bt_addr_le_t last_failed_addr;
static uint32_t last_failed_ms;

/* Parsed advertising/scan-response fields for one report. */
struct ad_info {
	bool match;       /* looks like a HOGP pointer we should connect to */
	bool has_hids;    /* advertised the HID service (0x1812) */
	uint16_t appearance;
	char name[32];
};

static void start_scan(void);
static void subscribe_pending(struct bt_conn *conn);

/* ───────────────────────── notify: the hexdump ─────────────────────────── */
static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	ARG_UNUSED(conn);

	if (!data) {
		params->value_handle = 0U; /* subscription torn down */
		LOG_INF("unsubscribed (handle %u)", params->ccc_handle);
		return BT_GATT_ITER_STOP;
	}

	LOG_HEXDUMP_INF(data, length, "HID report");
	return BT_GATT_ITER_CONTINUE;
}

/* ───────────────────────── GATT discovery state machine ────────────────── */
static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
		/* The current discovery pass finished. */
		if (disc_state == DISC_REPORT_CHRC) {
			if (pending_count == 0) {
				LOG_WRN("no notifiable HID report characteristics found");
				disc_state = DISC_IDLE;
				return BT_GATT_ITER_STOP;
			}
			pending_idx = 0;
			subscribe_pending(conn); /* start CCC discovery for report 0 */
		}
		return BT_GATT_ITER_STOP;
	}

	switch (disc_state) {
	case DISC_HIDS_PRIMARY: {
		const struct bt_gatt_service_val *svc = attr->user_data;

		hids_end_handle = svc->end_handle;
		LOG_INF("HID service: handles %u..%u", attr->handle, svc->end_handle);

		memcpy(&disc_uuid, BT_UUID_HIDS_REPORT, sizeof(disc_uuid));
		discover_params.uuid = &disc_uuid.uuid;
		discover_params.start_handle = attr->handle + 1;
		discover_params.end_handle = hids_end_handle;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		disc_state = DISC_REPORT_CHRC;

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			LOG_ERR("report-chrc discover failed (%d)", err);
		}
		return BT_GATT_ITER_STOP;
	}

	case DISC_REPORT_CHRC: {
		const struct bt_gatt_chrc *chrc = attr->user_data;

		/* Only Input reports notify; Output/Feature share 0x2A4D but lack NOTIFY. */
		if (chrc->properties & BT_GATT_CHRC_NOTIFY) {
			if (pending_count < MAX_REPORTS) {
				pending_reports[pending_count++] = chrc->value_handle;
				LOG_INF("notifiable report chrc (value handle %u)",
					chrc->value_handle);
			} else {
				LOG_WRN("> %d reports; ignoring extras", MAX_REPORTS);
			}
		}
		return BT_GATT_ITER_CONTINUE; /* collect them all */
	}

	case DISC_CCC: {
		/* attr is the CCC (0x2902) for pending_reports[pending_idx]. */
		if (sub_count < MAX_REPORTS) {
			struct bt_gatt_subscribe_params *sp = &sub_params[sub_count];

			sp->notify = notify_cb;
			sp->value = BT_GATT_CCC_NOTIFY;
			sp->value_handle = pending_reports[pending_idx];
			sp->ccc_handle = attr->handle;
			sp->min_security = BT_SECURITY_L2;

			err = bt_gatt_subscribe(conn, sp);
			if (err && err != -EALREADY) {
				LOG_ERR("subscribe failed (%d) value=%u", err, sp->value_handle);
			} else {
				LOG_INF("subscribed report value=%u ccc=%u",
					sp->value_handle, sp->ccc_handle);
				sub_count++;
			}
		}
		pending_idx++;
		subscribe_pending(conn); /* next report, or finish */
		return BT_GATT_ITER_STOP;
	}

	default:
		return BT_GATT_ITER_STOP;
	}
}

/* Discover the CCC for pending_reports[pending_idx]; if exhausted, we're done. */
static void subscribe_pending(struct bt_conn *conn)
{
	int err;

	if (pending_idx >= pending_count) {
		disc_state = DISC_IDLE;
		LOG_INF("discovery done: subscribed to %u report(s)", sub_count);
		return;
	}

	memcpy(&disc_uuid, BT_UUID_GATT_CCC, sizeof(disc_uuid)); /* 0x2902, NOT 0x2908 */
	discover_params.uuid = &disc_uuid.uuid;
	discover_params.start_handle = pending_reports[pending_idx] + 1;
	discover_params.end_handle = hids_end_handle;
	discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
	disc_state = DISC_CCC;

	err = bt_gatt_discover(conn, &discover_params);
	if (err) {
		LOG_ERR("CCC discover failed (%d)", err);
		disc_state = DISC_IDLE;
	}
}

static void start_discovery(struct bt_conn *conn)
{
	int err;

	pending_count = 0;
	pending_idx = 0;
	sub_count = 0;

	memcpy(&disc_uuid, BT_UUID_HIDS, sizeof(disc_uuid)); /* 0x1812 */
	memset(&discover_params, 0, sizeof(discover_params));
	discover_params.uuid = &disc_uuid.uuid;
	discover_params.func = discover_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_PRIMARY;
	disc_state = DISC_HIDS_PRIMARY;

	err = bt_gatt_discover(conn, &discover_params);
	if (err) {
		LOG_ERR("HIDS discover failed (%d)", err);
	}
}

/* ───────────────────────── target selection ────────────────────────────── */
static void bond_match_cb(const struct bt_bond_info *info, void *user_data)
{
	struct { const bt_addr_le_t *addr; bool found; } *bf = user_data;

	if (bt_addr_le_cmp(&info->addr, bf->addr) == 0) {
		bf->found = true;
	}
}

static bool addr_is_bonded(const bt_addr_le_t *addr)
{
	struct { const bt_addr_le_t *addr; bool found; } bf = { .addr = addr, .found = false };

	bt_foreach_bond(BT_ID_DEFAULT, bond_match_cb, &bf);
	return bf.found;
}

/* Parse all relevant AD / scan-response fields of one report into `ai`. */
static bool ad_parse_cb(struct bt_data *data, void *user_data)
{
	struct ad_info *ai = user_data;

	switch (data->type) {
	case BT_DATA_UUID16_SOME:
	case BT_DATA_UUID16_ALL:
		for (int i = 0; i + 1 < data->data_len; i += 2) {
			if (sys_get_le16(&data->data[i]) == BT_UUID_HIDS_VAL) {
				ai->has_hids = true;
				/* Advertising the HID service alone is NOT enough to target:
				 * a nearby ZMK keyboard dongle (Imprint Dongle, appearance
				 * 0x03c1) also advertises HIDS and would hijack the probe
				 * (and waste cycles failing to pair). Require a mouse
				 * appearance or an IST/ELECOM name below. */
			}
		}
		break;
	case BT_DATA_GAP_APPEARANCE:
		if (data->data_len >= 2) {
			ai->appearance = sys_get_le16(data->data);
			if (ai->appearance == APPEARANCE_MOUSE) {
				ai->match = true;
			}
		}
		break;
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_NAME_COMPLETE: {
		size_t n = MIN(data->data_len, sizeof(ai->name) - 1);

		memcpy(ai->name, data->data, n);
		ai->name[n] = '\0';
		if (strstr(ai->name, "IST") || strstr(ai->name, "ELECOM") ||
		    strstr(ai->name, "ELE")) {
			ai->match = true;
		}
		break;
	}
	default:
		break;
	}
	return true; /* parse every field */
}

/* True if this report is worth logging: first sighting of the address, or the
 * first time we learn its name (names often arrive in the scan response). */
static bool should_log(const bt_addr_le_t *addr, bool has_name)
{
	for (size_t i = 0; i < seen_count; i++) {
		if (bt_addr_le_cmp(&seen[i].addr, addr) == 0) {
			if (has_name && !seen[i].named) {
				seen[i].named = true;
				return true;
			}
			return false;
		}
	}
	if (seen_count < MAX_SEEN) {
		bt_addr_le_copy(&seen[seen_count].addr, addr);
		seen[seen_count].named = has_name;
		seen_count++;
	}
	return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	char s[BT_ADDR_LE_STR_LEN];
	struct ad_info ai = {0};
	int err;

	bt_addr_le_to_str(addr, s, sizeof(s));
	bt_data_parse(ad, ad_parse_cb, &ai);

	/* Only bother with advertisers that carry a name, the HID service, or
	 * match — the air is full of no-name RPA beacons whose rotating addresses
	 * would otherwise overflow seen[] (MAX_SEEN), break the dedup, and spam the
	 * deferred log buffer hard enough to drop the SMP pairing trace. */
	bool interesting = ai.match || ai.has_hids || ai.name[0] != '\0';

	/* Log each distinct interesting advertiser once (and again when we first
	 * learn its name) so the IST PRO can be told apart from e.g. the Imprint
	 * keyboard dongle (which now shows has_hids 1 but match 0). */
	if (interesting && should_log(addr, ai.name[0] != '\0')) {
		LOG_INF("saw %s rssi %d name '%s' appearance 0x%04x hids %d match %d", s,
			rssi, ai.name, ai.appearance, ai.has_hids, ai.match);
	}

	if (default_conn) {
		return;
	}
	/* Only initiate to connectable advertising (skip scan responses). */
	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
	    type != BT_GAP_ADV_TYPE_EXT_ADV) {
		return;
	}

	/* Connect to a bonded peer (reconnect) or anything that looks like a pointer. */
	bool bonded = addr_is_bonded(addr);

	if (!bonded && !ai.match) {
		return;
	}

	/* Skip a device that just failed pairing so the scanner cycles to others. */
	if (!bonded && bt_addr_le_cmp(addr, &last_failed_addr) == 0 &&
	    (k_uptime_get_32() - last_failed_ms) < FAIL_COOLDOWN_MS) {
		return;
	}

	LOG_INF("target %s (rssi %d) name '%s' — connecting", s, rssi, ai.name);

	err = bt_le_scan_stop();
	if (err) {
		LOG_ERR("scan stop failed (%d)", err);
		return;
	}

	/* Request 7.5ms at connect time (interval 6 x 1.25ms), latency 0, 4s timeout.
	 * The peer may downgrade it; see le_param_updated. */
	struct bt_le_conn_param *cp = BT_LE_CONN_PARAM(6, 6, 0, 400);

	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, cp, &default_conn);
	if (err) {
		LOG_ERR("create connection failed (%d)", err);
		start_scan();
	}
}

static void start_scan(void)
{
	struct bt_le_scan_param sp = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err = bt_le_scan_start(&sp, device_found);

	if (err) {
		LOG_ERR("scan start failed (%d)", err);
		return;
	}
	LOG_INF("scanning for a HOGP pointer (Elecom IST PRO)...");
}

/* ───────────────────────── connection callbacks ────────────────────────── */
static void connected(struct bt_conn *conn, uint8_t err)
{
	char s[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), s, sizeof(s));

	if (err) {
		LOG_ERR("failed to connect %s (0x%02x)", s, err);
		bt_conn_unref(default_conn);
		default_conn = NULL;
		start_scan();
		return;
	}

	LOG_INF("connected: %s", s);

	/* Request L2 (encryption, unauthenticated) — NOT L3. The IST PRO is
	 * NoInputNoOutput and LE-legacy (SMP Pairing Response io 0x03, auth_req
	 * 0x01: SC=0, MITM=0), so Just Works is the only possible method. Asking
	 * for MITM (L3) is fatal: SMP aborts with "Authentication Requirements"
	 * because a NoInputNoOutput peer can never satisfy MITM. Legacy pairing is
	 * permitted via BT_SMP_SC_PAIR_ONLY=n. Discovery is encryption-gated, so it
	 * begins only in security_changed. */
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_ERR("set security failed (%d)", err);
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (err != BT_SECURITY_ERR_SUCCESS || level < BT_SECURITY_L2) {
		LOG_WRN("pairing/security failed (level %u err %d) — disconnecting", level,
			err);
		/* Remember it (device_found skips it briefly) and drop the link so we
		 * don't sit silently on a peer that refuses to bond. */
		bt_addr_le_copy(&last_failed_addr, bt_conn_get_dst(conn));
		last_failed_ms = k_uptime_get_32();
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}
	LOG_INF("secured (level %u) — starting GATT discovery", level);
	start_discovery(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char s[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), s, sizeof(s));
	LOG_INF("disconnected: %s (reason 0x%02x)", s, reason);

	if (default_conn != conn) {
		return;
	}
	bt_conn_unref(default_conn);
	default_conn = NULL;
	sub_count = 0;
	pending_count = 0;
	pending_idx = 0;
	disc_state = DISC_IDLE;

	start_scan(); /* auto-reconnect: re-scan, match the bonded peer, re-discover */
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(conn);
	/* interval is in 1.25ms units; timeout in 10ms units. */
	LOG_INF("conn params: interval=%u (%u.%02u ms) latency=%u timeout=%u ms", interval,
		(interval * 125) / 100, (interval * 125) % 100, latency, timeout * 10);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.le_param_updated = le_param_updated,
};

/* ─────────────── pairing callbacks (NoInputNoOutput → Just Works) ───────────
 * The IST PRO is NoInputNoOutput and LE-legacy, so the only viable method is
 * Just Works (unauthenticated). Registering only .cancel keeps our IO caps at
 * NoInputNoOutput and keeps us out of MITM methods the mouse cannot complete. */
static void auth_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	LOG_INF("pairing cancelled");
}

/* Only .cancel set => NoInputNoOutput => Just Works. */
static struct bt_conn_auth_cb auth_cb = {
	.cancel = auth_cancel,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	LOG_INF("paired (bonded=%d)", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	LOG_WRN("pairing failed (reason %d)", reason);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

/* ───────────────────────── main ─────────────────────────────────────────── */
int main(void)
{
	const struct device *console_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;
	int err;

	if (usb_enable(NULL)) {
		return 0; /* INITIALIZE_AT_BOOT=n: we own the single usb_enable() */
	}

	/* Wait up to ~10s for a host to open the CDC port so the first lines aren't
	 * dropped, then proceed regardless (headless runs still log via deferred
	 * logging once a port opens later). */
	for (int i = 0; i < 100; i++) {
		uart_line_ctrl_get(console_dev, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			break;
		}
		k_sleep(K_MSEC(100));
	}

	LOG_INF("=== XIAO HOGP probe ===");

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return 0;
	}
	LOG_INF("bluetooth enabled");

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load(); /* restore bonds/IRKs — MUST be after bt_enable */
	}

	bt_conn_auth_cb_register(&auth_cb);
	bt_conn_auth_info_cb_register(&auth_info_cb);

	start_scan();
	return 0;
}
