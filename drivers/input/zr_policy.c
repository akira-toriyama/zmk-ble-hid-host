/*
 * Copyright (c) 2026 akira-toriyama
 * SPDX-License-Identifier: MIT
 *
 * Pure zombie-recovery escalation policy (#8 v3). See zr_policy.h.
 */
#include <zmk_ble_hid_host/zr_policy.h>

enum zr_action zr_decide(const struct zr_ctx *c)
{
    if (c->rx_delta >= c->rx_min) {
        return ZR_OK_RESET; /* flowing */
    }
    /* v3.7 patience: a long-idle episode with re-observe rounds left -> wait one more window
     * instead of bouncing. No disconnect/bounce/reboot and this never sets healthy_since_boot,
     * so it cannot fabricate a false healthy. Inert (falls through to the v3.6 ladder) when the
     * episode is not patience_eligible OR the budget is spent -> byte-for-byte v3.6 with patience
     * off. The caller decrements patience_left per ZR_WAIT, so it always eventually escalates. */
    if (c->patience_eligible && c->patience_left > 0) {
        return ZR_WAIT;
    }
    if (c->bounce_attempts < c->bounce_max) {
        return ZR_DELAYED_BOUNCE; /* still have light retries */
    }
    /* Bounces exhausted. Self-reboot (a USB re-enumeration) ONLY if:
     *  - the link worked this boot (so a post-boot zombie never starts a reboot loop),
     *  - we're past the uptime gate, AND
     *  - the reboot budget is not spent (rate-limit: a chronic peer degrades to
     *    GIVE_UP / manual recovery instead of re-enumerating USB on a tight cycle).
     * Otherwise give up and let the next natural wake reset the episode. */
    if (c->healthy_since_boot && c->uptime_ms >= c->reboot_min_uptime_ms &&
        c->reboot_count < c->reboot_budget) {
        return ZR_REBOOT;
    }
    return ZR_GIVE_UP;
}
