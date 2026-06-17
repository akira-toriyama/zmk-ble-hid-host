# Copyright (c) 2026 akira-toriyama
# SPDX-License-Identifier: MIT

zephyr_library_amend()

# M0: virtual input-device registration only.
zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST ble_hid_host.c)

# M1/M2 (added in later milestones):
#   zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST hog_central.c)
#   zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST hid_report_parser.c)
#   zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST hid_report_decode.c)
