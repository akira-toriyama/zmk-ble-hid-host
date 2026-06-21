/*
 * Copyright (c) 2026 akira-toriyama
 * SPDX-License-Identifier: MIT
 *
 * HOGP BLE-central engine for zmk-ble-hid-host.
 *
 * Ported from the M1 receive probe (probe/src/main.c, proven on an ELECOM
 * IST PRO): scan -> connect -> LE-legacy Just Works bond -> GATT discovery ->
 * subscribe to the HID input reports. Added on top of the probe:
 *   - reads + parses the peer's HID Report Map (0x2A4B) at runtime so any HOGP
 *     pointer can be decoded (zmk_hid_parse_report_map);
 *   - defers each notification out of the BT RX context (k_msgq + k_work) and
 *     decodes it there (zmk_hid_decode_report).
 *
 * This module owns the Bluetooth stack: CONFIG_ZMK_BLE is off (the dongle
 * outputs over USB), so ZMK core does not bt_enable()/settings_load() -- we do.
 *
 * Milestone status: M3. Each report characteristic's Report Reference (0x2908)
 * is read so its report-ID is known; the work handler publishes only the report
 * whose id matches the parsed pointer layout (ble_hid_host_publish), turning it
 * into INPUT_REL/BTN motion. Non-pointer reports are logged and dropped.
 */

#include "hog_central.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(ble_hid_host, CONFIG_ZMK_BLE_HID_HOST_LOG_LEVEL);

#if IS_ENABLED(CONFIG_BT_CENTRAL)

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>

#include <zmk_ble_hid_host/hid_report_parser.h>

#define MAX_REPORTS 12
#define APPEARANCE_MOUSE 0x03C2
#define FAIL_COOLDOWN_MS 8000
#define REPORT_MAP_MAX 512

/* The publish target (M3) and the optional GAP-name filter from devicetree. */
static const struct device *host_dev;
static const char *name_filter;

static struct bt_conn *default_conn;

/* Discovery is sequential, so one shared params struct is fine. */
static struct bt_gatt_discover_params discover_params;
static struct bt_uuid_16 disc_uuid = BT_UUID_INIT_16(0);

enum disc_state {
	DISC_IDLE,
	DISC_HIDS_PRIMARY,
	DISC_REPORT_MAP_CHRC,
	DISC_REPORT_CHRC,
	DISC_REPORT_REF,
	DISC_CCC,
};
static enum disc_state disc_state;

static uint16_t hids_end_handle;

/* HID Report Map (0x2A4B): read into rm_buf, then parsed into `layout`.
 *
 * `layout` is written once on the BT RX workqueue (at discovery, before any
 * notification flows) and read on the system workqueue (report_work_handler,
 * which now injects motion). To make the layout publish atomically across the
 * two work queues, the writer fully populates `layout`, then a memory barrier,
 * then sets the `volatile` `layout_valid` flag; the reader checks the flag,
 * then a barrier, before touching `layout`. On single-core nRF52840 this rules
 * out injecting a delta derived from a half-written layout. */
static struct bt_gatt_read_params rm_read_params;
static uint8_t rm_buf[REPORT_MAP_MAX];
static uint16_t rm_len;
static struct zmk_hid_report_layout layout;
static volatile bool layout_valid;

/* Report value handles found in the CHARACTERISTIC pass, awaiting CCC wiring. */
static uint16_t pending_reports[MAX_REPORTS];
static size_t pending_count;
static size_t pending_idx;

/* report-ID of the report currently being wired up (from its Report Reference
 * descriptor 0x2908), valid between the 0x2908 read and the CCC subscribe.
 * Discovery is sequential, so a single transient is enough. 0 == no/none. */
static uint8_t cur_report_id;
static struct bt_gatt_read_params rr_read_params;

/* One subscription per HID Input report. Each needs its OWN long-lived params
 * (never one shared global); report_id ties the notification back to a Report
 * Reference so only the pointer report becomes motion (M3). */
struct hid_subscription {
	struct bt_gatt_subscribe_params params;
	uint8_t report_id;
};
static struct hid_subscription subs[MAX_REPORTS];
static size_t sub_count;

