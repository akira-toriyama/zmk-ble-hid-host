# Design — Zombie auto-recovery v3 (escalation ladder)

- **Date:** 2026-06-23
- **Status:** Design (approved direction; pending spec review → writing-plans)
- **Scope:** `drivers/input/hog_central.c` (the dongle's BLE-central zombie-recovery logic). Firmware only.
- **Relates to:** GitHub issue #8; PR #15 (`feat/zombie-auto-recover`). Builds on v2 (2 s detect window + Fix-A clamp removed).
- **Owner latitude (this effort):** multi-session OK (plan now, execute later); quality-first; breaking changes OK; refactor OK; re-plan OK. Leave nothing undone implicitly.

---

## 1. Problem (data-confirmed 2026-06-22→23)

After the mouse reconnects (deep-sleep wake, or a dongle reboot from a KVM switch), the link can come
up fully — `connected` → `secured (L2)` → GATT discovery → `subscribed to 5 report(s)`, `conn=1 sub=5` —
**yet HID reports stop flowing** (the "post-reconnect zombie"). v2's auto-recover bounces the link
(`bt_conn_disconnect`) up to 3×; when every bounce re-zombies it **gives up**, leaving a dead cursor
until the user power-cycles the mouse / re-plugs the dongle / waits minutes.

### The smoking gun (serial log, 2026-06-22 23:50→23:59)
```
23:50:01  ZOMBIE persists after 3 bounces -> giving up   (rx flat 221793, conn=1 sub=5)
23:50–55  stuck (the 3 bounces all reconnected at gap=0s and re-zombied)
23:55:51  disconnected reason 0x13   ← MOUSE went to sleep on its own (idle)
~4 min    conn=0 (mouse asleep / not advertising)
23:59:56  connected → (0x3e fail) → connected → zombie-check armed gap=0s recovering=0
23:59:58  zombie-check OK: rx+250 in 2s   ← HEALTHY, fully automatic (no manual action)
```

**Conclusion:** a *fast* reconnect (our bounce, `gap=0s`) does NOT clear the zombie. A reconnect that
follows a **long down-gap + a mouse-side reset** (here: a natural `0x13` sleep, ~4 min down) DOES. This
matches every known manual cure — mouse OFF/ON, dongle re-plug ("…leave it 5 min and it came back"):
all of them give the peer a clean, fully-reset re-establishment, which a `gap=0s` bounce does not.

### Root cause (working theory, to confirm on-device)
The bounce tears the link and re-initiates within ~0–1 s. The peer (IST PRO) has not dropped/reset its
side (its supervision timeout is 2160 ms, and even that may not fully reset HID notification state), so
the immediate reconnect re-enters the same wedged state. A longer gap (≥ the peer's reset time) or a
full dongle BLE-stack reset (= re-plug) is what actually clears it.

---

## 2. Goal / success criteria / non-goals

**Success:** the dongle recovers a persistent zombie **fully automatically, with no manual action**
(no mouse OFF/ON, no re-plug, no "wait 5 minutes"), as fast as is reliable. Recovering via a brief
USB re-enumeration (self-reboot) is acceptable (it automates the re-plug the user already does).

**Non-goals:**
- The **KVM-switch reboot storm** itself (dongle power-cycles when the monitor switches PCs) — that is
  the *hardware* prong (self-powered USB hub, Elecom U2H-TZS428SBK, arriving ~6/24). v3 makes recovery
  reliable *when* the dongle is up; it does not stop the power-cuts. The two prongs are complementary.
- The **post-boot zombie** (zombie immediately after a fresh boot, never having streamed) — v3
  deliberately does NOT self-reboot in that case (see loop guard §4); that case is the hub's domain.
- Changing v2's detection (2 s window, `ZR_MIN_RX=100`) — kept as-is.

---

## 3. Approaches considered

- **A — Escalation ladder (CHOSEN).** Delayed-bounce → `sys_reboot` last resort, with loop guards.
  Reliable (the reboot rung == the known re-plug cure) and graceful (tries the light fix first).
- **B — Delayed bounce only (no reboot).** Simpler, no USB re-enumeration. *Rejected as sole fix:* the
  data shows the natural cure needed a long gap + mouse self-reset; a few-seconds wait may be
  insufficient, which would leave the user stuck again. Kept as **rung 1** of A.
- **v3-live — Re-arm CCCs on the live link (no disconnect).** Potentially near-instant. *Deferred:* if
  the peer is silent, re-subscribing may not help; unproven and higher-complexity. Revisit if A's rungs
  prove too disruptive.

Owner approved **A** ("推奨でOK").

---

## 4. Design — escalation ladder

Replace the single "bounce ≤3 then give up" with an ordered escalation. On each `zombie_check`
firing where `delta < ZR_MIN_RX`, take the next action up the ladder; reset to the bottom on a healthy
check.

**Rung 1 — Delayed bounce.** `bt_conn_disconnect`, then re-scan only after a delay
`ZR_BOUNCE_DELAY_MS` (start ~5000 ms; tune from logs) instead of the current immediate `start_scan()`.
Gives the peer time to drop the stale link and reset. Try up to `ZR_BOUNCE_MAX` times (start 2).
- *Mechanism:* mark zombie-recovery disconnects so `disconnected()` schedules `start_scan()` via a
  delayed work item rather than calling it inline. Normal (non-zombie) disconnects keep immediate
  re-scan.

**Rung 2 — Self-reboot.** If rung 1 is exhausted, `sys_reboot(SYS_REBOOT_WARM)` — the software
equivalent of the re-plug that is known to cure it. Bond is in NVS → survives → auto-reconnects.

**Loop guards (REQUIRED — prevent reboot loops):**
1. **Uptime gate:** self-reboot only if `k_uptime_get() > ZR_REBOOT_MIN_UPTIME_MS` (start 60 000 ms).
2. **"Healthy-since-boot" gate:** self-reboot only if the link has streamed healthily at least once
   since this boot (a RAM flag set on the first `zombie-check OK` / sustained rx). → A *post-boot*
   zombie (never healthy this boot) will NOT trigger a reboot → **no boot loop**; that case is left to
   the hub / natural recovery. A *mid-session* zombie (was working, then wedged — the current
   dead-state) WILL reboot and recover.
3. **(Hardening, optional) Reboot budget across reboots:** a counter in a dedicated retained-memory
   region (nRF52 retained domain; NOT the bootloader's GPREGRET magic) capping consecutive self-reboots
   (e.g., 2) until a healthy session resets it. Start without this (guards 1+2 already break loops);
   add if logs ever show repeated self-reboots.

**Backstops (unchanged):** the natural mouse sleep/wake recovery still works; "next wake resets
attempts" still applies. So even if a rung is gated off, the link is not worse than today.

### Tunable parameters (start values; tune from on-device logs)
| name | start | meaning |
|---|---|---|
| `ZR_BOUNCE_DELAY_MS` | 5000 | wait after a zombie disconnect before re-scan |
| `ZR_BOUNCE_MAX` | 2 | delayed bounces before escalating to reboot |
| `ZR_REBOOT_MIN_UPTIME_MS` | 60000 | min uptime before a self-reboot is allowed |

---

## 5. Quality / structure (refactor)

Separate **policy** (pure) from **mechanism** (I/O):

- **Pure policy function** — `zr_decide(ctx) -> action`, where `ctx = { rx_delta, attempts, uptime_ms,
  healthy_since_boot, reboot_budget }` and `action ∈ { OK_RESET, DELAYED_BOUNCE, REBOOT, GIVE_UP }`.
  No Zephyr calls → **host-unit-testable (TDD)**. This is the brain of the ladder and where the guard
  logic lives; it is where bugs would hide, so it gets tests.
- **Thin mechanism layer** — `zombie_check_handler` reads counters/uptime, calls `zr_decide`, and
  executes the action (`bt_conn_disconnect` + delayed re-scan, or `sys_reboot`, or reset state). No
  policy logic here.

This refactors the current ad-hoc `zr_attempts` / `zr_recovering` / `zr_rx_at_arm` globals into one
small, well-bounded state owned by the policy. Owner approved refactor.

---

## 6. Testing strategy

- **Host unit tests (TDD) for `zr_decide`** — the escalation/guard truth table: flowing→reset;
  zombie+attempts<max→delayed_bounce; attempts==max + uptime<gate→give_up (NOT reboot);
  attempts==max + uptime≥gate + healthy_since_boot→reboot; never-healthy-this-boot→give_up (no reboot,
  no loop); budget exhausted→give_up. This is the highest-value test surface and is pure.
- **On-device log verification (the real harness)** — flash the logging variant; each rung logs its
  firing + the post-action outcome (esp. "post-reboot: healthy or zombie again"). We learn which rung
  actually cures it and tune the params. Trigger is physical → accumulate trials, 1 result ≠ conclusive.
- **Build gate** — Docker build (`canon/scripts/build-zmk.sh ist --logging`) + the existing CI.

---

## 7. To verify on-device (open questions; resolve during execution)

1. **Does a delayed bounce (rung 1) alone cure it, and at what delay?** Unknown — the natural cure had
   ~4 min + a mouse self-sleep. Rung 1 may be insufficient; rung 2 (reboot) is the guaranteed fallback.
   Confirm the minimum effective `ZR_BOUNCE_DELAY_MS` from logs.
2. **Does a dongle self-reboot actually cure it (is the wedge dongle-side)?** Strong evidence yes
   (re-plug cures it). Confirm cheaply first via the **dongle-replug-only experiment** (next zombie:
   re-plug the dongle WITHOUT touching the mouse → if the cursor revives, the wedge is dongle-side and
   `sys_reboot` will cure it). The firmware also self-reports post-reboot health.
3. **Retained-memory region** for the optional reboot budget — confirm a region that survives
   `sys_reboot` but does not collide with the ZMK/MCUboot bootloader magic.

---

## 8. Rollout

1. Implement on `feat/zombie-auto-recover` (PR #15, "育てing" branch). Refactor policy/mechanism + add
   the ladder + guards + logging.
2. Host unit tests for `zr_decide` (green) + Docker build (green).
3. Flash the **logging** variant; observe across real zombies (mid-session + post-deep-sleep).
   The KVM-switch case is handled separately by the hub (~6/24); avoid conflating.
4. Once it reliably self-recovers without manual action and without loops → build a **prod (non-logging)**
   variant, un-draft PR #15, PR to `main`.

NOT pushed/merged without approval. Pacing: this session = design/spec/plan; execution next session(s).
