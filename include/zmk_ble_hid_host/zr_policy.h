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
    /* v3.7 patience: not flowing yet, but spend a patience round on a long-idle episode:
     * re-arm the SAME window -- do NOT disconnect/bounce/reboot, and do NOT set
     * healthy_since_boot. Bounded by the caller's patience_left; falls through to the v3.6
     * ladder when exhausted. APPENDED LAST so the four legacy enumerators keep their integer
     * values (precedence in the ladder comes from rung ORDER, not from this enum value). */
    ZR_WAIT,
};

struct zr_ctx {
    uint32_t rx_delta;             /* rx_notif gained since the check was armed */
    uint32_t rx_min;               /* ZR_MIN_RX: >= this in the window == flowing */
    uint32_t bounce_attempts;      /* delayed bounces already used this episode */
    uint32_t bounce_max;           /* ZR_BOUNCE_MAX */
    uint32_t uptime_ms;            /* milliseconds since boot (k_uptime_get_32; wraps ~49.7d, benign) */
    uint32_t reboot_min_uptime_ms; /* ZR_REBOOT_MIN_UPTIME_MS: gate before any reboot */
    uint32_t reboot_count;         /* self-reboots already spent this streak (retained across reboots) */
    uint32_t reboot_budget;        /* ZR_REBOOT_BUDGET: max reboots before GIVE_UP (rate-limit) */
    bool healthy_since_boot;       /* the link streamed healthily at least once this boot */
    /* v3.7 patience (both default 0/false -> the ZR_WAIT rung is INERT == v3.6 byte-for-byte): */
    bool patience_eligible;        /* this episode qualifies for patience (long-idle gap, latched
                                    * by the caller). false => the WAIT rung is unreachable. */
    uint32_t patience_left;        /* re-observe rounds STILL available this episode; caller
                                    * decrements per ZR_WAIT (like bounce_attempts). 0 => exhausted. */
};

/* Decide the next recovery action. Pure: no side effects, no globals. */
enum zr_action zr_decide(const struct zr_ctx *c);

#endif /* ZMK_BLE_HID_HOST_ZR_POLICY_H_ */