/* Cooldown: skip a peer that just failed pairing so the scanner cycles on. */
static bt_addr_le_t last_failed_addr;
static uint32_t last_failed_ms;

/* Parsed advertising/scan-response fields for one report. */
struct ad_info {
	bool match;    /* looks like a HOGP pointer we should connect to */
	bool has_hids; /* advertised the HID service (0x1812) */
	uint16_t appearance;
	char name[32];
};

static void start_scan(void);
static void subscribe_pending(struct bt_conn *conn);
static void discover_ccc(struct bt_conn *conn);
static void start_report_discovery(struct bt_conn *conn);

/* ─────────────── notify -> defer -> decode (k_msgq + k_work) ─────────────── */
struct report_evt {
	uint16_t value_handle;
	uint8_t report_id; /* from the source report's Report Reference (0x2908) */
	uint8_t len;
	uint8_t data[32];
};

K_MSGQ_DEFINE(report_msgq, sizeof(struct report_evt), 16, 4);
static struct k_work report_work;

/* #8 diag counters (NEVER reset — monotonic; deltas across HB/disconnect lines are
 * the signal): rx_notif = GATT notifications received in notify_cb; pub_reports =
 * reports actually forwarded to USB. Surfaced in the heartbeat + disconnect lines so
 * a ZOMBIE reconnect (conn up, subscribed, but no report flow) is visible at INF. */
static uint32_t rx_notif;
static uint32_t pub_reports;
/* #8 Fix-A diag: last negotiated peripheral latency (0xFFFF = not connected).
 * Surfaced in the heartbeat so a zombie reconnect shows whether the latency clamp
 * to 0 took (fixed) or the peer NAK'd back to 44 (clamp insufficient -> sniff next). */
static uint16_t cur_latency = 0xFFFF;

static void report_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	struct report_evt evt;

	while (k_msgq_get(&report_msgq, &evt, K_NO_WAIT) == 0) {
		struct zmk_hid_pointer_report report;

		if (!layout_valid) {
			LOG_HEXDUMP_DBG(evt.data, evt.len, "report (no layout yet)");
			continue;
		}
		barrier_dmem_fence_full(); /* pair with the publish in report_map_read_cb */

		/* Only the report whose Report Reference (0x2908) report-ID matches the
		 * parsed pointer layout becomes motion. A peer that multiplexes several
		 * reports (keyboard/consumer) on one connection would otherwise have its
		 * non-pointer reports injected as bogus movement.
		 *
		 * Exception: with exactly one notifiable report there is nothing to
		 * disambiguate, so publish it regardless of id. This keeps a single-report
		 * mouse working even if its Report Reference read failed (which would
		 * otherwise leave the report tagged id 0 and never match a non-zero
		 * layout id -- a silently dead cursor). */
		if (sub_count != 1 && evt.report_id != layout.report_id) {
			LOG_DBG("ignoring report h=%u id=%u (pointer id=%u)",
				evt.value_handle, evt.report_id, layout.report_id);
			continue;
		}

		if (zmk_hid_decode_report(&layout, evt.data, evt.len, &report) == 0) {
			LOG_DBG("report h=%u dx=%d dy=%d wheel=%d hwheel=%d buttons=0x%04x",
				evt.value_handle, report.dx, report.dy, report.wheel,
				report.hwheel, (unsigned)report.buttons);
			ble_hid_host_publish(host_dev, &report);
			pub_reports++; /* #8 diag: reports actually forwarded to USB */
		}
	}
}

static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	ARG_UNUSED(conn);

	if (!data) {
		params->value_handle = 0U; /* subscription torn down */
		return BT_GATT_ITER_STOP;
	}

	rx_notif++; /* #8 diag: a real GATT notification arrived from the mouse */

	struct hid_subscription *sub = CONTAINER_OF(params, struct hid_subscription, params);
	struct report_evt evt;

	evt.value_handle = params->value_handle;
	evt.report_id = sub->report_id;
	evt.len = MIN(length, sizeof(evt.data));
	memcpy(evt.data, data, evt.len);

	/* BT RX context: hand off, never decode/publish inline. Drop on overflow. */
	if (k_msgq_put(&report_msgq, &evt, K_NO_WAIT) == 0) {
		k_work_submit(&report_work);
	} else {
		LOG_WRN("report queue full; dropping");
	}
	return BT_GATT_ITER_CONTINUE;
}

