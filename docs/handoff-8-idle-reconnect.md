# Handoff — #8 idle-recovery (mouse sleeps → dongle won't recover)

> **Status (2026-06-21): Step 1 DONE — diagnostic firmware BUILT, committed, and
> flashable on demand. NOT yet flashed (user chose to wait). Steps 2–4 pending.**
> - Diagnostic edit committed on `feat/reconnect-diagnostics` as `581d477`
>   (the `ADV-seen` log in `device_found`).
> - Logging firmware built & verified: `canon/firmware/ble_hid_host_receiver-logging.uf2`
>   (sha256 `d1b3c1364d1a97cb18f8b13e84c4c045367021d883b3139113db5b3a1437fc81`).
>   The `ADV-seen %s type=%u …` string is confirmed baked into `zmk.elf`.
> - **One-shot flasher: `~/bin/flash-ist-logging.sh`** — double-tap the dongle
>   reset, run it (waits for the XIAO mount, sanity-checks the board, copies the
>   uf2). Or tell Claude "焼いて".
> - This build is off `main` (no cached-resubscribe) → flashing it also clears the
>   Mode A zombie regression currently on the device.
> - The dongle currently STILL has the REGRESSION flashed (not replaced until the
>   flash happens). **Nothing is pushed or merged.** Read this top-to-bottom first.

---

## TL;DR (the corrected picture)

- The bug is **dongle-side and fixable** (not the mouse). Proven by a clean A/B:
  the user has **two physical IST PRO units** — Mouse A on the dongle (fails),
  Mouse B on the Mac via native BT (recovers on motion). Same model, host is the
  only variable.
- There are **two distinct failure modes**, do not conflate them:
  - **A — Zombie connection (a regression I introduced).** The `feat/cached-reconnect-resubscribe`
    build (commit `9cbc3ec`, currently flashed) reconnects via `bt_gatt_resubscribe`
    without re-writing the CCC. After a *deep* sleep the mouse drops its runtime
    CCC, so notifications never resume → dongle shows `conn=1 sub=5` but the cursor
    is **dead**, and it won't re-scan → only a **dongle re-plug** recovers (reboot
    clears the RAM cache → full discovery re-writes the CCC). This is what "ドングル
    だけ OFF/ON で復活" was. **Fix: drop the cached-resubscribe (i.e. ship from `main`,
    which never had it).**
  - **B — The original #8: motion doesn't re-attach.** Pre-existed my change. After
    sleep, moving Mouse A did not make the dongle reconnect (a 2-min firm-move test
    produced zero `target found`), yet the Mac reconnects the same-model mouse on
    motion. **This is the real open question.**
- **My first root-cause theory was WRONG** and has been retracted (see Evidence).
  The accept-list / `bt_conn_le_create_auto` rewrite is **NOT** the right first
  move. ZMK's own split central proves a plain scanner handles this exact case.

---

## THE PLAN for the next session (diagnostic-first)

We do **not** yet know *why* (B) happens — three candidates remain (race vs
address-mismatch vs active-scan). Resolve it with data before changing behavior.

### Step 1 — build a diagnostic firmware from `main`

Branch off `main` (which has **no** cached-resubscribe → also kills Failure Mode A).
A branch `feat/reconnect-diagnostics` was already created off `main` for this.

Add this at the **top of `device_found()`** in `drivers/input/hog_central.c`, right
after the `if (default_conn) { return; }` guard:

```c
	/* #8 DIAGNOSTIC (remove after diagnosis): while scanning, log every advert
	 * from a BONDED peer OR any DIRECTED advert (aimed at us). This reveals
	 * whether Mouse A's motion-wake adverts actually reach us and whether the
	 * source address matches the bond. Targeted (bonded || directed) so it can
	 * NOT flood — a flood of INF logs perturbs BLE timing (hard lesson). */
	if (addr_is_bonded(addr) || type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		char ds[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(addr, ds, sizeof(ds));
		LOG_INF("ADV-seen %s type=%u rssi=%d bonded=%d", ds, type, rssi,
			addr_is_bonded(addr) ? 1 : 0);
	}
```

