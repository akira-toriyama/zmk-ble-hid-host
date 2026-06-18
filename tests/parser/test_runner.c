/*
 * Copyright (c) 2026 akira-toriyama
 *
 * SPDX-License-Identifier: MIT
 *
 * Host unit tests for the pure HID report parser/decoder (milestone M2).
 *
 *   M0 : smoke test -- the harness builds against the Zephyr-free public header.
 *   M2 : link hid_report_parser.c / hid_report_decode.c and assert:
 *          - the real Elecom IST PRO Report Map parses to the documented layout
 *            (fixtures/ist_pro.report_map.hex),
 *          - every real captured input report decodes to the documented deltas
 *            (fixtures/ist_pro.live_reports.hex -- the comments are the oracle),
 *          - a synthetic standard USB mouse (report-ID'd, with padding) and a
 *            boot mouse (3-byte, int8, no report ID) parse and decode, including
 *            both the with- and without-report-ID-byte payload forms.
 *
 * Fixtures are loaded from FIXTURES_DIR (set by the Makefile) so the test is
 * independent of the current working directory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk_ble_hid_host/hid_report_parser.h>

#ifndef FIXTURES_DIR
#define FIXTURES_DIR "fixtures"
#endif

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ─────────────────────────── fixture loading ───────────────────────────── */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static FILE *open_fixture(const char *fname) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURES_DIR, fname);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("FAIL: cannot open fixture %s\n", path);
        exit(2);
    }
    return f;
}

/* Decode every hex pair in the file into one byte blob, ignoring whitespace and
 * '#' comments. Used for the Report Map (one logical descriptor). */
