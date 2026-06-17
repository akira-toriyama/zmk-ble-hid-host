# probe/ — XIAO HOGP receive probe (M1)

A standalone **Zephyr** app for the **Seeed XIAO nRF52840** (`xiao_ble`) that acts
as a BLE **HID-over-GATT (HOGP) host**: it scans for the **Elecom IST PRO**
trackball, bonds, subscribes to every HID input report, and **hexdumps each
report over USB serial**. It reconnects to the bonded peer automatically.

This is a **diagnostic probe** — milestone **M1 ("receive only")** of this
project. It is **not** ZMK. It de-risks the BLE link before the ZMK module is
built and confirms what only a real headless central can:

- the IST PRO bonds with a **NoInputNoOutput / Just Works** central,
- HID report notifications actually flow,
- it **reconnects** after a power-cycle (incl. through a Resolvable Private Address),
- the live report bytes / connection interval.

The HOGP discovery code here ports into the ZMK module afterwards.

## Build

### Option A — Docker (recommended, no SDK install)

Needs only Docker. First run pulls the image (~6.5 GB) + clones Zephyr (cached after):

```sh
cd probe
./scripts/build.sh            # -> probe/firmware/zephyr.uf2
```

### Option B — GitHub Actions

Pushing changes under `probe/` runs the **probe-build** workflow. Download the
**`probe-uf2`** artifact (contains `zephyr.uf2`) from the
[Actions](../../../actions) run. Same Zephyr v4.1.0 + SDK 0.17.0 as Docker.

## Flash

The XIAO uses the Adafruit UF2 bootloader:

1. Plug the XIAO into the Mac via USB-C.
2. **Double-tap the reset button** → a USB drive mounts (e.g. `XIAO-SENSE`).
3. **Drag `zephyr.uf2` onto that drive.** The board reboots into the app.

## View the logs (macOS)

```sh
ls /dev/tty.usbmodem*                  # find the port (appears after boot)
screen /dev/tty.usbmodem<XXXX> 115200  # open it (baud nominal for USB CDC)
# quit screen: Ctrl-A, K, y
```

Opening the port releases the probe's ~10 s startup wait, so logs start flowing.

### What success looks like

```
=== XIAO HOGP probe ===
bluetooth enabled
scanning for a HOGP pointer (Elecom IST PRO)...
target XX:XX:.. (rssi -47) — connecting
connected: XX:XX:..
paired (bonded=1)
secured (level 2) — starting GATT discovery
HID service: handles 14..40
subscribed report value=17 ccc=18
discovery done: subscribed to N report(s)
HID report
                 02 00 05 00 00 00 00      |..      <-- move the ball / click
```

Move the ball / press buttons / scroll — each action emits a `HID report`
hexdump. Compare against the captured descriptor in the parent repo
(`tests/parser/fixtures/ist_pro_report_descriptor.md`: Report ID 2 = buttons,
X int16, Y int16, wheel, AC-pan).

### Reconnect test

Power-cycle the trackball (or toggle its dial off the slot and back). The probe
should re-`connected` / `secured` / `subscribed` **without re-pairing**. If it only
reconnects after re-pairing, that's a finding to record.

## Pinned versions

Board `xiao_ble`; toolchain `arm-zephyr-eabi`; Zephyr **v4.1.0**; Zephyr SDK
**0.17.0** (0.17.1+ breaks multithreaded v4.1.0 builds).
