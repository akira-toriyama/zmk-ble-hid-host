/*
 * Copyright (c) 2026 akira-toriyama
 *
 * SPDX-License-Identifier: MIT
 *
 * Host unit tests for the pure HID report parser/decoder.
 *
 *   M0 [now] : smoke test -- proves the harness builds against the Zephyr-free
 *              public header and runs green in CI.
 *   M2       : #include + link hid_report_parser.c / hid_report_decode.c and
 *              assert decoded {dx,dy,buttons,wheel} against captured report
 *              descriptors: a standard mouse, a boot mouse (3 bytes), a
 *              report-ID'd descriptor, and -- once captured -- the real Elecom
 *              IST PRO report map (see README "Section 8: device capture").
 */
#include <stdio.h>
#include <string.h>

#include <zmk_ble_hid_host/hid_report_parser.h>

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* M0: the public contract header is valid C and the structs behave as declared.
 * Real parser/decoder assertions are added in M2. */
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

int main(void) {
    test_contract_compiles();

    if (failures) {
        printf("%d host check(s) failed\n", failures);
        return 1;
    }
    printf("all host tests passed\n");
    return 0;
}
