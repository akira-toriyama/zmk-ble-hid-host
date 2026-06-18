/*
 * Copyright (c) 2026 akira-toriyama
 *
 * SPDX-License-Identifier: MIT
 *
 * ZMK BLE HID Host -- virtual input device.
 *
 * Responsibility split (do not violate):
 *   ingest  : THIS module  (BLE HOGP central -> decode -> input_report_*)
 *   remap   : ZMK core      (zmk,input-listener + input-processors)
 *   output  : ZMK core      (USB/BLE HID)
 *
 * Milestone status (see HANDOFF.md):
 *   M0 [this file] : register the virtual input device. No BLE yet.
 *   M1            : hog_central.c -- scan / connect / GATT discovery / subscribe.
 *   M2            : hid_report_parser.c + hid_report_decode.c (pure, host-tested).
 *   M3            : decode -> input_report_rel/key here (with button edge tracking).
 */

#define DT_DRV_COMPAT zmk_ble_hid_host

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk_ble_hid_host/hid_report_parser.h>

#include "hog_central.h"

LOG_MODULE_REGISTER(ble_hid_host, CONFIG_ZMK_BLE_HID_HOST_LOG_LEVEL);

/* ZMK's input-listener maps INPUT_BTN_0..4 to the 5 standard mouse-HID buttons
 * (button index == code - INPUT_BTN_0). We publish up to 8 so buttons 6..8 can
 * still be remapped to a key/layer behavior via zmk,input-processor-behaviors,
 * which consumes the event (ZMK_INPUT_PROC_STOP) before the mouse-HID path.
 * Codes above INPUT_BTN_4 are ignored by that path if left unmapped, so emitting
 * them is harmless; the 5-button mouse-HID cap (ZMK_HID_MOUSE_NUM_BUTTONS) only
 * limits genuine mouse-button passthrough, not key/layer remap. */
#define BLE_HID_HOST_PUBLISH_BTNS 8

struct ble_hid_host_config {
    const char *device_name;
};

struct ble_hid_host_data {
    const struct device *dev;
    /* Previous report's button bitmask, for edge-triggered button events.
     * Only ever touched from the single report work handler (one peer at a
     * time), so no locking is needed. */
    uint32_t prev_buttons;
#if IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_HWHEEL_AS_KEYS) ||                                           \
    IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_WHEEL_AS_KEYS)
    /* Bitmask (1u << enum dpad_idx) of wheel-tap release events the input queue
     * dropped while full; retried at the top of the next publish so a lost
     * release cannot latch the mapped key/layer. Same single-handler ownership
     * as prev_buttons, so no locking. */
    uint8_t pending_dpad_release;
#endif
};

#if IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_HWHEEL_AS_KEYS) ||                                           \
    IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_WHEEL_AS_KEYS)
/* The DPAD codes a wheel tick can tap, indexed so a dropped release can be
 * tracked compactly in data->pending_dpad_release. */
enum dpad_idx {
    DPAD_IDX_LEFT,
    DPAD_IDX_RIGHT,
    DPAD_IDX_UP,
    DPAD_IDX_DOWN,
    DPAD_IDX_COUNT,
};
static const uint16_t dpad_codes[DPAD_IDX_COUNT] = {
    [DPAD_IDX_LEFT] = INPUT_BTN_DPAD_LEFT,
    [DPAD_IDX_RIGHT] = INPUT_BTN_DPAD_RIGHT,
    [DPAD_IDX_UP] = INPUT_BTN_DPAD_UP,
    [DPAD_IDX_DOWN] = INPUT_BTN_DPAD_DOWN,
};

/*
 * Convert one wheel tick into a press+release tap of a ZMK-mouse-ignored DPAD
 * code so a zmk,input-processor-behaviors can remap it to a key/layer exactly
 * like a button. Both edges carry sync=false: the DPAD events are consumed
 * (ZMK_INPUT_PROC_STOP) by the behaviors processor, which invokes the bound
 * behavior per event (no listener flush needed); the report's REL_WHEEL
 * terminator (sync=true) flushes any accompanying dx/dy/button events.
 *
 * input_report_*() uses K_NO_WAIT and DROPS when the input queue is full. A
 * dropped *press* invoked nothing, so there is nothing to release. A dropped
 * *release* after an accepted press would latch the key/layer with no future
 * edge to heal it (unlike a physical button, the wheel has no later release),
 * so the index is remembered and the release retried at the top of the next
 * publish. Never blocks the workqueue.
 */
