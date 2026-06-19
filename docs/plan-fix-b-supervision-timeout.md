# Plan — FIX-B: central-forced shorter supervision timeout (move-freeze MITIGATION)

> **Self-contained handoff for a cold execution session.** This is a PLAN only —
> no code is committed yet. The move-freeze itself is a confirmed hard ceiling with
> NO firmware cure (see `investigation-reconnect-freeze.md` §15). FIX-B does **not**
> stop the drops; it makes each freeze **shorter and faster-recovering** by lowering
> the supervision timeout so the already-working auto-reconnect fires sooner.
> **Status:** ❌ IMPLEMENTED + FLASHED + TESTED ON-DEVICE → **DEAD END** (2026-06-19).
> Both phases work as designed (Phase-1 central update applies non-vetoed; Phase-2
> clamp holds 1200 ms), but the IST PRO re-requests 2160 ms every ~400 ms (435 clamps
> in one capture = churn) and drops became MORE frequent (~5-8 s vs ~6-13 s baseline).
> Not fixed, arguably worse. Code preserved on branch `experiment/fix-b-supervision-
> timeout` (NOT merged). See investigation §16. Plan kept for the record / a possible
> future non-IST-PRO peer. **Date:** 2026-06-19.

---

## 0. TL;DR for the executor

1. Add ~25 lines to `drivers/input/hog_central.c`: a delayable work that, ~700 ms
   after `discovery done`, calls `bt_conn_le_param_update()` to request a shorter
   supervision **timeout** (interval/latency unchanged). Central-initiated update is
   **non-vetoable** by the mouse (verified, §2).
2. Build the **logging** combined firmware, flash, run the aggressive-move repro.
3. Read the serial log: confirm `conn params updated: ... timeout 1200 ms` appears
   (proof it took). Measure the freeze→recover gap (was ~2.5–3.0 s).
4. Apply the **decision rules** (§6): success → optionally tighten; peer reverts →
   add Phase-2 `le_param_req` clamp; reconnect-storm/worse → back off or abandon.
5. **Get user OK before flashing.** Respond in Japanese. macOS `cp` to the
   bootloader needs `dangerouslyDisableSandbox`.

**Honest expectation:** best case = freeze blip drops from ~2160 ms to ~1200 ms
(and tunable toward ~800 ms). Drops are NOT eliminated. Real risk it feels *worse*
(more frequent drops / reconnect storm) — start conservative, watch for it.

---

## 1. Goal & why FIX-B is different from the §14 failure

- The IST PRO negotiates **supervision timeout 2160 ms** every connect (§14). On a
  0x08 freeze the driver already auto-reconnects, but the user waits the full
  ~2.5–3.0 s (timeout + reconnect) before the cursor returns.
- **§14 changed the create-time `BT_LE_CONN_PARAM`** (the params proposed at
  `bt_conn_le_create`). The mouse then sent its OWN L2CAP param-update request after
  connect, which Zephyr auto-accepted → our request was effectively ignored, and the
  per-connect renegotiation churn destabilized the fragile reconnect window → it got
  WORSE. That lever is dead.
- **FIX-B is a CENTRAL-forced post-discovery update**, a different mechanism
  (verified §2): the central drives `HCI LE Connection Update`, which the peripheral
  **cannot veto**. It is issued **once, ~700 ms after discovery-done**, when the link
  is already stable — deliberately NOT in the fragile post-secured window that §14
  churn hit.

## 2. Source-verified facts this plan rests on

All in the local Zephyr tree
`/Volumes/workspace/github.com/zmkfirmware/zmk/zephyr`:

- **Central update is non-vetoable.** `bt_conn_le_param_update()` (`host/conn.c`)
  → for `role == CENTRAL` calls `send_conn_le_param_update()` → which, because
  `conn->role == BT_HCI_ROLE_CENTRAL`, calls `bt_conn_le_conn_update()` →
  `HCI LE Connection Update` (`BT_HCI_OP_LE_CONN_UPDATE`). It does **not** take the
  `bt_l2cap_update_conn_param` (request/accept) path the peripheral used in §14. The
  peripheral must apply a central-initiated connection update.
