# zmk-ble-hid-host

A [ZMK](https://zmk.dev) module that turns an nRF52840 into a **BLE HID host**:
it connects to a Bluetooth pointing device (HOGP / HID-over-GATT) as a BLE
**central**, decodes its HID reports, and republishes them into ZMK's input
subsystem — so the movement/buttons can be remapped by ZMK's standard
`input-processors` and sent to the PC over USB.

In one line: **the receive half of an HID-Remapper, built as a ZMK module —
remapping and output are left entirely to ZMK core.**

> **Status: work in progress.** The module skeleton, the pure decode contract,
> and host-side tests are in place and green. The BLE HOGP central and the
> report decoder are not implemented yet, and nothing has been validated on
> real hardware. See **[Project status](#project-status)** and
> [`HANDOFF.md`](./HANDOFF.md) for exactly what is and isn't done.

Primary use case: a Seeed XIAO nRF52840 dongle bridging an **Elecom IST PRO**
trackball (BLE) to a PC over USB.

## Architecture

Responsibility split is strict (this is the whole design):

| Stage  | Owner            | What                                                            |
| ------ | ---------------- | -------------------------------------------------------------- |
| ingest | **this module**  | BLE HOGP central → GATT discovery → decode → `input_report_*`  |
| remap  | **ZMK core**     | `zmk,input-listener` + `input-processors` (invert/scale/scroll/snipe) |
| output | **ZMK core**     | USB (or BLE) HID                                               |

The module never remaps. It just receives reports and publishes input events;
ZMK's composable parts do the rest. (This is the architecture the ZMK
maintainer recommends in [issue #2395](https://github.com/zmkfirmware/zmk/issues/2395).)

```
 BLE trackball ──HOGP/GATT notify──▶ [ this module ]
                                       scan/connect/discover/subscribe
                                       decode report (report-map driven)
                                       input_report_rel/key()
                                                │
                                                ▼  Zephyr input subsystem
                                     [ ZMK core: input-listener ]
                                       input-processors (remap)
                                                │
                                                ▼
                                     [ ZMK core: USB HID ] ──▶ PC
```

## Hardware

- **MCU:** Seeed XIAO nRF52840 — ZMK board target `seeeduino_xiao_ble`
  (UF2 bootloader: double-reset to mount the drive, drag-drop the `.uf2`).
- **Input:** Elecom IST PRO, connected over **BLE** (HOGP). 2.4 GHz is not an
  option — the nRF52840 USB is device-only (no USB host) and its proprietary
  2.4 GHz protocol can't be received.
- **Output:** USB (the XIAO stays plugged into the PC as a dongle). Output is
  USB, not BLE, to minimise latency.

## Repository layout

```
zmk-ble-hid-host/
├── zephyr/module.yml                       # Zephyr module manifest (driver type)
├── CMakeLists.txt / Kconfig                # module roots
├── drivers/input/
│   ├── ble_hid_host.c                      # virtual input device  [M0 ✓]
│   ├── hog_central.c                       # scan/connect/discover/subscribe  [M1]
│   ├── hid_report_parser.c                 # Report Map → field layout  [M2]
│   └── hid_report_decode.c                 # raw report → {dx,dy,buttons,wheel}  [M2]
├── include/zmk_ble_hid_host/
│   └── hid_report_parser.h                 # pure, Zephyr-free decode contract  [M0 ✓]
├── dts/bindings/input/zmk,ble-hid-host.yaml
├── tests/parser/                           # host unit tests (plain cc, no Zephyr)  [M0 ✓]
├── config-example/                         # files to copy into your zmk-config
├── .github/workflows/hosttest.yml          # CI: host tests  [M0 ✓]
└── HANDOFF.md                              # cross-session status / next steps
```

## Project status

Experiment-driven milestones (from the project brief):

- [x] **M0 — scaffold + CI base.** Module skeleton, DTS binding, virtual input
  device registration, pure decode contract, host-test harness, host-test CI.
- [ ] **M1 — receive (the hardest gate).** Scan → connect to the IST PRO →
  GATT discovery (HID 0x1812 → Report 0x2A4D / Report Map 0x2A4B / Protocol
  Mode 0x2A4E) → subscribe → log raw report bytes. Central+peripheral
  coexistence, bonding/reconnect, 7.5 ms connection-interval request.
- [ ] **M2 — decode.** Report-Map-driven parser + decoder (pure, host-tested).
- [ ] **M3 — input device.** Decoded values → `input_report_rel/key` with
  button edge tracking.
- [ ] **M4 — wiring.** `input-listener` + `input-processors` in your zmk-config.
- [ ] **M5 — dongle.** Case, daily use.

What is **not** verified yet: anything requiring the physical device. See the
[device capture](#section-8-capturing-the-ist-pro-hid-reports) step and
`HANDOFF.md`.

## Installing into your zmk-config

ZMK core is **not** forked — you consume this as a module from your own
`zmk-config`. See [`config-example/`](./config-example) for copy-paste snippets:

1. **`config/west.yml`** — add this module as a project
   (`config-example/west.yml.snippet`).
2. **board/shield overlay** — add the `zmk,ble-hid-host` node + an
   `input-listener` (`config-example/seeeduino_xiao_ble.overlay`).
3. **keymap** — attach `input-processors` to the listener
   (`config-example/ist_pro.keymap.snippet`).
4. **`.conf`** — enable the required Kconfig
   (`config-example/zmk-config.conf.snippet`).

<details>
<summary><b>Section 8: Capturing the IST PRO HID reports (do this before M2 tuning)</b></summary>

The IST PRO's HID report byte layout is **not publicly documented**. The
decoder is designed to derive the layout at runtime from the device's HID
Report Map, but you should still capture a ground-truth sample to (a) confirm
the device is plain HOGP and (b) build decoder test fixtures.

Using **nRF Connect for Mobile** (iOS/Android):

1. Put the IST PRO into BLE pairing mode and **Connect** from nRF Connect.
2. Expand the **Human Interface Device** service (UUID `0x1812`).
3. Record the **Report Map** characteristic (`0x2A4B`) — read its value and
   copy the full hex. This is the descriptor the decoder parses.
4. For each **Report** characteristic (`0x2A4D`), enable notifications (tap the
   triple-arrow / "Subscribe" icon).
5. Move the ball and press each button **one action at a time**, noting which
   bytes change for: X, Y, wheel, tilt/AC-pan, and each button. Save the logs.
6. Note the **Protocol Mode** (`0x2A4E`) and the pairing method
   (Just Works vs passkey), and whether the device accepts a 7.5 ms connection
   interval (visible in the connection parameters).

Drop the captured Report Map hex and a few annotated report samples into
`tests/parser/` as fixtures so the decoder can be unit-tested against the real
device without hardware in the loop.

</details>

## Building & testing

**Host unit tests (no hardware, no Zephyr toolchain)** — the report parser and
decoder are pure functions, so they're tested with a plain C compiler:

```sh
make -C tests/parser test
```

This runs in CI on every push (`.github/workflows/hosttest.yml`).

**Firmware build** — done from your `zmk-config` via ZMK's normal GitHub
Actions build (or a local `west build -b seeeduino_xiao_ble`). A dedicated
firmware-build CI for this repo is planned for M1 (see `HANDOFF.md`).

<details>
<summary><b>Design notes</b></summary>

- **Why raw Zephyr GATT, not Nordic's `bt_hogp`.** ZMK builds on upstream
  Zephyr, not the nRF Connect SDK. NCS's `bt_hogp` is tied to NCS-only
  infrastructure (`gatt_dm`, NCS settings), so it isn't portable here. The
  HOGP central is reimplemented with Zephyr's GATT client primitives
  (`bt_gatt_discover` / `bt_gatt_subscribe`), modeled on ZMK's own split
  central code (`app/src/split/bluetooth/central.c`).
- **Why a runtime Report-Map parser.** Rather than hardcoding the IST PRO's
  byte layout (unknown, and device-specific), the decoder parses the HID
  Report Map at connect time to locate the X/Y/buttons/wheel fields. This works
  for any HOGP mouse and keeps the device-specific knowledge out of the code.
  A HID Boot Protocol fallback covers devices whose report map can't be parsed.
- **Why the decode core is Zephyr-free.** Report-map parsing and report
  decoding are the parts most likely to have bugs and the parts that can't be
  exercised without a device. Keeping them as pure functions over byte arrays
  makes them unit-testable on the host — the real correctness gate.

</details>

## References

- ZMK pointing / input-processors: <https://zmk.dev/docs/features/pointing>,
  <https://zmk.dev/docs/keymaps/input-processors>
- ZMK split keyboards (central template): <https://zmk.dev/docs/features/split-keyboards>
- Design origin / maintainer hints: <https://github.com/zmkfirmware/zmk/issues/2395>
- Virtual-input reference: <https://github.com/badjeff/zmk-split-peripheral-input-relay>
- XIAO nRF52840 (ZMK board `seeeduino_xiao_ble`): <https://zmk.dev/docs/hardware>

## License

MIT — see [LICENSE](./LICENSE).
