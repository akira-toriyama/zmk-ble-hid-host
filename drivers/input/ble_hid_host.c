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

LOG_MODULE_REGISTER(ble_hid_host, CONFIG_ZMK_BLE_HID_HOST_LOG_LEVEL);

struct ble_hid_host_config {
    const char *device_name;
};

struct ble_hid_host_data {
    const struct device *dev;
    /* M1: BLE central / HOGP connection state lands here
     * (struct bt_conn *, subscribe params, discovered handles, and the
     *  parsed struct zmk_hid_report_layout for the connected peer). */
};

/*
 * M3 publish contract (implemented in milestone M3):
 *
 *   On each decoded report, emit input events on `data->dev`:
 *     - per changed button: input_report_key(dev, INPUT_BTN_*, pressed, false, K_NO_WAIT)
 *       (edge-triggered vs. the previous report's button bitmask)
 *     - input_report_rel(dev, INPUT_REL_X,      dx,     false, ...)  if dx
 *     - input_report_rel(dev, INPUT_REL_Y,      dy,     false, ...)  if dy
 *     - input_report_rel(dev, INPUT_REL_HWHEEL, hwheel, false, ...)  if hwheel
 *     - input_report_rel(dev, INPUT_REL_WHEEL,  wheel,  true,  ...)  <-- sync=true
 *   The final event carries sync=true so the listener processes one HID update.
 */

static int ble_hid_host_init(const struct device *dev) {
    struct ble_hid_host_data *data = dev->data;
    const struct ble_hid_host_config *cfg = dev->config;

    data->dev = dev;

    LOG_INF("ble-hid-host registered (peer filter: %s)",
            cfg->device_name ? cfg->device_name : "<any HOGP pointer>");

    /* M1: start BLE scanning / connect to the peer here. */
    return 0;
}

#define BLE_HID_HOST_INST(n)                                                                       \
    static struct ble_hid_host_data ble_hid_host_data_##n = {};                                    \
    static const struct ble_hid_host_config ble_hid_host_cfg_##n = {                               \
        .device_name = DT_INST_PROP_OR(n, device_name, NULL),                                      \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, ble_hid_host_init, NULL, &ble_hid_host_data_##n,                      \
                          &ble_hid_host_cfg_##n, POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(BLE_HID_HOST_INST)