/* ─────────────── Report Map (0x2A4B): read long, then parse ──────────────── */
static uint8_t report_map_read_cb(struct bt_conn *conn, uint8_t err,
				  struct bt_gatt_read_params *params, const void *data,
				  uint16_t length)
{
	ARG_UNUSED(params);

	if (err) {
		LOG_ERR("report map read failed (0x%02x)", err);
		start_report_discovery(conn); /* fall back to boot/length-gated decode */
		return BT_GATT_ITER_STOP;
	}

	if (data && length) {
		uint16_t room = (uint16_t)(sizeof(rm_buf) - rm_len);
		uint16_t n = MIN(length, room);

		if (n < length) {
			/* The Zephyr stack drives single.offset and stops on the short
			 * PDU; we just stop accumulating. Warn so an oversized descriptor
			 * doesn't fail to decode silently. (IST PRO's map is well under.) */
			LOG_WRN("report map exceeds %u B; truncating", (unsigned)sizeof(rm_buf));
		}
		memcpy(rm_buf + rm_len, data, n);
		rm_len += n;
		return BT_GATT_ITER_CONTINUE; /* keep reading the long value */
	}

	/* data == NULL: read complete. */
	if (zmk_hid_parse_report_map(rm_buf, rm_len, &layout) == 0 && layout.valid) {
		/* Publish `layout` atomically to report_work_handler: the struct is
		 * fully written above; fence, then flip the volatile flag last. */
		barrier_dmem_fence_full();
		layout_valid = true;
		LOG_INF("report map parsed (%u B): report_id=%u buttons=%u", rm_len,
			layout.report_id, layout.button_count);
	} else {
		LOG_WRN("report map parse found no pointer layout (%u B)", rm_len);
	}

	start_report_discovery(conn);
	return BT_GATT_ITER_STOP;
}

static void read_report_map(struct bt_conn *conn, uint16_t value_handle)
{
	int err;

	rm_len = 0;
	memset(&rm_read_params, 0, sizeof(rm_read_params));
	rm_read_params.func = report_map_read_cb;
	rm_read_params.handle_count = 1;
	rm_read_params.single.handle = value_handle;
	rm_read_params.single.offset = 0;

	err = bt_gatt_read(conn, &rm_read_params);
	if (err) {
		LOG_ERR("report map read start failed (%d)", err);
		start_report_discovery(conn);
	}
}

/* ──────── Report Reference (0x2908): which report-ID a characteristic carries ─ */

/* Upper handle bound for the descriptors of pending_reports[pending_idx]: stop
 * before the next report's characteristic declaration, so a report that lacks a
 * 0x2908/0x2902 can't accidentally pick up the following report's. */
static uint16_t cur_report_end_handle(void)
{
	if (pending_idx + 1 < pending_count) {
		return pending_reports[pending_idx + 1] - 1;
	}
	return hids_end_handle;
}

static uint8_t report_ref_read_cb(struct bt_conn *conn, uint8_t err,
				  struct bt_gatt_read_params *params, const void *data,
				  uint16_t length)
{
	ARG_UNUSED(params);

	if (!err && data && length >= 1) {
		/* Report Reference value: byte[0] = report ID, byte[1] = report type. */
		cur_report_id = ((const uint8_t *)data)[0];
	} else {
		LOG_WRN("report reference read failed (0x%02x); assuming id 0", err);
		cur_report_id = 0;
	}

	discover_ccc(conn); /* now wire up this report's CCC and subscribe */
	return BT_GATT_ITER_STOP;
}