static void tap_btn(const struct device *dev, struct ble_hid_host_data *data,
                    enum dpad_idx idx) {
    if (input_report_key(dev, dpad_codes[idx], 1, false, K_NO_WAIT) != 0) {
        return;
    }
    if (input_report_key(dev, dpad_codes[idx], 0, false, K_NO_WAIT) != 0) {
        data->pending_dpad_release |= (1u << idx);
    }
}

/*
 * Retry any wheel-tap release a full input queue dropped on a previous report.
 * Called at the top of each publish; the released code clears only once its
 * release is accepted. These events are consumed per-event by the behaviors
 * processor, so they need no sync flush of their own.
 */
static void flush_pending_dpad_releases(const struct device *dev,
                                        struct ble_hid_host_data *data) {
    for (enum dpad_idx idx = 0; idx < DPAD_IDX_COUNT; idx++) {
        if ((data->pending_dpad_release & (1u << idx)) &&
            input_report_key(dev, dpad_codes[idx], 0, false, K_NO_WAIT) == 0) {
            data->pending_dpad_release &= ~(1u << idx);
        }
    }
}
#endif

/*
 * Publish one decoded pointer report as ZMK input events (the M3 ingest path).
 *
 * Called from hog_central's report work handler (system workqueue) for the
 * pointer report only. Emits, in order:
 *   - (wheel-as-keys) any DPAD release dropped by a previous full queue, retried
 *   - per changed button (edge vs. prev_buttons): input_report_key(BTN_i)
 *   - input_report_rel(REL_X / REL_Y) when non-zero
 *   - hwheel: INPUT_REL_HWHEEL, or — with CONFIG_..._HWHEEL_AS_KEYS — a
 *     DPAD_LEFT/RIGHT tap per tick (no scroll event)
 *   - input_report_rel(REL_WHEEL, ..., sync=true) as the terminator; with
 *     CONFIG_..._WHEEL_AS_KEYS the wheel becomes a DPAD_UP/DOWN tap and the
 *     terminator value is forced to 0 (flush without scrolling)
 * Every event but the last carries sync=false; the final WHEEL event sets
 * sync=true (even when wheel == 0) so the listener flushes exactly one HID
 * update covering this whole report. K_NO_WAIT: never block the workqueue.
 */
void ble_hid_host_publish(const struct device *dev,
                          const struct zmk_hid_pointer_report *r) {
    struct ble_hid_host_data *data = dev->data;

#if IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_HWHEEL_AS_KEYS) ||                                           \
    IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_WHEEL_AS_KEYS)
    /* Heal any wheel-tap release a full queue dropped last time, before adding
     * this report's events (and possibly re-filling the queue). */
    flush_pending_dpad_releases(dev, data);
#endif

    /* Buttons: emit only the bits that changed since the last report, and
     * advance prev_buttons ONLY for edges the input queue actually accepted
     * (INPUT_MODE_THREAD drops with -errno when full). A dropped edge is left
     * un-advanced so it is retried on the next report instead of latching. */
    uint32_t changed = (r->buttons ^ data->prev_buttons);
    uint32_t applied = 0;

    for (int i = 0; i < BLE_HID_HOST_PUBLISH_BTNS; i++) {
        if (changed & (1u << i)) {
            if (input_report_key(dev, INPUT_BTN_0 + i, (r->buttons >> i) & 1u,
                                 false, K_NO_WAIT) == 0) {
                applied |= (1u << i);
            }
        }
    }
    data->prev_buttons = (data->prev_buttons & ~applied) | (r->buttons & applied);

    /* Relative motion: skip zero deltas (no-op events). */
    if (r->dx) {
        input_report_rel(dev, INPUT_REL_X, r->dx, false, K_NO_WAIT);
    }
    if (r->dy) {
        input_report_rel(dev, INPUT_REL_Y, r->dy, false, K_NO_WAIT);
    }

    /* Horizontal wheel (tilt): scroll, or — when keyed — a DPAD_LEFT/RIGHT tap
     * per tick. Sign convention: hwheel < 0 = left, > 0 = right. One tap per
     * report: wheel ticks arrive as discrete ±1, so a (rare) |hwheel| > 1
     * batched report taps once, not |hwheel| times. */
