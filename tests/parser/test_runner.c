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

/* ───────────────────────── keyboard assertions ─────────────────────────── */
struct kexpect {
    uint8_t modifiers;
    uint8_t keys[ZMK_HID_MAX_KEYS];
    uint8_t key_count;
};

static void check_kbd_decode(int idx, const struct zmk_hid_keyboard_report *r,
                             const struct kexpect *e) {
    int ok = (r->modifiers == e->modifiers && r->key_count == e->key_count);
    for (uint8_t i = 0; ok && i < e->key_count; i++) {
        if (r->keys[i] != e->keys[i]) {
            ok = 0;
        }
    }
    if (!ok) {
        printf("FAIL kbd[%d]: got {mod=0x%02x n=%u k=[%02x %02x %02x]} "
               "want {mod=0x%02x n=%u k=[%02x %02x %02x]}\n",
               idx, r->modifiers, r->key_count, r->keys[0], r->keys[1], r->keys[2],
               e->modifiers, e->key_count, e->keys[0], e->keys[1], e->keys[2]);
        failures++;
    }
}

/* A standard boot-style keyboard report map: 8-bit modifier bitfield, a
 * reserved byte, then a 6-entry keycode Array. Optionally report-ID'd. */
static void make_boot_keyboard_map(uint8_t *m, size_t *len, int with_report_id) {
    static const uint8_t head_id[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, /* GD/Keyboard, Collection(App), Report ID 1 */
    };
    static const uint8_t head_noid[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,             /* GD/Keyboard, Collection(App)              */
    };
    static const uint8_t body[] = {
        0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,             /*   Keyboard page, Usage Min/Max 0xE0..0xE7 */
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, /*   logical 0..1, size 1, count 8           */
        0x81, 0x02,                                     /*   Input (Data,Var,Abs) modifier byte      */
        0x95, 0x01, 0x75, 0x08, 0x81, 0x03,             /*   Input (Const) reserved byte             */
        0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, /*   count 6, size 8, logical 0..101         */
        0x05, 0x07, 0x19, 0x00, 0x29, 0x65,             /*   Keyboard page, Usage Min/Max 0x00..0x65 */
        0x81, 0x00,                                     /*   Input (Data,Array) keycode array        */
        0xc0,                                           /* End Collection                            */
    };
    size_t n = 0;
    const uint8_t *head = with_report_id ? head_id : head_noid;
    size_t hlen = with_report_id ? sizeof(head_id) : sizeof(head_noid);
    memcpy(m + n, head, hlen);
    n += hlen;
    memcpy(m + n, body, sizeof(body));
    n += sizeof(body);
    *len = n;
}

/* ───────────────── M2+: synthetic boot keyboard (no report ID) ──────────── */
static void test_boot_keyboard(void) {
    uint8_t map[128];
    size_t maplen;
    make_boot_keyboard_map(map, &maplen, 0);

    struct zmk_hid_keyboard_layout l;
    CHECK(zmk_hid_parse_keyboard_report_map(map, maplen, &l) == 0);
    CHECK(l.valid);
    CHECK(l.report_id == 0); /* no Report ID item */
    check_field("kbd modifiers", &l.modifiers, 0, 8, false);
    check_field("kbd keys", &l.keys, 16, 8, false);
    CHECK(l.key_count == 6);

    /* HOGP payload: no report-ID byte. byte0 modifiers, byte1 reserved, 2..7 keys. */
    struct {
        const char *name;
        uint8_t payload[8];
        struct kexpect want;
    } cases[] = {
        {"LeftShift+a", {0x02, 0x00, 0x04, 0, 0, 0, 0, 0}, {.modifiers = 0x02, .keys = {0x04}, .key_count = 1}},
        {"LCtrl+LShift+a+b", {0x03, 0x00, 0x04, 0x05, 0, 0, 0, 0}, {.modifiers = 0x03, .keys = {0x04, 0x05}, .key_count = 2}},
        {"idle", {0, 0, 0, 0, 0, 0, 0, 0}, {.modifiers = 0, .key_count = 0}},
        {"drop 0x00/ErrorRollOver", {0x00, 0x00, 0x01, 0x04, 0, 0, 0, 0}, {.modifiers = 0, .keys = {0x04}, .key_count = 1}},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct zmk_hid_keyboard_report r;
        CHECK(zmk_hid_decode_keyboard_report(&l, cases[i].payload, 8, &r) == 0);
        check_kbd_decode((int)(300 + i), &r, &cases[i].want);
    }

    /* Too-short payload must be rejected (layout needs 8 bytes). */
    uint8_t tiny[] = {0x02, 0x00, 0x04};
    struct zmk_hid_keyboard_report r;
    CHECK(zmk_hid_decode_keyboard_report(&l, tiny, sizeof(tiny), &r) != 0);
}

