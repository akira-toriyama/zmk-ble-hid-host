/*
 * Copyright (c) 2026 akira-toriyama
 * SPDX-License-Identifier: MIT
 *
 * Pure zombie-recovery escalation policy (#8 v3). No Zephyr deps -> host-testable.
 * Given the current recovery context, decide the next recovery action. The caller
 * (hog_central.c) executes the action; this unit holds the ladder + loop guards.
 */
#ifndef ZMK_BLE_HID_HOST_ZR_POLICY_H_
#define ZMK_BLE_HID_HOST_ZR_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

enum zr_action {
    ZR_OK_RESET,       /* link is flowing -> clear recovery state */
    ZR_DELAYED_BOUNCE, /* disconnect, then re-scan after a delay (let the peer reset) */
    ZR_REBOOT,         /* self-reboot the dongle (== the known re-plug cure) */
    ZR_GIVE_UP,        /* stop; wait for the next natural wake (no reboot loop) */
};

struct zr_ctx {
    uint32_t rx_delta;             /* rx_notif gained since the check was armed */
    uint32_t rx_min;               /* ZR_MIN_RX: >= this in the window == flowing */
    uint32_t bounce_attempts;      /* delayed bounces already used this episode */
    uint32_t bounce_max;           /* ZR_BOUNCE_MAX */
    uint32_t uptime_ms;            /* milliseconds since boot */
    uint32_t reboot_min_uptime_ms; /* ZR_REBOOT_MIN_UPTIME_MS: gate before any reboot */
    bool healthy_since_boot;       /* the link streamed healthily at least once this boot */
};

/* Decide the next recovery action. Pure: no side effects, no globals. */
enum zr_action zr_decide(const struct zr_ctx *c);

#endif /* ZMK_BLE_HID_HOST_ZR_POLICY_H_ */