#if IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_HWHEEL_AS_KEYS)
    if (r->hwheel < 0) {
        tap_btn(dev, data, DPAD_IDX_LEFT);
    } else if (r->hwheel > 0) {
        tap_btn(dev, data, DPAD_IDX_RIGHT);
    }
#else
    if (r->hwheel) {
        input_report_rel(dev, INPUT_REL_HWHEEL, r->hwheel, false, K_NO_WAIT);
    }
#endif

    /* Vertical wheel: scroll, or — when keyed — a DPAD_UP/DOWN tap per tick
     * (one tap per report; see the hwheel note on the ±1 tick assumption). Sign
     * convention: wheel > 0 = up, < 0 = down. The REL_WHEEL terminator below
     * always carries sync=true to flush the report; when the wheel is keyed its
     * value is forced to 0 so it flushes without also scrolling. */
#if IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_WHEEL_AS_KEYS)
    if (r->wheel > 0) {
        tap_btn(dev, data, DPAD_IDX_UP);
    } else if (r->wheel < 0) {
        tap_btn(dev, data, DPAD_IDX_DOWN);
    }
    input_report_rel(dev, INPUT_REL_WHEEL, 0, true, K_NO_WAIT);
#else
    /* Terminator: WHEEL carries sync=true so the listener processes one update.
     * Emitted unconditionally (even wheel == 0) so a button-only or move-only
     * report is still flushed. */
    input_report_rel(dev, INPUT_REL_WHEEL, r->wheel, true, K_NO_WAIT);
#endif
}

/*
 * Release every button the host still believes is pressed, then clear the
 * button state. Called by hog_central when the BLE link drops so a button held
 * at disconnect (e.g. a click-drag that goes out of range) does not latch on the
 * PC, and so the next peer (BT_MAX_PAIRED=2) starts from a clean slate. The last
 * release carries sync=true to flush; a no-op when nothing was held.
 */
void ble_hid_host_reset(const struct device *dev) {
    struct ble_hid_host_data *data = dev->data;

#if IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_HWHEEL_AS_KEYS) ||                                           \
    IS_ENABLED(CONFIG_ZMK_BLE_HID_HOST_WHEEL_AS_KEYS)
    /* Release any wheel-tap key/layer left latched by a dropped release so it
     * does not stick on the host across the disconnect. Reuses the accept-gated
     * retry: a release the (full) queue drops here keeps its bit set, so the next
     * connection's first publish retries it — the USB-HID link to the host
     * outlives the torn-down BLE peer, so a still-pending release can and must
     * still heal. Consumed per-event by the behaviors processor (no sync flush). */
    flush_pending_dpad_releases(dev, data);
#endif

    uint32_t held = data->prev_buttons;
    int last = -1;

    if (!held) {
        return; /* nothing latched; no event needed */
    }
    for (int i = 0; i < BLE_HID_HOST_PUBLISH_BTNS; i++) {
        if (held & (1u << i)) {
            last = i;
        }
    }
    for (int i = 0; i < BLE_HID_HOST_PUBLISH_BTNS; i++) {
        if (held & (1u << i)) {
            input_report_key(dev, INPUT_BTN_0 + i, 0, i == last, K_NO_WAIT);
        }
    }
    data->prev_buttons = 0;
}

static int ble_hid_host_init(const struct device *dev) {
    struct ble_hid_host_data *data = dev->data;
    const struct ble_hid_host_config *cfg = dev->config;

    data->dev = dev;

    LOG_INF("ble-hid-host registered (peer filter: %s)",
            cfg->device_name ? cfg->device_name : "<any HOGP pointer>");

    /* Hand off to the HOGP central engine. It defers the heavy BT bring-up, so
     * this is safe from POST_KERNEL device init. (Decode -> input_report_* in
     * the M3 publish path is wired through this same device.) */
    return zmk_ble_hid_host_central_start(dev, cfg->device_name);
}

#define BLE_HID_HOST_INST(n)                                                                       \
    static struct ble_hid_host_data ble_hid_host_data_##n = {};                                    \
    static const struct ble_hid_host_config ble_hid_host_cfg_##n = {                               \
        .device_name = DT_INST_PROP_OR(n, device_name, NULL),                                      \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, ble_hid_host_init, NULL, &ble_hid_host_data_##n,                      \
                          &ble_hid_host_cfg_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(BLE_HID_HOST_INST)
