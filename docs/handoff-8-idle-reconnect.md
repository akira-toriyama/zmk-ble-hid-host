# Handoff — #8 idle-recovery (mouse sleeps → dongle won't recover)

> # ⭐⭐⭐ LATEST (2026-06-22 PM #2) — v2 SHIPPED (2 s window + Fix-A clamp removed), ON-DEVICE VALIDATED, NOW OBSERVING. READ THIS FIRST.
>
> **v2 is on the device and working.** Owner confirms normal operation OK and is observing over time. Two
> changes, both in `drivers/input/hog_central.c` (the ONLY source file that differs from main; commit `bbb02ee`
> on branch `feat/zombie-auto-recover`, NOT pushed/merged):
> 1. **`ZR_WINDOW_MS` 10000 → 2000** (zombie-detect window). The data-driven LIMIT for this count-detector:
>    `~/zmk-logs` 2026-06-22 showed healthy reconnects add **246-598 rx in 10 s** while zombies add **0 or 88**
>    (the flush burst, lands <1 s). `ZR_MIN_RX` stays **100** (can't go below the ~88 burst ceiling → false
>    negatives). Owner asked "as fast as possible"; **2 s is the floor** — faster needs v3 (live re-subscribe),
>    not a shorter window. No battery cost (the window is a dongle-side timer; the dongle is USB-powered).
> 2. **Removed the Fix-A latency-0 clamp** (`le_param_req` + its conn_callbacks entry + the active
>    `bt_conn_le_param_update` re-drive in `subscribe_pending`). lat=0 was PROVEN not to fix the zombie and it
>    forced the mouse awake every 7.5 ms (worse battery). With no `le_param_req` the host auto-accepts the peer's
>    params (Zephyr `conn.c:2078` "Default to accepting", validated by `bt_le_conn_params_valid`) → the IST PRO
>    keeps **latency 44** → it sleeps between idle events → **better mouse battery.** The create-time
>    `BT_LE_CONN_PARAM(...,0,...)` at `device_found` is a DIFFERENT mechanism (initiator window, 5 s supervision)
>    and is intentionally left as-is.
>
> **Build:** logging uf2 sha `0eb3902e36be23300cb2268e8649d38d5ea5b8367872a8f51e0bca680f695062` (byte-identical
> across two builds — comment-only cleanup between them). Built via the overlay method (copy edited hog_central.c
> into `~/.cache/zmk-canon/cfgrepo/zmk-ble-hid-host/drivers/input/`, `canon/scripts/build-zmk.sh ist --logging`,
> then `git -C <cache> checkout` to restore — that cache is at an OLD base but hog_central.c is the ONLY differing
> file, so overlay-one-file is correct & proven). Reviewed by a **3-lens adversarial workflow → all
> "safe-to-flash", ZERO blockers** (C/build, BLE behavior, false-positive/cross-file).
>
> **On-device validation (boot 14:33:31):** `lat=44` ✅ (`conn params updated: ... latency 44`, was lat=0);
> 2 s window passes healthy (`zombie-check OK: rx+197 in 2s`) ✅ and catches a zombie (`ZOMBIE: rx+0<100 in 2s
> -> bounce`) ✅; post-boot turbulence (2 bounces + one 0x08) self-healed to a steady link in ~15 s; HB 14:34:27
> `conn=1 sub=5 rx=738 pub=726 lat=44 zr=2` (healthy, pub tracks rx). Owner: "通常操作OK".
>
> ## ⏭️ OBSERVATION PHASE — what to watch in `~/zmk-logs` (24/7 LaunchAgent `com.tommy.zmk-log`; alert monitor live)
> 1. **False-positive bounce (the v2 tradeoff — BOTH reviewers flagged):** look for `ZOMBIE: rx+NN<100` with NN
>    in **80-99** IMMEDIATELY followed by a healthy reconnect. That = a HEALTHY link bounced because the user did
>    "nudge-to-wake → ~88 burst → pause ~2 s". Bounded (`ZR_MAX_BOUNCE`=3) + self-correcting, but if FREQUENT in
>    daily use → bump `ZR_WINDOW_MS` to **2500-3000** (one-line change + rebuild). 2 s honors the owner's "fastest"
>    ask; this is the one parameter worth revisiting on-device.
> 2. **The real #8 test:** after the mouse DEEP-sleeps (離席/放置) and the owner returns + moves it, does it now
>    recover in ~2 s (+ reconnect) instead of ~10 s+? Trigger is physical, **1 result ≠ conclusive** (owner's
>    standing method — accumulate several deep-sleep trials).
> 3. **Unguarded timeout (review W2, low risk):** any `conn params updated: ... timeout <2000 ms`. Only matters if
>    a 2nd bonded peer requests a short supervision timeout (IST PRO asks 2160 ms → N/A for it).
> 4. **lat=44 vs lat=0 era:** does removing the clamp change zombie FREQUENCY? (lat=0 still zombied, so expected
>    no worse — confirm over time.)
>
> ## ⏭️ AFTER observation proves out
> - Build a PROD (non-logging) variant + PR to `main` (owner: only after many self-heals + acceptable freeze).
> - **v3 (R&D — the real "instant"):** on zombie detect, re-arm the CCCs on the LIVE link (unsubscribe+subscribe /
>   re-write CCC) WITHOUT `bt_conn_disconnect`. If notifications resume → near-instant, no reconnect, no motion
>   needed. UNPROVEN; fall back to the bounce. The only lever left once 2 s detection isn't fast enough.
>
> ---
>
> # ⭐⭐ (2026-06-22 PM, SUPERSEDED by v2 above) — auto-recover SHIPPED + WORKING; NEXT SESSION = v2 speed-up. READ THIS FIRST.
>
> **The fix WORKS.** The dongle self-heals the post-deep-sleep "connected-but-not-streaming" zombie with NO
> owner re-plug. On-device confirmed self-heals: 12:25 (rx+88 zombie) and 13:46 (rx+0 HARD zombie) → detect →
> `bt_conn_disconnect` bounce → reconnect → `zombie-check OK`, `zr` increments. The zombie recurs
> probabilistically and each time it recovers itself. (Full story: §"FIRST/2nd auto-recover" lower in this doc.)
>
> **ON THE DEVICE NOW:** branch `feat/zombie-auto-recover` (code `651d39b`, docs head), **gate-fixed logging
> uf2 sha `f38c1e1cc1243e7d2da2afb956cf2eb43a2c26bedd0e9604ccb23d623f2f32a4`**. Behaviour: zombie-check arms on
> EVERY reconnect; if `rx_notif` climbs < `ZR_MIN_RX`(100) in `ZR_WINDOW_MS`(10 s) → `bt_conn_disconnect`
> bounce, ≤`ZR_MAX_BOUNCE`(3)/episode. Built atop Fix-A (latency-0 clamp + `lat=`/`zr=` HB diag). Mouse usable.
>
> ## ⏭️ NEXT SESSION = v2: make recovery FASTER / less frequent (owner's explicit ask "回復の頻度あげれる？").
> The freeze window per episode = detection (10 s) + reconnect-after-bounce (the mouse re-advertises only on
> motion → fast if moving, ~52 s if the user pauses). Implement → build → flash → observe:
> 1. **Shorten detection: `ZR_WINDOW_MS` 10000 → 5000** in `drivers/input/hog_central.c` (keep `ZR_MIN_RX`=100;
>    the ~88-report flush burst lands in <1 s so a 5 s window still catches burst-zombies, and rx+0 trivially).
>    → bounces ~5 s sooner.
> 2. **Drop the latency-0 clamp (Fix-A):** remove `le_param_req` (+ its `conn_callbacks` entry) and the active
>    `bt_conn_le_param_update(... latency 0 ...)` block in `subscribe_pending`'s discovery-done branch. It never
>    fixed the zombie (PROVEN: `lat=0` still zombied) and may add 0x08 / raise episode frequency; auto-recover
>    doesn't depend on it. (Keep the `lat=` HB field or drop it — low value now.) Owner ok'd ①+②; note which helps.
> 3. **(v3, R&D — biggest win, only if ①②'s freeze is still too long) re-subscribe on the LIVE link instead of
>    disconnecting:** on zombie detect, re-arm the CCCs (unsubscribe+subscribe / re-write CCC) WITHOUT a
>    `bt_conn_disconnect`. If notifications resume → near-INSTANT recovery (no reconnect, no motion needed).
>    UNPROVEN; fall back to the bounce if flow doesn't resume in a short window.
> - **UX tip to relay to the owner:** after a freeze, KEEP MOVING the mouse (don't 放置) → the bounce's reconnect
>   fires within ~1-2 s; pausing is what stretched it to ~52 s.
> - **BUILD:** `/Volumes/workspace/github.com/akira-toriyama/canon/scripts/build-zmk.sh ist --logging` — overlay
>   the edited `hog_central.c` into `~/.cache/zmk-canon/cfgrepo/zmk-ble-hid-host/drivers/input/`, build, restore
>   the cache (back up + restore). **FLASH with `cat uf2 > /Volumes/XIAO-SENSE/fw.uf2`, NOT `cp`** (macOS
>   fcopyfile throws I/O error). `~/bin/flash-ist-logging.sh` already uses `cat`; owner double-taps the dongle reset.
> - **RE-ARM monitoring:** `bash ~/bin/zmk-monitor-fixa.sh` via the Monitor tool, persistent (alert-only:
>   surfaces `ZOMBIE`/bounce + hard errors; macOS-pings owner only on real anomalies). The 24/7 serial capture is
>   the always-on LaunchAgent `com.tommy.zmk-log` → `~/zmk-logs/zmk-YYYY-MM-DD.log` (independent of any session,
>   so deep-sleep zombies that happen between sessions are still logged — grep `ZOMBIE`/`zombie-check`/`zr=`).
> - **Record progress** in this doc + GitHub **issue #8** (owner expects issue updates) + commits. NOT
>   pushed/merged. After v2 proves out (many self-heals, acceptable freeze), build a prod (non-logging) variant + PR to `main`.
>
> ---
>
> # ⭐ (2026-06-21 PM, SUPERSEDED by the 2026-06-22 section above) — the scan theory below is OVERTURNED
>
> Diagnostic firmware (`581d477` = main + `ADV-seen` log, logging build, sha
> `d1b3c136…`) was **flashed and tested ON-DEVICE today**. The result re-scopes #8
> **again** and kills the "scan / directed-advert / PASSIVE" theory in the TL;DR +
> "THE PLAN" sections below (kept only as history). **Investigation CONTINUES across
> sessions** — owner's method (IMPORTANT): *do NOT conclude from one result; the
> trigger is physical and hard to reproduce; accumulate many results patiently.*
>
> ## The corrected finding (the smoking gun)
> On a **motion-wake reconnect** the dongle does everything right at the BLE/GATT
> layer — catches the advert, `connected`, `secured (level 2)`, full GATT discovery,
> `discovery done: subscribed to 5 report(s)` (ids 1/2/6/4/9), `conn=1 sub=5` — **yet
> the cursor is dead ("zombie"), or moves a moment then freezes.** Only a **MOUSE
> power-cycle** gives a working link. The INF log of a ZOMBIE reconnect is identical
> to a WORKING one (the only motion/forward evidence is `LOG_DBG`, suppressed at the
> default INF level). **So the failure is ABOVE the CCC-subscribe step — in HID
> report-notification FLOW, not in scan/connect/discovery.** It is NOT a zombie when
> freshly working (13:24 user-confirmed cursor moves at conn=1) — it appears on the
> *reconnect after the mouse deep-sleeps*.
>
> The flashed `.config` ALREADY has the RX mitigation (`BT_CTLR_RX_BUFFERS=12`,
> `BT_BUF_ACL_RX_COUNT_EXTRA=10`, `BT_GATT_AUTO_RESUBSCRIBE=n`) → "raise RX / it's the
> move-freeze" is NOT this bug. Distinct from the CLOSED aggressive-move freeze
> (`investigation-reconnect-freeze.md`: 0x08 under hard motion) and from that doc's
> §12 idle-death (which needs a **DONGLE re-plug**; today's needs a **MOUSE** cycle).
>
> ## On-device truth table (2026-06-21 — NOT conclusive; accumulate more)
> | stimulus after the mouse sleeps | cursor | log |
> |---|---|---|
> | A: move @14:39:42 | ❌ ZOMBIE | full discovery+sub, conn=1 sub=5, zero motion |
> | A: move SLOWLY @14:54:16 | ⚠️ moved a moment → FROZE | full discovery+sub; flow starts then stops |
> | B: MOUSE power-cycle @14:42:12 | ✅ works | identical reconnect, cursor live |
> | (freshly connected, 13:24) | ✅ works | conn=1, no zombie |
>
> Log forensics — 12 disconnect/reconnect cycles since the 12:37:10 boot: **EVERY
> advert was `type=0` (ADV_IND); ZERO `type=1` (directed) EVER** → "scanner misses
> directed adv" is DEAD for this peer. `scan_fail=0`; **12/12 reached "subscribed to
> 5 reports"**. Disconnects: `0x13`×8 (clean mouse sleep), `0x08`×4 (supervision
> timeout; some self-heal in <2 s). Evidence: `~/zmk-logs/zmk-2026-06-21.log`.
>
> ## Three candidate mechanisms (the NEXT diagnostic decides which)
> - **(A) Mouse silent** — peer doesn't resume HID notifications after reconnect (no
>   ATT notifications arrive). Matches "only a MOUSE power-cycle fixes it." Mouse/link side.
> - **(B) Dongle drops in the work handler** — `report_work_handler` silently
>   `continue`s at `!layout_valid` (`hog_central.c:147`), id-mismatch (`:163`), or
>   `decode != 0` (`:169`) → publishes nothing though conn=1 sub=5.
> - **(C) USB side eats it** — `ble_hid_host_publish` (`ble_hid_host.c:139`) uses
>   `input_report_*` K_NO_WAIT, which DROPS on a full input queue / USB-suspend-not-resumed.
>
> ## ⏭️ NEXT SESSION — START HERE (now in OBSERVE mode)
> **✅ The counter diagnostic is BUILT + FLASHED + LIVE (2026-06-21 18:30).** Commit
> `d91b654` on `feat/reconnect-diagnostics`; logging uf2 sha
> `6c4e62607117988ca6b0a293b49067d56d45c59b34ac9cde082f6755b6832ec5`. Verified: HB now
> prints `… rx_notif=%u pub=%u` and the `disconnected:` line too. Normal use is fine.
> **So the next session does NOT build — it ACCUMULATES + READS.** The owner runs the
> dongle normally; every HB (60 s) + disconnect line lands in `~/zmk-logs/zmk-YYYY-MM-DD.log`
> 24/7 (LaunchAgent, independent of any session). When a zombie/freeze happens, the owner
> notes the time; ANY later session greps the counters around it — no live monitoring needed.
>
> What was added (for reference / if a rebuild is ever needed), in `drivers/input/hog_central.c`:
> 1. `static uint32_t rx_notif, pub_reports;` declared before `report_work_handler` (so both
>    use-sites compile — NOT beside the scan counters, which are defined later in the file).
> 2. `rx_notif++;` in `notify_cb` **after** the `data == NULL` teardown check.
> 3. `pub_reports++;` right after the `ble_hid_host_publish(...)` call.
> 4. `rx_notif=%u pub=%u` appended to the 60 s heartbeat `LOG_INF` AND the `disconnected:` line.
> 5. Counters are **never reset** (monotonic; deltas across lines are the signal).
>
> Accumulate MANY motion-wake / long-idle trials, read `rx_notif`/`pub` deltas across HB +
> disconnect lines spanning a zombie vs a healthy window:
> - `rx_notif` **FLAT** (conn=1 sub=5) → **(A) mouse silent** → not fixable by re-subscribe;
>   candidate fix = detect "subscribed but 0 rx for N s" → force disconnect/re-pair to nudge it.
> - `rx_notif` **CLIMBS** but `pub` **FLAT** → **(B) dongle drop** → add per-guard counters
>   `drop_layout`/`drop_id`/`drop_decode` at `:147/:163/:169` to localize, then fix that guard.
> - **both climb** + dead cursor → **(C) USB side** (ZMK input queue / USB suspend) — other layer.
>
> ## Ruled out today (don't re-investigate)
> scan ACTIVE↔PASSIVE; directed-advert miss (`BT_SCAN_WITH_IDENTITY=y`; only ADV_IND ever);
> address/bond mismatch (security reaches L2, discovery completes, `bonded=1` every time); RX
> depth / AUTO_RESUBSCRIBE (already 12/10/n); the CLOSED aggressive-move freeze (0x08 under hard
> motion — today's zombie HOLDS the link with no 0x08).
>
> ## Session tooling (re-arm next session)
> - Serial capture LaunchAgent `com.tommy.zmk-log` → `~/zmk-logs/zmk-YYYY-MM-DD.log` (120 d).
>   Query: `~/bin/zmk-log around "HH:MM"`. Flasher: `~/bin/flash-ist-logging.sh`.
> - This session ran a live monitor that fired a **persistent macOS notification on each
>   `0x13` sleep** (pings the owner when a trial window opens). Re-arm: `tail -n0 -F <log> |
>   grep --line-buffered -E "disconnected:|ADV-seen|connected:|scanning for a HOGP" | while
>   read l; do echo "$l"; case "$l" in *"reason 0x13"*) osascript -e 'display notification …';;
>   esac; done`. Persistence = System Settings → 通知 → Script Editor → "通知パネル(Alert)".
> - Unrelated fix shipped: HID shift-count UB on `fix/hid-decode-shift-ub` (`ce52e25`, off
>   `main`, **UNPUSHED**, needs PR approval) — details in "Latent bugs" below.
>
> **Nothing pushed/merged. #8 OPEN.**
>
> ---
> <details><summary>History below (2026-06-21 AM and earlier) — the scan/PASSIVE theory,
> now SUPERSEDED by the on-device results above. Kept for context only.</summary></details>

---

## 📍 OBSERVE-mode samples (counter diag `d91b654`) — accumulate here

### Sample 1 — 2026-06-21 ~18:55 (first clean LIVE capture) — POST-RECONNECT FREEZE confirmed
Owner was away from the desk (離席) → mouse deep-slept (`0x13` @18:41:51, rx=6309 pub=6298) →
~14 min conn=0. **The 14 min is NOT a reconnect failure** — the owner simply wasn't moving the
mouse, so it wasn't advertising (rules out the old "motion doesn't re-attach" Failure-B for this
sample). Owner returns, moves it → reconnect @18:55:46 (ADV `type=0`, secured L2, `conn=1 sub=5`).
- ~37-report burst at reconnect (6309→6346), then **`rx_notif` FLAT at 6346 for 7+ min**
  (HB 18:56:07 → 19:03:07, `conn=1 sub=5` throughout) **while the owner was actively moving it**
  (owner-confirmed live: "今動かした、そして動いていない").
- `pub` tracked `rx` (flat at 6324) → **NOT (B) work-handler drop, NOT (C) USB.** Reports simply
  stop ARRIVING at the host after the initial burst.
- **The LL link stayed UP the whole 7 min — no `0x08` supervision timeout** → the controllers keep
  exchanging LL PDUs; only the ATT HID notifications stop. = the truth-table "一瞬動いて固まる" mode.
- The zombie connection then **`0x13`-slept on its own @19:04:21** (rx still 6346 = ZERO reports the
  entire ~9 min window). Owner did NOT power-cycle the mouse this time (it re-slept first). Owner says
  mouse OFF/ON would cure it; the other unit on Mac-native BT works fine (owner-confirmed A/B).
- **Correlation: the zombie followed a LONG/deep sleep (14 min; "離席後に戻ると動かなくなる").** The
  two SHORT sleeps earlier today (18:34:29→18:35:48, ~1.3 min) reconnected and **worked** (rx climbed
  into the thousands). → working hypothesis: **deep/long sleep — not the reconnect mechanics per se —
  is what triggers it.** This matches the owner's lived pattern exactly.
- ⚠️ **Serial-capture caveat (IMPORTANT for future forensics):** the log was CORRUPTED/lossy
  18:50–18:56 (a ~5-min flush gap, then a batch where two log lines were concatenated onto one physical
  line). The zombie reconnect's discovery/subscribe lines were eaten by this → **cannot diff its GATT
  sequence against a working one.** The working 18:35:48 reconnect logged the full clean sequence incl.
  `conn params updated: interval 7.50 ms, latency 44, timeout 2160 ms`. So "param update missing on the
  zombie" is **UNPROVEN** (merely absent from a corrupted log). Note `latency=44` is high — worth
  checking whether the Mac negotiates a lower peripheral latency (possible differentiator).
- **Still open: (A) mouse stops emitting HID** vs **(D) dongle controller RX-path wedges after a burst**
  (link held by TX empty PDUs). Both are cured by ANY forced reconnect, so "mouse OFF/ON cures it" does
  NOT separate them. **Decisive next test:** the next time it zombies, **re-plug ONLY the dongle (do NOT
  touch the mouse).** Cursor revives from a dongle reset alone → dongle-side → the handoff's auto-bounce
  workaround (`bt_conn_disconnect` on "subscribed but rx flat for N s after reconnect") is viable. Only a
  mouse reset works → mouse-internal.

### Sample 2 — 2026-06-21 19:04→21:52 — idle-death (conn=0) + dongle re-plug → healthy
- The Sample-1 zombie `0x13`-slept @19:04:21 (rx=6346) → the dongle then stayed **conn=0 for ~2h45m**
  (19:04 → 21:49), scanning (scan_ok=5, scan_fail=0), rx frozen at 6346. Owner away (evening).
- **Dongle rebooted @21:49:25** (`*** Booting Zephyr OS ***`, uptime reset, fresh USB
  "Device connected/reset/configured" @00:00:00) = a **dongle re-plug** (almost certainly the owner).
  Counters reset to 0/0 — this is why the 22:29 sleep showed rx==pub (gap 0), not the prior gap.
- After reboot: reconnect @21:52:52 was **HEALTHY** — rx==pub climb in lockstep 0→440→3365→…→18439→
  …→63292, gap 0 throughout, until a normal `0x13` sleep @22:29:07. A fresh boot gave a clean ~37-min session.
- ⚠️ **This re-plug did NOT test the (A)/(D) zombie question** — it was done while the mouse was
  **conn=0 (asleep)**, not during a live zombie (conn=1, rx flat). Keep the two DISTINCT failures separate:
  **(i) post-reconnect ZOMBIE** (conn=1 sub=5, rx flat, link held — Sample 1; cured by a MOUSE cycle) vs
  **(ii) idle-death** (conn=0 after long idle, scanner not catching the wake advert — the §12 failure in
  `investigation-reconnect-freeze.md`; cured by a DONGLE re-plug). PENDING owner confirmation whether
  19:04→21:49 was *real* idle-death (owner moved it, no reconnect) or merely owner-away/no-advert.
- 🛠️ **Monitor reliability note:** the persistent `tail -F` monitor MISSED the 21:49 `Booting` line — a
  re-plug disrupts the USB serial device, the LaunchAgent recreates the log file, and `tail -F` lost the
  line in the reopen race. FIX applied: also key reboot detection on the first `HB up=60s` (emitted 60 s
  post-boot once the file is stable → reliably caught). Always corroborate by grepping the log directly.

### Sample 3 — 2026-06-21 22:33 ZOMBIE (clean logs) → 22:37:43 self-RECOVERED via short sleep+wake
- 22:29:07 normal sleep (rx=63292). 22:33:21 reconnect after a **~4-min** sleep → **ZOMBIE again**, but
  this time discovery logged CLEANLY (no corruption): report map parsed → all 5 reports subscribed
  (id 1/2/6/4/9) → discovery done → `conn params updated interval 7.5ms latency 44 timeout 2160ms`.
  ~30-report burst (63292→63322) then **rx flat at 63322 for ~4 min** while the owner operated the
  trackball; conn=1 sub=5; no 0x08. → **discovery/subscribe ALL succeed; the bug is purely that
  notifications stop after the burst** (kills any "silent discovery failure" hypothesis for good).
- 22:37:33 the zombie mouse `0x13`-slept on its own (idle, owner had paused). 22:37:43 (10 s later) it
  reconnected and this time **WORKED** (rx resumed climbing 63322→63423→…; owner: "いま動く").
- ⭐ **The zombie self-cleared via a SHORT sleep+wake cycle** — a mouse power-cycle is NOT the only cure;
  any shallow reconnect resumes flow.
- ⭐ **Sleep-duration → outcome, now across 5 reconnects:** 10 s → OK, 1.3 min → OK, **4 min → ZOMBIE**,
  **14 min → ZOMBIE**. **Deeper/longer sleep triggers the zombie; threshold ~1.3–4 min.** Strongest
  characterization yet; matches the owner's "離席して戻ると動かなくなる" (long absence) exactly.
- The dongle-replug experiment did NOT get to run (the mouse self-recovered first). Per owner request the
  re-plug prompt now lives in the sleep notification ("!ドングル抜き差し!").
- ▶ Root-cause investigation launched (background Workflow `zmk8-deepsleep-zombie-rootcause`): why does a
  DEEP-sleep reconnect fully succeed yet HID notifications stop after a burst with the link held — and why
  is a macOS host immune? Leading hypothesis to test: the high negotiated peripheral **latency=44** + a
  deep-sleep reconnect leaves the link alive (keepalives) but data PDUs not surfaced; fix candidate =
  central-side conn-param renegotiation to low latency after connect.

  **RESULT (2026-06-21 PM — workflow `wf_f9d153a8-027`, 13 agents, adversarial). The strong output is
  ELIMINATIONS (source-proven), which narrow #8 and save dead-end fixes:**
  - ❌ **RX-buffer / NESN-latch — REFUTED.** The controller RX node is released at HCI-encode time
    (`zephyr/.../hci/hci_driver.c:535`) BEFORE host delivery (`:737`), so our `notify_cb`/decode can't
    latch it; the ACK gate (`.../nordic/lll/lll_conn.c:1144-1150`) is real but self-clearing; and the OLD
    wedge always hit 0x08 within seconds whereas #8 holds 7+ min with NO 0x08 = a different mode. The conf
    already documents RX 12/10 was MEASURED not to cure it. → **Do NOT bump RX buffers again.**
  - ❌ **latency=44 as ROOT CAUSE — REFUTED (non-discriminating).** The IST PRO renegotiates latency=44 on
    EVERY connect incl. the HEALTHY short-sleep ones (`hog_central.c:676-678`); a property identical in
    pass+fail can't be the cause; latency only licenses skipping when the peer has NO data (a moving ball
    has data). [It remains the best *lever* — see Fix A.]
  - ❌ **Service-Changed (0x2A05) not-subscribed — REFUTED.** `zephyr/.../host/att.c:2743-2748` auto-confirms
    every indication regardless of subscription → an unconfirmed SC can't wedge peer TX. → **Do NOT add 0x2A05.**
  - ✅ **Surviving picture:** `rx_notif` flat (`hog_central.c:196`) = reports genuinely NOT ARRIVING at our
    GATT layer (a driver-side drop would fire the silent `report queue full` WRN `hog_central.c:210`). =
    after a DEEP sleep the peer comes up "connected but not streaming to US", cured only by re-establishing
    (mouse cycle / shallow reconnect). macOS is immune because it ACTIVELY re-drives conn params post-
    reconnect; we are observe-only (`hog_central.c:460-467`; no `bt_conn_le_param_update` anywhere). The
    duration→state threshold is a peer/link timing property **NOT resolvable from this source tree — needs
    an OTA sniff (Sniffle/btmon) to attribute peer-vs-link definitively.**
  - ▶ **NEXT (ranked):** (1) **diagnostic** — add live `bt_conn_get_info().le.latency` to the INF heartbeat
    (NO BT DBG) to SEE the latency during a zombie. (2) **Fix A (plausible, low-risk, NOT proven)** — add
    `.le_param_req` clamping `latency=0` (`hog_central.c:859`) + active `bt_conn_le_param_update(conn,
    BT_LE_CONN_PARAM(6,12,0,400))` at discovery-done (replace observe-only `hog_central.c:463-467`),
    mirroring macOS. If the peer NAKs latency 0 and the zombie persists → escalate to a Sniffle OTA capture.
    FIXED signature = after a >4-min sleep reconnect, HB `rx_notif` climbs into the thousands while moving +
    `le_param_updated latency 0`. Validate ≥5 reconnects each at the 4-min AND 14-min cases (probabilistic;
    one green run ≠ proof). Full report: `tasks/wajq7wtjt.output` / workflow `wf_f9d153a8-027`.