- **Validity rule** (`host/hci_core.c:1810` `bt_le_conn_params_valid`), enforced
  before send (returns `-EINVAL` if violated):
  ```
  6 <= interval_min <= interval_max <= 3200
  latency <= 499
  10 <= timeout <= 3200
  timeout * 4  >  (1 + latency) * interval_max      <-- the binding constraint
  ```
  (interval unit 1.25 ms, timeout unit 10 ms.)
- **The prior synthesis's value `BT_LE_CONN_PARAM(6,12,44,100)` is INVALID** and
  would be rejected: `100*4 = 400` is NOT `> (1+44)*12 = 540`. **Must pin
  interval_max = 6** (7.5 ms, matching the peer) so a short timeout stays legal with
  latency 44.
- **`default_conn`** (`hog_central.c:57`) is the live conn; populated as the
  out-param of `bt_conn_le_create` in `device_found` (`:658`), unref'd + NULL'd in
  `disconnected()` (`:733-734`), which runs on the BT RX context. The central update
  runs a synchronous HCI command on the system workqueue, so it briefly (~one conn
  interval) serializes against `report_work` on that same WQ — negligible. `<zephyr/kernel.h>` and `<zephyr/bluetooth/conn.h>` are already
  included. A `le_param_updated` logger already exists (`:763`) and will report the
  negotiated timeout — that is the verification signal.

## 3. Parameter math + the tuning ladder

Keep **latency = 44** (the mouse's power-saving choice; harmless during motion since
it sends every event while moving) and **interval pinned to 7.5 ms** (units 6).
Then `timeout * 4 > (1+44)*6 = 270` → `timeout > 67.5` → **hard floor = 68 (680 ms)**.

| step | `timeout` units | ms | vs peer 2160 ms | when to use |
|---|---|---|---|---|
| **start** | **120** | **1200** | −45 % | first real test — meaningful but not extreme |
| tighten | 100 | 1000 | −54 % | if start is stable & you want shorter |
| aggressive | 80 | 800 | −63 % | only if no storm at 1000 ms |
| **floor** | 68 | 680 | −69 % | do **not** go below (invalid params) |
| back off | 180 | 1800 | −17 % | if 1200 ms causes a reconnect storm |

Start at **120 (1200 ms)**. Shorter timeout = faster recovery **but** more frequent
drops on brief RF glitches and more reconnect cycles (each re-enters the discovery
window). Tune empirically; never below 68.

## 4. The change — Phase 1 (do this first)

All in `drivers/input/hog_central.c`.

**(a)** Near the other file-scope statics (after `default_conn`, ~`:57`):

```c
/* FIX-B (docs/plan-fix-b-supervision-timeout.md): the move-freeze is a hard
 * ceiling; we cannot stop the 0x08 drops, but we can make each one recover faster
 * by lowering the supervision timeout below the peer's 2160 ms. A CENTRAL-forced LE
 * Connection Update is non-vetoable by the peripheral (conn.c send_conn_le_param_
 * update: role==CENTRAL -> bt_conn_le_conn_update). Issued ONCE, ~700 ms AFTER
 * discovery-done, so it never lands in the fragile post-secured window that the
 * §14 create-time-param churn destabilized. interval pinned 7.5 ms + latency 44
 * (the peer's own values) so only the timeout changes. Validity (hci_core.c:1810):
 * timeout*4 (480) > (1+44)*6 (270). Floor for these params = 68 (680 ms). */
#define FIXB_DELAY_MS 700
static const struct bt_le_conn_param fixb_param =
	BT_LE_CONN_PARAM_INIT(6, 6, 44, 120); /* 7.5 ms, latency 44, timeout 1200 ms */

static void fixb_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	/* Safety (verified against the build .config): the system workqueue is
	 * COOPERATIVE (CONFIG_SYSTEM_WORKQUEUE_PRIORITY=-1) and BT RX is preemptible
	 * (CONFIG_BT_RX_PRIO=8), so this read-then-ref runs atomically w.r.t.
	 * disconnected() (which clears default_conn on the BT RX context) -- BT RX
	 * cannot interleave between the read and the ref. We take our OWN ref before
	 * the first blocking call below, so if the link drops mid-update the object
	 * stays alive and bt_conn_le_param_update merely returns an error. */
	struct bt_conn *conn = default_conn ? bt_conn_ref(default_conn) : NULL;

	if (!conn) {
		return;
	}
	int err = bt_conn_le_param_update(conn, &fixb_param);

	LOG_INF("FIX-B: requested supervision timeout %u ms (err %d)",
		fixb_param.timeout * 10U, err);
	bt_conn_unref(conn);
}
static K_WORK_DELAYABLE_DEFINE(fixb_work, fixb_work_handler);
```

