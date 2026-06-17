# IST PRO capture record (fill in during P-A)

Fill this from the macOS capture (see `docs/device-capture-macos.md`). Once
complete, this becomes the ground truth for the M2 decoder host tests.

## Device / link
- Advertised name: `____`
- GAP Appearance: `____` (expect Mouse = 0x03C2 / 962)
- Address type: `public | static-random | resolvable-private (RPA)`  ← affects reconnect
- Pairing method: `Just Works (no/plain prompt) | passkey | numeric-compare`
- Bond succeeded: `yes | no`
- Negotiated connection interval (macOS): `____ ms` (note: macOS's value, not the dongle's)
- Reconnect after power-cycle WITHOUT re-pairing: `yes | no` (+ directed/undirected if known)

## HID structure
- Service 0x1812 present: `yes | no`
- Protocol Mode (0x2A4E): `0x01 Report | 0x00 Boot`
- Boot Mouse Input Report (0x2A33) exists: `yes | no`
- Input Report (0x2A4D, 0x2908 byte[1]=0x01): Report ID = `____`, total length = `____` bytes

## Report Map (0x2A4B) — full hex (verbatim, not truncated)
```
<paste hex here>
```

## Per-action report bytes (isolate one action at a time)
| action        | full report hex | byte/bit that changed |
|---------------|-----------------|-----------------------|
| +X (right)    |                 |                       |
| -X (left)     |                 |                       |
| +Y (down)     |                 |                       |
| -Y (up)       |                 |                       |
| button 1 down |                 |                       |
| button 1 up   |                 |                       |
| ... (each button, up to 10) | | |
| wheel up      |                 |                       |
| wheel down    |                 |                       |
| tilt left     |                 |                       |
| tilt right    |                 |                       |

## Artifacts
- PacketLogger `.pklg` saved at: `____`
- `ioreg` ReportDescriptor matches 0x2A4B: `yes | no`
