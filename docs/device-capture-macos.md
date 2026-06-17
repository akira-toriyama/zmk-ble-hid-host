# Device capture & de-risk runbook (macOS) — Elecom IST PRO

Goal: **before** investing in firmware, confirm the IST PRO is a usable BLE HOGP
device and capture its HID Report Map + report bytes (which become the M2
decoder test fixtures). This is project phase **P-A** (see [HANDOFF.md](../HANDOFF.md)).

> **Read this first — what a macOS test can and cannot prove.**
> A green macOS result is **necessary but not sufficient**. macOS pairs with full
> IO capability and auto-resolves privacy addresses, so it can *disqualify* the
> device fast and capture every fixture, but it **cannot** confirm the
> embedded-specific risks below. Those require a headless Zephyr central
> (the XIAO running M1):
> - whether the device accepts a **NoInputNoOutput** central (macOS isn't one),
> - whether a **plain** central reconnects through a **Resolvable Private Address** (macOS auto-resolves via the OS),
> - whether it **re-advertises** so a fresh-booted central can reconnect unattended.
>
> So: P-A (macOS) = *fast disqualifier + fixture capture*. The XIAO M1 firmware
> is the *true* go/no-go gate. There is no fully firmware-free way to clear the
> reconnect / NoInputNoOutput risks.

## IST PRO specifics (verify against your physical unit)

- **Dial, not a button-cycle.** The IST PRO has a side **dial with 6 detented
  slots**: BT×2, 2.4 GHz×1, wired USB-C×1, custom×2 (the 2 custom slots can be
  set to BT → up to **4 BT bonds**). Each slot stores an **independent,
  persistent bond**. **Do the test on a FREE BT slot** so you don't overwrite an
  existing bond.
- **No auto-roam.** It only talks to the host on the **currently selected** dial
  slot. The future dongle is live only while the dial is on its slot — a design
  constraint to accept, not a defect.
- **Pairing mode:** turn the dial to a free BT slot, then **hold the pairing
  button on the bottom** until the lights change (~3–4 s per a single manual
  summary — *treat the exact time as unverified, just hold until it blinks*).
  **Slow blink, side LED BLUE + bottom light WHITE = BT pairing**. **ORANGE =
  2.4 GHz** pairing (wrong mode — re-check the slot).
- **Descriptor quirk risk:** ELECOM is known to reuse one report descriptor
  across models and **under-report buttons** (DEFT/HUGE expose 5 of 8). The 10
  programmable buttons may not all enumerate as standard HID buttons, or may emit
  vendor/consumer-page usages, without ELECOM's Windows/Mac "Mouse Assistant".
  The Linux `hid-elecom` fixups bind by USB VID/PID and **do not** apply over BLE.
- **Unknown until you capture it:** that it advertises standard HOGP at all
  (inferred from OS-pairing UX only), the report byte layout, Just-Works vs
  passkey, and whether it uses a Resolvable Private Address on reconnect.

## Part 1 — Pair + cursor test (any macOS, ~1 min, gross GO)

1. Dial to a **free BT slot**, enter pairing mode (above).
2. **System Settings → Bluetooth** → pair the "IST PRO" / "ELECOM…" device.
3. Move the ball, click each button, scroll/tilt → **does the cursor move?**

- ✅ moves → it's a real, pairable BLE HID device on a non-proprietary host. Gross GO.
- ❌ won't pair / no cursor → re-check slot is free + pairing mode (not a NO-GO yet; see traps).

Zero-install sanity check in Terminal:
```sh
hidutil list            # the IST PRO should appear as a HID device (note VendorID/ProductID)
```

## Part 2 — Capture the Report Map + report bytes (PacketLogger)

PacketLogger passively records macOS's own Bluetooth HCI, so it sees the bonded
HID session (Report Map read + the report notifications macOS receives) — the
OS-owns-HID problem doesn't apply.

**Tool:** *Additional Tools for Xcode* (free) →
<https://developer.apple.com/download/all/> → search "Additional Tools for
Xcode" → in the dmg's `Bluetooth/` you get **PacketLogger.app** and
**Bluetooth Explorer.app**.

1. Launch **PacketLogger.app** (live HCI capture starts).
2. **Remove** the IST PRO from System Settings → Bluetooth (so the next pair
   re-runs full discovery — the Report Map read only happens at discovery time).
3. With PacketLogger recording, **re-pair** the IST PRO.
4. Move/click **one action at a time** (ball +X, −X, +Y, −Y; each button down/up;
   wheel up/down; tilt L/R), pausing between.
5. **Save** the `.pklg`. Optionally open it in **Wireshark** (dissects BT ATT /
   HOGP cleanly).

What to find in the trace:
- **Report Map** = the ATT **Read Response** for characteristic UUID **0x2A4B**.
  Copy the full hex verbatim — this is the master fixture for the M2 decoder.
  *Sanity-check it isn't truncated* (ends on a complete HID item; if it looks cut
  mid-item, the read hit the ATT MTU — re-read / check MTU).
