# Investigation handoff — "move immediately after power-on → cursor freezes"

> Standalone handoff so this can be picked up cold in a fresh session. The
> ORIGINAL reported bug (re-pairing on every reconnect) is **fixed and merged**.
> This doc is only about the **residual** freeze that remains.

**Status:** CLOSED as a hardware ceiling (no firmware cure). Every firmware lever
incl. the last-resort mitigations was tried ON-DEVICE and exhausted (FIX-B §16,
max-RX §17). Mitigation = behavioral + RF only. The mouse is fully usable for
normal use; only the immediate-aggressive-move stress case still hiccups.
**Branch:** `fix/reconnect-rx-buffer-wedge` (rebased onto `main` 6964292 with the
M4 features). Not yet merged; rebase rewrote history so a push needs
`--force-with-lease`. FIX-B code lives on `experiment/fix-b-supervision-timeout`
(not merged).
**Date:** first 2026-06-18; latest 2026-06-19.

---

> ## ⏭️ NEXT SESSION — START HERE
>
> **VERDICT (2026-06-19, §15): the move-freeze has NO firmware cure — it is a hard
> architectural ceiling. The last lever (subscription pruning) is now PROVEN dead,
> AND a forensic re-read overturned the reconnect-window hypothesis: the freeze is
> primarily STEADY-STATE, not a discovery-window failure.** ALL levers exhausted:
> RX buffer depth (§9), thread-starvation (§9, refuted), 2M PHY + DLE (§13),
> FORCE_MD (§13), connection-interval (§14, VETOED by peer + harmful), and
> subscription pruning (§15 — `verify.log` shows the flood is 100% id=2; ids
> 1/4/6/9 never notify; pruning removes 0 notifications). Root cause = single-
> threaded controller RX-node recycle gated by the hard-coded
> `ull_pdu_rx_alloc_peek(3)` ACK reserve (`lll_conn.c:1148`), no HCI flow control;
> the only true fixes need a Nordic-LLL fork (forbidden) or peer cooperation
> (impossible for a 3rd-party mouse).
>
> **ON-DEVICE follow-through is now DONE (2026-06-19) — all mitigations exhausted:**
> FIX-B (central-forced shorter supervision timeout, both phases) was implemented +
> flashed + tested → **DEAD END** (peer fights it relentlessly, drops got more
> frequent — §16); and maxing the RX pool to 18/20 was flashed + tested → **NO
> benefit** (§17). The freeze is a confirmed hardware ceiling. **Shipped firmware =
> RX 12/10** (the documented baseline; RX 18/20 was tested and is interchangeable,
> so 12/10 is kept — the device is flashed RX 12/10). **Only mitigations are
> non-firmware:** behavioral (wait ~2-3 s
> after power-on before flailing; keep the link up) + RF (user confirmed already
> optimal). Full analysis: **§15** (no-cure verdict), **§16** (FIX-B), **§17** (RX-18).
>
> **NEXT (if anything): the SEPARATE open bug** — "idle ~1 h → dead, only a dongle
> re-plug revives" — see §12 (not yet investigated; owner asked to do it after the
> move-freeze, which is now closed).
>
> **Working tree is CLEAN.** The OBSERVE diagnostic loggers (le_param_updated +
> le_phy_updated + effective-params) and `CONFIG_ZMK_LOGGING_MINIMAL`/
> `CONFIG_BT_USER_PHY_UPDATE` are committed on `fix/reconnect-rx-buffer-wedge` (handy
> for the §12 idle bug); strip them before any production/no-logging merge.
>
> **Device build recipe** (A/B/C/D + RX fix + clean logging): build in
> `/Volumes/workspace/.zmk-blehh-build` via Docker `zmkfirmware/zmk-build-arm:stable`
> with `-DZMK_CONFIG=<zmk-mouse>/config -DZMK_EXTRA_MODULES=<zmk-ble-hid-host>
> -DSHIELD=ble_hid_host_receiver -DCONFIG_ZMK_USB_LOGGING=y` (combines zmk-mouse's
> A/B/C/D keymap+conf with this module). Flash = double-tap reset → cp uf2 to
> `/Volumes/XIAO-SENSE/CURRENT.UF2` (TOP-LEVEL Bash cmd + `dangerouslyDisableSandbox`,
> retry on "Permission denied"). Rollback `.uf2`s in `.zmk-blehh-build/rollback/`.
> Logging gotcha (§ here): ZMK defaults `ZMK_LOG_LEVEL=4` (DBG) under USB logging and
> its per-motion DBG flood DROPS the sparse `ble_hid_host` INF lines from the USB
> log — `CONFIG_ZMK_LOGGING_MINIMAL=y` silences that so connect/disconnect/param
> lines survive.

---

## 1. Symptom (precise)

- Repro: power-cycle the trackball (Elecom IST PRO), then **move it IMMEDIATELY
  and aggressively** on power-on. The cursor moves a little, then freezes.
- Before the RX-buffer fix: it **wedged permanently** (cursor dead until the XIAO
  was power-cycled).
- After the RX-buffer fix (current state): it **auto-recovers**, but under
  sustained aggressive movement the link still drops every **~6–13 s** and each
  drop is a ~2 s frozen-cursor reconnect. User perceives this as "still freezes."
- **If you WAIT a couple seconds after power-on before moving, it is stable.** The
  trigger is heavy incoming-notification load during/just-after the reconnect.

The disconnect is always **`reason 0x08` = `BT_HCI_ERR_CONN_TIMEOUT` (supervision
timeout)** — our side stops hearing from the mouse for the supervision window.

## 2. What is ALREADY FIXED — do not re-investigate

