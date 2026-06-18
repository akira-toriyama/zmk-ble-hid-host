/*
 * Copyright (c) 2026 akira-toriyama
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure HID input-report decoder (milestone M2).
 *
 * Turns a raw HOGP input-report notification payload into a decoded pointer
 * report using a layout produced by zmk_hid_parse_report_map(). HID packs
 * fields LSB-first within the little-endian byte stream (USB HID 1.11 §6.2.2 /
 * §8). No Zephyr dependencies.
 *
 * NOTE on the report-ID byte: HOGP delivers each report on its own GATT Report
 * characteristic, so the notification payload carries NO leading report-ID byte
 * (confirmed on real hardware in M1). `report` is therefore the field payload
 * directly; layout->report_id is metadata identifying which characteristic this
 * layout belongs to (matched by hog_central via the Report Reference 0x2908),
 * not something to strip here. Trailing bytes beyond the modeled fields (e.g. a
 * peer report with extra usages we don't decode) are ignored.
 */

#include <zmk_ble_hid_host/hid_report_parser.h>

#include <string.h>

/* Extract `bit_size` bits starting at `bit_offset` (LSB-first), sign-extended
 * when requested. All bit math is done in size_t so a large offset can never
 * wrap; reads past `len` contribute 0 (defensive -- need >= len is enforced). */
static int32_t extract(const uint8_t *p, size_t len, const struct zmk_hid_field *f) {
    if (f->bit_size == 0) {
        return 0; /* field absent from this layout */
    }

    uint32_t v = 0;
    for (uint8_t i = 0; i < f->bit_size; i++) {
        size_t bit = (size_t)f->bit_offset + i;
        size_t byte = bit >> 3;
        if (byte >= len) {
            break;
        }
        v |= ((uint32_t)(p[byte] >> (bit & 7)) & 1u) << i;
    }

    if (f->is_signed && f->bit_size < 32) {
        uint32_t sign_bit = 1u << (f->bit_size - 1);
        if (v & sign_bit) {
            v |= ~((1u << f->bit_size) - 1);
        }
    }
    return (int32_t)v;
}

/* Highest bit index touched by the layout == minimum payload size needed. */
static size_t payload_bits(const struct zmk_hid_report_layout *l) {
    size_t bits = 0;
    const struct zmk_hid_field *fields[] = {&l->x, &l->y, &l->wheel, &l->hwheel};

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (fields[i]->bit_size) {
            size_t end = (size_t)fields[i]->bit_offset + fields[i]->bit_size;
            if (end > bits) {
                bits = end;
            }
        }
    }
    if (l->button_count) {
        size_t stride = l->buttons.bit_size ? l->buttons.bit_size : 1;
        size_t end = (size_t)l->buttons.bit_offset + (size_t)l->button_count * stride;
        if (end > bits) {
            bits = end;
        }
    }
    return bits;
}

int zmk_hid_decode_report(const struct zmk_hid_report_layout *layout,
                          const uint8_t *report, size_t len,
                          struct zmk_hid_pointer_report *out) {
    if (!layout || !report || !out || !layout->valid) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    size_t need = (payload_bits(layout) + 7) / 8;
    if (len < need) {
        return -1; /* payload too short for this layout */
    }

    out->dx = extract(report, len, &layout->x);
    out->dy = extract(report, len, &layout->y);
    out->wheel = extract(report, len, &layout->wheel);
    out->hwheel = extract(report, len, &layout->hwheel);

    /* Buttons are a contiguous run of `button_count` fields, each `stride` bits
     * (1 for a normal mouse). Button i is pressed if its field is non-zero. */
    size_t stride = layout->buttons.bit_size ? layout->buttons.bit_size : 1;
    for (uint8_t i = 0; i < layout->button_count && i < ZMK_HID_MAX_BUTTONS; i++) {
        size_t base = (size_t)layout->buttons.bit_offset + (size_t)i * stride;
        uint32_t field = 0;
        for (size_t b = 0; b < stride; b++) {
            size_t bit = base + b;
            size_t byte = bit >> 3;
            if (byte < len) {
                field |= (uint32_t)((report[byte] >> (bit & 7)) & 1u) << b;
            }
        }
        if (field) {
            out->buttons |= (uint32_t)1u << i;
        }
    }

    return 0;
}
