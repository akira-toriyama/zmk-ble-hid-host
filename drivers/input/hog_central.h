/*
 * Copyright (c) 2026 akira-toriyama
 * SPDX-License-Identifier: MIT
 *
 * Internal interface between the input-device shell (ble_hid_host.c) and the
 * HOGP BLE-central engine (hog_central.c).
 */

#ifndef ZMK_BLE_HID_HOST_HOG_CENTRAL_H_
#define ZMK_BLE_HID_HOST_HOG_CENTRAL_H_

#include <zephyr/device.h>

/**
 * Start the HOGP BLE central.
 *
 * Enables Bluetooth (this module owns the stack; ZMK_BLE is off), restores
 * bonds, then scans for a HOGP pointing device, connects, bonds (LE-legacy
 * Just Works), reads + parses the peer's HID Report Map, and subscribes to its
 * input reports. Decoded reports are logged (M2) and, from M3, published on
 * @p dev via the input subsystem.
 *
 * The heavy BT init is deferred off the device-init path, so this is safe to
 * call from the device's POST_KERNEL init.
 *
 * @param dev          The zmk,ble-hid-host input device (publish target for M3).
 * @param name_filter  If non-NULL, only connect to a peer whose advertised GAP
 *                     name equals this string; if NULL, use the mouse-appearance
 *                     / IST-ELECOM-name heuristic.
 * @return 0 on success (or when CONFIG_BT_CENTRAL is disabled -- then a no-op).
 */
int zmk_ble_hid_host_central_start(const struct device *dev, const char *name_filter);

struct zmk_hid_pointer_report; /* defined in <zmk_ble_hid_host/hid_report_parser.h> */

/**
 * Publish one decoded pointer report on @p dev's input subsystem (M3).
 *
 * Implemented in ble_hid_host.c; called by hog_central's report work handler
 * for the pointer report only. Emits edge-triggered button events plus relative
 * X/Y/HWHEEL and a sync-terminating WHEEL event.
 */
void ble_hid_host_publish(const struct device *dev,
                          const struct zmk_hid_pointer_report *report);

/**
 * Release any buttons still held and clear the publish device's button state.
 *
 * Implemented in ble_hid_host.c; called by hog_central on disconnect so a button
 * held when the BLE link drops does not latch on the host PC.
 */
void ble_hid_host_reset(const struct device *dev);

#endif /* ZMK_BLE_HID_HOST_HOG_CENTRAL_H_ */
