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

/* Baseline healthy-mid-session context; tests override the fields they exercise.
 * Re-arm is DISABLED here (rearm_max = 0 == v3.1 behaviour) so the existing
 * bounce/reboot-ladder tests below exercise the same rungs as before; the
 * re-arm tests opt in explicitly with rearm_max = 1. */
static struct zr_ctx base(void) {
    struct zr_ctx c = {
        .rx_delta = 0, .rx_min = 100,
        .bounce_attempts = 0, .bounce_max = 2,
        .uptime_ms = 600000, .reboot_min_uptime_ms = 60000,
        .reboot_count = 0, .reboot_budget = 2,
        .healthy_since_boot = true,
        .rearms_used = 0, .rearm_max = 0,
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

    /* reboot BUDGET gate (#8 freeze fix: rate-limit USB-re-enumeration reboots).
     * Otherwise-eligible reboot is suppressed once the budget is spent -> GIVE_UP. */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 2;
      c.reboot_count = 2; c.reboot_budget = 2; CHECK(zr_decide(&c) == ZR_GIVE_UP); }
    /* budget remaining -> still reboots */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 2;
      c.reboot_count = 1; c.reboot_budget = 2; CHECK(zr_decide(&c) == ZR_REBOOT); }
    /* count over budget (defensive) -> GIVE_UP */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 2;
      c.reboot_count = 5; c.reboot_budget = 2; CHECK(zr_decide(&c) == ZR_GIVE_UP); }

    /* precedence: a FLOWING link returns OK_RESET even with bounces exhausted AND the
     * reboot path otherwise armed -> the ladder must never reboot a recovered link. */
    { struct zr_ctx c = base(); c.rx_delta = 300; c.bounce_attempts = 2; CHECK(zr_decide(&c) == ZR_OK_RESET); }
    { struct zr_ctx c = base(); c.rx_delta = 100; c.bounce_attempts = 2; CHECK(zr_decide(&c) == ZR_OK_RESET); }

    /* uptime gate boundary: exactly == gate -> REBOOT (>=, not >) */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 2; c.uptime_ms = 60000;
      CHECK(zr_decide(&c) == ZR_REBOOT); }

    /* attempts strictly past max still escalates (no off-by-one re-running the ladder) */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 3; CHECK(zr_decide(&c) == ZR_REBOOT); }
    { struct zr_ctx c = base(); c.rx_delta = 0; c.bounce_attempts = 3; c.healthy_since_boot = false;
      CHECK(zr_decide(&c) == ZR_GIVE_UP); }

    /* --- #8 idle false-positive fix: bounded re-arm (debounce) before any bounce ---
     * On the first sub-threshold detection of an episode, re-arm one more window
     * (no disconnect) instead of bouncing. The IST PRO reconnects but the user may
     * not have moved yet -> rx+0 in the first 2s window -> a healthy link gets
     * bounced (the felt freeze). Re-arming lets the user's motion arrive and cancel
     * the false bounce. A genuine zombie stays silent across the re-arm -> still bounces. */

    /* fresh idle zombie (rx+0), re-arm budget available -> REARM, NOT a bounce */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.rearm_max = 1; c.rearms_used = 0;
      CHECK(zr_decide(&c) == ZR_REARM); }
    /* fresh just-under-threshold zombie (rx+88, flowing-but-low), re-arm available -> REARM */
    { struct zr_ctx c = base(); c.rx_delta = 88; c.rearm_max = 1; c.rearms_used = 0;
      CHECK(zr_decide(&c) == ZR_REARM); }

    /* K=1 boundary: the one re-arm is spent and the link is still silent -> fall through
     * to the existing bounce ladder (re-arm does not loop forever on a real zombie) */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.rearm_max = 1; c.rearms_used = 1;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    /* precedence: a link that started FLOWING during the re-arm window recovers (OK_RESET);
     * re-arm must never preempt a recovered link */
    { struct zr_ctx c = base(); c.rx_delta = 250; c.rearm_max = 1; c.rearms_used = 0;
      CHECK(zr_decide(&c) == ZR_OK_RESET); }

    /* rearm_max = 0 disables the debounce entirely == exact v3.1 behaviour (no regression):
     * a fresh sub-threshold zombie goes straight to a bounce */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.rearm_max = 0; c.rearms_used = 0;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all policy tests passed\n");
    return 0;
}
