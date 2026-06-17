# ELECOM IST PRO — HID Report Map (real capture)

**Provenance:** captured 2026-06-18 on macOS (Mac mini) via
`ioreg -r -c IOHIDDevice -l` while the IST PRO was connected over Bluetooth.
macOS reads the BLE HID **Report Map (GATT 0x2A4B)** and exposes it verbatim as
the IOHIDDevice `ReportDescriptor`, so this hex **is** the BLE Report Map.

- Product: `ELECOM IST PRO`
- VendorID: `1390` = `0x056E` (ELECOM), VendorIDSource `2` (USB-IF)
- ProductID: `394` = `0x018A`
- Raw bytes: [`ist_pro.report_map.hex`](./ist_pro.report_map.hex)

> This confirms the IST PRO is a **standard HOGP / HID** device exposing a clean,
> decodable pointer report — a strong GO for the project. Decoded and verified
> with a HID item walker (not hand-read).

## Reports in this descriptor

| Report ID | Collection            | Notes                                              |
| --------- | --------------------- | -------------------------------------------------- |
| **2**     | **Mouse / Pointer**   | **what we remap** — see byte map below             |
| 1         | Keyboard              | programmable buttons emitting keys (8 mod + 6 keys + 152-bit usage bitmap; LED output) |
| 6         | Consumer control      | 16-bit usage (media keys)                          |
| 4         | System control        | power/sleep etc.                                   |
| 9 / 5     | Vendor (0xFF00)       | ELECOM "Mouse Assistant" config (in=16B / feature=63B) |

## Pointer report — Report ID 2 (the M2 target)

Total 8 bytes including the Report ID byte:

| byte  | field            | type / range                          |
| ----- | ---------------- | ------------------------------------- |
| 0     | Report ID = 0x02 | u8 — **may be omitted in the BLE notification payload** (the 0x2A4D characteristic's Report Reference descriptor carries the ID); confirm in M1 |
| 1     | buttons 1–8      | bitfield (bit0 = button1 = left)      |
| 2–3   | X                | int16, **little-endian**, −32767..32767 (Rel) |
| 4–5   | Y                | int16, little-endian (Rel)            |
| 6     | wheel            | int8, −127..127 (Rel)                 |
| 7     | AC Pan (horizontal / tilt) | int8 (Rel)                  |

Verified item stream (mouse collection):
`Usage Page Generic Desktop → Usage Mouse → Report ID 2 → Pointer →
Buttons 1..8 (8×1bit) → X,Y (2×16bit signed Rel) → Wheel (8bit signed Rel) →
Consumer AC Pan (8bit signed Rel)`.

## For M2

- Parse [`ist_pro.report_map.hex`](./ist_pro.report_map.hex) → expect the layout
  above for Report ID 2 (this is the host-test assertion).
- **Still to confirm on-device (M1 raw-report logging):** whether the BLE Report
  (0x2A4D) notification payload includes the Report ID byte or not, and the
  actual byte order in practice. M1's firmware will `LOG_HEXDUMP` real reports —
  that capture replaces the need for a macOS PacketLogger session for the bytes.
