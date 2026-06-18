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
};

/*
 * Publish one decoded pointer report as ZMK input events (the M3 ingest path).
 *
 * Called from hog_central's report work handler (system workqueue) for the
 * pointer report only. Emits, in order:
 *   - per changed button (edge vs. prev_buttons): input_report_key(BTN_i)
 *   - input_report_rel(REL_X / REL_Y / REL_HWHEEL) when non-zero
 *   - input_report_rel(REL_WHEEL, wheel, sync=true) as the terminator
 * Every event but the last carries sync=false; the final WHEEL event sets
 * sync=true (even when wheel == 0) so the listener flushes exactly one HID
 * update covering this whole report. K_NO_WAIT: never block the workqueue.
 */
void ble_hid_host_publish(const struct device *dev,
                          const struct zmk_hid_pointer_report *r) {
    struct ble_hid_host_data *data = dev->data;

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

    /* Relative motion / horizontal scroll: skip zero deltas (no-op events). */
    if (r->dx) {
        input_report_rel(dev, INPUT_REL_X, r->dx, false, K_NO_WAIT);
    }
    if (r->dy) {
        input_report_rel(dev, INPUT_REL_Y, r->dy, false, K_NO_WAIT);
    }
    if (r->hwheel) {
        input_report_rel(dev, INPUT_REL_HWHEEL, r->hwheel, false, K_NO_WAIT);
    }

    /* Terminator: WHEEL carries sync=true so the listener processes one update.
     * Emitted unconditionally (even wheel == 0) so a button-only or move-only
     * report is still flushed. */
    input_report_rel(dev, INPUT_REL_WHEEL, r->wheel, true, K_NO_WAIT);
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