`addr_is_bonded()` is defined later in the file but is already forward-usable in
this TU (it's a static at file scope above `device_found`). Adv `type` values:
`0=ADV_IND, 1=ADV_DIRECT_IND, 2=ADV_SCAN_IND, 3=ADV_NONCONN_IND, 4=SCAN_RSP, 5=EXT_ADV`.

### Step 2 — flash (logging) and read the data

Flash the logging variant (user can drive with Mouse B on the Mac). Let **Mouse A**
sleep (wait for `disconnected ... reason 0x13` + `conn=0` in the log), then move it
firmly for ~10 s. Read `~/bin/zmk-log around "HH:MM"`:

| Observation during motion | Meaning | Next fix |
|---|---|---|
| `ADV-seen ... type=1 ... bonded=1` | directed advert arrives **and** addr matches | bug is the scan-stop→create race or the connect path → adopt ZMK directed short-circuit + try **passive** scan |
| `ADV-seen ... type=1 ... bonded=0` | directed advert arrives but **addr mismatch** | the real bug is address matching — **accept-list would NOT help either** (also keyed on the bonded addr). Fix addressing/identity |
| nothing during motion | controller isn't delivering Mouse A's advert (mouse not advertising to dongle, or filtered) | switch reconnect scan to **passive**; if still nothing, escalate (BLE sniffer / accept-list initiator) |

### Step 3 — apply the precise fix (data-driven)

Most-likely fix (mirrors ZMK split central, the proven in-stack template):
1. In `device_found`, on `type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND` from a bonded
   addr, **short-circuit to connect** — skip `bt_data_parse`/`ai.match`/the
   `last_failed` cooldown (a directed advert has empty AD; gating on it is the
   suspect). Mirror `zmk/app/src/split/bluetooth/central.c:870-883`.
2. Use **PASSIVE** scan while bonded (active buys nothing for non-scannable
   directed adverts and the SCAN_REQ churn can lower catch odds). Keep ACTIVE only
   for first-time pairing (name discovery). Ref `central.c:911-912`.
3. Keep the existing A1 scan-retry + heartbeat (already as robust as ZMK).
4. Only if Step 2 shows the controller never delivers the directed advert despite
   passive scan: escalate to `bt_conn_le_create_auto()` + `CONFIG_BT_FILTER_ACCEPT_LIST=y`
   (Plan B — a controller background initiator removes the stop→create dead window;
   but it does **not** fix an address-mismatch).

### Step 4 — verify on-device, then ship

Flash, let Mouse A sleep, **move → cursor must come back with no OFF/ON**. Only
then claim it fixed. Then build a non-logging prod variant, PR to module `main`
(push needs the owner's approval), and have canon follow `main`.

---

## Evidence (why the theory changed) — details

<details>
<summary>Failure Mode A — the zombie is the cached-resubscribe (my regression)</summary>

- `feat/cached-reconnect-resubscribe` added a RAM cache of the GATT layout + a
  fast reconnect via `bt_gatt_resubscribe()` (no CCC write) to cut ATT load in the
  post-reconnect window.
- It works after a brief `0x08` drop (mouse never deep-slept, CCC still live — proven
  by the 0x08 churn delivering motion yesterday 15:24–15:31).
- It FAILS after a deep `0x13` sleep: the mouse cleared its runtime CCC, and
  `bt_gatt_resubscribe` does not re-enable it → `conn=1 sub=5` but zero notifications
  = dead cursor. Log signature: `0x13` → ~minutes `conn=0` → `target found` →
  `fast reconnect: 5 cached sub(s) re-armed` → stays `conn=1` but user reports dead.
- `main` (b42b608) does a full GATT discovery every reconnect → always re-writes the
  CCC → no zombie. So **shipping from `main` removes Mode A for free.**
- (I retracted the zombie theory once on 2026-06-20 by misreading a `conn=1` that was
  actually a healthy post-OFF/ON connection. The two-mice setup made it unambiguous.)
</details>

<details>
<summary>Failure Mode B — the scanner CAN see directed adverts (theory corrected)</summary>

Two independent ultra workflows (root-cause verification + ZMK-reference extraction)
converged on correcting the original "scanner structurally misses HDC ADV_DIRECT_IND"
theory:

- **Zephyr source (root-cause run):** the only place a directed report is dropped is
  `subsys/bluetooth/host/scan.c:631-636`, and that branch requires
  `!CONFIG_BT_SCAN_WITH_IDENTITY`. The shield sets `CONFIG_BT_SCAN_WITH_IDENTITY=y`,
  so the drop never fires → directed reports reach `device_found`. `id.c:1840-1856`
  makes the scanner use our **identity** address so the controller matches a directed
  advert whose `TargetA == our identity`. Mouse A's `CD:CF:BF:79:68:00` is a **static
  random identity** (stable across days), so **no privacy/RPA handling is needed**.
- **ZMK split central (reference run):** ZMK reconnects to bonded peripherals that
  wake with the **same** high-duty `ADV_DIRECT_IND`, using a **passive** scanner whose
  callback explicitly connects on the directed type — **no accept-list, no
  `bt_conn_le_create_auto`, no privacy**, only `BT_SCAN_WITH_IDENTITY`. Refs:
  `zmk/app/src/split/bluetooth/central.c:870-883` (directed branch), `:911-912`
  (PASSIVE scan), `peripheral.c:59-72` (bonded → `BT_LE_ADV_CONN_DIR[_LOW_DUTY]`).
- **Our `device_found` today** accepts `ADV_DIRECT_IND` in its type filter but then
  runs `bt_data_parse` (empty for directed) and gates on `bonded || ai.match`; since
  `ai.match` is false for an empty AD, the connect hinges entirely on
  `addr_is_bonded(addr)`. ZMK never takes that risk. The `target` log line is AFTER
  this gate, so a silent drop here looks identical to "saw nothing."
- **Remaining candidates for (B):** (1) scan-stop→create dead window losing the
  ~1.28 s HDC burst; (2) `addr_is_bonded` not matching the directed advert's AdvA;
  (3) ACTIVE scan reducing catch odds. The diagnostic above tells us which.
- **Key caveat:** an accept-list initiator is keyed on the **same** bonded address,
  so if (B) is an address-match problem, accept-list does **not** fix it.

User's open PRs reviewed (ZMK #3377/#3385 = bond-clear on `PIN_OR_KEY_MISSING`;
Cyboard zmk-keyboards #7 = devicetree only) — none are the reconnect fix, but
#3377/#3385's `bt_unpair`+`bt_conn_disconnect(... AUTH_FAIL)` in `security_changed`
is worth mirroring as insurance against a key-mismatch loop.
</details>

---

## Build / flash mechanics (so this isn't re-derived)

- **Build:** `/Volumes/workspace/github.com/akira-toriyama/canon/scripts/build-zmk.sh ist --logging`
  — run from the **canon SOURCE repo**, NOT the cache copy at
  `~/.cache/zmk-canon/cfgrepo/scripts/...` (that dies because `REPO==CFG` makes the
  `zephyr/module.yml` self-copy fail under `set -e`).
- **To build local module edits:** the canon build uses the west-managed module
  checkout at `~/.cache/zmk-canon/cfgrepo/zmk-ble-hid-host/`. Overlay the edited
  `drivers/input/hog_central.c` there (with a cached/initialized workspace, NEED_UPDATE=0,
  so `west update` won't overwrite it), build, then `git -C <that> checkout -- <file>`
  to restore the cache pristine. Verify the canon baseline `== main` first
  (`diff <(git show main:drivers/input/hog_central.c) <cache>/.../hog_central.c`).
- **Output:** `canon/firmware/ble_hid_host_receiver-logging.uf2`.
- **Flash:** user double-taps reset the dongle → `/Volumes/XIAO-SENSE/` mounts
  (`INFO_UF2.TXT` contains "XIAO") → `cp` the uf2 → reboots. The `cp` needs the Bash
  tool's `dangerouslyDisableSandbox`. `canon/scripts/flash-watch.sh` is imprint-only
  (would flash the wrong firmware) — flash the ist uf2 manually.
- **One-shot flasher (this work):** `~/bin/flash-ist-logging.sh` — waits for the XIAO
  mount, refuses non-XIAO boards (`INFO_UF2.TXT` guard), prints the firmware sha, then
  copies `ble_hid_host_receiver-logging.uf2`. Run it in a terminal, or have Claude run
  it (with `dangerouslyDisableSandbox`). Rebuild the uf2 with `build-zmk.sh ist --logging`.
- **LESSON:** never add verbose BT DBG logging (it breaks BLE timing). Keep
  diagnostics targeted + INF-level.

## Diagnostic infra (already running)

- Always-on capture: LaunchAgent `com.tommy.zmk-log` → `~/zmk-logs/zmk-YYYY-MM-DD.log`
  (daily, 120-day retention, device auto-detected by USB product name "BLE HID Host").
- Query: `~/bin/zmk-log around "HH:MM"` / `find N` / `since HH:MM`.
- Dongle = `/dev/cu.usbmodem21201`, serial `5A88ACF503D4166E`. Mouse A bond =
  `CD:CF:BF:79:68:00` (static random).

## Branch / state

| ref | what | action |
|---|---|---|
| `main` (`b42b608`) | #14 continuous-scan + A1 retry. **No** cached-resubscribe. Clean baseline. | build the diagnostic on top |
| `feat/cached-reconnect-resubscribe` (`9cbc3ec`) | my cached-resubscribe = **Failure Mode A**. **Currently FLASHED on the device.** | do NOT merge; the Step-1 flash replaces it |
| `feat/reconnect-diagnostics` (`581d477`, off `main`) | the `ADV-seen` diagnostic edit (committed). Logging uf2 built from this. | flash & gather data; after diagnosis, drop the diagnostic + implement Step 3 |

## Progress / Unachieved (explicit — do not leave implicit)

- ✅ Diagnostic firmware **built, verified (`ADV-seen` in `zmk.elf`), committed** (`581d477`),
  and **flashable on demand** via `~/bin/flash-ist-logging.sh`.
- ⏸️ **NOT yet flashed** — user chose to wait ("まだ待機で / いつでも焼ける状態にして").
  The flasher is ready; flashing is one action away.
- ❌ Failure Mode **B root cause not yet confirmed on device** (3 candidates open) —
  needs the flash + the Mouse-A sleep→motion test (Step 2).
- ❌ The fix is **not implemented** (Step 3, data-driven, blocked on Step 2 data).
- ❌ The **zombie regression (Mode A) is still on the device** until the flash happens →
  Mouse A on the dongle is broken meanwhile. (User is on Mouse B / Mac.)
- ❌ #8 **not resolved**; not closed.
- ❌ Nothing **pushed/merged** (needs owner approval).

### Exact resume point for the next session
1. Flash: double-tap the dongle reset, then `~/bin/flash-ist-logging.sh` (or "焼いて").
2. Confirm Mouse A auto-reconnects; let it deep-sleep (`disconnected … reason 0x13` + `conn=0`).
3. Move Mouse A firmly ~10 s; note the time; `~/bin/zmk-log around "HH:MM"`.
4. Match the `ADV-seen … type=… bonded=…` outcome to the Step-2 table → apply Step 3 fix.

## Step-3 prepared fixes (verified by 2026-06-21 code review — apply by ADV-seen outcome)

ZMK refs re-verified at `ff09f2d0` (handoff's line cites were CORRECT): `central.c:877-883`
directed branch (`ADV_DIRECT_IND -> split_central_eir_found(addr)`, no AD parse), `central.c:911-912`
PASSIVE scan, `peripheral.c:64-68` bonded re-adv = `BT_LE_ADV_CONN_DIR[_LOW_DUTY]`. Generated
`.config` confirms `BT_SCAN_WITH_IDENTITY=y`, `BT_PRIVACY` off, `BT_FILTER_ACCEPT_LIST` off.
NOTE: the address handed to `device_found` is ALREADY resolved to the peer identity
(`scan.c:638-645` `bt_lookup_id_addr`), so `addr_is_bonded` compares identity↔identity;
`bt_addr_le_cmp` compares the type byte AND the value.

| ADV-seen outcome | Root cause | Fix (verified anchors) |
|---|---|---|
| `type=1 bonded=1` (most likely) | candidate 1 (scan-stop→create race) / 3 (ACTIVE) | (a) **directed short-circuit**: on `type==ADV_DIRECT_IND && addr_is_bonded`, skip `bt_data_parse`/`ai.match`/`last_failed` cooldown → straight to the existing `bt_le_scan_stop()`+`bt_conn_le_create()` (gate at `hog_central.c:640-653`; mirror ZMK `central.c:880-882`). (b) **PASSIVE scan** when a bond exists (`start_scan` sp struct `hog_central.c:724-729`; keep ACTIVE only for first-time pairing). **Keep `.options=BT_LE_SCAN_OPT_NONE`** — do NOT adopt the `BT_LE_SCAN_PASSIVE` macro (it sets `OPT_FILTER_DUPLICATE`, which breaks our re-trigger-after-failed-connect). Must still `bt_le_scan_stop()` before create. |
| `type=1 bonded=0` | candidate 2 (addr/identity mismatch) | NOT a scan/connect fix; accept-list does NOT help (same key). FIRST **widen the diagnostic** to dump the stored bond addr beside the AdvA (`bond_match_cb`/new `log_first_bond_cb`, `hog_central.c:547-562`). If **type-only** mismatch on a directed advert → value-only gate used ONLY for `type==1`. If the **value** differs → re-pair. |
| nothing seen | candidate 3 / controller not delivering | (a) **PASSIVE** first (cheap). (b) LAST RESORT Plan B: `CONFIG_BT_FILTER_ACCEPT_LIST=y` + `bt_conn_le_create_auto()` (removes the stop→create dead window; `FAL_SIZE=8` already present). Confirm with a BLE sniffer the mouse actually emits. Does NOT fix an addr mismatch. |

**Insurance (ZMK #3377/#3385 `bt_unpair`+`AUTH_FAIL` in `security_changed`)**: do **NOT** pre-stage.
`bt_unpair` is a permanent bond wipe; gate strictly on `BT_SECURITY_ERR_PIN_OR_KEY_MISSING`
+ a repeat-count, never on generic failure. `CONFIG_BT_SMP_ENFORCE_MITM=y` vs the Just-Works
NoInputNoOutput peer is a footgun (could itself loop security failures). Only add if the log
shows a repeating `err 4` cycle.

## Latent bugs found during review (INDEPENDENT of #8 — fix on a separate branch off `main`)

- ✅ **DONE — HID-descriptor shift-count UB (was MEDIUM, real UB).** Fixed on branch
  **`fix/hid-decode-shift-ub`** (commit `ce52e25`, off `main`, **unpushed — needs PR approval**).
  `report_size > 32` made the decoder do `x << i` with `i≥32` (UB) for X/Y/wheel/hwheel, the
  button stride, and keyboard keys. Fix (defense in depth): parse-time reject of size 0 / >32 in
  `set_field`, button width, keycode-entry width; decode-time guard in `extract()` + button-loop
  cap at 32. TDD: 4 new cases in `tests/parser/test_runner.c`, verified RED (11 fails) → GREEN
  under plain **and** `-fsanitize=undefined`; added `make test-ubsan` + a UBSan step to
  `hosttest.yml`. Worked in a linked worktree at `../zmk-ble-hid-host.wt-shift-ub`. NOTE: macOS
  `-fsanitize=address` runtime HANGS (spins at 100% CPU) — use UBSan only locally; ASan is fine on
  the Linux CI but the target deliberately uses UBSan only for portability.
- **MEDIUM — `device_found` create-fail doesn't NULL `default_conn`** (`hog_central.c:672-676`).
  Benign today (create takes no ref on error) but fragile = potential permanent "deaf central".
  Add `default_conn = NULL;` + replace the immediate `start_scan()` with a `scan_retry_work` backoff.
- **MEDIUM — cooldown writes `last_failed_addr` for bonded peers** (`security_changed` `:780-781`),
  dead state today (bonded path is cooldown-exempt). Gate the copy on `!addr_is_bonded(...)`.
- **LOW** — `sub_count` cross-workqueue read without the `layout_valid` fence discipline (benign on
  single-core nRF52840; document that `layout_valid=false` at disconnect is the real guard).
  `EXT_ADV` accepted by the type filter but connect uses legacy `BT_CONN_LE_CREATE_CONN` (peer is
  legacy; drop `EXT_ADV` from `:635`). No backoff on repeated create-fail respin.
- **INFO** — `BT_SMP_ENFORCE_MITM=y` with the Just-Works NoInputNoOutput peer is benign (IO-cap
  degrades to Just Works + `BT_SMP_ALLOW_UNAUTH_OVERWRITE=y`); worth a one-line code comment.

Positive findings (verified-correct, do not re-investigate): heartbeat/scan-retry self-reschedule
has no gap; all BT callbacks run on the BT RX workqueue (`CONFIG_BT_RECV_WORKQ_BT=y`) so the
`scan_stop`/`create`/`gatt_*` calls are in a legal sleepable context; msgq handoff is `K_NO_WAIT`.

## Context

- Roadmap #12 (single-dongle integration / M6) needs robust bonded reconnect too,
  so this work is foundational, not throwaway.
- I (the assistant) mis-concluded several times on 2026-06-20 (zombie retraction;
  "mouse-side" verdict). The two-mice A/B + the ultra source verification are what
  finally pinned it. Trust the on-device diagnostic over any a-priori theory.
