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

/** Maximum number of simultaneously-held keycodes we decode from an array
 *  report (boot/6KRO keyboards report 6; allow headroom for larger arrays). */
#define ZMK_HID_MAX_KEYS 16

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
 * A decoded keyboard report.
 *
 * modifiers is a bitmask of the eight modifier keys (bit i == HID usage 0xE0+i:
 * bit0 Left Ctrl .. bit7 Right GUI). keys[] holds the non-zero HID keycodes
 * currently reported in the keycode array, key_count of them (0x00 slots and
 * 0x01 ErrorRollOver are dropped by the decoder).
 */
struct zmk_hid_keyboard_report {
    uint8_t modifiers;
    uint8_t keys[ZMK_HID_MAX_KEYS];
    uint8_t key_count;
};

/**
 * Location of one logical field inside a raw input report.
 *
 * Bit offsets are measured from the start of the HOGP notification payload.
 * HOGP delivers each report on its own GATT Report characteristic, so the
 * payload carries NO leading report-ID byte -- offset 0 is the first field bit.
 * bit_size == 0 means the field is absent from this report.
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
    uint8_t report_id;            /**< Report ID this layout describes (0 == the
                                   *   device uses no report IDs). Identifies which
                                   *   Report characteristic carries this layout
                                   *   (matched via the Report Reference descriptor);
                                   *   the HOGP payload itself does NOT include it. */
    struct zmk_hid_field x;
    struct zmk_hid_field y;
    struct zmk_hid_field wheel;
    struct zmk_hid_field hwheel;  /**< AC Pan, when present. */
    struct zmk_hid_field buttons; /**< Contiguous button bitfield. */
    uint8_t button_count;
    bool valid;                   /**< true once a usable pointer layout is found. */
};

/**
 * Where the keyboard fields live in a peer's input report, derived from its HID
 * Report Map. A standard boot-style keyboard reports an 8-bit modifier
 * bitfield, a reserved byte, then a keycode Array (6 bytes for 6KRO). NKRO
 * bitmap reports are not modeled yet (the modifier + array form is).
 */
struct zmk_hid_keyboard_layout {
    uint8_t report_id;              /**< Report ID this layout describes (0 == none). */
    struct zmk_hid_field modifiers; /**< Contiguous modifier bitfield (usages 0xE0-0xE7). */
    struct zmk_hid_field keys;      /**< Keycode Array: bit_offset == first entry,
                                     *   bit_size == per-entry width (8 for a standard kbd). */
    uint8_t key_count;              /**< Number of entries in the keycode array. */
    bool valid;                     /**< true once a usable keyboard layout is found. */
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
 * @param report  Raw HOGP input-report notification payload (no leading
 *                report-ID byte -- see struct zmk_hid_field).
 * @param len     Length of @p report in bytes. Trailing bytes beyond the
 *                fields the layout models are ignored.
 * @param out     Decoded pointer report.
 * @return 0 on success, negative if the layout is invalid or @p report is
 *         shorter than the layout requires.
 *
 * Pure function (no Zephyr deps). Implemented in M2.
 */
int zmk_hid_decode_report(const struct zmk_hid_report_layout *layout,
                          const uint8_t *report, size_t len,
                          struct zmk_hid_pointer_report *out);

/**
 * Parse a HID Report Map into a keyboard-report layout.
 *
 * Walks the same report-descriptor item stream as zmk_hid_parse_report_map() but
 * records the modifier bitfield and keycode Array of the first usable keyboard
 * (Generic Desktop / Keyboard application) collection.
 *
 * @param report_map  Raw bytes of the HID Report Map characteristic.
 * @param len         Length of @p report_map in bytes.
 * @param out         Output layout; out->valid indicates success.
 * @return 0 on success, negative on parse error / no keyboard found.
 *
 * Pure function (no Zephyr deps). NKRO bitmap reports are not modeled yet.
 */
int zmk_hid_parse_keyboard_report_map(const uint8_t *report_map, size_t len,
                                      struct zmk_hid_keyboard_layout *out);

/**
 * Decode one raw keyboard input report using a parsed keyboard layout.
 *
 * @param layout  Layout produced by zmk_hid_parse_keyboard_report_map().
 * @param report  Raw HOGP input-report notification payload (no leading
 *                report-ID byte -- see struct zmk_hid_field).
 * @param len     Length of @p report in bytes.
 * @param out     Decoded keyboard report.
 * @return 0 on success, negative if the layout is invalid or @p report is
 *         shorter than the layout requires.
 *
 * Pure function (no Zephyr deps).
 */
int zmk_hid_decode_keyboard_report(const struct zmk_hid_keyboard_layout *layout,
                                   const uint8_t *report, size_t len,
                                   struct zmk_hid_keyboard_report *out);

#ifdef __cplusplus
}
#endif

#endif /* ZMK_BLE_HID_HOST_HID_REPORT_PARSER_H_ */