static void read_report_ref(struct bt_conn *conn, uint16_t handle)
{
	int err;

	memset(&rr_read_params, 0, sizeof(rr_read_params));
	rr_read_params.func = report_ref_read_cb;
	rr_read_params.handle_count = 1;
	rr_read_params.single.handle = handle;
	rr_read_params.single.offset = 0;

	err = bt_gatt_read(conn, &rr_read_params);
	if (err) {
		LOG_ERR("report reference read start failed (%d)", err);
		cur_report_id = 0;
		discover_ccc(conn); /* fall back: subscribe without an id match */
	}
}

/* ───────────────────────── GATT discovery state machine ────────────────── */
static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	ARG_UNUSED(params);
	int err;

	if (!attr) {
		/* The current discovery pass finished. */
		if (disc_state == DISC_REPORT_MAP_CHRC) {
			/* No Report Map characteristic; proceed without a layout. */
			LOG_WRN("no HID Report Map (0x2A4B) found");
			start_report_discovery(conn);
		} else if (disc_state == DISC_REPORT_CHRC) {
			if (pending_count == 0) {
				LOG_WRN("no notifiable HID report characteristics found");
				disc_state = DISC_IDLE;
				return BT_GATT_ITER_STOP;
			}
			pending_idx = 0;
			subscribe_pending(conn); /* start with report 0's Report Reference */
		} else if (disc_state == DISC_REPORT_REF) {
			/* This report has no Report Reference; keep id 0 and subscribe. */
			discover_ccc(conn);
		} else if (disc_state == DISC_CCC) {
			/* This report has no CCC (shouldn't happen for a notifiable
			 * report); skip it so discovery doesn't wedge. */
			LOG_WRN("no CCC for report value=%u; skipping",
				pending_reports[pending_idx]);
			pending_idx++;
			subscribe_pending(conn);
		}
		return BT_GATT_ITER_STOP;
	}

	switch (disc_state) {
	case DISC_HIDS_PRIMARY: {
		const struct bt_gatt_service_val *svc = attr->user_data;

		hids_end_handle = svc->end_handle;
		LOG_INF("HID service: handles %u..%u", attr->handle, svc->end_handle);

		/* First read the Report Map characteristic (0x2A4B). */
		memcpy(&disc_uuid, BT_UUID_HIDS_REPORT_MAP, sizeof(disc_uuid));
		discover_params.uuid = &disc_uuid.uuid;
		discover_params.start_handle = attr->handle + 1;
		discover_params.end_handle = hids_end_handle;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		disc_state = DISC_REPORT_MAP_CHRC;

		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			LOG_ERR("report-map discover failed (%d)", err);
			disc_state = DISC_IDLE; /* no callback pending; don't wedge */
		}
		return BT_GATT_ITER_STOP;
	}

	case DISC_REPORT_MAP_CHRC: {
		const struct bt_gatt_chrc *chrc = attr->user_data;

		LOG_INF("report map chrc (value handle %u)", chrc->value_handle);
		read_report_map(conn, chrc->value_handle); /* async; resumes discovery */
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

	case DISC_REPORT_REF: {
		/* attr is the Report Reference (0x2908) for pending_reports[pending_idx];
		 * read it to learn this report's report-ID, then resume at the CCC. */
		read_report_ref(conn, attr->handle);
		return BT_GATT_ITER_STOP;
	}

	case DISC_CCC: {
		/* attr is the CCC (0x2902) for pending_reports[pending_idx]. */
		if (sub_count < MAX_REPORTS) {
			struct hid_subscription *s = &subs[sub_count];

			memset(s, 0, sizeof(*s)); /* clear stale node/flags before reuse */
			s->report_id = cur_report_id;
			s->params.notify = notify_cb;
			s->params.value = BT_GATT_CCC_NOTIFY;
			s->params.value_handle = pending_reports[pending_idx];
			s->params.ccc_handle = attr->handle;
			s->params.min_security = BT_SECURITY_L2;

			err = bt_gatt_subscribe(conn, &s->params);
			if (err && err != -EALREADY) {
				LOG_ERR("subscribe failed (%d) value=%u", err,
					s->params.value_handle);
			} else {
				LOG_INF("subscribed report value=%u ccc=%u id=%u",
					s->params.value_handle, s->params.ccc_handle,
					s->report_id);
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

/* Wire up pending_reports[pending_idx]: first read its Report Reference (0x2908)
 * to learn its report-ID, then (in discover_ccc) discover its CCC and subscribe.
 * When the pending list is exhausted, discovery is done. */
static void subscribe_pending(struct bt_conn *conn)
{
	int err;

	if (pending_idx >= pending_count) {
		struct bt_conn_info info;
		disc_state = DISC_IDLE;
		LOG_INF("discovery done: subscribed to %u report(s)", sub_count);
		/* OBSERVE: log the EFFECTIVE link params even if the peer never sent a
		 * param-update. interval unit 1.25 ms, timeout unit 10 ms. */
		if (bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE) {
			cur_latency = info.le.latency; /* #8 Fix-A diag: surface in heartbeat */
			LOG_INF("effective params @disc-done: interval %u.%02u ms, latency %u, timeout %u ms",
				(info.le.interval * 5U) / 4U, ((info.le.interval * 5U) % 4U) * 25U,
				info.le.latency, info.le.timeout * 10U);
		}
		/* #8 Fix-A: actively re-drive the link to latency 0 after discovery, the
		 * way a macOS host does -- denies the peer the long skip-window that, after
		 * a DEEP sleep, correlates with the "connected but not streaming" zombie.
		 * Low risk: -EALREADY if already there; if the peer NAKs nothing changes. */
		{
			struct bt_le_conn_param *np = BT_LE_CONN_PARAM(6, 12, 0, 400);
			int perr = bt_conn_le_param_update(conn, np);

			if (perr && perr != -EALREADY) {
				LOG_WRN("param re-drive (latency 0) request failed (%d)", perr);
			} else {
				LOG_INF("param re-drive requested: latency 0, timeout 4000 ms");
			}
		}
		return;
	}

	cur_report_id = 0; /* default if this report carries no Report Reference */

	memcpy(&disc_uuid, BT_UUID_HIDS_REPORT_REF, sizeof(disc_uuid)); /* 0x2908 */
	discover_params.uuid = &disc_uuid.uuid;
	discover_params.start_handle = pending_reports[pending_idx] + 1;
	discover_params.end_handle = cur_report_end_handle();
	discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
	disc_state = DISC_REPORT_REF;

	err = bt_gatt_discover(conn, &discover_params);
	if (err) {
		LOG_ERR("report-ref discover failed (%d); subscribing without id", err);
		discover_ccc(conn); /* fall back to CCC + subscribe, id stays 0 */
	}
}

/* Discover the CCC (0x2902) for pending_reports[pending_idx], then subscribe. */
static void discover_ccc(struct bt_conn *conn)
{
	int err;

	memcpy(&disc_uuid, BT_UUID_GATT_CCC, sizeof(disc_uuid)); /* 0x2902, NOT 0x2908 */
	discover_params.uuid = &disc_uuid.uuid;
	discover_params.start_handle = pending_reports[pending_idx] + 1;
	discover_params.end_handle = cur_report_end_handle();
	discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
	disc_state = DISC_CCC;

	err = bt_gatt_discover(conn, &discover_params);
	if (err) {
		LOG_ERR("CCC discover failed (%d)", err);
		disc_state = DISC_IDLE;
	}
}

/* Discover the notifiable Report characteristics (0x2A4D). */
static void start_report_discovery(struct bt_conn *conn)
{
	int err;

	pending_count = 0;
	pending_idx = 0;
	sub_count = 0;

	memcpy(&disc_uuid, BT_UUID_HIDS_REPORT, sizeof(disc_uuid)); /* 0x2A4D */
	discover_params.uuid = &disc_uuid.uuid;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = hids_end_handle;
	discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	disc_state = DISC_REPORT_CHRC;

	err = bt_gatt_discover(conn, &discover_params);
	if (err) {
		LOG_ERR("report-chrc discover failed (%d)", err);
		disc_state = DISC_IDLE;
	}
}

static void start_discovery(struct bt_conn *conn)
{
	int err;

	layout_valid = false;
	hids_end_handle = 0;

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
struct bond_match {
	const bt_addr_le_t *addr;
	bool found;
};

static void bond_match_cb(const struct bt_bond_info *info, void *user_data)
{
	struct bond_match *bf = user_data;

	if (bt_addr_le_cmp(&info->addr, bf->addr) == 0) {
		bf->found = true;
	}
}

static bool addr_is_bonded(const bt_addr_le_t *addr)
{
	struct bond_match bf = {.addr = addr, .found = false};

	bt_foreach_bond(BT_ID_DEFAULT, bond_match_cb, &bf);
	return bf.found;
}

static bool ad_parse_cb(struct bt_data *data, void *user_data)
{
	struct ad_info *ai = user_data;

	switch (data->type) {
	case BT_DATA_UUID16_SOME:
	case BT_DATA_UUID16_ALL:
		for (int i = 0; i + 1 < data->data_len; i += 2) {
			if (sys_get_le16(&data->data[i]) == BT_UUID_HIDS_VAL) {
				ai->has_hids = true;
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
		/* If a device-name filter is configured, match it exactly; otherwise
		 * fall back to the IST/ELECOM heuristic (and mouse appearance above). */
		if (name_filter) {
			if (strcmp(ai->name, name_filter) == 0) {
				ai->match = true;
			}
		} else if (strstr(ai->name, "IST") || strstr(ai->name, "ELECOM") ||
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

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	char s[BT_ADDR_LE_STR_LEN];
	struct ad_info ai = {0};
	int err;

	if (default_conn) {
		return;
	}

	/* #8 DIAGNOSTIC (remove after diagnosis): while scanning, log every advert
	 * from a BONDED peer OR any DIRECTED advert (aimed at us). This reveals
	 * whether Mouse A's motion-wake adverts actually reach us and whether the
	 * source address matches the bond. Targeted (bonded || directed) so it can
	 * NOT flood — a flood of INF logs perturbs BLE timing (hard lesson). */
	if (addr_is_bonded(addr) || type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		char ds[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(addr, ds, sizeof(ds));
		LOG_INF("ADV-seen %s type=%u rssi=%d bonded=%d", ds, type, rssi,
			addr_is_bonded(addr) ? 1 : 0);
	}

	/* Only initiate to connectable advertising (skip scan responses). */
	if (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
	    type != BT_GAP_ADV_TYPE_EXT_ADV) {
		return;
	}

	bt_addr_le_to_str(addr, s, sizeof(s));
	bt_data_parse(ad, ad_parse_cb, &ai);

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

	/* Interval 7.5-15ms (6-12 x 1.25ms), latency 0, 5s supervision timeout.
	 * NOTE (docs §14): the interval-widen EXPERIMENT (requested 15-30ms to give the
	 * host more wall-clock per event to recycle RX nodes) was VETOED on device --
	 * the IST PRO renegotiates to its OWN params every connect: interval 7.50 ms,
	 * latency 44, timeout 2160 ms (logged by le_param_updated). So the create-time
	 * interval request barely matters; reverted to 6-12 to avoid pointless
	 * renegotiation churn. The interval throughput lever is dead for this peer. */
	struct bt_le_conn_param *cp = BT_LE_CONN_PARAM(6, 12, 0, 500);

	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, cp, &default_conn);
	if (err) {
		LOG_ERR("create connection failed (%d)", err);
		start_scan();
	}
}

/* ── idle-reconnect reliability (#8): make the auto-reconnect scan self-healing ──
 * Two surgical additions, both load-bearing for "放置してもペアリングを維持":
 *  A1 retry — bt_le_scan_start() previously only LOG_ERR'd and returned, so a
 *    single transient failure on the disconnected()->start_scan() path left the
 *    central permanently deaf until a USB re-plug (matches the ORIGINAL "only a
 *    re-plug revives it" report). Retrying from inside start_scan() protects every
 *    caller (disconnected, connected-err, device_found create-fail). This is the
 *    same code proven on device on branch fix/idle-1h-scan-retry.
 *  Heartbeat — a 60 s INF line (NO verbose BT DBG, which has broken BLE timing
 *    before) so a future idle death is finally localizable in the daily-use log:
 *    if HB stops -> dongle/USB wedged; if HB continues with conn=0 + scan_fail
 *    climbing -> central went deaf. Keep it until a real death is classified. */
static uint32_t scan_starts;
static uint32_t scan_fails;

static void scan_retry_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!default_conn) {
		start_scan(); /* keep retrying until the scanner is actually running */
	}
}
static K_WORK_DELAYABLE_DEFINE(scan_retry_work, scan_retry_handler);

static void heartbeat_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_handler);
static void heartbeat_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("HB up=%us conn=%d sub=%u disc=%d scan_ok=%u scan_fail=%u rx_notif=%u pub=%u lat=%u",
		k_uptime_get_32() / 1000U, default_conn ? 1 : 0,
		(unsigned)sub_count, (int)disc_state, scan_starts, scan_fails,
		rx_notif, pub_reports, cur_latency);
	k_work_reschedule(&heartbeat_work, K_SECONDS(60));
}

static void start_scan(void)
{
	/* CONTINUOUS scan (window == interval == 30 ms, 100% duty) so the mouse's
	 * brief directed-advert reconnect burst on wake is caught on the FIRST advert,
	 * not after several mouse power-cycles (the NEW 2026-06-20 "繰り返してたら接続"
	 * symptom). The dongle is USB-powered, so the higher radio duty has no cost,
	 * and scan only runs while default_conn == NULL (device_found early-returns
	 * otherwise), so an established link is never perturbed. Keep ACTIVE (first-time
	 * name discovery) and OPT_NONE (no FILTER_DUPLICATE: every advert must keep
	 * arriving so a re-trigger after a failed connect still fires device_found). */
	struct bt_le_scan_param sp = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL_MIN,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err = bt_le_scan_start(&sp, device_found);

	if (err == -EALREADY) {
		return; /* already scanning -- fine, don't count as a failure */
	}
	if (err) {
		scan_fails++;
		/* A1 FIX: previously this returned and the scanner was deaf forever.
		 * Retry with a short backoff so a transient failure can't wedge it. */
		LOG_ERR("scan start failed (%d) total_fail=%u -> retry in 1s", err,
			scan_fails);
		k_work_reschedule(&scan_retry_work, K_MSEC(1000));
		return;
	}
	scan_starts++;
	LOG_INF("scanning for a HOGP pointer... (epoch=%u)", scan_starts);
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

	/* Request L2 (encryption, unauthenticated) -- NOT L3. The IST PRO is
	 * NoInputNoOutput / LE-legacy, so Just Works is the only viable method;
	 * MITM (L3) is fatal. Legacy is permitted via BT_SMP_SC_PAIR_ONLY=n.
	 * Discovery is encryption-gated, so it begins only in security_changed. */
	err = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (err) {
		LOG_ERR("set security failed (%d)", err);
	}
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	if (err != BT_SECURITY_ERR_SUCCESS || level < BT_SECURITY_L2) {
		LOG_WRN("pairing/security failed (level %u err %d) — disconnecting", level, err);
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
	LOG_INF("disconnected: %s (reason 0x%02x) rx_notif=%u pub=%u", s, reason, rx_notif,
		pub_reports);

	if (default_conn != conn) {
		return;
	}
	bt_conn_unref(default_conn);
	default_conn = NULL;
	sub_count = 0;
	pending_count = 0;
	pending_idx = 0;
	disc_state = DISC_IDLE;
	cur_latency = 0xFFFF; /* #8 Fix-A diag: latency n/a while disconnected */
	/* Invalidate the peer's layout the instant the link drops: reports still
	 * queued (or arriving before the next peer's report map is parsed) must NOT
	 * be decoded against a stale layout -- critical when reconnecting to a
	 * DIFFERENT bonded peer (BT_MAX_PAIRED=2). */
	layout_valid = false;
	rm_len = 0;
	/* Drop this peer's still-queued reports so none can be matched against the
	 * NEXT peer's layout (the report-id guard alone misses two peers that share
	 * a report id). disconnected() and notify_cb both run on the BT RX context,
	 * so no new event can be enqueued after this purge. */
	k_msgq_purge(&report_msgq);
	/* Release any button held at disconnect so it doesn't latch on the host PC. */
	if (host_dev) {
		ble_hid_host_reset(host_dev);
	}

	start_scan(); /* auto-reconnect: re-scan, match the bonded peer, re-discover */
}

/* #8 Fix-A: clamp the peer's requested peripheral latency to 0. The IST PRO asks
 * for latency 44 on every (re)connect; after a DEEP sleep that long skip-window
 * correlates with a "connected but not streaming" zombie (reports stop arriving
 * though the link stays up, with no supervision timeout). macOS, which is immune,
 * drives HID links toward latency 0. Accept the peer's interval/timeout but force
 * latency 0 (floor the timeout for safety). Also actively re-driven after discovery
 * in subscribe_pending(). PLAUSIBLE, not proven: if the peer NAKs, the heartbeat
 * 'lat=' field still shows 44 and the zombie persists -> escalate to an OTA sniff. */
static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	ARG_UNUSED(conn);
	param->latency = 0U;
	if (param->timeout < 100U) { /* keep >= 1 s supervision (unit 10 ms) */
		param->timeout = 400U;
	}
	return true;
}

/* OBSERVE: log the FINAL negotiated link params. Fires once per (re)negotiation,
 * NOT per-PDU. interval unit = 1.25 ms, timeout unit = 10 ms. */
static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);
	cur_latency = latency; /* #8 Fix-A diag: track effective latency for the heartbeat */
	LOG_INF("conn params updated: interval %u.%02u ms, latency %u, timeout %u ms",
		(interval * 5U) / 4U, ((interval * 5U) % 4U) * 25U, latency, timeout * 10U);
}

/* OBSERVE-only: log the negotiated PHY to PROVE the link runs at LE 2M (expected,
 * auto-requested by CONFIG_BT_AUTO_PHY_UPDATE) vs being pinned to 1M by the peer.
 * Requires CONFIG_BT_USER_PHY_UPDATE=y -- without it the host neither records the
 * PHY nor calls this back. 1 = LE 1M, 2 = LE 2M, 4 = LE Coded. */
static void le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	ARG_UNUSED(conn);
	LOG_INF("PHY updated: tx %u, rx %u (1=1M 2=2M 4=coded)", param->tx_phy, param->rx_phy);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
	.le_phy_updated = le_phy_updated,
};

/* ─────────────── pairing callbacks (NoInputNoOutput -> Just Works) ───────── */
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

/* ─────────────── deferred BT bring-up (off the device-init path) ─────────── */
static void start_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int err;

	err = bt_enable(NULL);
	if (err && err != -EALREADY) {
		LOG_ERR("bt_enable failed (%d)", err);
		return;
	}

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		settings_load(); /* restore bonds/IRKs -- MUST be after bt_enable */
	}

	bt_conn_auth_cb_register(&auth_cb);
	bt_conn_auth_info_cb_register(&auth_info_cb);

	LOG_INF("ble-hid-host central up on %s (filter: %s)",
		host_dev ? host_dev->name : "?",
		name_filter ? name_filter : "<any HOGP pointer>");
	start_scan();
	k_work_reschedule(&heartbeat_work, K_SECONDS(60)); /* #8: arm idle heartbeat */
}

static K_WORK_DELAYABLE_DEFINE(start_work, start_work_handler);

int zmk_ble_hid_host_central_start(const struct device *dev, const char *filter)
{
	host_dev = dev;
	name_filter = filter;

	k_work_init(&report_work, report_work_handler);

	/* Defer the heavy BT bring-up so flash/settings are ready and we are off
	 * the POST_KERNEL device-init thread. */
	k_work_schedule(&start_work, K_MSEC(500));
	return 0;
}

#else /* !CONFIG_BT_CENTRAL */

int zmk_ble_hid_host_central_start(const struct device *dev, const char *filter)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(filter);
	LOG_WRN("CONFIG_BT_CENTRAL disabled; ble-hid-host will not receive");
	return 0;
}

#endif /* CONFIG_BT_CENTRAL */
