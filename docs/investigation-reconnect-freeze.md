# Investigation handoff — "move immediately after power-on → cursor freezes"

> Standalone handoff so this can be picked up cold in a fresh session. The
> ORIGINAL reported bug (re-pairing on every reconnect) is **fixed and merged**.
> This doc is only about the **residual** freeze that remains.

**Status:** OPEN. Partially mitigated (no longer a permanent wedge), not eliminated.
**Branch:** `fix/reconnect-rx-buffer-wedge` (commit `ef1e623` + an uncommitted
`LOG_INF`→`LOG_DBG` hygiene change in `hog_central.c`). Not yet merged.
**Date:** 2026-06-18.

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
