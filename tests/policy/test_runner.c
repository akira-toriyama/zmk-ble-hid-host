/*
 * Copyright (c) 2026 akira-toriyama
 * SPDX-License-Identifier: MIT
 *
 * Host unit tests for the pure zombie-recovery escalation policy (#8 v3).
 */
#include <stdio.h>
#include <zmk_ble_hid_host/zr_policy.h>

static int failures;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* Baseline healthy-mid-session context; tests override the fields they exercise. */
static struct zr_ctx base(void) {
    struct zr_ctx c = {
        .rx_delta = 0, .rx_min = 100,
        .bounce_attempts = 0, .bounce_max = 2,
        .uptime_ms = 600000, .reboot_min_uptime_ms = 60000,
        .healthy_since_boot = true,
    };
    return c;
}

int main(void) {
    /* flowing -> OK_RESET (and boundary rx_delta == rx_min) */
    { struct zr_ctx c = base(); c.rx_delta = 250; CHECK(zr_decide(&c) == ZR_OK_RESET); }
    { struct zr_ctx c = base(); c.rx_delta = 100; CHECK(zr_decide(&c) == ZR_OK_RESET); }

    /* zombie, attempts left -> DELAYED_BOUNCE */
    { struct zr_ctx c = base(); c.rx_delta = 88; c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }
    { struct zr_ctx c = base(); c.rx_delta = 0;  c.bounce_attempts = 1; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    /* bounces exhausted + healthy-this-boot + uptime ok -> REBOOT */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 2; CHECK(zr_decide(&c) == ZR_REBOOT); }

    /* bounces exhausted but NEVER healthy this boot (post-boot zombie) -> GIVE_UP (no loop) */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 2; c.healthy_since_boot = false;
      CHECK(zr_decide(&c) == ZR_GIVE_UP); }

    /* bounces exhausted, healthy, but uptime below gate -> GIVE_UP (too early to reboot) */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 2; c.uptime_ms = 30000;
      CHECK(zr_decide(&c) == ZR_GIVE_UP); }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all policy tests passed\n");
    return 0;
}