All on `main` (PR #2) and effective:
- **Bond persistence** (`CONFIG_SETTINGS`/`FLASH`/`NVS` chain; `CONFIG_BT_SETTINGS`
  was silently dropped before). Bonds survive reboot.
- **Directed-advert reconnect** (`CONFIG_BT_SCAN_WITH_IDENTITY=y`).
- **Reconnect GATT discovery -ENOMEM** (`CONFIG_BT_GATT_AUTO_RESUBSCRIBE=n` + TX
  buffer bumps to 8). Reconnect now re-subscribes cleanly, bond reused (`secured
  (level 2)`, no re-pair).
The mouse is fully usable for normal use; only the immediate-aggressive-move
stress case freezes.

## 3. What was tried for the freeze, and the result

| Attempt | Where | Result |
|---|---|---|
| **RX buffer bump**: `CONFIG_BT_CTLR_RX_BUFFERS=6`, `CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA=6` | `ef1e623` | **Helped a lot** — permanent wedge → auto-recovers, survival 1–2 s → 6–13 s. Did NOT eliminate the periodic 0x08 drops. |
| **Conn param relax**: `BT_LE_CONN_PARAM(6,6,0,400)` → `(6,12,0,500)` (7.5–15 ms, 5 s timeout, latency 0) | `ef1e623` | Margin only; not the cure. |
| **Logging overhead hypothesis** (the logging variant does heavy per-report logging) | — | **RULED OUT.** The DEFAULT (no-log) firmware freezes too. So logging is not the cause. |
| **Verbose BT debug build** (`CONFIG_BT_CONN_LOG_LEVEL_DBG` + `BT_HCI_CORE_LOG_LEVEL_DBG`) to capture the mechanism | — | **FAILED / DANGEROUS.** Verbose BT DBG logging slowed the BLE timing so much the mouse **could not pair at all** (0 serial output). **Do NOT use verbose BT DBG logging** — it perturbs the very timing under test. Use lightweight instrumentation (counters, INF-level one-liners) instead. |

## 4. Root-cause analysis so far (evidence-backed)

Traced into the local Zephyr / Nordic LLL
(`/Volumes/workspace/github.com/zmkfirmware/zmk/zephyr`):

- The bonded mouse has **persistent CCC**, so on reconnect it **resumes
  notifications immediately**, flooding the RX path *during* our GATT discovery
  (Report-Map read + 5 CCC subscribes). Zephyr drops pre-subscribe notifications
  at the GATT layer but still **receives them over-air**, consuming RX nodes.
- **The ACK gate:** the Nordic central only ACKs an incoming data PDU (advances
  NESN) when **≥3 RX nodes are free** — `lll_conn.c` `ull_pdu_rx_alloc_peek(3)`
  (~line 1144). Below 3, the peer's PDU is left **unacked → the peer
  retransmits**.
- **The two failure shapes** both follow from the pool draining under the flood:
  - drains to 0 free → conn events can't run (`lll_conn.c:339` needs ≥1) →
    `crc_valid` never set → supervision counts down ~timeout and fires → **0x08**.
  - stays at 1–2 free → events run (supervision never fires) but `peek(3)` keeps
    failing → nothing acked → no host buffer recycles → **self-sustaining NESN
    deadlock** = the silent permanent wedge.
- We had only bumped the **TX** buffers in the earlier -ENOMEM fix; the **RX**
  pool was depth 1 (`CONFIG_BT_BUF_ACL_RX_COUNT=0`+`_EXTRA=1`,
  `CONFIG_BT_CTLR_RX_BUFFERS=1`). Bumping both to 6 raised the threshold (hence
  the big improvement) but a **sustained** aggressive flood still occasionally
  drops the pool below the 3-node reserve → residual 0x08.
- The earlier `err 9` seen in `retest.log` is a **red herring**:
  `BT_HCI_ERR_CONN_FAIL_TO_ESTAB (0x3e)` — an RF connection-establishment failure
  that self-heals via rescan in ~125 ms (`smp.c:4715`+`conn.c:1366`). Not the
  freeze cause.

## 5. Leading hypotheses for the RESIDUAL drop (untested — start here)

1. **Thread starvation (top suspect).** RX nodes are recycled when the **BT RX
   thread** runs `notify_cb` (which is fast: memcpy + `k_msgq_put` +
   `k_work_submit`). But the heavy work (`report_work_handler`: decode + 5–6×
   `input_report_*` → ZMK input thread → USB HID) runs on the **system
   workqueue**. If the system workqueue priority is ≥ the BT RX thread priority,
   a sustained flood lets the heavy consumer **starve the BT RX thread**, so
   `notify_cb` doesn't drain RX nodes fast enough → pool < 3 → 0x08. **Check**
   `CONFIG_SYSTEM_WORKQUEUE_PRIORITY` vs `CONFIG_BT_RX_PRIO` /
   `CONFIG_BT_HCI_TX_PRIO`, and `CONFIG_BT_RX_STACK_SIZE`. **Fix idea:** move
   `report_work` to a **dedicated workqueue with priority LOWER than BT RX**, so
   decode/publish can never starve packet reception/ACKing.
2. **RX pool still too small for sustained flood.** Try `CONFIG_BT_CTLR_RX_BUFFERS`
   up to its max (18) and `CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA` higher. Cheap to test
   but feels like papering over (1) if the real issue is recycling speed.
3. **Shorten the vulnerable window** (helps the early-window variant, not the
   sustained one): cache the parsed `layout` + report handles per bonded peer and
   **skip the Report-Map long-read + re-discovery on reconnect to the same
   peer** (the report-map read is the longest discovery step, ~150–210 ms). And/or
   **drop notifications while `disc_state != DISC_IDLE`** (don't enqueue to the
   msgq during discovery) to cut consumer load during the fragile window.
4. **Mouse-side / RF.** Less likely (waiting fixes it, which points to load not
   RF), but confirm the actual report rate and whether the mouse requests a conn
   param update. Note: this is a **combined controller+host build**
   (`BT_LL_SW_SPLIT=y`, `BT_HCI_ACL_FLOW_CONTROL` not set), so host and controller
   share one buffer pool with no HCI flow control — pool depth + recycling speed
   is the whole game.

Reference for how a known-good central handles sustained notify load:
`zmk/app/src/split/bluetooth/central.c`.

## 6. How to reproduce / diagnose (procedure that works)

- **Build** (Docker, no SDK install): see `docs/roadmap.md`. `validate-both.sh`
  builds default + logging `.uf2`. For a one-off diagnostic build, a fresh build
  dir needs `west zephyr-export` first (see `/Volumes/workspace/.zmk-blehh-build/build-diag.sh`).
- **Flash** (macOS): double-tap XIAO reset → `XIAO-SENSE` mounts → `cp uf2
  /Volumes/XIAO-SENSE/CURRENT.UF2`. **The cp needs the sandbox disabled** (Bash
  tool `dangerouslyDisableSandbox: true`) or it fails "Permission denied". A
  trailing I/O error is normal.
- **Capture serial** (logging variant only): the **live port is `21101`**, the
  **ghost is `21201`** (only holds the 46-byte boot banner). zsh does NOT
  word-split `$VAR`, so name ports literally:
  ```bash
  cat /dev/cu.usbmodem21101 > /tmp/log.log 2>&1 &   # live
  # do the repro, then: pkill -f 'cat /dev/cu.usbmodem'
  sed -E 's/\x1b\[[0-9;]*m//g' /tmp/log.log | grep -ivE 'report h=|zmk_hid_mouse'
  ```
  A reconnect that reused the bond logs `secured (level 2)` with **no** `paired
  (bonded=1)` (proof of no re-pair).
- **DO NOT** add verbose BT DBG logging (breaks pairing, §3). For instrumentation,
  add INF-level one-liners or counters (e.g. log RX-pool low-water, or count
  msgq-full drops) — lightweight only.

## 7. Saved artifacts (this session's real-device logs)

Persisted (the `/tmp` copies are gone after reboot):
`/Volumes/workspace/.zmk-blehh-build/freeze-logs/`
- `verify.log` — RX-fix logging variant, immediate-move repro: 3× `reason 0x08`,
  ~6 s and ~12.7 s survival between drops, auto-recovers. The "after RX fix" state.
- `r1.log` — earlier run incl. the **permanent wedge**: log goes silent after
  `discovery done` at `00:11:15.909` (the NESN-deadlock), needed power-cycle.
- `retest.log` — pre-RX-fix; shows the `err 9`/`0x3e` red herring and the
  `-ENOMEM` discovery failure (now fixed).

Key timeline (from `verify.log`, firmware uptime):
```
00:01:05.014 discovery done -> 185 motion reports over ~10.5 s (cursor works)
00:01:17.726 disconnected reason 0x08   <- drops mid-motion, ~12.7 s in
             -> rescan -> reconnect ~2 s (frozen) -> discovery done -> repeat
```

## 8. Files / branches

- BLE central engine: `drivers/input/hog_central.c` — `device_found()` (conn
  param ~L641), `connected()`/`security_changed()`/`disconnected()`, `notify_cb`
  (BT RX context, defers to msgq), `report_work_handler` (system workqueue:
  decode + `ble_hid_host_publish`).
- Shield Kconfig (all the buffer/conn fixes): `boards/shields/ble_hid_host_receiver/ble_hid_host_receiver.conf`.
- Publish path: `drivers/input/ble_hid_host.c`.
- `main` = original bug fixed (PR #2). `fix/reconnect-rx-buffer-wedge` = RX-buffer
  mitigation (not merged; decide whether to merge as a partial improvement).
- Local Zephyr to read: `/Volumes/workspace/github.com/zmkfirmware/zmk/zephyr`
  (`subsys/bluetooth/controller/ll_sw/nordic/lll/lll_conn.c`, `.../ull_conn.c`,
  `subsys/bluetooth/host/`).

## 9. RX 12/10 experiment — RESULT: partial improvement, NOT a cure (2026-06-18 PM)

Acting on a multi-agent diagnose+verify pass (see below), we **refuted the
thread-starvation hypothesis** and bumped only the proven-mechanism knobs:
`CONFIG_BT_CTLR_RX_BUFFERS` 6→12 (controller PDU pool 9→15) and
`CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA` 6→10 (host ACL pool 7→11). Deliberately did
**not** touch the deprecated `BT_BUF_ACL_RX_COUNT` base, and did **not** add a
dedicated workqueue / notify_cb early-drop (those harden the refuted concern).

**Why the thread-starvation hypothesis is dead** (verified against the generated
`.config`): sys-wq priority = -1, BT RX WQ = `K_PRIO_COOP(8)` = -8, both
cooperative, `BT_RECV_WORKQ_BT` (BT RX on its own workqueue). -8 is strictly
higher priority than -1, so the heavy `report_work` consumer can never starve the
BT RX thread that recycles RX nodes; the depth-16 `report_msgq` (K_NO_WAIT)
fully decouples them. The real gate is purely the controller:
`lll_conn.c:1143-1150` only ACKs (nesn++) when `ull_pdu_rx_alloc_peek(3) != 0`.

**On-device retest (logging build, same immediate-aggressive-move repro). Result
matches the verifiers' prediction exactly — buffers raise BURST absorption, not
steady-state throughput:**
- Idle is now rock-solid: a ~2.5-min connection with only light/no motion never
  dropped (pre-fix it dropped every 7-15 s regardless).
- Under sustained aggressive movement it STILL drops with reason 0x08, but
  survival is ~2-4× longer at a comparable rate:

  | rate | pre-fix (RX 6/6) survival | post-fix (RX 12/10) survival |
  |---|---|---|
  | ~100/s | 1.6-2.1 s | 75/s → 8.8 s |
  | ~130/s | 2.8 s | 121/s → 4.5 s |

- `report queue full` warnings: **still ZERO** — the host consumer keeps up, so
  the limiter is NOT host-side msgq/throughput; it is the controller RX-node
  recycle rate / on-air retransmit dynamics.
- Every drop has the same shape: **motion stops first (cursor freezes), then
  ~2.5-3.0 s later the formal 0x08**. A few reconnect cycles delivered 0 reports
  before 0x08 (link wedged from the resume-CCC flood during discovery).

**Verdict: do NOT keep bumping buffers** (12→18 would only extend survival a
little). RX 12/10 is kept as a strict improvement over 6/6 (no downside, idle is
now stable) and committed as the new baseline, but the residual freeze needs a
**throughput / connection-parameter** lever, not more pool depth.

## 10. NEW lead — connection parameters are unobserved and uncontrolled

`hog_central.c:645` REQUESTS `BT_LE_CONN_PARAM(6, 12, 0, 500)` = interval
7.5-15 ms, latency 0, supervision 5 s. But there is **no `.le_param_req` and no
`.le_param_updated` callback** registered (`BT_CONN_CB_DEFINE` at :747 has only
connected/disconnected/security_changed). So:
- We **blindly accept** whatever conn-param-update the IST PRO requests after
  connect, and we **never log the negotiated values**.
- The ~2.5-3.0 s freeze→0x08 gap strongly implies the *actually negotiated*
  supervision timeout is ~2.5-3 s, i.e. the mouse **overrode our 5 s request**.
  So both the on-air PDU rate (interval) and the timeout are currently the
  mouse's choice.

**Next step (DIAGNOSE BEFORE CHANGING, needs one flash — get user OK first):**
add a one-line `LOG_INF` in a `.le_param_updated` callback to print the real
negotiated interval/latency/timeout. Then decide the fix — leading candidate is a
`.le_param_req` that clamps the request (enforce a larger supervision timeout
so a transient RX stall is ridden through, and/or a slightly longer interval to
cut the on-air PDU rate the recycle pipeline must sustain). Latency must stay 0.
Secondary candidate (candidate 3 above): cache parsed layout+handles per bonded
peer to skip the report-map read on reconnect — helps the "0-report, wedged from
reconnect" cycles, not the mid-motion drops.

**Rollback artifacts** (so the device can always be restored to this best-known
state without rebuilding): `/Volumes/workspace/.zmk-blehh-build/rollback/`
- `rx12-10_default.uf2` — production firmware (RX 12/10, no logging).
- `rx12-10_logging.uf2` — same + USB logging (currently flashed).

Retest log: `/Volumes/workspace/.zmk-blehh-build/freeze-logs/` (live capture was
`/tmp/retest-rxfix12.log`; copy in if keeping). The motion marker in the logging
build is `<dbg> zmk: zmk_hid_mouse_movement_set` (NOT the old `report h=` line).

## 11. Conn-param deep dive — RESULT: tuning can only MITIGATE, not cure (2026-06-18 PM, no flash)

A second multi-agent diagnose+verify pass, all load-bearing claims checked
against the Zephyr source, produced a counterintuitive and decisive conclusion.

- **"Raise the supervision timeout" is BACKWARDS — the comment at `hog_central.c:640-644`
  is wrong.** During the NESN stall the peer's retransmits are CRC-OK:
  `lll_conn.c:366` sets `crc_valid=1` on ANY CRC-OK PDU (even below the 3-node ACK
  reserve, where `lll_conn.c:1144-1150` skips ack/enqueue), and
  `ull_conn.c:1117-1118` then ZEROES `supervision_expire` every event while
  crc_valid is set. So the timer is continuously reset by retransmits and 0x08
  fires ONLY once CRC-valid reception fully stops — a LONGER timeout just lengthens
  the freeze.
- **conn-param tuning is a UX MITIGATION, not a cure.** The proven RX-node
  recycle-throughput limiter is unchanged; 0x08 still occurs under sustained flood
  (queue-full stays ZERO). The only helpful lever is a SHORTER supervision timeout:
  the driver auto-reconnects on 0x08, so a shorter timeout converts a long freeze
  into a quicker recover — at the cost of MORE FREQUENT (but shorter) drops.
- Mouse is **motion-bound** (one notification per delta, 18-130/s); longer interval
  cannot reduce report generation, only repack on-air into burstier events with more
  recycle time between (mild, indirect, empirical). latency must stay 0.
- A pure **central never auto-applies its own preferred params** (`conn.c:2243-2251`
  short-circuits for central) and `le_param_req` only fires IF the peer requests a
  change. So the deterministic lever is a central-forced `bt_conn_le_param_update()`,
  which works whether the mouse is passive or active.

**Honest bottom line:** the residual freeze under hard sustained motion is a link
throughput / RF ceiling (self-sustaining NESN stall + retransmit pressure; "RF
noise?" warnings observed). Firmware conn-params can make each freeze SHORTER, not
stop the drops. A real cure needs a different layer (better RF/antenna/distance, a
lower on-air error rate, or accepting the hardware ceiling).

### Refined plan IF pursuing the UX mitigation (needs flashes — get user OK first)

1. **OBSERVE build** (zero behavior change): add a `le_param_updated` LOG_INF
   (insert at `hog_central.c:746`, register `.le_param_updated` in
   `BT_CONN_CB_DEFINE` at :750). Learn the real negotiated interval/latency/timeout.
   Interpret: (a) ~2.5-3 s ⇒ mouse overrode our 5 s; (b) ~5 s/unchanged ⇒ mouse
   passive, a `le_param_req` clamp would be INERT; (c) neg-reply ⇒ a validity rule
   was hit. Single info line per (re)negotiation — NOT per-PDU, and do NOT also turn
   on BT subsystem DBG logging (it perturbs the timing under test, per §3).
2. **FIX (lead = central-forced update, "FIX-B")**: at discovery-done
   (`subscribe_pending()` :447-448) call `bt_conn_le_param_update(conn, ...)` with
   latency=0, interval 12-24 (15-30 ms), supervision **MODERATE first (~250-300 cs,
   i.e. ≈ the mouse's own value or slightly below)** — NOT 100-150 cs initially: a
   very short timeout raises drop frequency and every reconnect re-enters the fragile
   resume-CCC discovery window (observed to wedge → 0-report cycles), risking a
   reconnect-storm that feels worse than one long freeze. Tighten toward ~150 cs only
   on a later flash if no storm appears. Keep a `le_param_req` clamp as
   belt-and-suspenders. Do NOT lower the create-time `BT_LE_CONN_PARAM` at :645 (keep
   first-connect's -ENOMEM-sensitive 5-CCC discovery robust). Correct the :640-644
   comment.
3. Optional reconnect-window hardening: "drop notifications while
   `disc_state != DISC_IDLE`" (~5 lines, no cache, no handle-staleness). Apply only
   if a 0-report reconnect wedge still reproduces after the fix. **NOT Candidate-3**
   (per-bond handle cache): wrong failure mode (one-time discovery window, not the
   steady-state flood) + re-introduces the stale-handle/-ENOMEM risk class that
   `AUTO_RESUBSCRIBE=n` was added to remove — do not flash.
4. **Expectation to set with the user:** long freeze → shorter (and possibly more
   frequent) blip; the drops themselves are NOT eliminated. If the requirement is
   "no drops under hard motion," conn-params are the wrong layer.

Full design + adversarial verdicts: workflow run `wf_f0d8d56f-4b4`, output saved at
`/private/tmp/.../tasks/wyy6pfz9h.output` (FIX-A `le_param_req` body, FIX-B hook,
validity math, three-way OBSERVE interpretation guide).

## 12. SEPARATE OPEN BUG — "idle ~1 hour → dead, only a dongle re-plug revives" (reported 2026-06-18)

User-reported, distinct from the move-immediately freeze above (that one is a
sustained-FLOOD failure; this one is an IDLE/long-duration failure):
- Leave it idle ~1 hour → the cursor stops working entirely.
- A power-cycle of the MOUSE does NOT fix it; only **unplugging/replugging the
  XIAO dongle** (a full dongle reboot) revives it.

That "only a dongle reboot fixes it" signature points at the DONGLE side getting
wedged, not the mouse. Hypotheses to investigate (no device needed first; then
confirm on-device with the logging build):
1. **Mouse auto-sleep → disconnect → dongle reconnect path wedges.** Most BLE mice
   sleep after a few minutes idle and drop the link. The dongle should re-scan and
   reconnect when the mouse wakes (the directed-adv / `BT_SCAN_WITH_IDENTITY`
   path). If, after an idle disconnect, the dongle's scan stops or never restarts
   (or leaks a conn/scan resource over repeated sleep/wake cycles across an hour),
   it would go permanently dead until rebooted. Check `disconnected()` →
   `start_scan()` always re-arms, and that nothing accumulates over many
   sleep/wake cycles.
2. **USB suspend not resumed.** If the host PC USB-suspends the dongle during idle
   and ZMK's USB HID does not resume cleanly, output would die until re-enumeration
   (a re-plug). Check the `usb_hid: Device suspended/resumed` handling (these lines
   appear at boot in the logs).
3. **Scan/connection resource leak** across repeated mouse sleep/wake over an hour
   (conn refs, scan restart errors). Watch for an accumulating error in a long
   idle capture.
4. Less likely: a timer/counter effect at ~1 h.

How to capture: flash the logging combined build, leave it idle (mouse on a
desk, no motion) for ~1 h with the serial capture running, and read what the
dongle logs at the moment it dies (a disconnect with no following reconnect? a
scan error? USB suspend with no resume?). The `report queue full` / motion lines
are irrelevant here; the connect/disconnect/scanning INF lines are the signal.
**Owner asked to tackle this AFTER the move-freeze root-cause hunt.**

## 13. Root-cause hunt RESULT — soft throughput ceiling; FORCE_MD/2M/DLE ruled out (2026-06-18, no flash)

Third multi-agent hunt (run `wf_16dcf436-7b0`), every claim source-verified and
adversarially corrected. Goal: a ROOT-CAUSE fix beyond conn-param mitigation.

**Confirmed root cause:** a single-threaded RX-node RECYCLE-THROUGHPUT ceiling. A
controller RX node frees only after the host BT RX thread runs
`bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER)` (hci_driver.c:458) and the GATT path
drains the matching host ACL net_buf; there is NO HCI flow control (combined
`BT_LL_SW_SPLIT` build). When inbound PDU rate × per-PDU recycle latency exceeds
what fits between connection anchor points, the pool drains to the 3-node ACK
floor (lll_conn.c:1148) → NESN stall → 0x08. **No stock Kconfig knob raises the
single-threaded recycle rate.**

**RULED OUT (source-verified, do not pursue):**
- **2M PHY** — already enabled + auto-negotiated (`BT_CTLR_PHY_2M=y`,
  `BT_AUTO_PHY_UPDATE=y`, conn.c:1786). The recycle gate clears in CPU time (host
  net_buf turnover), not on-air time, so halving air time does nothing. (Still
  worth a `le_phy_updated` logger to CONFIRM the link is at 2M, not pinned to 1M.)
- **DLE** — already maxed (`BT_CTLR_DATA_LENGTH_MAX=69`); each tiny notification is
  its own PDU regardless.
- **Pool depth** — already bumped (controller 15 / host 11), proven not the cure.
- **FORCE_MD** — INERT here (all three verifiers). `FORCE_MD_CNT_SET()` only arms
  when the central has `trx_cnt >= BT_BUF_ACL_TX_COUNT-1` (=7) ACL TX nodes in
  flight (lll_conn.c:101-107, :1131); an RX-only HID host never reaches that → dead
  code. The event already stays open on the peer's `md` bit anyway. Do NOT set it.

**Real levers (can RAISE the ceiling / mitigate — none guaranteed):**
1. **Subscription pruning → subscribe only to the motion report (id=2).** Host-side,
   peer-INDEPENDENT cut to inbound PDU rate (rate scales time-to-freeze: 29/s→22s,
   130/s→2.8s). EFFICACY is gated on whether the other 4 reports (ids 1/4/6/9)
   actually notify DURING the flood — confirm on device (look for "ignoring report"
   DBG during motion); if they are idle, pruning won't cut the motion flood. SAFETY
   is fine: motion + buttons (incl. BTN_5/6=A/B) + wheel (tilt=C/D) are all decoded
   from id=2, so pruning to id=2 preserves every used function.
2. **Longer conn interval** (hog_central.c:654 `BT_LE_CONN_PARAM(6,12,…)` →
   `(12,24,…)`): more host wall-clock per event to recycle. PEER-VETOABLE (the IST
   PRO may pin 7.5 ms — confirm via `le_param_updated`). Keep timeout 500 in the
   throughput test (don't shrink supervision margin in the same build — a longer
   interval + shorter timeout on a marginal link could make the first post-reconnect
   burst MORE likely to trip 0x08).
3. **Shorter supervision timeout** — MITIGATION only (faster auto-reconnect, not
   fewer drops). Apply separately, after the throughput experiment.
4. **TX +8 dBm** (`BT_CTLR_TX_PWR_PLUS_8`) — marginal RF; only strengthens
   dongle→mouse ACKs, not the inbound flood. Separate A/B run.

**Honest verdict:** a soft ceiling = mouse motion rate vs single-threaded host
recycle (RF a ~25-40% amplifier). Firmware can RAISE the ceiling (pruning /
interval / TX-power+placement) and SHORTEN recovery (timeout), but cannot
guarantee elimination at maximum aggressive motion without an architectural change
(a separate RX-node drain thread, or HCI flow control / faster ACL path) that
stock Kconfig does not expose. Likely outcome: strong mitigation, possibly
freeze-free at normal motion rates, residual drops under extreme flood.

**Recommended FIRST experiment (when device returns)** — ONE build:
(1) interval `BT_LE_CONN_PARAM(12,24,0,500)`; (2) prune subscriptions to id=2;
(3) `CONFIG_BT_USER_PHY_UPDATE=y` + a `.le_phy_updated` logger; keep
`le_param_updated`. HOLD FORCE_MD (dead) and TX+8 dBm (separate A/B). Decision
rule: PHY log tx2/rx2 ⇒ 2M ruled out; longer interval accepted + pruning ⇒
time-to-freeze jumps ⇒ fixed/strongly mitigated; interval vetoed + pruning marginal
⇒ architectural ceiling ⇒ mitigation-only (shorter timeout + antenna placement).

Full output: run `wf_16dcf436-7b0` → `/private/tmp/.../tasks/wl6bscbo0.output`.

## 14. Interval experiment RESULT — VETOED by the peer; mouse params revealed (2026-06-19)

On-device test of §13's first experiment (interval widened to 15-30 ms + a PHY
logger + `CONFIG_BT_USER_PHY_UPDATE=y`), built combined with the zmk-mouse config
so A/B/C/D stayed live. Result is decisive:

- **The IST PRO renegotiates its OWN conn params on every connect; our interval
  request is REJECTED.** `le_param_updated` logged, every cycle:
  `interval 7.50 ms, latency 44, timeout 2160 ms`.
  - **Interval pinned at 7.5 ms** (our 15-30 ms vetoed) ⇒ the interval throughput
    lever is DEAD for this peer.
  - **Peripheral latency = 44** (NEW finding): the mouse may skip up to 44 events
    when idle (~337 ms effective idle wake) ⇒ power saving; irrelevant during the
    motion flood (it sends every 7.5 ms event while moving).
  - **Supervision timeout = 2160 ms** — matches the earlier ~2.5-3 s freeze→0x08
    gap measured in §9.
- Freeze got WORSE, not just persisted: the bug occurred MORE EASILY (user: "動き
  悪くなった" = it freezes more readily, NOT a laggy feel) — 6× reason 0x08 in ~45 s.
  The interval request is renegotiated by the peer on EVERY connect, and that
  per-connect renegotiation churn destabilizes the fragile reconnect window, so the
  experiment was net-NEGATIVE. ⇒ reverting the interval was the correct action,
  and the interval lever is not merely dead but harmful for this peer.
- PHY: `le_phy_updated` produced no line this capture (rapid reconnect churn
  likely truncated it before flush). 2M stays ruled-out-by-mechanism (§13).
- Action taken: reverted the create-time interval to `(6,12,0,500)` — the request
  was pointless against this peer.

**Decision-rule outcome (per §13): interval VETOED → architectural ceiling.** What
remains:
- **NEXT EXPERIMENT (untested, the last peer-independent lever): subscription
  pruning — subscribe ONLY to the motion report (id=2).** Efficacy is gated on
  whether the other 4 reports (ids 1/4/6/9) actually notify during the flood
  (temporarily set `ble_hid_host` to DBG and look for the "ignoring report" line at
  hog_central.c:164 during motion; if absent, those reports are idle and pruning
  won't help). SAFETY is fine — motion + buttons (BTN_5/6=A/B) + tilt (C/D) are all
  on id=2.
- **MITIGATION (independent of any cure): shorter supervision timeout for faster
  auto-recovery.** The mouse pins timeout=2160 ms, so the CENTRAL must force a
  shorter one via `bt_conn_le_param_update()` (the §11 "FIX-B" hook at discovery-
  done) AND the peer may re-veto — uncertain. Complement with antenna placement +
  optional TX +8 dBm.
- **HONEST END STATE:** if subscription pruning gives little, the move-freeze under
  hard aggressive motion is a hardware/architectural ceiling NOT fixable in stock
  firmware (single-threaded RX recycle, no HCI flow control — §13). The realistic
  deliverable is then "a shorter, faster-recovering blip," not "no drops."

Capture: `/tmp/exp1.log` (143 lines: the `conn params updated` / `effective params`
lines show 7.50 ms / lat 44 / 2160 ms; 6× reason 0x08).

## 15. FINAL VERDICT — subscription pruning PROVEN dead; no firmware cure; freeze is STEADY-STATE (2026-06-19)

Two decisive results this session: (1) the last untested lever is dead, proven
from existing logs with **no flash**; (2) a 16-agent adversarial hunt
(`wf_645d2eb1-4f8`) re-read the real source + logs and **overturned the
reconnect-window hypothesis** — the move-freeze is dominantly STEADY-STATE.

### 15a. Subscription pruning is INEFFECTIVE — proven from `verify.log` (no flash)

The §14 gate ("do ids 1/4/6/9 notify during the flood?") is answered by the
existing immediate-move capture, because the publish line AND the "ignoring report"
line are BOTH `LOG_DBG` and the module was at DBG (5492 motion DBG lines present):

- Published motion reports: **5492, ALL handle `h=49` (= id=2)**. No other handle
  appears anywhere in the log.
- `ignoring report` lines (= a non-pointer report id 1/4/6/9 notified): **0**.
- Buttons fired (30× `buttons=0x0001`) and rode `h=49` (id=2), confirming §13.
- The `ignoring report` source string existed at the verify.log-era commits
  (`b7f6cce`, `ef1e623`) — so 0 means "never notified", not "line absent".

⇒ The aggressive-motion flood is **100% id=2**; pruning to id=2 removes ZERO
notifications. Pruning is **DEAD for the steady-state flood.** (It could still cut
the reconnect *discovery* window from 5 reports to 1, but see 15c — the window is
not where this repro fails.)

### 15b. The reconnect-window hypothesis is WRONG — the freeze is STEADY-STATE

The hunt's log-forensics (angle E), then adversarially corrected with
full-precision timestamps on `verify.log`:

- In **all 9 reconnect cycles the GATT discovery window COMPLETES** (`secured →
  discovery done` ≈ 0.56–0.74 s every time). The window is dominated by the
  un-prunable **377 ms report-map read** (`secured 57.010 → report-map parsed
  57.387`), not the per-report round-trips — so "shrink the window" saves ~12%
  (~67 ms), not 5×.
- Every one of the 8 post-discovery `0x08` drops fires **4.6–12.7 s LATER, deep in
  steady-state motion** (e.g. done `verify.log:3368` @00:01:05.014 → 0x08
  @00:01:17.726 = 12.7 s). The link survives the whole window in every cycle and
  dies seconds afterward.
- The "wait a couple seconds → stable" clue is real but does **not** implicate the
  discovery window; it reflects that aggressive motion *immediately* after power-on
  reaches the steady-state recycle ceiling fastest.

⇒ The dominant `0x08` trigger in the only repro logs is the **steady-state recycle
ceiling**, NOT the reconnect window. Levers that only shrink the window (handle
cache / drop-during-discovery / reconnect-pruning) target the wrong layer.

### 15c. No cure — root cause re-confirmed end-to-end (source-verified)

A `PDU_RX` node frees only after the host BT RX thread runs `bt_buf_get_rx(
BT_BUF_ACL_IN, K_FOREVER)` (`hci_driver.c:458`), `hci_acl_encode` copies the PDU
into a host ACL net_buf (`:459`), then `ll_rx_mem_release` recycles the node
(`:535`). With **no HCI flow control** the Nordic central only ACKs (advances NESN)
when `ull_pdu_rx_alloc_peek(3)` succeeds — a **hard-coded literal `3`**, not a
Kconfig (`lll_conn.c:1148`). Under the id=2 flood the pool drains below 3 → peer
retransmits → supervision timeout `0x08` (or 1–2-free NESN deadlock = silent
permanent wedge, `r1.log:12528→12529`). **No stock Kconfig raises the
single-threaded recycle rate.** True fixes (a separate RX-drain thread / forking
the Nordic LLL reserve, or peer cooperation) are out of scope for stock firmware +
a 3rd-party mouse. Adversarially-refuted non-fixes: handle caches (A1–A4: removing
discovery removes zero over-air notifications), deeper pools (proven-dead #1),
HCI flow control (inert/harmful in a combined `BT_LL_SW_SPLIT` build), all ZMK
split-central levers (D: already maxed/equalled for us).

### 15d. Ranked mitigations (none are cures; honest efficacy)

1. **SHIP RX 12/10** (already on this branch, UNMERGED) — strict improvement (idle
   rock-solid, permanent wedge gone). Strip the working-tree diagnostics first
   (le_param/phy loggers, `ZMK_LOGGING_MINIMAL`, `USER_PHY_UPDATE`). No new device
   work needed beyond what's proven. **Do this regardless.**
2. **Free, no-flash RF complements (try FIRST):** shorten dongle↔trackball
   distance, cut 2.4 GHz interference (USB3 hubs / Wi-Fi), reposition antenna; if
   the board exposes it, TX **+8 dBm**. These raise retransmit-success headroom
   before supervision expiry and cannot destabilize param negotiation.
3. **FIX-B — central-forced shorter supervision timeout.** Full executable plan:
   **`docs/plan-fix-b-supervision-timeout.md`** (source-verified + adversarially
   red-teamed). Summary: a delayable work fires ~700 ms after discovery-done and
   calls `bt_conn_le_param_update(default_conn, …)` to lower only the timeout. As
   CENTRAL this drives a non-vetoable `HCI LE Connection Update` (≠ the §14 L2CAP
   path the peer overrode). Validity rule (`hci_core.c:1810`) is
   `timeout*4 > (1+latency)*interval_max`, so **pin interval_max=6** and start at
   `BT_LE_CONN_PARAM_INIT(6,6,44,120)` = 7.5 ms / latency 44 / **timeout 1200 ms**
   (floor = 68 = 680 ms). NOTE: the earlier `(6,12,44,100)` value was INVALID
   (`400 ≯ 540`). Converts a long freeze into a shorter blip — **does NOT remove
   drops**; risk it feels worse (more frequent drops / reconnect storm). Issue ONCE,
   well after discovery-done; optional Phase-2 `le_param_req` clamp if the peer
   re-requests 2160 ms. Try only if RF (#2) is insufficient.
4. **C7 / B3 (DEPRIORITIZED):** C7 = `if (disc_state != DISC_IDLE) return
   BT_GATT_ITER_CONTINUE;` early-return in `notify_cb` (~`hog_central.c:178`); B3 =
   cache the parsed `layout` addr-keyed (same-boot only) and skip the report-map
   read on reconnect (`hog_central.c:234/374/519`, NEVER cache handles, restore
   `layout_valid` after the L523/L743 clears). Both only shrink the reconnect
   window — which 15b shows is not where this repro fails — and RX 12/10 already
   killed the permanent wedge, so their marginal value is low. Implement only as
   belt-and-suspenders / to A/B-confirm 15b on device.

**Bottom line:** the move-freeze under hard sustained aggressive motion is a hard
architectural ceiling, **weakly** (not strongly) mitigable. Deliverable = "a
shorter, faster-recovering blip + a rock-solid idle link", not "no drops". Ship
RX 12/10, try RF/distance first, layer FIX-B only if needed.

Full hunt output: `wf_645d2eb1-4f8` →
`/private/tmp/claude-501/.../tasks/wk584fdux.output` (5-angle dossier + adversarial
verdicts). Key file:lines: root cause `lll_conn.c:1148`; recycle chain
`hci_driver.c:458/459/535`; FIX-B hook `hog_central.c:448-459`; param-veto context
`hog_central.c:649-656`; forensic timeline `verify.log:3368` vs `:3369/:4368`.
## 16. FIX-B tried ON-DEVICE — DEAD END (2026-06-19)

The §15d / plan-doc mitigation (`docs/plan-fix-b-supervision-timeout.md`) was
implemented and flashed to the device in two phases. Result: **conn-param
mitigation does not deliver a usable improvement on the IST PRO — confirmed on
hardware.** Code preserved on branch `experiment/fix-b-supervision-timeout` (NOT
merged).

**Phase 1 — central-forced `bt_conn_le_param_update(1200 ms)` at discovery-done +700 ms:**
- The central update is **non-vetoable and APPLIES** (proof: `conn params updated:
  ... timeout 1200 ms` logged) — refutes the earlier "peer may reject it" worry.
- BUT the IST PRO **re-requests its 2160 ms ~400 ms later**, which Zephyr (no
  `le_param_req`) auto-accepts → reverts. Net: timeout is 2160 ms almost always →
  no improvement. (Capture: `/tmp/fixb.log` — per cycle: 2160 → 1200 → 2160.)

**Phase 2 — add `le_param_req` that clamps the peer's timeout down to 1200 ms:**
- The clamp **holds** the applied timeout at 1200 ms (never reverts to 2160 ms).
- BUT the peer re-requests 2160 ms **relentlessly, every ~400 ms** → the clamp fired
  **435 times in one capture** = constant LLCP connection-update churn (~11 % of
  airtime at 7.5 ms interval).
- 0x08 drops became **MORE frequent (~5-8 s apart)** than the RX-12/10 baseline
  (~6-13 s) — exactly the §11 prediction "shorter timeout ⇒ shorter but MORE
  FREQUENT drops." Each recovery is shorter (~1.9 s incl. reconnect) but they happen
  more often, plus the churn. **Net user experience: not fixed, arguably worse.**
  (Capture: `/tmp/fixb2.log`.)

**VERDICT (now confirmed on hardware, not just by analysis): every firmware lever —
including the last-resort conn-param mitigation — is exhausted. The move-freeze
under hard aggressive motion is a hard architectural ceiling (single-threaded RX
recycle gated by `ull_pdu_rx_alloc_peek(3)`, no HCI flow control; the IST PRO
aggressively maintains its own 2160 ms and fights any change). The only remaining
lever is non-firmware: RF link quality (dongle↔mouse distance, USB3 / 2.4 GHz
interference, antenna placement, TX +8 dBm).** Firmware best-known state =
RX 12/10 (idle rock-solid, permanent wedge gone); ship that, accept the residual.

Rollback firmware (restore the clean pre-FIX-B device):
`/Volumes/workspace/.zmk-blehh-build/rollback/abcd_rx12_reverted_logging.uf2`
(RX 12/10 + §14 interval revert + logging, no FIX-B). FIX-B builds kept as
`fixb_1200ms_logging.uf2` / `fixb_phase2_clamp_logging.uf2` (do NOT use for daily).

## 17. RX buffers MAXED to 18 — tried on-device, NO benefit (2026-06-19)

With RF/distance confirmed already optimal by the user and FIX-B failed (§16), the
last free firmware lever was tested: **max the RX pool** —
`CONFIG_BT_CTLR_RX_BUFFERS` 12→**18** (the max; controller PDU_RX = 3+18 = 21) and
`CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA` 10→**20** (host ACL = 1+20 = 21, matched so the
host never bottlenecks the controller recycle). Built, **`.config` verified 18/20**,
flashed (logging combo), and run against the immediate-aggressive-move repro.

**Result — no meaningful change from the RX 12/10 baseline:**
- 5× `reason 0x08` in ~27 s; survival between drops 4.1 / 6.5 / 4.1 / 9.6 s;
  intervals ~5-10 s — statistically the same as RX 12/10's ~6-13 s (§9). The peer's
  timeout stayed its own 2160 ms (no FIX-B; `FIX-B`/`clamping` lines = 0, confirming
  a clean RX-only build).

**Confirms §9/§13 exactly:** deeper pools raise BURST absorption only marginally and
do **not** move the freeze; the steady-state RX-node recycle ceiling dominates even
the immediate-burst case. **DECISION: keep RX 12/10** (the documented baseline). RX
18/20 was tested with no measured benefit (and no downside), so there is no reason to
deviate; `.conf` stays 12/10 and the device was flashed back to RX 12/10
(`abcd_rx12_reverted_logging.uf2`). 12/10 and 18/20 are interchangeable for this
freeze; the RX-18 build is kept as `rollback/rx18_logging.uf2`.

**FINAL on-device tally — every firmware lever is now exhausted on hardware:** RX
depth (incl. max 18), thread-starvation (refuted), 2M/DLE/FORCE_MD, conn-interval
(peer-vetoed), subscription pruning (flood is 100% id=2), FIX-B conn-param timeout
(peer fights, §16). **The move-freeze under hard aggressive motion is a hardware
ceiling — not fixable in stock firmware, period.** Only mitigations: behavioral
(wait ~2-3 s after power-on before flailing; keep the link up — don't power-cycle the
mouse often) + RF (already optimal here). Shipped firmware = RX 12/10 (the documented baseline; RX 18/20 is interchangeable).
