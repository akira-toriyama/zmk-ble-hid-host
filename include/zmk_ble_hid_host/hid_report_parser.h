/*
 * Copyright (c) 2026 akira-toriyama
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure HID report-map parsing + report decoding.
 *
 * This header has NO Zephyr dependencies (only <stdint.h> / <stdbool.h> /
 * <stddef.h>) so the implementation can be compiled and unit-tested on the
 * host with a plain C compiler -- no nRF52840 required. See tests/parser/.
 *
 * Implementations land in milestone M2 (hid_report_parser.c / hid_report_decode.c).
 */

#ifndef ZMK_BLE_HID_HOST_HID_REPORT_PARSER_H_
#define ZMK_BLE_HID_HOST_HID_REPORT_PARSER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of mouse buttons we track in a decoded report. */
#define ZMK_HID_MAX_BUTTONS 16

/**
 * A decoded pointer report.
 *
 * dx/dy/wheel/hwheel are relative deltas (REL semantics). buttons is a bitmask
 * where bit i is set while button i is pressed (bit 0 == button 1 == left).
 */
struct zmk_hid_pointer_report {
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    int32_t hwheel;
    uint32_t buttons;
};

/**
 * Location of one logical field inside a raw input report.
 *
 * Bit offsets are measured from the start of the report payload (i.e. AFTER the
 * leading report-ID byte, when the report has one). bit_size == 0 means the
 * field is absent from this report.
 */
struct zmk_hid_field {
    uint16_t bit_offset;
    uint8_t bit_size;
    bool is_signed;
};

/**
 * Where each pointer field lives in a peer's input report, as derived from its
 * HID Report Map (GATT characteristic 0x2A4B).
 */
struct zmk_hid_report_layout {
    uint8_t report_id;            /**< 0 == reports carry no report-ID byte. */
    struct zmk_hid_field x;
    struct zmk_hid_field y;
    struct zmk_hid_field wheel;
    struct zmk_hid_field hwheel;  /**< AC Pan, when present. */
    struct zmk_hid_field buttons; /**< Contiguous button bitfield. */
    uint8_t button_count;
    bool valid;                   /**< true once a usable pointer layout is found. */
};

/**
 * Parse a HID Report Map into a pointer-report layout.
 *
 * Walks the report-descriptor item stream, tracking the global/local item state
 * (usage page, usages, report size/count, report ID) and records the bit
 * positions of the Buttons / X / Y / Wheel / AC-Pan fields of the first usable
 * pointer (mouse) collection.
 *
 * @param report_map  Raw bytes of the HID Report Map characteristic.
 * @param len         Length of @p report_map in bytes.
 * @param out         Output layout; out->valid indicates success.
 * @return 0 on success, negative on parse error / no pointer found.
 *
 * Pure function (no Zephyr deps). Implemented in M2.
 */
int zmk_hid_parse_report_map(const uint8_t *report_map, size_t len,
                             struct zmk_hid_report_layout *out);

/**
 * Decode one raw input report using a previously parsed layout.
 *
 * @param layout  Layout produced by zmk_hid_parse_report_map().
 * @param report  Raw input-report bytes as received over GATT notification.
 * @param len     Length of @p report in bytes.
 * @param out     Decoded pointer report.
 * @return 0 on success, negative if the report does not match the layout
 *         (e.g. report-ID mismatch or too short).
 *
 * Pure function (no Zephyr deps). Implemented in M2.
 */
int zmk_hid_decode_report(const struct zmk_hid_report_layout *layout,
                          const uint8_t *report, size_t len,
                          struct zmk_hid_pointer_report *out);

#ifdef __cplusplus
}
#endif

#endif /* ZMK_BLE_HID_HOST_HID_REPORT_PARSER_H_ */