**(b)** At **discovery-done**, inside `subscribe_pending()` in the
`if (pending_idx >= pending_count)` branch (right after the existing
`LOG_INF("discovery done...")` / effective-params log, ~`:454`):

```c
		/* FIX-B: once the link is settled, force a shorter supervision timeout. */
		k_work_schedule(&fixb_work, K_MSEC(FIXB_DELAY_MS));
```

**(c)** In `disconnected()`, **before** `bt_conn_unref(default_conn)` (~`:733`), to
cancel a still-PENDING (not-yet-started) update for this connection:

```c
	k_work_cancel_delayable(&fixb_work);
```

> Note: `k_work_cancel_delayable` does **not** stop a handler that is already
> running — it only deschedules a pending one. The already-running case is made safe
> by the handler's own `bt_conn_ref` + cooperative system-WQ priority (see the
> handler comment), not by this cancel. Keep the cancel anyway: it cleanly avoids a
> stale fire on the next connection.

That is the entire Phase 1. ~25 lines, no Kconfig change, no new buffers, no handle
cache, latency stays the peer's, single update per connection.

## 5. The change — Phase 2 (ONLY if the peer reverts the timeout)

If the serial log shows the timeout taking (1200 ms) but then the mouse **re-requests
2160 ms** (a later `conn params updated: ... timeout 2160 ms` with no disconnect),
Zephyr auto-accepted the peer's L2CAP request because no `le_param_req` is registered.
Add a clamp that rejects the peer's attempt to widen the timeout back:

```c
static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	ARG_UNUSED(conn);
	/* Reject the peer's bid to restore its long 2160 ms timeout; accept its
	 * interval/latency. Keeps FIX-B's short timeout sticky. */
	if (param->timeout > 150 /* 1500 ms */) {
		LOG_INF("FIX-B: rejecting peer timeout %u ms", param->timeout * 10U);
		return false;
	}
	return true;
}
```

Register it in `BT_CONN_CB_DEFINE(conn_callbacks)` (~`:782`) next to `.le_param_updated`:
```c
	.le_param_req = le_param_req,
```
**Caveat:** if the peer keeps re-requesting after each rejection, that is churn —
watch the log; if it storms, prefer abandoning FIX-B over fighting the peer.

## 6. Build → flash → repro → decision rules

### Build (Docker, no SDK install) — LOGGING variant (needed for verification)
In `/Volumes/workspace/.zmk-blehh-build`, Docker `zmkfirmware/zmk-build-arm:stable`,
combined with the zmk-mouse config so A/B/C/D stay live:
```
-DZMK_CONFIG=<zmk-mouse>/config -DZMK_EXTRA_MODULES=<zmk-ble-hid-host>
-DSHIELD=ble_hid_host_receiver -DCONFIG_ZMK_USB_LOGGING=y
```
A fresh build dir needs `west zephyr-export` first — see
`/Volumes/workspace/.zmk-blehh-build/build-diag.sh`. The shield `.conf` already has
`CONFIG_ZMK_LOGGING_MINIMAL=y` so the sparse INF lines survive the motion DBG flood.

