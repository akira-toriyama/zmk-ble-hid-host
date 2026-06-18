# Roadmap & continuation guide

This is the portable, machine-independent guide for continuing development —
written so you can pick the work up **from any PC** (it does not depend on the
original Mac's local Docker setup or attached hardware). The dense session-relay
scratchpad lives in [`../HANDOFF.md`](../HANDOFF.md); this file is the clean,
forward-looking version.

---

## Status snapshot

| Milestone | State |
|---|---|
| M0 scaffold + host-test CI | ✅ done |
| M1 receive (scan/bond/discover/subscribe) | ✅ proven on device |
| M2 decode (runtime Report-Map parser + decoder) | ✅ host-tested + on device |
| M2b firmware-build CI + `hog_central.c` | ✅ green |
| M3 publish (decoded reports → `input_report_*`, cursor moves) | ✅ proven on device |
| **Reconnect** (bond persists; auto-reconnect after mouse power-cycle / dongle replug, no re-pair) | ✅ fixed, on `fix/reconnect-directed-adv` |
| M4 customization (axis/scroll processors, **button → key**) | ⏳ next — config-only, see below |
| M6 universal BLE-HID bridge (**BT keyboard** support) | ⏳ planned — see below |
| M5 dongle case / daily use | ⏳ physical |

The Bluetooth-central foundation (scan → connect → legacy Just-Works bond →
GATT discovery → subscribe → runtime Report-Map decode) is **HID-generic** and
already works. Mouse support is complete and validated on an Elecom IST PRO.

---

## Build & flash from any machine (CI-first — no local toolchain)

You do **not** need this repo's local Docker build to make firmware. CI builds a
flashable `.uf2` on every push.

### Get a `.uf2` from CI

1. Edit config / code, commit, and `git push` (any branch).
2. GitHub → **Actions** → the **“Build ZMK firmware”** run for your commit.
3. Download the **artifact** (a zip). The build matrix is
   [`../build.yaml`](../build.yaml); it produces two variants:
   - **default** — the normal dongle firmware (`zmk.uf2`).
   - **`ble_hid_host_receiver-logging`** — same, plus USB-CDC serial logging
     (`-DCONFIG_ZMK_USB_LOGGING=y`). Use this one for on-device debugging: it
     prints the connect/bond/discover/decode flow over serial.

`build.yml` just calls ZMK's reusable `build-user-config.yml@main`; the repo is
both the zmk-config (`config/west.yml`) **and** the Zephyr module
(`zephyr/module.yml`, injected via `-DZMK_EXTRA_MODULES`). Board target is
**`xiao_ble/nrf52840/zmk`** (the ZMK board variant — *not* plain `xiao_ble`,
which is the raw-Zephyr board the M1 probe used).

### Flash the `.uf2` onto the XIAO nRF52840

1. **Double-tap the reset button** quickly (two presses within ~0.5 s). The
   `XIAO-SENSE` USB mass-storage drive mounts and the LED breathes.
   *(Single tap = a normal reboot, not the bootloader. 1200-baud-touch is
   unreliable on this hardware; the physical double-tap is the dependable way.)*
2. Drag-and-drop (or copy) the `.uf2` onto the `XIAO-SENSE` drive. A trailing
   I/O error on the copy is **normal** — the bootloader reboots on the final
   block.
   - On macOS, writing to a removable volume from a sandboxed shell can fail
     with *Permission denied*; do it from Finder or an un-sandboxed copy.

### Local Docker build (original Mac only — optional)

For a fast inner loop on the machine that has it set up:

```sh
WS=/Volumes/workspace/.zmk-blehh-build         # scratch (already west-init'd)
REPO=/path/to/zmk-ble-hid-host
docker run --rm -v "$REPO":/repo:ro -v "$WS":/ws -w /ws \
  zmkfirmware/zmk-build-arm:stable bash /ws/validate-both.sh
#   -> /ws/build/zephyr/zmk.uf2  and  /ws/build-log/zephyr/zmk.uf2 (logging)
```

---

## M4 — ZMK customization (config-only; do it in a separate config repo)

**Key finding (verified against the ZMK source, survived adversarial review):**
remapping a mouse button to a keyboard key, plus all axis/scroll tuning, is
achievable with **stock ZMK config only** — `zmk-ble-hid-host` needs **no code
change**. The module keeps publishing `INPUT_BTN_x` / `INPUT_REL_*`; ZMK's input
pipeline does the rest.

### Where to put it

Create a **separate, new public `zmk-config` repo** (do not touch `canon`, the
user's keyboard config). It pins this module and owns the keymap/overlay/conf:

- `config/west.yml` — import ZMK + add `akira-toriyama/zmk-ble-hid-host` as a
  module (pin a commit/tag).
- `build.yaml` — `board: xiao_ble/nrf52840/zmk`, `shield: ble_hid_host_receiver`.
- `.conf` — same Kconfig as
  [`boards/shields/ble_hid_host_receiver/ble_hid_host_receiver.conf`](../boards/shields/ble_hid_host_receiver/ble_hid_host_receiver.conf)
  (`ZMK_BLE=n` / `ZMK_USB=y` / `ZMK_POINTING=y` / `BT_CENTRAL=y` /
  `BT_SMP_SC_PAIR_ONLY=n` + the reconnect settings).
- keymap/overlay — attach `input-processors` to the module's `trackball_listener`.

The bundled `ble_hid_host_receiver` shield + `build.yaml` here are the template.

### Button → key (e.g. mouse button 4 → `a`)

Mouse buttons live in Zephyr's **input** subsystem; keys live in the **keymap**.
The bridge is the stock **`zmk,input-processor-behaviors`** processor (the mirror
of `&mkp`): it maps an incoming `INPUT_BTN_*` code to a keymap behavior. It is
enabled by default (`CONFIG_ZMK_INPUT_PROCESSOR_BEHAVIORS=y`, needs
`CONFIG_ZMK_POINTING=y`) and is proven by an in-tree `native_sim` test
(`app/tests/pointing/.../behaviors_basic`).

```dts
#include <zephyr/dt-bindings/input/input-event-codes.h>   /* INPUT_BTN_3 */
#include <dt-bindings/zmk/keys.h>                          /* A */
#include <behaviors.dtsi>                                  /* &kp */

/ {
    input_processors {
        zip_btn4_to_a: zip_btn4_to_a {
            compatible = "zmk,input-processor-behaviors";
            #input-processor-cells = <0>;     /* REQUIRED, or the DT build fails */
            codes    = <INPUT_BTN_3>;         /* mouse button 4 — verify the real code first! */
            bindings = <&kp A>;               /* codes.length == bindings.length (BUILD_ASSERT) */
        };
    };
};

&trackball_listener { input-processors = <&zip_btn4_to_a>; };
```

Notes / gotchas:
- **Verify the real button code first.** `ble_hid_host_publish` emits
  `INPUT_BTN_0 + i`; on device, only left/right have been confirmed. Flash the
  logging variant, press the physical button, and read the `buttons=0x..` /
  emitted code before fixing `codes`.
- On a match the processor returns `ZMK_INPUT_PROC_STOP` — the button is
  **consumed** (no longer a mouse click). That is the intended remap.
- Want it only on a layer? Use a listener **child node** with `layers = <N>;`.
- Pin the module's ZMK to a revision that includes the
  `input-processor-behaviors` feature (PR #2714, commit `cb867f9`). The archived
  community module `badjeff/zmk-input-behavior-listener` is **not** needed —
  the feature was upstreamed.

### Axis tuning (free, stock processors)

Invert / swap axes, scale, snipe (slow), and scroll are all stock
`input-processors` you attach to `trackball_listener` (see
<https://zmk.dev/docs/keymaps/input-processors>). No module change.

---

## M6 — Universal BLE-HID bridge (BT keyboard support)

**Goal:** plug in any BT keyboard (or mouse) and have it work over USB — first
as **passthrough** (type-through), remap later.

**What is already done:** the central side. Scan, connect, legacy/secure bond,
GATT discovery, Report-Map read, and per-report subscribe are all **HID-generic
and keyboard-capable today** — during M1 the central even mis-connected to a
nearby keyboard (Imprint Dongle, appearance `0x03c1`). So the transport is ~80%
of the way there; the remaining work is keyboard **interpretation + output**,
roughly the same size as the mouse path was.

### Work items

1. **Parse the keyboard collection** in
   [`drivers/input/hid_report_parser.c`](../drivers/input/hid_report_parser.c).
   Today the parser locates the first **pointer** collection (X/Y/wheel/buttons).
   Add extraction of a **keyboard** collection: the modifier byte (usage page
   `0x07`, usages `0xE0–0xE7`) and the keycode array (report-count × report-size
   of usage-page-`0x07` usages, usually 6 bytes). Reuse the same item-walking
   machinery (Global/Local/Main items, Report ID, etc.).
2. **Decode** the keyboard report → `{modifiers, keycodes[]}` (a new pure
   function alongside `zmk_hid_decode_report`, host-testable the same way).
3. **Output path — the open design question (research first, like M4 was).**
   Keys do **not** ride ZMK's input `INPUT_REL/BTN` path. Candidate approaches,
   cheapest first:
   - **Passthrough:** emit the received keyboard report as a **USB HID keyboard
     report** directly (modifiers + keycodes). Gets keys working, but bypasses
     the keymap (no per-key remap). Needs a way to send keyboard usages —
     investigate ZMK's HID/endpoints (`app/src/hid.c`, `app/src/endpoints.c`):
     either call ZMK's keyboard-HID API to set/clear usages, or have the module
     register its own USB HID keyboard interface.
   - **Keymap integration:** feed keys into ZMK's keymap/behavior layer for full
     remap. Non-obvious (kscan/keymap is a different subsystem); defer until
     passthrough works. *(Resolve this with a short feasibility investigation
     before coding — the same way M4's button→key path was de-risked.)*
4. **Pairing window** (cross-cutting; **required** before matching keyboards, or
   the central will grab the nearby Imprint keyboard). See below.

**DoD (M6):** a hand-held BT keyboard connected through the dongle types over USB.

### File pointers

- Central engine / discovery / subscribe: [`drivers/input/hog_central.c`](../drivers/input/hog_central.c)
- Report-Map parser + decoder (add keyboard here): [`drivers/input/hid_report_parser.c`](../drivers/input/hid_report_parser.c), `hid_report_decode.c`
- Publish (mouse today; keyboard output is new): [`drivers/input/ble_hid_host.c`](../drivers/input/ble_hid_host.c)
- Reference for the central pattern: `zmk/app/src/split/bluetooth/central.c`
- ZMK USB HID output: `zmk/app/src/hid.c`, `zmk/app/src/endpoints.c`

---

## Pairing model (cross-cutting)

Decided design (“recommended option C”): **known bonds always auto-reconnect**
(works today), but **new pairings are only accepted inside a triggered pairing
window** (e.g. hold a button at boot → N-second scan-to-pair). This keeps the
“put it in pairing mode and it connects” UX while **not** hijacking the nearby
Cyboard Imprint keyboard (`0x03c1`) — which is exactly the mis-match that bit M1.
A `device-name` allow-list is a secondary filter.

**Not yet implemented.** Today the central auto-connects to any mouse-appearance
or known bond, ignores others while connected, and has no forget/unpair UX. The
pairing-window mechanism becomes mandatory once M6 starts matching keyboards.
Relevant: `CONFIG_BT_MAX_PAIRED=2`, `CONFIG_BT_MAX_CONN=1`. Match logic lives in
`device_found()` / `ad_parse_cb()` in `hog_central.c`.

---

## Reference: capturing serial logs on macOS (gotchas we hit)

The logging variant prints over USB-CDC. To read it:

- **Port name changes when you reflash**, and a **ghost node** appears: one port
  streams live logs, the other only ever holds the 46-byte boot banner. Capture
  from **all** `usbmodem` ports at once and see which one streams while you move
  the mouse, e.g. (bash, not zsh — zsh does not word-split `$PORTS`):
  ```bash
  cat /dev/cu.usbmodem21101 > /tmp/a.log 2>&1 &
  cat /dev/cu.usbmodem21201 > /tmp/b.log 2>&1 &
  sleep 8; pkill -f 'cat /dev/cu.usbmodem'; wc -c /tmp/a.log /tmp/b.log
  ```
- `screen` works too, but the old macOS `screen` (v4) has no `-Logfile`; use a
  screenrc with `logfile /tmp/x.log`.
- A successful reconnect that **reuses the bond** logs `secured (level 2)` with
  **no** `paired (bonded=1)` — that is the proof there was no re-pairing.

See [`device-capture-macos.md`](./device-capture-macos.md) for the full
device-capture runbook (Report Map, fixtures, GO/NO-GO).
