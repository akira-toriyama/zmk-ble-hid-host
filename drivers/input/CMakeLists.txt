# Copyright (c) 2026 akira-toriyama
# SPDX-License-Identifier: MIT

zephyr_library_amend()

# M0: virtual input-device registration.
zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST ble_hid_host.c)

# M2: pure, Zephyr-independent HID decode (host-tested under tests/parser/).
zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST hid_report_parser.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST hid_report_decode.c)

# M2-remainder: HOGP BLE-central engine. Its BLE body is compiled only when
# CONFIG_BT_CENTRAL is set (the shield .conf enables it); otherwise the file
# provides a no-op start() stub so an INPUT-only build still links.
zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST hog_central.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST zr_policy.c)
