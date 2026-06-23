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
    if (c->bounce_attempts < c->bounce_max) {
        return ZR_DELAYED_BOUNCE; /* still have light retries */
    }
    /* Bounces exhausted. Self-reboot ONLY if the link worked this boot (so a
     * post-boot zombie never triggers a reboot loop) and we're past the uptime
     * gate. Otherwise give up and let the next natural wake reset the episode. */
    if (c->healthy_since_boot && c->uptime_ms >= c->reboot_min_uptime_ms) {
        return ZR_REBOOT;
    }
    return ZR_GIVE_UP;
}
