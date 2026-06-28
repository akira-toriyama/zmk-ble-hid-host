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
        .reboot_count = 0, .reboot_budget = 2,
        .healthy_since_boot = true,
    };
    return c;
}

int main(void) {
    /* flowing -> OK_RESET (and boundary rx_delta == rx_min) */
    { struct zr_ctx c = base(); c.rx_delta = 250; CHECK(zr_decide(&c) == ZR_OK_RESET); }
    { struct zr_ctx c = base(); c.rx_delta = 100; CHECK(zr_decide(&c) == ZR_OK_RESET); }

    /* #8 v3.6 burst-ceiling boundary (regression-lock the deployed ZR_MIN_RX=100): a zombie's
     * one-shot post-reconnect flush burst (field max observed 97) must stay BELOW the bar so it
     * is bounced, NOT declared healthy. 97/99 < 100 -> zombie; 100 >= 100 -> healthy. Lowering
     * the threshold to 50 (tried, reverted) declared +58/+89 bursts healthy = frozen cursor. */
    { struct zr_ctx c = base(); c.rx_delta = 97; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }
    { struct zr_ctx c = base(); c.rx_delta = 99; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

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

    /* ───────────────────────── #8 v3.7 patience (ZR_WAIT rung) ─────────────────────────
     * On a long-idle episode (patience_eligible), re-observe the SAME window instead of an
     * immediate bounce, letting a just-woken mouse self-stream past the UNCHANGED 100 ceiling.
     * Grounded in the 2026-06-28 23:30 log: fast bounces delivered rx+0; the cure was rx
     * self-climbing 33->66->99->133 in one quiet window. The rung sits ABOVE bounce, BELOW the
     * 100 OK gate, and never sets healthy_since_boot. */

    /* NEW CORE: long-idle silent window (rx+0) -> re-observe, not an immediate useless bounce
     * (the 23:30:49 window). The central behaviour change vs v3.6. */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.patience_eligible = true; c.patience_left = 3;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_WAIT); }

    /* LINCHPIN: the 23:31:10 burst that STALLS at ~71 is < 100 -> rung 0 rejects it (never
     * healthy) and patience re-observes instead of bouncing. Patience USES the 100 ceiling. */
    { struct zr_ctx c = base(); c.rx_delta = 71; c.patience_eligible = true; c.patience_left = 2;
      CHECK(zr_decide(&c) == ZR_WAIT); }

    /* field-max 97 stall UNDER patience: still < 100 -> never OK, re-observes (ceiling lock). */
    { struct zr_ctx c = base(); c.rx_delta = 97; c.patience_eligible = true; c.patience_left = 3;
      CHECK(zr_decide(&c) == ZR_WAIT); }

    /* the cure (23:31:24): a self-climb to >=100 in one window -> rung 0 beats patience -> OK
     * immediately with rounds still unspent (no wasted waiting, no bounce/reboot). The DoD win. */
    { struct zr_ctx c = base(); c.rx_delta = 133; c.patience_eligible = true; c.patience_left = 2;
      CHECK(zr_decide(&c) == ZR_OK_RESET); }

    /* rung ORDER: patience outranks bounce -- even with bounces exhausted, remaining patience
     * defers escalation (we never bounce/reboot a peer we are still patiently observing). */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.patience_eligible = true; c.patience_left = 1;
      c.bounce_attempts = 2; CHECK(zr_decide(&c) == ZR_WAIT); }

    /* bounded escalation: budget drained -> the WAIT rung is inert -> resume the v3.6 ladder.
     * (The caller routes this first post-patience bounce as a long-down-gap bounce; policy only
     * decides DELAYED_BOUNCE.) */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.patience_eligible = true; c.patience_left = 0;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    /* KILL SWITCH A: short-gap reconnect (eligible=false) is exactly v3.6 even with rounds set. */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.patience_eligible = false; c.patience_left = 3;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    /* KILL SWITCH B: eligible but zero rounds is also exactly v3.6 (either field off disables). */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.patience_eligible = true; c.patience_left = 0;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    /* full escalation end-to-end: patience AND bounces exhausted, healthy/past-gate/in-budget
     * -> reboot still reachable as the true last resort. */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.patience_eligible = true; c.patience_left = 0;
      c.bounce_attempts = 2; c.healthy_since_boot = true; c.uptime_ms = 600000;
      CHECK(zr_decide(&c) == ZR_REBOOT); }

    /* patience cannot fabricate a false healthy: spent patience+bounces on a NEVER-healthy
     * post-boot episode still GIVES UP (no reboot loop). */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.patience_eligible = true; c.patience_left = 0;
      c.bounce_attempts = 2; c.healthy_since_boot = false; CHECK(zr_decide(&c) == ZR_GIVE_UP); }

    /* a self-climb on the LAST patience round is honored as healthy (rung 0 beats a nearly
     * exhausted patience guard), not waited or bounced. */
    { struct zr_ctx c = base(); c.rx_delta = 120; c.patience_eligible = true; c.patience_left = 1;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_OK_RESET); }

    /* 99 burst AFTER patience is drained is never declared healthy -- ceiling lock holds under
     * exhausted patience too. */
    { struct zr_ctx c = base(); c.rx_delta = 99; c.patience_eligible = true; c.patience_left = 0;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all policy tests passed\n");
    return 0;
}
