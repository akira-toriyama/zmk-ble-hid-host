# Zombie auto-recovery v3 (escalation ladder) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When the dongle's BLE link wedges into a "post-reconnect zombie" (conn=1, subscribed, but reports stop flowing), recover it fully automatically — escalating from a delayed bounce to a self-reboot — instead of bouncing 3× and giving up.

**Architecture:** Split the recovery *policy* into a pure, host-unit-testable function `zr_decide()` (no Zephyr deps), and keep the *mechanism* (BLE disconnect, delayed re-scan, `sys_reboot`, RAM flags) thin in `drivers/input/hog_central.c`. The policy implements an escalation ladder with loop guards; the mechanism executes whatever action the policy returns.

**Tech Stack:** Zephyr 4.1 / ZMK out-of-tree module, C11. Host tests = plain `cc` (no Zephyr), run via `make -C tests/<dir> test` (mirrors the existing `tests/parser/` harness). Firmware build = `canon/scripts/build-zmk.sh ist --logging` (Docker). CI = `.github/workflows/hosttest.yml`.

## Global Constraints

- Code under host test MUST have **no Zephyr dependencies** (compiles with plain `cc -std=c11 -Wall -Wextra -Werror`). The policy unit (`zr_policy.[ch]`) therefore includes only `<stdint.h>`/`<stdbool.h>`.
- INF logging only in the BLE path. **No `LOG_DBG`/verbose BT DBG** (breaks BLE timing — house lesson).
- Bond lives in NVS and MUST survive `sys_reboot` (it already does; do not erase settings).
- Do not change v2's detection (`ZR_WINDOW_MS=2000`, `ZR_MIN_RX=100`).
- Branch: `feat/zombie-auto-recover` (PR #15). Commit frequently. Do NOT push/merge without owner approval.
- Spec: `docs/superpowers/specs/2026-06-23-zombie-recovery-v3-design.md` (authoritative).

---

## File Structure

- **Create** `include/zmk_ble_hid_host/zr_policy.h` — pure policy interface: `enum zr_action`, `struct zr_ctx`, `zr_decide()`.
- **Create** `drivers/input/zr_policy.c` — pure policy implementation (the escalation ladder + guards). No Zephyr includes.
- **Create** `tests/policy/Makefile` — host-test build (mirrors `tests/parser/Makefile`).
- **Create** `tests/policy/test_runner.c` — host unit tests for `zr_decide()`.
- **Modify** `.github/workflows/hosttest.yml` — add a `policy` job running `make -C tests/policy test`.
- **Modify** `drivers/input/hog_central.c` — call `zr_decide()` from `zombie_check_handler`; add the `healthy_since_boot` flag, the delayed-rescan mechanism, the `sys_reboot` rung, and rung logging.

---

## Task 1: Pure escalation policy `zr_decide` + host tests

**Files:**
- Create: `include/zmk_ble_hid_host/zr_policy.h`
- Create: `drivers/input/zr_policy.c`
- Create: `tests/policy/Makefile`
- Create: `tests/policy/test_runner.c`
- Modify: `.github/workflows/hosttest.yml`

**Interfaces:**
- Produces: `enum zr_action { ZR_OK_RESET, ZR_DELAYED_BOUNCE, ZR_REBOOT, ZR_GIVE_UP };`
- Produces: `struct zr_ctx { uint32_t rx_delta, rx_min, bounce_attempts, bounce_max, uptime_ms, reboot_min_uptime_ms; bool healthy_since_boot; };`
- Produces: `enum zr_action zr_decide(const struct zr_ctx *c);`

- [ ] **Step 1: Write the header**

Create `include/zmk_ble_hid_host/zr_policy.h`:
```c
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
```

- [ ] **Step 2: Write the failing test harness**

Create `tests/policy/test_runner.c`:
```c
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
```

- [ ] **Step 3: Write the test Makefile**

Create `tests/policy/Makefile`:
```make
# Copyright (c) 2026 akira-toriyama
# SPDX-License-Identifier: MIT
#
# Host-side unit tests for the pure zombie-recovery escalation policy (#8 v3).
# No Zephyr toolchain required -- zr_policy.c has no Zephyr deps.
REPO_INCLUDE := $(abspath ../../include)

CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -I$(REPO_INCLUDE)

SRCS := test_runner.c ../../drivers/input/zr_policy.c

.PHONY: test clean
test: test_runner
	./test_runner

test_runner: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@

clean:
	rm -f test_runner
```

- [ ] **Step 4: Run the test to verify it FAILS (no implementation yet)**

Run: `make -C tests/policy test`
Expected: FAIL — link error, `undefined reference to 'zr_decide'` (the .c does not exist yet).

- [ ] **Step 5: Write the minimal implementation**

Create `drivers/input/zr_policy.c`:
```c
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
```

- [ ] **Step 6: Run the test to verify it PASSES**

Run: `make -C tests/policy test`
Expected: PASS — `all policy tests passed`, exit 0.

- [ ] **Step 7: Add the CI job**

Modify `.github/workflows/hosttest.yml` — after the existing `parser` job (same indentation, same `steps` shape), add:
```yaml
  policy:
    name: zombie-recovery policy host tests
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Run host-side policy tests
        run: make -C tests/policy test
```
(Match the exact `steps`/`uses` of the existing `parser` job in the same file — copy its checkout action version verbatim.)

- [ ] **Step 8: Commit**

```bash
git add include/zmk_ble_hid_host/zr_policy.h drivers/input/zr_policy.c \
        tests/policy/Makefile tests/policy/test_runner.c .github/workflows/hosttest.yml
git commit -m "feat(reconnect): pure zr_decide escalation policy + host tests (#8 v3)"
```

---

## Task 2: Wire `zr_decide` into hog_central.c + delayed-bounce rung + healthy flag

No host test (BLE mechanism; verified by build + on-device). Build is the gate.

**Files:**
- Modify: `drivers/input/hog_central.c`
- Modify: `drivers/input/CMakeLists.txt`

**Interfaces:**
- Consumes: `zr_decide()`, `enum zr_action`, `struct zr_ctx` from Task 1.
- Produces: `static bool zr_healthy_since_boot;` and a delayed-rescan path used by `disconnected()`.

- [ ] **Step 1: Add the include and tunables**

In `drivers/input/hog_central.c`, near the other `#include`s add:
```c
#include <zmk_ble_hid_host/zr_policy.h>
#include <zephyr/sys/reboot.h>
```
Replace the `#define ZR_MAX_BOUNCE 3U` line with the v3 tunables:
```c
#define ZR_BOUNCE_MAX            2U      /* delayed bounces before escalating to reboot */
#define ZR_BOUNCE_DELAY_MS       5000U   /* wait after a zombie disconnect before re-scan (peer reset) */
#define ZR_REBOOT_MIN_UPTIME_MS  60000U  /* don't self-reboot within this uptime (post-boot loop guard) */
```

- [ ] **Step 1b: Add `zr_policy.c` to the firmware build**

`drivers/input/CMakeLists.txt` lists sources explicitly (not globbed). After the `hog_central.c` source line, add:
```cmake
zephyr_library_sources_ifdef(CONFIG_ZMK_BLE_HID_HOST zr_policy.c)
```

- [ ] **Step 2: Add the RAM state**

Next to the existing `zr_*` statics add:
```c
static bool zr_healthy_since_boot; /* link streamed healthily at least once this boot (reboot guard) */
static bool zr_delay_rescan;       /* set before a zombie disconnect -> disconnected() delays re-scan */
```

- [ ] **Step 3: Add a delayed re-scan work item**

After the `scan_retry_work` definition (or near the other `K_WORK_DELAYABLE_DEFINE`s) add:
```c
static void zr_rescan_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!default_conn) {
        start_scan();
    }
}
static K_WORK_DELAYABLE_DEFINE(zr_rescan_work, zr_rescan_handler);
```

- [ ] **Step 4: Delay the re-scan on a zombie-initiated disconnect**

In `disconnected()`, where it currently calls `start_scan();` unconditionally, gate it:
```c
    /* #8 v3: a zombie-recovery bounce delays the re-scan so the peer fully resets;
     * all other disconnects re-scan immediately. */
    if (zr_delay_rescan) {
        zr_delay_rescan = false;
        k_work_reschedule(&zr_rescan_work, K_MSEC(ZR_BOUNCE_DELAY_MS));
    } else {
        start_scan();
    }
```

- [ ] **Step 5: Replace the body of `zombie_check_handler` to use the policy**

Replace the decision block (the `if (delta >= ZR_MIN_RX) {...} else if (zr_attempts < ZR_MAX_BOUNCE) {...} else {...}`) with:
```c
    struct zr_ctx ctx = {
        .rx_delta = delta,
        .rx_min = ZR_MIN_RX,
        .bounce_attempts = zr_attempts,
        .bounce_max = ZR_BOUNCE_MAX,
        .uptime_ms = k_uptime_get_32(),
        .reboot_min_uptime_ms = ZR_REBOOT_MIN_UPTIME_MS,
        .healthy_since_boot = zr_healthy_since_boot,
    };

    switch (zr_decide(&ctx)) {
    case ZR_OK_RESET:
        zr_healthy_since_boot = true;
        zr_recovering = false;
        zr_attempts = 0;
        LOG_INF("zombie-check OK: rx+%u in %us (flowing)", delta, ZR_WINDOW_MS / 1000U);
        return;
    case ZR_DELAYED_BOUNCE:
        zr_attempts++;
        zr_bounces++;
        zr_recovering = true;
        zr_delay_rescan = true; /* disconnected() will re-scan after ZR_BOUNCE_DELAY_MS */
        LOG_WRN("ZOMBIE: rx+%u<%u in %us (conn=1 sub=%u) -> delayed bounce %u/%u (re-scan in %ums)",
                delta, ZR_MIN_RX, ZR_WINDOW_MS / 1000U, (unsigned)sub_count,
                zr_attempts, ZR_BOUNCE_MAX, ZR_BOUNCE_DELAY_MS);
        bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    case ZR_REBOOT:
        LOG_WRN("ZOMBIE persists after %u bounces (up=%us, healthy_since_boot=1) -> self-reboot",
                zr_attempts, k_uptime_get_32() / 1000U);
        /* implemented in Task 3 */
        zr_recovering = false;
        zr_attempts = 0;
        return;
    case ZR_GIVE_UP:
    default:
        LOG_WRN("ZOMBIE persists after %u bounces -> giving up until next wake "
                "(healthy_since_boot=%d up=%us)",
                zr_attempts, zr_healthy_since_boot ? 1 : 0, k_uptime_get_32() / 1000U);
        zr_recovering = false;
        zr_attempts = 0;
        return;
    }
```
(Keep the existing `delta = rx_notif - zr_rx_at_arm;` and the `if (!c) return;` guard above this block.)

- [ ] **Step 6: Build (must compile)**

Run (from repo root): overlay + build per the handoff's BUILD note:
```bash
CACHE=~/.cache/zmk-canon/cfgrepo/zmk-ble-hid-host
cp drivers/input/hog_central.c drivers/input/zr_policy.c drivers/input/CMakeLists.txt "$CACHE/drivers/input/"
cp include/zmk_ble_hid_host/zr_policy.h "$CACHE/include/zmk_ble_hid_host/"
/Volumes/workspace/github.com/akira-toriyama/canon/scripts/build-zmk.sh ist --logging
git -C "$CACHE" checkout -- . ; git -C "$CACHE" clean -fdq
```
Expected: `BUILD ... DONE`, a `ble_hid_host_receiver-logging.uf2` produced, exit 0.
(NOTE: `zr_policy.c` must be picked up by the module's CMake glob for `drivers/input/*.c`; if the module lists sources explicitly instead of globbing, add `zr_policy.c` to that list — check `CMakeLists.txt` / `drivers/input/CMakeLists.txt` first.)

- [ ] **Step 7: Commit**

```bash
git add drivers/input/hog_central.c
git commit -m "feat(reconnect): delayed-bounce rung + healthy-since-boot guard via zr_decide (#8 v3)"
```

---

## Task 3: Self-reboot rung + post-reboot health logging

**Files:**
- Modify: `drivers/input/hog_central.c`

**Interfaces:**
- Consumes: `zr_healthy_since_boot`, the `ZR_REBOOT` case from Task 2.

- [ ] **Step 1: Implement the reboot action**

In `zombie_check_handler`, replace the `ZR_REBOOT` case body (`/* implemented in Task 3 */`) with:
```c
    case ZR_REBOOT:
        LOG_WRN("ZOMBIE persists after %u bounces (up=%us, healthy_since_boot=1) -> self-reboot now",
                zr_attempts, k_uptime_get_32() / 1000U);
        k_msleep(50);          /* let the log line flush over USB-CDC before reset */
        sys_reboot(SYS_REBOOT_WARM);
        return;                /* unreachable */
```

- [ ] **Step 2: Make post-reboot health visible**

Confirm the heartbeat already prints `zr=` and that the boot banner + first `zombie-check OK` after a reboot are logged (they are). No new code required — the existing `*** Booting Zephyr OS ***` + the first `zombie-check OK: rx+...` line tell us whether a self-reboot cured it. Add one boot-marker log in `main()` init so a self-reboot is attributable: near the end of the module init function add:
```c
    LOG_INF("ble_hid_host up (v3 escalation: bounce_max=%u delay=%ums reboot_gate=%us)",
            ZR_BOUNCE_MAX, ZR_BOUNCE_DELAY_MS, ZR_REBOOT_MIN_UPTIME_MS / 1000U);
```
(Place it wherever the module currently does its startup `LOG_INF`; if none exists, add it at the end of the init function that starts scanning.)

- [ ] **Step 3: Build (must compile)**

Run the same overlay+build as Task 2 Step 6.
Expected: builds clean, uf2 produced.

- [ ] **Step 4: Commit**

```bash
git add drivers/input/hog_central.c
git commit -m "feat(reconnect): self-reboot rung (last-resort recovery) + boot marker (#8 v3)"
```

---

## Task 4: On-device verification protocol (manual — owner-in-the-loop)

No code. This task documents how to validate; it produces a results note appended to the handoff.

- [ ] **Step 1: Pre-flight diagnostic (confirms the reboot rung will cure it)**

Before flashing v3: the next time the CURRENT firmware zombies (gives up), have the owner **re-plug ONLY the dongle (do not touch the mouse)**. If the cursor revives → the wedge is dongle-side → `sys_reboot` (rung 2) will cure it. Record the result.

- [ ] **Step 2: Flash v3 logging variant**

`bash ~/bin/flash-ist-logging.sh` (owner double-taps reset; uses `cat`). Confirm the boot marker `ble_hid_host up (v3 escalation...)` and the sha.

- [ ] **Step 3: Re-arm the alert monitor**

`bash ~/bin/zmk-monitor-fixa.sh` via the Monitor tool (it greps `ZOMBIE`/bounce/`self-reboot`). 24/7 `com.tommy.zmk-log` keeps the durable log.

- [ ] **Step 4: Observe + decide (accumulate; 1 result ≠ conclusive)**

Watch `~/zmk-logs` for real zombies (mid-session + post-deep-sleep). For each: did `delayed bounce` alone cure it (rung 1)? If not, did `self-reboot` cure it (boot marker → first `zombie-check OK`)? Confirm NO reboot loop (the `healthy_since_boot`/uptime guards). Tune `ZR_BOUNCE_DELAY_MS` if rung 1 needs longer. Keep the KVM-switch case separate (that's the hub's job, arriving ~6/24).

- [ ] **Step 5: Record + graduate**

Append results to `docs/handoff-8-idle-reconnect.md` (top) + issue #8. Once it self-recovers reliably with no manual action and no loop → build a **prod (non-logging)** variant, un-draft PR #15, PR to `main`.

---

## Self-Review

- **Spec coverage:** rung 1 delayed bounce (Task 2) ✓; rung 2 sys_reboot (Task 3) ✓; loop guards = uptime + healthy-since-boot (policy in Task 1, flag wired in Task 2/3) ✓; pure policy + host tests (Task 1) ✓; logging each rung + post-reboot health (Task 2/3) ✓; on-device verification + dongle-replug pre-flight (Task 4) ✓; tunables match spec §4 table ✓. Optional retained-memory reboot budget = explicitly deferred in the spec (guards 1+2 suffice) → not a task; revisit if logs show looping.
- **Placeholder scan:** the only forward-reference is Task 2's `ZR_REBOOT` case body marked "implemented in Task 3" — intentional (Task 2 keeps it a safe no-op reset so it builds/flashes; Task 3 fills the reboot). All other steps have complete code/commands.
- **Type consistency:** `zr_decide`, `enum zr_action` (`ZR_OK_RESET`/`ZR_DELAYED_BOUNCE`/`ZR_REBOOT`/`ZR_GIVE_UP`), `struct zr_ctx` field names identical across header, tests, policy.c, and the hog_central call site.
- **Build wiring (resolved):** `drivers/input/CMakeLists.txt` lists sources explicitly (not globbed) — `zr_policy.c` is added there in Task 2 Step 1b and copied into the build cache in Step 6.