- **Input report** = ATT **Handle Value Notification** on the Report (0x2A4D)
  whose **Report Reference descriptor (0x2908)** byte[1] = **0x01 (Input)**.
  Record the **Report ID** (0x2908 byte[0]) and which bytes/bits change per action.
- **Address type** (in the LE advertising report / connection complete): note
  **Public / Static-random** vs **Resolvable Private Address (RPA)**. RPA is *not*
  a NO-GO, but it makes "firmware must store the peer IRK + enable address
  resolution to reconnect" a hard requirement.
- **Connection interval** (LE Connection Complete / Update): record it, but note
  it's *macOS's* negotiated value, not the dongle's.

Cross-check the descriptor without PacketLogger (handy):
```sh
# macOS exposes the BLE HID report descriptor as the IOHIDDevice "ReportDescriptor"
ioreg -r -c IOHIDDevice -l | grep -A2 -i "ReportDescriptor"
```
This hex should match the 0x2A4B Report Map.

## Part 3 — Reconnect test (the risk the gross test hides)

After pairing, **power-cycle the trackball** (or dial away from the slot and
back) and watch (PacketLogger + cursor):

- ✅ It **reconnects to macOS automatically** (cursor works again) **without
  re-pressing the pairing button** → bond persists and it re-advertises
  connectably. (Note: macOS auto-resolves RPAs, so this proving "macOS
  reconnects" does NOT prove a *plain* Zephyr central will — that's the XIAO test.)
- ❌ It only ever reconnects after **re-pairing** (button press), or in
  PacketLogger you see it **only directs advertising at macOS** and never returns
  to general/resolvable connectable advertising → reconnection-hostile (a real
  concern for an unattended dongle; confirm on the XIAO).

## What to record (→ becomes M2 fixtures)

Fill in [`tests/parser/fixtures/ist_pro_capture.template.md`](../tests/parser/fixtures/ist_pro_capture.template.md):
Report Map full hex · advertised name + Appearance (Mouse = 0x03C2) + **address
type** · Input report's Report ID + total length · per-action payload bytes (each
axis/button/wheel/tilt, isolated) · Protocol Mode (0x2A4E, expect 0x01) · whether
0x2A33 Boot Mouse Input Report exists · pairing method (Just Works vs passkey) ·
connection interval · multi-host result (other slots' bonds survived) · the saved
`.pklg`.

## GO / NO-GO

**GO (proceed to build M1) — all of:**
- Enters BT pairing mode and macOS **bonds** with an encrypted link (cursor works).
- Service **0x1812** present (after a post-bond discovery) with a readable
  **Report Map (0x2A4B)** and an Input **0x2A4D** (0x2908 byte[1]=0x01) that notifies.
- You **captured the full Report Map hex** (not truncated).
- **Address type recorded** (and if RPA, accept the IRK-resolution requirement).
- **Reconnect** works without re-pairing (macOS-level; full proof deferred to XIAO).
- *Pairing method is informational only* — a passkey on macOS is **not** a NO-GO
  (a NoInputNoOutput central negotiates Just Works regardless).

**NO-GO (rethink the approach) — any of:**
- Refuses generic BLE bonding on a free slot (vendor-dongle-locked).
- After a confirmed bond + fresh discovery, **0x1812 genuinely absent** / no Input report.
- On power-cycle it **only directs-advertises to macOS / forces re-pairing** every
  reconnect (blocks unattended reconnection) — verify on the XIAO.
- (Caution, verify on XIAO, not auto-NO-GO) it **pins a slow connection interval**
  and ignores update requests → may cap pointer smoothness.

**Cannot be decided on macOS — defer to the XIAO M1 test:**
- Accepts a **NoInputNoOutput** central / does **not** demand authenticated (MITM) pairing.
- A **plain** central reconnects through any **RPA** (needs stored IRK + address resolution).

## False-NO-GO traps (don't mistake these for a dead device)

- **No notifications until bonded** is spec-compliant (HID attributes are
  encryption-gated). macOS bonds automatically, so this mainly bites active GATT
  browsers — use PacketLogger (passive) and it's a non-issue.
- **Wrong dial slot / 2.4 GHz mode** (ORANGE blink) → move to a free BT slot.
- **Short button press does nothing** → must long-press to enter pairing.
- **Advertises with a blank/unexpected name** → identify by Appearance=Mouse
  (0x03C2) / service 0x1812, not by the name.
- **"Insufficient Authentication/Encryption" (ATT 0x05/0x0F)** on a read/notify =
  "bond first", not a bug.
- **Stale GATT cache** showing a partial table → force a fresh discovery after bonding.
- **Sluggish cursor** later on the dongle = connection-interval tuning, not device
  incompatibility.
