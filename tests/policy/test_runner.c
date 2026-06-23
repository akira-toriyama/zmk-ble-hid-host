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
 * Live-resubscribe is DISABLED here (resub_max = 0 == prior behaviour) so the existing
 * bounce/reboot-ladder tests below exercise the same rungs as before; the resubscribe
 * tests opt in explicitly with resub_max = 1. */
static struct zr_ctx base(void) {
    struct zr_ctx c = {
        .rx_delta = 0, .rx_min = 100,
        .bounce_attempts = 0, .bounce_max = 2,
        .uptime_ms = 600000, .reboot_min_uptime_ms = 60000,
        .reboot_count = 0, .reboot_budget = 2,
        .healthy_since_boot = true,
        .resub_attempts = 0, .resub_max = 0,
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

    /* --- #8 v3.4: live CCC re-subscribe rung, ONE-SHOT before the bounce ---
     * The post-reconnect zombie (conn up, subscribed, but 0 notifications) is cured by an
     * over-air CCC re-write. Try that cheap, no-disconnect kick ONCE before paying the
     * bounce's reconnect cost; if it doesn't restore flow, fall through to the proven bounce. */

    /* fresh zombie, resubscribe budget available -> RESUBSCRIBE (tried BEFORE any bounce) */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.resub_max = 1; c.resub_attempts = 0;
      CHECK(zr_decide(&c) == ZR_RESUBSCRIBE); }
    { struct zr_ctx c = base(); c.rx_delta = 88; c.resub_max = 1; c.resub_attempts = 0;
      CHECK(zr_decide(&c) == ZR_RESUBSCRIBE); }

    /* one-shot boundary: the resubscribe was spent and the verify window is still silent ->
     * fall through to the bounce ladder (never a 2nd resubscribe in one episode) */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.resub_max = 1; c.resub_attempts = 1;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    /* precedence: flow seen during the verify window -> OK_RESET; resubscribe never preempts
     * a recovered link */
    { struct zr_ctx c = base(); c.rx_delta = 250; c.resub_max = 1; c.resub_attempts = 0;
      CHECK(zr_decide(&c) == ZR_OK_RESET); }

    /* resub_max = 0 disables the rung == prior v3.3 behaviour (no regression): a fresh zombie
     * goes straight to the bounce */
    { struct zr_ctx c = base(); c.rx_delta = 0; c.resub_max = 0; c.resub_attempts = 0;
      c.bounce_attempts = 0; CHECK(zr_decide(&c) == ZR_DELAYED_BOUNCE); }

    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("all policy tests passed\n");
    return 0;
}