/* ───────────── M2+: report-ID'd keyboard parses the ID from the map ─────── */
static void test_keyboard_with_report_id(void) {
    uint8_t map[128];
    size_t maplen;
    make_boot_keyboard_map(map, &maplen, 1);

    struct zmk_hid_keyboard_layout l;
    CHECK(zmk_hid_parse_keyboard_report_map(map, maplen, &l) == 0);
    CHECK(l.valid);
    CHECK(l.report_id == 1); /* parsed from the map; NOT present in the payload */
    /* The report-ID item resets the bit cursor, so field offsets are unchanged. */
    check_field("kbd(id) modifiers", &l.modifiers, 0, 8, false);
    check_field("kbd(id) keys", &l.keys, 16, 8, false);
    CHECK(l.key_count == 6);

    uint8_t payload[] = {0x02, 0x00, 0x04, 0, 0, 0, 0, 0}; /* no leading ID byte */
    struct zmk_hid_keyboard_report r;
    CHECK(zmk_hid_decode_keyboard_report(&l, payload, sizeof(payload), &r) == 0);
    struct kexpect want = {.modifiers = 0x02, .keys = {0x04}, .key_count = 1};
    check_kbd_decode(310, &r, &want);
}

/* ── M2+: a non-standard (report_size != 1) modifier field is NOT recorded ── */
static void test_keyboard_nonstandard_modifier_ignored(void) {
    /* Modifier item declared as 8 x 2-bit (spec-legal, never seen in real
     * keyboards). The decoder reports modifiers as a 1-bit-per-key uint8_t mask,
     * so the parser must leave modifiers absent rather than record a 16-bit field
     * that would truncate/mis-map on decode. The keycode array is unaffected. */
    static const uint8_t map[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,             /* GD/Keyboard, Collection(App)        */
        0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,             /*   Keyboard page, Usage 0xE0..0xE7   */
        0x15, 0x00, 0x25, 0x03, 0x75, 0x02, 0x95, 0x08, /*   logical 0..3, SIZE 2, count 8     */
        0x81, 0x02,                                     /*   Input (Var) 16-bit modifier field */
        0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, /*   count 6, size 8, logical 0..101   */
        0x05, 0x07, 0x19, 0x00, 0x29, 0x65,             /*   Keyboard page, Usage 0x00..0x65   */
        0x81, 0x00,                                     /*   Input (Array) keycodes            */
        0xc0,                                           /* End Collection                      */
    };

    struct zmk_hid_keyboard_layout l;
    CHECK(zmk_hid_parse_keyboard_report_map(map, sizeof(map), &l) == 0);
    CHECK(l.valid);                  /* still usable via the keycode array */
    CHECK(l.modifiers.bit_size == 0); /* the 2-bit-wide modifier field is dropped */
    check_field("nonstd keys", &l.keys, 16, 8, false); /* array starts after the 16 modifier bits */
    CHECK(l.key_count == 6);

    /* Even with every modifier bit set, decode reports modifiers == 0 (absent),
     * and the keycode array still decodes correctly. */
    uint8_t payload[] = {0xff, 0xff, 0x04, 0, 0, 0, 0, 0};
    struct zmk_hid_keyboard_report r;
    CHECK(zmk_hid_decode_keyboard_report(&l, payload, sizeof(payload), &r) == 0);
    struct kexpect want = {.modifiers = 0, .keys = {0x04}, .key_count = 1};
    check_kbd_decode(320, &r, &want);
}

/* ─── M2+: composite-device parsing + class discrimination (real hardware) ─ */
static void test_composite_and_discrimination(void) {
    /* The IST PRO is a COMPOSITE device: its Report Map carries a Mouse
     * collection (report 2) AND a Keyboard collection (report 1, the
     * programmable buttons -- 8 modifiers + a 6-key array). Each parser pulls
     * out its own class from the SAME map. */
    uint8_t ist[512];
    size_t ilen = load_hex_blob("ist_pro.report_map.hex", ist, sizeof(ist));

    struct zmk_hid_report_layout pl;
    CHECK(zmk_hid_parse_report_map(ist, ilen, &pl) == 0);
    CHECK(pl.valid && pl.report_id == 2);

    struct zmk_hid_keyboard_layout kl;
    CHECK(zmk_hid_parse_keyboard_report_map(ist, ilen, &kl) == 0);
    CHECK(kl.valid && kl.report_id == 1);
    check_field("ist kbd modifiers", &kl.modifiers, 0, 8, false);
    CHECK(kl.keys.bit_size == 8 && kl.key_count == 6);

    /* A keyboard-only map yields a keyboard but NO pointer. */
    uint8_t kbd[128];
    size_t klen;
    make_boot_keyboard_map(kbd, &klen, 0);
    struct zmk_hid_report_layout pl2;
    CHECK(zmk_hid_parse_report_map(kbd, klen, &pl2) == -1 && !pl2.valid);

    /* A pointer-only map (synthetic boot mouse) yields a pointer but NO keyboard. */
    static const uint8_t mouse[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
        0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
    };
    struct zmk_hid_keyboard_layout kl2;
    CHECK(zmk_hid_parse_keyboard_report_map(mouse, sizeof(mouse), &kl2) == -1 && !kl2.valid);
}

int main(void) {
    test_contract_compiles();
    test_ist_pro_report_map();
    test_ist_pro_live_reports();
    test_standard_mouse_with_report_id();
    test_requires_x_and_y();
    test_first_pointer_collection_wins();
    test_boot_mouse();
    test_boot_keyboard();
    test_keyboard_with_report_id();
    test_keyboard_nonstandard_modifier_ignored();
    test_composite_and_discrimination();

    if (failures) {
        printf("%d host check(s) failed\n", failures);
        return 1;
    }
    printf("all host tests passed\n");
    return 0;
}