### Flash (macOS)
Double-tap XIAO reset → `XIAO-SENSE` mounts → copy the `.uf2`:
```
cp <build>/zephyr/zmk.uf2 /Volumes/XIAO-SENSE/CURRENT.UF2
```
**Top-level Bash + `dangerouslyDisableSandbox: true`** (else "Permission denied");
retry on failure; a trailing I/O error is normal.

### Capture serial
Live port is `cu.usbmodem21101` (ghost `21201` only holds the boot banner):
```bash
cat /dev/cu.usbmodem21101 > /tmp/fixb.log 2>&1 &
# power-cycle the trackball; move immediately+aggressively ~30 s; then 60 s sustained
pkill -f 'cat /dev/cu.usbmodem'
sed -E 's/\x1b\[[0-9;]*m//g' /tmp/fixb.log | grep -ivE 'report h=|zmk_hid_mouse'
```
**Do NOT enable verbose BT subsystem DBG logging** — it perturbs BLE timing so badly
the mouse can't pair (§3). INF one-liners only (FIX-B's logs are INF).

### Verification signals (in order)
1. `discovery done: subscribed to 5 report(s)` — normal.
2. ~700 ms later: `FIX-B: requested supervision timeout 1200 ms (err 0)`. (`err` ≠ 0
   has TWO causes: (i) the link dropped during the 700 ms window — BENIGN, the next
   reconnect re-fires; (ii) a genuine `-EINVAL` from invalid params — recheck §3
   math. Distinguish by whether a `disconnected` line precedes it.)
3. Then `conn params updated: interval 7.50 ms, latency 44, timeout 1200 ms` — **this
   is the proof the central update TOOK.** (Still 2160 ms ⇒ didn't take; reverts to
   2160 ms later ⇒ peer re-requested → Phase 2.)
4. On a freeze: measure the gap from the last motion line to the next `connected`
   /`discovery done`. Was ~2.5–3.0 s; target ~1.2–1.5 s.

### Decision rules
- **A — timeout shows 1200 ms & stays, freeze shorter, no storm** → SUCCESS. Optionally
  tighten (120→100→80, §3) on a follow-up flash; stop if a storm appears.
- **B — timeout reverts to 2160 ms** (peer re-requests) → apply **Phase 2** clamp, reflash.
- **C — reconnect storm / 0-report cycles / feels worse than one long freeze** → back off
  to 180 (1800 ms); if still bad, **abandon FIX-B** — the residual is then RF-only
  (distance / USB3 avoidance / antenna / TX +8 dBm) and the honest verdict stands:
  the move-freeze is not firmware-fixable.
- **D — `err != 0` at step 2** → params invalid (re-check the §3 formula) or called too
  early; confirm it fires after `discovery done`.

### Rollback
Pre-built `.uf2`s in `/Volumes/workspace/.zmk-blehh-build/rollback/`:
- `abcd_rx12_reverted_logging.uf2` — current best-known logging state (no FIX-B).
- `rx12-10_m4_default.uf2` — clean production (no logging, no FIX-B).
Flash either to restore instantly without rebuilding.

## 7. Open decisions for the executor / user
- **Worth it at all?** If RF/distance (free, no flash — see §15d of the investigation
  doc) already makes the freeze acceptable, FIX-B may not be needed. Confirm with the
  user before flashing.
- **Where to land it if it works:** likely a small commit on
  `fix/reconnect-rx-buffer-wedge` (or its own branch), PR to `main`. Push needs user
  approval (this repo is not in the push-pre-authorized set).
- **Keep or strip the OBSERVE loggers** (`le_param_updated`/`le_phy_updated`/effective-
  params) when productionizing — they are useful for the separate §12 "idle ~1 h → dead"
  investigation, so keeping them is reasonable.

## 8. Definition of done
FIX-B is "done" when EITHER: (a) the serial log proves a shorter timeout took and
held, the on-device freeze→recover gap is measurably shorter with no reconnect storm,
and the change is committed/PR'd with user approval; OR (b) it is proven on-device to
not help / make things worse, that result is recorded here, and the code is reverted —
**either way the outcome is written down, not left implicit.**