static size_t load_hex_blob(const char *fname, uint8_t *buf, size_t max) {
    FILE *f = open_fixture(fname);
    size_t n = 0;
    int hi = -1, c, comment = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') { comment = 1; continue; }
        if (c == '\n') { comment = 0; continue; }
        if (comment) continue;
        int v = hexval(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            if (n < max) buf[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    fclose(f);
    return n;
}

/* One report per line; hex before any '#' comment. Returns the report count. */
static size_t load_hex_lines(const char *fname, uint8_t reports[][8], size_t *lens, size_t maxrep) {
    FILE *f = open_fixture(fname);
    char line[256];
    size_t count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = 0;
        int hi = -1;
        for (char *c = line; *c && *c != '#' && *c != '\n'; c++) {
            int v = hexval((unsigned char)*c);
            if (v < 0) continue;
            if (hi < 0) {
                hi = v;
            } else {
                if (n < 8) reports[count][n++] = (uint8_t)((hi << 4) | v);
                hi = -1;
            }
        }
        if (n > 0 && count < maxrep) {
            lens[count] = n;
            count++;
        }
    }
    fclose(f);
    return count;
}

/* ───────────────────────────── assertions ──────────────────────────────── */
static void check_field(const char *what, const struct zmk_hid_field *f, uint16_t off,
                        uint8_t size, bool is_signed) {
    if (f->bit_offset != off || f->bit_size != size || f->is_signed != is_signed) {
        printf("FAIL field %s: got {off=%u size=%u signed=%d} want {off=%u size=%u signed=%d}\n",
               what, f->bit_offset, f->bit_size, f->is_signed, off, size, is_signed);
        failures++;
    }
}

struct expect {
    int32_t dx, dy, wheel, hwheel;
    uint32_t buttons;
};

static void check_decode(int idx, const struct zmk_hid_pointer_report *r, const struct expect *e) {
    if (r->dx != e->dx || r->dy != e->dy || r->wheel != e->wheel || r->hwheel != e->hwheel ||
        r->buttons != e->buttons) {
        printf("FAIL report[%d]: got {dx=%d dy=%d wh=%d hw=%d btn=0x%02x} "
               "want {dx=%d dy=%d wh=%d hw=%d btn=0x%02x}\n",
               idx, r->dx, r->dy, r->wheel, r->hwheel, r->buttons, e->dx, e->dy, e->wheel,
               e->hwheel, e->buttons);
        failures++;
    }
}

/* ───────────────────────── M0: contract smoke ──────────────────────────── */
static void test_contract_compiles(void) {
    struct zmk_hid_pointer_report r;
    memset(&r, 0, sizeof r);
    CHECK(r.dx == 0 && r.dy == 0 && r.wheel == 0 && r.hwheel == 0);
    CHECK(r.buttons == 0u);

    struct zmk_hid_report_layout layout;
    memset(&layout, 0, sizeof layout);
    CHECK(layout.valid == false);
    CHECK(layout.report_id == 0);

    CHECK(ZMK_HID_MAX_BUTTONS >= 3);
}

/* ──────────────────── M2: real Elecom IST PRO Report Map ────────────────── */
static void test_ist_pro_report_map(void) {
    uint8_t map[512];
    size_t len = load_hex_blob("ist_pro.report_map.hex", map, sizeof(map));
    CHECK(len > 0);

    struct zmk_hid_report_layout l;
    int rc = zmk_hid_parse_report_map(map, len, &l);
    CHECK(rc == 0);
    CHECK(l.valid);
    CHECK(l.report_id == 2);
    CHECK(l.button_count == 8);
    check_field("buttons", &l.buttons, 0, 1, false);
    check_field("x", &l.x, 8, 16, true);
    check_field("y", &l.y, 24, 16, true);
    check_field("wheel", &l.wheel, 40, 8, true);
    check_field("hwheel(AC Pan)", &l.hwheel, 48, 8, true);
}

/* ─────────── M2: real captured input reports decode to the oracle ───────── */
static void test_ist_pro_live_reports(void) {
    uint8_t map[512];
    size_t maplen = load_hex_blob("ist_pro.report_map.hex", map, sizeof(map));
    struct zmk_hid_report_layout l;
    CHECK(zmk_hid_parse_report_map(map, maplen, &l) == 0);

    uint8_t reports[32][8];
    size_t lens[32];
    size_t n = load_hex_lines("ist_pro.live_reports.hex", reports, lens, 32);

    /* The oracle: exactly the deltas documented in the fixture's comments,
     * in file order. byte0 maps buttons 1..8 straight onto bits 0..7. */
    static const struct expect want[] = {
        {.dx = 59, .dy = -16},                  /* move dx=+59 dy=-16     */
        {.dx = -2, .dy = 8},                    /* move dx=-2  dy=+8      */
        {.dx = 19, .dy = -1},                   /* move dx=+19 dy=-1      */
        {.dy = -1, .buttons = 0x01},            /* button1 (left)        */
        {.dy = -1, .buttons = 0x02},            /* button2 (right)       */
        {.dy = 1, .buttons = 0x03},             /* buttons 1+2 chord     */
        {.dy = -2, .buttons = 0x10},            /* button5               */
        {.buttons = 0x20},                      /* button6               */
        {.buttons = 0x40},                      /* button7               */
        {.dy = -1, .buttons = 0x80},            /* button8               */
        {.dy = -1, .buttons = 0x82},            /* buttons 2+8 chord     */
        {.wheel = 1},                           /* wheel +1 (up)         */
        {.wheel = -1},                          /* wheel -1 (down)       */
        {.hwheel = 1},                          /* AC pan +1 (right)     */
        {.hwheel = -1},                         /* AC pan -1 (left)      */
        {0},                                    /* idle / all release    */
    };
    CHECK(n == sizeof(want) / sizeof(want[0]));

    for (size_t i = 0; i < n && i < sizeof(want) / sizeof(want[0]); i++) {
        struct zmk_hid_pointer_report r;
        int rc = zmk_hid_decode_report(&l, reports[i], lens[i], &r);
        CHECK(rc == 0);
        check_decode((int)i, &r, &want[i]);
    }
}

/* ───── M2: synthetic standard mouse with a report ID + a padding field ──── */
static void test_standard_mouse_with_report_id(void) {
    static const uint8_t map[] = {
        0x05, 0x01,       /* Usage Page (Generic Desktop)   */
        0x09, 0x02,       /* Usage (Mouse)                  */
        0xa1, 0x01,       /* Collection (Application)       */
        0x85, 0x01,       /*   Report ID (1)                */
        0x09, 0x01,       /*   Usage (Pointer)              */
        0xa1, 0x00,       /*   Collection (Physical)        */
        0x05, 0x09,       /*     Usage Page (Buttons)       */
        0x19, 0x01,       /*     Usage Minimum (1)          */
        0x29, 0x03,       /*     Usage Maximum (3)          */
        0x15, 0x00,       /*     Logical Minimum (0)        */
        0x25, 0x01,       /*     Logical Maximum (1)        */
        0x95, 0x03,       /*     Report Count (3)           */
        0x75, 0x01,       /*     Report Size (1)            */
        0x81, 0x02,       /*     Input (Data,Var,Abs) btns  */
        0x95, 0x01,       /*     Report Count (1)           */
        0x75, 0x05,       /*     Report Size (5)            */
        0x81, 0x03,       /*     Input (Const) 5-bit pad    */
        0x05, 0x01,       /*     Usage Page (Generic Desktop) */
        0x09, 0x30,       /*     Usage (X)                  */
        0x09, 0x31,       /*     Usage (Y)                  */
        0x15, 0x81,       /*     Logical Minimum (-127)     */
        0x25, 0x7f,       /*     Logical Maximum (127)      */
        0x75, 0x08,       /*     Report Size (8)            */
        0x95, 0x02,       /*     Report Count (2)           */
        0x81, 0x06,       /*     Input (Data,Var,Rel) X,Y   */
        0xc0,             /*   End Collection               */
        0xc0,             /* End Collection                 */
    };

    struct zmk_hid_report_layout l;
    CHECK(zmk_hid_parse_report_map(map, sizeof(map), &l) == 0);
    CHECK(l.valid);
    CHECK(l.report_id == 1); /* parsed from the map; NOT present in the payload */
    CHECK(l.button_count == 3);
    check_field("std buttons", &l.buttons, 0, 1, false);
    check_field("std x", &l.x, 8, 8, true);
    check_field("std y", &l.y, 16, 8, true);
    CHECK(l.wheel.bit_size == 0);  /* no wheel in this descriptor */
    CHECK(l.hwheel.bit_size == 0);

    struct expect want = {.dx = 10, .dy = -10, .buttons = 0x05}; /* btn1+btn3 */

    /* HOGP payload: no report-ID byte, even though report_id == 1. The first
     * byte (0x05) is button data -- a naive "strip byte0 if it looks like the
     * report id" decoder would corrupt this. need == 3 bytes. */
    uint8_t payload[] = {0x05, 0x0a, 0xf6};
    struct zmk_hid_pointer_report r;
    CHECK(zmk_hid_decode_report(&l, payload, sizeof(payload), &r) == 0);
    check_decode(100, &r, &want);

    /* Trailing bytes beyond the modeled fields are ignored. */
    uint8_t trailing[] = {0x05, 0x0a, 0xf6, 0xff, 0x7f};
    CHECK(zmk_hid_decode_report(&l, trailing, sizeof(trailing), &r) == 0);
    check_decode(101, &r, &want);

    /* Too-short payload must be rejected. */
    uint8_t tiny[] = {0x05, 0x0a};
    CHECK(zmk_hid_decode_report(&l, tiny, sizeof(tiny), &r) != 0);
}

/* ── M2: contract -- a map needs BOTH relative X and Y to yield a layout ──── */
static void test_requires_x_and_y(void) {
    /* Mouse collection exposing only X (no Y). */
    static const uint8_t only_x[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
        0x05, 0x01,                   /* Usage Page (Generic Desktop) */
        0x09, 0x30,                   /* Usage (X)                    */
        0x15, 0x81, 0x25, 0x7f,       /* Logical -127..127            */
        0x75, 0x08, 0x95, 0x01,       /* Size 8, Count 1              */
        0x81, 0x06,                   /* Input (Data,Var,Rel)         */
        0xc0, 0xc0,
    };
    /* Same but only Y. */
    static const uint8_t only_y[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
        0x05, 0x01, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
        0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
        0xc0, 0xc0,
    };

    struct zmk_hid_report_layout l;
    CHECK(zmk_hid_parse_report_map(only_x, sizeof(only_x), &l) == -1);
    CHECK(!l.valid);
    CHECK(zmk_hid_parse_report_map(only_y, sizeof(only_y), &l) == -1);
    CHECK(!l.valid);
}

/* ── M2: contract -- the FIRST usable pointer collection wins ─────────────── */
static void test_first_pointer_collection_wins(void) {
    static const uint8_t map[] = {
        /* Collection #1: report id 1, 3 buttons, int8 X/Y. */
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x03, 0x75, 0x01, 0x81, 0x02,   /* 3 buttons */
        0x95, 0x01, 0x75, 0x05, 0x81, 0x03,   /* 5-bit pad */
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
        0x75, 0x08, 0x95, 0x02, 0x81, 0x06,   /* int8 X,Y */
        0xc0, 0xc0,
        /* Collection #2: report id 3, 5 buttons, int16 X/Y -- must be ignored. */
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xa1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x05, 0x75, 0x01, 0x81, 0x02,   /* 5 buttons */
        0x95, 0x01, 0x75, 0x03, 0x81, 0x03,   /* 3-bit pad */
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f,
        0x75, 0x10, 0x95, 0x02, 0x81, 0x06,   /* int16 X,Y */
        0xc0, 0xc0,
    };

    struct zmk_hid_report_layout l;
    CHECK(zmk_hid_parse_report_map(map, sizeof(map), &l) == 0);
    CHECK(l.valid);
    CHECK(l.report_id == 1);     /* first collection */
    CHECK(l.button_count == 3);  /* first collection's 3 buttons, not the 5 */
    check_field("first x", &l.x, 8, 8, true); /* int8, not the int16 of #2 */
    check_field("first y", &l.y, 16, 8, true);
}

/* ─────────────── M2: synthetic boot mouse (3-byte, no report ID) ────────── */
static void test_boot_mouse(void) {
    static const uint8_t map[] = {
        0x05, 0x01,       /* Usage Page (Generic Desktop)   */
        0x09, 0x02,       /* Usage (Mouse)                  */
        0xa1, 0x01,       /* Collection (Application)       */
        0x09, 0x01,       /*   Usage (Pointer)              */
        0xa1, 0x00,       /*   Collection (Physical)        */
        0x05, 0x09,       /*     Usage Page (Buttons)       */
        0x19, 0x01,       /*     Usage Minimum (1)          */
        0x29, 0x03,       /*     Usage Maximum (3)          */
        0x15, 0x00, 0x25, 0x01,
        0x95, 0x03, 0x75, 0x01,
        0x81, 0x02,       /*     Input (Data,Var,Abs) btns  */
        0x95, 0x01, 0x75, 0x05,
        0x81, 0x03,       /*     Input (Const) 5-bit pad    */
        0x05, 0x01,
        0x09, 0x30, 0x09, 0x31,
        0x15, 0x81, 0x25, 0x7f,
        0x75, 0x08, 0x95, 0x02,
        0x81, 0x06,       /*     Input (Data,Var,Rel) X,Y   */
        0xc0,
        0xc0,
    };

    struct zmk_hid_report_layout l;
    CHECK(zmk_hid_parse_report_map(map, sizeof(map), &l) == 0);
    CHECK(l.valid);
    CHECK(l.report_id == 0); /* no Report ID item -> reports carry no ID byte */
    CHECK(l.button_count == 3);
    check_field("boot x", &l.x, 8, 8, true);
    check_field("boot y", &l.y, 16, 8, true);

    uint8_t report[] = {0x01, 0x05, 0xfb}; /* btn1, X=+5, Y=-5 */
    struct zmk_hid_pointer_report r;
    CHECK(zmk_hid_decode_report(&l, report, sizeof(report), &r) == 0);
    struct expect want = {.dx = 5, .dy = -5, .buttons = 0x01};
    check_decode(200, &r, &want);
}

int main(void) {
    test_contract_compiles();
    test_ist_pro_report_map();
    test_ist_pro_live_reports();
    test_standard_mouse_with_report_id();
    test_requires_x_and_y();
    test_first_pointer_collection_wins();
    test_boot_mouse();

    if (failures) {
        printf("%d host check(s) failed\n", failures);
        return 1;
    }
    printf("all host tests passed\n");
    return 0;
}
