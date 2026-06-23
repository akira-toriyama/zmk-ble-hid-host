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
    if (c->resub_attempts < c->resub_max) {
        /* The post-reconnect zombie is a peer whose notification config (CCC) drifted:
         * the link is up + subscribed but streams 0 reports. Try a cheap over-air CCC
         * re-write on the LIVE link (no disconnect) ONCE before paying the bounce's
         * reconnect cost. If it doesn't restore flow, the verify window falls through
         * here again with resub_attempts spent -> the proven bounce. */
        return ZR_RESUBSCRIBE;
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