### Fix-A BUILT — 2026-06-21 23:41 (branch `feat/reconnect-param-redrive` `d622bd0`, UNPUSHED)
- Implements diagnostic + Fix-A in `drivers/input/hog_central.c` (atop the counter diag):
  1. `le_param_req` clamps the peer's requested **latency to 0** (accept its interval/timeout; floor
     timeout ≥1 s). 2. After discovery (`subscribe_pending` done-branch) actively
     `bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(6,12,0,400))` = latency 0, **4000 ms** timeout
     (mirror macOS; longer timeout also guards the old 0x08 wedge). 3. Heartbeat gains **`lat=`**
     (`cur_latency`, set in `le_param_updated` + at disc-done, reset 0xFFFF on disconnect).
- Logging uf2: `canon/firmware/ble_hid_host_receiver-logging.uf2`, **sha256
  `4df497e863c5de30c8c1d88ed822c9f1cdc334b5df3147d2e9f96730527c3d64`**. Build green (Docker
  `zmk-build-arm:stable`); Fix-A strings verified present in `zmk.elf` (`lat=%u`, `param re-drive …`).
- **TEST PLAN (on-device):** flash → reconnect should log `lat=0` (not 44). Then let the mouse DEEP-sleep
  **>4 min**, return, move continuously 60 s. **FIXED** = HB `rx_notif` climbs into the thousands while
  moving AND `lat=0`. **Fix-A insufficient** = HB shows `lat=44` (peer NAK'd the clamp) and/or rx flat →
  next step = OTA sniff (Sniffle). Watch the 0x08 monitor for any regression from the lower latency.
  Validate ≥5 reconnects each at the 4-min AND 14-min sleep cases (probabilistic; one green run ≠ proof).
- NOT pushed/merged. Default (non-logging) prod variant NOT built yet — build `ist` (no `--logging`) after
  Fix-A is proven on-device.
- ✅ **FLASHED 2026-06-21 23:51** (boot `*** Booting Zephyr OS ***` @23:51:04; `lat=` field live, sentinel
  `lat=65535` while disconnected = code confirmed). **First post-flash reconnect @23:56:31:** peer requested
  latency 44 → **`le_param_req` clamped it to latency 0** (`conn params updated … latency 0`), then the active
  re-drive applied (`param re-drive requested … latency 0 timeout 4000 ms` → `conn params updated … latency 0
  timeout 4000 ms`); the peer then re-requested timeout 2160 ms (latency stayed 0). **Cursor works at latency
  0** (owner-confirmed "動く") → latency 0 does NOT break normal operation and the clamp PROVABLY takes (the
  peer accepts latency 0). Benign churn: a timeout tug-of-war (ours 4000 vs peer 2160) but latency held at 0.
- ⏳ **NOT yet the fix proof** — 23:56 was a SHALLOW reconnect (always worked pre-fix too). The decider is the
  **DEEP-sleep (>4 min) reconnect**: FIXED = `lat=0` + `rx_notif` climbs into the thousands while moving (no
  zombie). Validate ≥5× each at the 4-min AND 14-min sleep cases (probabilistic; one green run ≠ proof).
- ❌ **Fix-A FAILED on-device (2026-06-22 00:09–00:12) — latency DEFINITIVELY ruled out.** After a ~10-min
  deep sleep (23:59:40→00:09:31), reconnect logged **`lat=0`** (clamp + active re-drive both worked; the peer
  ACCEPTED latency 0), conn=1 sub=5, full discovery. HB 00:10:02 = `lat=0 rx_notif=253` (218→**+35 burst**)
  `pub=243` → then **rx flat; owner: "一瞬動いて固まった"** = the SAME zombie despite latency 0. Mouse
  idle-slept out of it @00:12:35 (rx still 253). → **latency was never the cause** — this empirically confirms
  the workflow's adversarial elimination of the latency hypothesis. `feat/reconnect-param-redrive` is a dead
  end as a FIX (keep only the `lat=` heartbeat diagnostic; the clamp is harmless but pointless).
- ▶ **Next options (Fix-A dead, latency + RX-pool + SC all ruled out):**
  (1) **OTA sniffer (Sniffle/nRF) — the clean path.** `rx_notif` flat only proves OUR HOST doesn't receive;
  only an over-the-air capture distinguishes **(A) mouse goes silent** vs **(D) mouse transmits but the dongle
  controller drops**. That verdict picks the real fix. REQUIRED for certainty.
  (2) **Pragmatic auto-recover (heuristic, no sniffer).** On a reconnect, if an initial burst arrives (≥K
  reports, proving the mouse CAN send) then `rx_notif` stays flat for N s, `bt_conn_disconnect()` once to force
  a fresh reconnect (which clears the zombie — shallow reconnects always work). False-positives (bouncing a
  healthy *idle* link) are mostly INVISIBLE because they fire during a no-motion window; gate to ≤1-2 bounces
  per connection and require the initial burst (so a truly-idle "user away, no burst" link is never bounced).
  Not a root-cause fix, but likely turns the dead-cursor into a ~1-2 s self-heal. The earlier "auto-bounce is
  unsafe" caveat is softened by the burst-gate + the fact the bounce lands during idle.
- Re-plug experiment STILL not run cleanly: the zombie mouse idle-sleeps (0x13) within ~3 min every time,
  closing the conn=1 window before a re-plug can be staged. (And shallow reconnects already prove a forced
  reconnect clears it, so the experiment's marginal value has dropped.)
- ⚠️ **The zombie is PROBABILISTIC, not duration-gated (correction to earlier samples).** Post-Fix-A
  deep-sleep reconnects, BOTH `lat=0`: **00:09:31** (after ~10-min sleep) = **ZOMBIE** (rx 218→253 = +35
  burst then flat; owner "一瞬動いて固まった"); **00:32:07** (after ~20-min sleep) = **HEALTHY** (rx 253→543
  = +290 sustained, owner "動く"). A LONGER sleep worked while a SHORTER one zombied → the earlier
  "deeper sleep = worse" threshold was an over-read of small data. The zombie fires **stochastically** per
  reconnect. ⇒ cannot judge "fixed" or "Fix-A helped/hurt" from single samples; must accumulate the zombie
  **RATE** over many reconnects (ideally compare with vs without Fix-A). Tally so far (post-Fix-A, all lat=0):
  **1 zombie / 1 healthy** at deep sleeps. This is exactly why the owner's "accumulate, don't conclude from
  one" rule is load-bearing here.
- 2026-06-22 morning: owner away ~7 h, Mac deep-slept → **this Mac CUTS dongle USB power on a long sleep**
  (dongle rebooted `*** Booting ***` @09:05:35 on wake; short naps had kept it powered). First post-boot HB
  @09:06:32 = `conn=1 sub=5 lat=0 rx_notif=3232 pub=3232` → the **deepest sleep yet (~7 h) reconnected
  HEALTHY** (3232 reports in 60 s, rx==pub). Kills "deeper = worse" entirely. Running tally (post-Fix-A, all
  lat=0): **1 zombie / 2 healthy**.
- 🔬 **Tentative new lead (small n — do NOT conclude):** split by dongle state at reconnect — **fresh-boot**
  reconnects 23:56 + 09:06 = **healthy (2/2)**; **already-running-dongle** reconnects 00:09 (zombie) + 00:32
  (healthy) = 1/2. Hypothesis to test as samples grow: the zombie may favor reconnects on a dongle that has
  been *running a while* (accumulated host/GATT/conn RAM state) vs a clean boot. If it holds, a periodic
  self-reboot or a state-reset-on-reconnect becomes a candidate fix — but the workflow already eliminated the
  obvious dongle-internal latches, so treat with suspicion until the OTA sniff or many more samples weigh in.
- ✅ **RE-PLUG EXPERIMENT RAN CLEANLY (2026-06-22 09:24) — recovery is DONGLE-SIDE (owner + counters confirm).**
  09:24:01 reconnect after a ~6.5-min sleep, `lat=0` → **ZOMBIE** (owner: "動かない"). Owner **re-plugged the
  DONGLE ONLY (mouse NOT power-cycled)** @09:24:34 (`*** Booting ***`). Post-reboot HB @09:25:30 =
  `conn=1 sub=5 lat=0 rx_notif=1874 pub=1874` = **HEALTHY** (owner: "動く"). ⇒ **a dongle reset/reconnect cures
  the zombie; the mouse needs NO power-cycle.** Confirms the fix can be **100% dongle-side**.
- ▶ **Auto-recover (path #2) — branch `feat/zombie-auto-recover`.** Detection: on a reconnect following a ≥90 s
  disconnect (deep-sleep wake) — or a recovery bounce — snapshot `rx_notif`; 10 s later if the delta < 100
  (zombie = only the ~35 burst) force a fresh reconnect via `bt_conn_disconnect`, up to 3 bounces, then give up
  until the next wake. Deep-sleep gate + bounce-cap prevent idle/bounce loops; a false positive (user wakes but
  doesn't move 10 s) costs only invisible reconnects during a no-motion window. v1 = LIGHT bounce (also tests
  whether a dongle-initiated reconnect, not a full reboot, clears it). If bounces fail on-device → v2 = guarded
  `sys_reboot`. HB gains `zr=` (total bounces).
- ✅ **BUILT + FLASHED + LIVE 2026-06-22** (branch `feat/zombie-auto-recover` `4545b46`; logging uf2 sha
  `4223b16d9ba15b790c93be8f30d2544cfa0c42dc325e6022aeff7bf161c3c2b3`; auto-recover strings verified in
  `zmk.elf`). Flash reboot @10:15:30; post-boot HB @10:16:27 = `conn=1 sub=5 lat=0 rx_notif=3116 pub=3116
  **zr=0**` → firmware confirmed running (new `zr=` field present) + healthy. `zr=0` = no zombie/bounce yet
  (expected — needs a deep-sleep wake). **On-device validation pending:** watch for `zombie-check armed` →
  `ZOMBIE: … auto-recover bounce 1/3` → reconnect → rx resumes + `zr` increments = self-heal WORKS; or
  `ZOMBIE persists after 3 bounces` = the light bounce is insufficient → v2 = guarded `sys_reboot`. Accumulate
  many deep-sleep wakes (probabilistic). NOT pushed/merged; prod (non-logging) variant after it's proven.
- ⚠️ **Mac USB-power oscillation BLOCKS #8 validation (2026-06-22 ~11:11–11:27, owner away).** This Mac CUTS
  dongle USB power on sleep and was napping aggressively (~7 reboots since 09:05; the last several ~2 min
  apart: 11:11 / 11:22 / 11:24 / 11:26). EVERY reboot is BENIGN — logging GAP before it + `usb_hid: Device
  suspended` / `Device reset detected` at boot + NO fault/ZOMBIE + `zr=0` healthy while up → the **FIRMWARE IS
  INNOCENT** (it never `sys_reboot`s; auto-recover only `bt_conn_disconnect`s). Consequence: each reboot →
  fresh-boot reconnect (always healthy, 4/4+), so the **running-dongle deep-sleep reconnect — the actual zombie
  condition — is never reached** → `zr` stays 0 and auto-recover is UNtested. Also makes the mouse janky (~2 s
  dropout per reboot). **To validate #8, keep the Mac AWAKE (`caffeinate`) so the dongle stays running, let the
  MOUSE deep-sleep (≥~10 min), then move it** = the running-dongle deep-sleep reconnect. (Owner triggers
  caffeinate on return; separately, the USB-power-on-sleep behavior is a macOS setting worth fixing for daily use.)
- 🎯 **FIRST auto-recover firing — SELF-HEALED (2026-06-22 12:25; 1 sample, with nuance).** Finally a
  running-dongle deep-sleep reconnect (dongle did NOT reboot): 12:25:07 reconnect after gap=306s (~5 min mouse
  sleep), lat=0 → `zombie-check armed rx0=3932`. Over the next ~28 s: **ZOMBIE detected** (`rx+88<100 in 10s`)
  → **`auto-recover bounce 1/3`** (`bt_conn_disconnect` → logs reason **0x16**, distinguishable from 0x13 sleep
  / 0x08 timeout) → after the bounce + 2 interspersed **`0x08` supervision timeouts** (12:25:16, 12:25:24, each
  self-reconnecting) → `zombie-check OK: rx+273 in 10s (flowing)` @12:25:35, HB `conn=1 sub=5 lat=0 zr=1`, rx
  climbing. ⇒ **the dongle self-healed the zombie with NO owner re-plug (zr=1, ended healthy).** Detection +
  recovery both worked. **NUANCES (watch):** (a) this episode mixed in **0x08 supervision timeouts** — unlike
  the earlier clean *link-held* zombies; possibly the latency-0 clamp (Fix-A) trades link-held-zombie for 0x08
  thrashing → **consider DROPPING the latency clamp in v2** (it never fixed the zombie anyway). (b) recovery
  took ~28 s + several reconnect cycles, not one clean bounce. (c) **1 sample only** — accumulate more.
- 🔧 **Gate bug found + fixed + reflashed (2026-06-22 13:24–13:34).** The 90 s `ZR_DEEP_MS` gate MISSED a 79 s
  sleep zombie (13:24) → stuck ~4.5 min until the mouse idle-slept + woke healthy (13:31; self-recover via a
  sleep cycle, no re-plug). Fix `651d39b`: **arm the zombie-check on EVERY reconnect** (the zombie is
  probabilistic, not duration-gated — a 78 s sleep was healthy, 79 s zombied; storms stay safe via the
  per-reconnect reschedule). Reflashed (sha `f38c1e1cc1243e7d2da2afb956cf2eb43a2c26bedd0e9604ccb23d623f2f32a4`)
  — note `cp` failed with macOS `fcopyfile: Input/output error`; **`cat <uf2> > /Volumes/XIAO-SENSE/fw.uf2`
  succeeded** (flasher updated to use cat). CONFIRMED running: post-flash reconnect logged `zombie-check armed:
  gap=22s` (the old 90 s gate would've SKIPPED a 22 s gap) → `zombie-check OK rx+598`. Owner: mouse works →
  into 様子見 (observe). Next: accumulate auto-recover firings on the gate-fixed firmware.
- 🎯🎯 **2nd + recurring auto-recover SUCCESSES on the gate-fixed fw (2026-06-22 13:46+).** 13:46:41 a HARD
  zombie `ZOMBIE: rx+0<100` (ZERO reports in 10 s) on a reconnect after a 0x08 → 137 s gap → `auto-recover
  bounce 1/3` (0x16) → the bounce's reconnect (gap=52 s) → `zombie-check OK: rx+246 (flowing)`, `zr=1`, owner
  "今は動く" = **self-healed, NO re-plug.** Owner then reports the cycle recurring **何回か (several times)**:
  "しばらく放置 → 一瞬動く → すぐ固まる → 少し放置 → 普通に動く". ⇒ on the gate-fixed fw the auto-recover
  RELIABLY ends in recovery, but **every episode has a freeze window and the zombie is happening fairly often.**
- **Freeze-window breakdown (the v2 target):** ~`ZR_WINDOW_MS` (10 s) detection + reconnect-after-bounce. The
  reconnect waits for the mouse to re-advertise, which it only does **ON MOTION** → fast (~1-2 s) if the user
  keeps moving, slow (~52 s observed) if they pause/放置. Plus a handful of single 0x08s today (maybe the
  latency-0 clamp adds instability). Owner asked to make recovery faster/less frequent → see the v2 plan at the
  TOP of this doc.

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

---

# 📒 Session record + operating guide for the NEXT Claude Code session (2026-06-21 PM)

A fresh Claude Code session can resume from THIS section alone. Device is in **OBSERVE
mode** with the counter diagnostic flashed; the owner uses the mouse normally and the
LaunchAgent logs the counters 24/7. You do NOT rebuild or reflash unless the owner picks
a direction (see "What the owner will report" below).

## What happened this session (chronology)
1. Built + flashed the `ADV-seen` diagnostic (`581d477`, uf2 sha `d1b3c136…`). Confirmed
   booted; cleared the cached-resubscribe zombie regression that was on the device.
2. On-device A/B trials (owner drives Mouse A "ドングルマウス"; Mouse B is the Mac's
   "通常マウス"). **Result overturned the scan/PASSIVE theory** — see the ⭐LATEST section
   at the top. Failure = **post-reconnect ZOMBIE** (conn=1 sub=5, full discovery, but
   0/brief report flow), cured only by a **MOUSE power-cycle**.
3. Built + flashed the **counter diagnostic** (`d91b654`, uf2 sha
   `6c4e62607117988ca6b0a293b49067d56d45c59b34ac9cde082f6755b6832ec5`) = `581d477` +
   `rx_notif`/`pub_reports` in the HB and disconnect lines. Verified live (HB at 18:31
   showed `rx_notif=0 pub=0`). **This is what is on the device now.**
4. Side-fix (unrelated): HID descriptor shift-count UB → `fix/hid-decode-shift-ub`
   (`ce52e25`, off `main`, RED→GREEN plain+UBSan, `make test-ubsan`+CI, **UNPUSHED**).

## Artifacts (all on `feat/reconnect-diagnostics` unless noted; nothing pushed/merged)
| ref | what |
|---|---|
| `581d477` | `ADV-seen` diagnostic in `device_found` |
| `d91b654` | **counter diagnostic** (`rx_notif`/`pub_reports`) — **flashed now** |
| `d329c3a` / `5b29bde` | the ⭐LATEST re-scope + observe-mode handoff |
| firmware | `canon/firmware/ble_hid_host_receiver-logging.uf2` (currently sha `6c4e6260…`) |
| `fix/hid-decode-shift-ub` `ce52e25` | shift-UB fix (separate worktree `../zmk-ble-hid-host.wt-shift-ub`), UNPUSHED |
| `~/bin/flash-ist-logging.sh` | one-shot flasher (waits for XIAO mount, board-guarded, mount-race-fixed) |
| `~/zmk-logs/zmk-YYYY-MM-DD.log` | 24/7 serial capture (LaunchAgent `com.tommy.zmk-log`, 120-day) |

## Monitoring method (re-arm verbatim in a new session)
The owner likes a **persistent macOS notification when the mouse sleeps** (= "your turn
to test"), and Claude pinged only on anomalies. Re-arm with a **persistent background
Monitor** (note: use TODAY's date in the log filename):
```bash
LOG=~/zmk-logs/zmk-$(date +%F).log   # resolve to the literal current-day file in the Monitor cmd
tail -n0 -F "$LOG" \
| grep --line-buffered -E "disconnected:|create connection failed|scan start failed" \
| while IFS= read -r line; do
    clean=$(printf '%s' "$line" | sed 's/\x1b\[[0-9;]*m//g')
    case "$clean" in
      *"reason 0x13"*)  # clean mouse sleep = a trial window opened -> notify the owner only
        osascript -e 'display notification "ドングルマウスが寝ました。動かして結果(動く/ゾンビ/一瞬で固まる)を教えてください。" with title "ZMK #8: 寝た A=動かす" sound name "Glass"' >/dev/null 2>&1 ;;
      *)  # 0x08 / 0x3e / errors -> surface to Claude (chat event)
        printf '%s\n' "$clean" ;;
    esac
  done
```
- Run it via the **Monitor tool, `persistent: true`** (it ends at session end; re-arm each session).
- macOS notification **persistence** is a one-time owner setting: System Settings → 通知 →
  **Script Editor** → スタイル **「通知パネル」(Alert)**. (Already set this session.)
- The notification only fires the Mac alert — it does NOT need Claude online; but the
  Monitor (the chat-side ping) only runs while a session is live. The **LaunchAgent log
  capture is independent and always-on**, so data accumulates regardless.

## How to READ the counters (the whole point)
Around any zombie/freeze the owner reports (note the time), look at the HB lines (every
60 s) + the `disconnected:` line; the counters are monotonic so use **deltas** across lines:
```bash
~/bin/zmk-log around "HH:MM"                       # owner-friendly window query, OR:
grep -E "rx_notif=|disconnected:|connected:|discovery done" ~/zmk-logs/zmk-$(date +%F).log \
  | sed -E 's/\x1b\[[0-9;]*m//g'
```
Decision (a healthy window has BOTH climbing fast under motion + a live cursor):
- zombie + `rx_notif` **delta ≈ 0** (conn=1 sub=5) → **(A) mouse silent** → not a dongle
  forward bug; candidate dongle workaround = detect "subscribed but rx_notif flat for N s"
  → `bt_conn_disconnect()` to force a fresh reconnect (UNPROVEN — a link bounce may not
  equal the mouse's internal reset; and idle vs zombie is ambiguous because no-motion also
  = rx_notif flat, so any auto-bounce must be gated carefully to not kill a healthy idle link).
- `rx_notif` **climbs** but `pub` **flat** → **(B) dongle drops in `report_work_handler`**
  → add per-guard counters `drop_layout`/`drop_id`/`drop_decode` at `hog_central.c:147/163/169`
  to localize, then fix that guard. **Fully fixable on the dongle.**
- **both climb** + dead cursor → **(C) USB side** (ZMK input queue full / USB suspend) —
  different layer (`ble_hid_host.c` publish / ZMK USB-HID), not this driver's BLE path.

## What the owner will report next — and what to do
The owner expects the observation to resolve toward one of two directions:

- **「採用」(adopt) — this version/behaviour is acceptable, keep it.** Likely phrasing:
  *"採用でいい / もう問題ない / これで使う"* (maybe with how long observed / how rare the zombie).
  → Action: build a **clean PRODUCTION (non-logging) variant** = strip BOTH diagnostics
  (`ADV-seen` 581d477 + counters d91b654) from `device_found`/`notify_cb`/`report_work_handler`/
  HB/disconnect, build `build-zmk.sh ist` (no `--logging`), flash the default uf2, and
  consider a PR to `main`. (The diagnostics are harmless INF/counters, but production should
  be clean.) Confirm with the owner before flashing.
- **「不具合」(defect) — it still zombies/freezes.** Likely phrasing:
  *"またゾンビった / 一瞬で固まった、だいたい HH:MM、動かした(or 動かしてない)"*.
  → Action: read the `rx_notif`/`pub` counters around HH:MM (recipe above) → classify A/B/C →
  implement the corresponding fix. **Accumulate several samples before concluding** (owner's
  explicit method: physical, hard to reproduce, never conclude from one result).

Either way: report what the counters show with evidence (file:line/log timestamps), don't
assert a verdict from a single trial, and keep the owner's "accumulate patiently" rule.
