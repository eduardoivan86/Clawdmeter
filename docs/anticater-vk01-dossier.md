# ANTICATER VK-01 — Knob technical dossier

Reference material for **B2 — Knob Anticater BLE HID central** phase. The ESP32-S3 acts as BLE central, receives HID reports from the knob, maps them to Sonos commands directly (no wake of display required).

## Identity

- **Commercial name**: ANTICATER VK01 Desktop Knob Dual Mode Wireless Bluetooth Knob (also listed as `VK-01` or `ANTICATER_MINI` in BLE advertising)
- **Type**: Desktop knob with rotation + push, wireless connectivity for volume / media-key control

### This unit
- **Bluetooth name**: `ANTICATER_MINI`
- **Bluetooth MAC**: `EC:E1:67:F7:98:1F`
- **Vendor ID**: `0x05AC` (spoofed Apple)
- **Product ID**: `0x022C` (spoofed Apple Magic Keyboard)
- **Firmware**: `0.0.1`
- **Battery on first pair**: 94%

The spoofed Apple VID/PID is a common shortcut in cheap Chinese hardware so macOS/iOS accept it without driver prompts. Practical consequence for us: it behaves like a standard BLE HID Keyboard, not a proprietary protocol.

## Confirmed behaviour (macOS discovery)

`system_profiler SPBluetoothDataType` reports:
- Minor Type: `Keyboard`
- Services: `0x400000 < BLE >`

→ Pure BLE (not BT Classic). Compatible with ESP32-S3 (BLE-only).

## Default HID behaviour (BT mode)

When paired to a host, the knob emits BLE HID Consumer Control reports:

| Physical action | Usage Page | Usage ID | Typical host effect |
|---|---|---|---|
| Rotate CW | Consumer Control (0x0C) | 0xE9 (Volume Increment) | Volume +1 step |
| Rotate CCW | Consumer Control (0x0C) | 0xEA (Volume Decrement) | Volume -1 step |
| Press (click) | Consumer Control (0x0C) | 0xE2 (Mute) OR 0xCD (Play/Pause) | Varies by firmware |
| Long press | TBD | TBD | Depends on firmware |

Press behaviour (mute vs play/pause vs other) is **not documented** — confirm by dumping live HID reports during B2.4.

## Operating modes

The VK-01 has 3 connection modes (physical switch on side):
1. **2.4G wireless** (USB-A dongle) — N/A, no dongle
2. **Bluetooth** — the mode we use, BLE HID
3. **USB-C wired** — N/A

Confirm switch is on BT before B2.1 work — in 2.4G or wired, knob does not advertise via BLE.

## Companion app (Windows/macOS)

Vendor ships a companion app to remap each physical action (keystroke / macro / scroll / brightness / etc.). For B2 we recommend **leaving defaults** (volume up/down/mute) — that's what HID centrals expect to parse.

## Expected GATT services (to confirm in B2.1 via nRF Connect)

| Service UUID | Name | Why it matters |
|---|---|---|
| 0x1812 | HID Service | Where input reports live — subscribe here |
| 0x180A | Device Information | Manufacturer, model, fw version |
| 0x180F | Battery Service | Battery % (useful for future ESP32 indicator) |
| 0x1800 | Generic Access | Name, appearance |
| 0x1801 | Generic Attribute | Service-changed notifications |

### HID Service characteristics we'll use

| Char UUID | Name | Notes |
|---|---|---|
| 0x2A4D | HID Report | Input report — subscribe via NOTIFY |
| 0x2A4B | Report Map | Defines report format — read once on connect |
| 0x2A4A | HID Information | HID version, country code |
| 0x2A4C | HID Control Point | Suspend / exit suspend |
| 0x2A4E | Protocol Mode | Report vs Boot mode |

## Expected report format

Consumer Control 16-bit usages typically arrive as 2 bytes:
```
byte[0] = LSB of usage code
byte[1] = MSB of usage code
```

Examples:
- Volume Up (0xE9): `[0xE9, 0x00]`
- Volume Down (0xEA): `[0xEA, 0x00]`
- Mute (0xE2): `[0xE2, 0x00]`
- Release: `[0x00, 0x00]`

Some devices send bitmap or packed reports with a modifier byte — dump first notifies with `ESP_LOG_BUFFER_HEX` to confirm real format.

## End-state architecture (post-B2)

```
Knob (BLE peripheral)              ESP32 (BLE central + WiFi)              Sonos
   ↓                                       ↓                                    ↑
   Rotate CW
   Notify byte[0]=0xE9, byte[1]=0x00
   ─────────────────────────────→
                                          Parse: Volume Up
                                          Accumulate delta: +1
                                          ───(throttle 100ms)───
                                          Flush: setVolume(IP, current+2)
                                          ─────────────────────────────────→
                                                                              Volume up
```

## Constraints and considerations

- **Unpair from Mac first** — if knob stays paired to Mac, Mac intercepts notifies and ESP32 sees nothing.
- **Auto-reconnect** required — if knob sleeps (inactivity), ESP32 must detect disconnect and re-scan.
- **Throttle critical** — fast rotation can emit 30-50 events/s; without throttle, the Sonos endpoint saturates.
- **Knob battery** — when depleted, knob disappears from scan. Not an ESP32 bug.
- **Press is unknown** — until reports are dumped, we don't know if it's mute / play-pause / both. Design the handler to map arbitrarily.

## Open design decisions (to resolve at B2 start)

### Rotation → Sonos volume
- Step per click? Recommended: ±2 (0-100 scale, useful granularity).
- Acceleration based on speed? Nice-to-have, not in B2 initial.

### Press → ?
- Option 1: Toggle mute
- Option 2: Toggle play/pause
- Option 3: Map by real HID code emitted

### Which speaker the knob controls
- (A) Active room of `SCREEN_SONOS` (follows UI) — **recommended**
- (B) Fixed assigned room
- (C) In `MODE_BOTH`, controls both (same as touch slider)

### Feedback on Sonos failure
- No knob LEDs / no speaker on the knob → feedback only on ESP32 screen.

## Required Eduardo prep before B2 work

1. **Unpair knob from Mac**: Settings → Bluetooth → (i) next to `ANTICATER_MINI` → Forget Device
2. **Switch in BT mode** (not 2.4G, not wired)
3. **Charge battery** before debug session
4. Keep knob within ~2m of ESP32 during debug

## Unknowns to resolve in B2.1 (nRF Connect dump, ~30 min)

- Exact HID report format (assume 2-byte Consumer Control, verify)
- Press = mute or play-pause
- Long-press / double-press availability
- Companion-app remap state
- Knob battery drain rate with persistent BLE connection

## ESP32 firmware notes (for B2 author)

- LVGL / touch don't participate
- NimBLE-Arduino with `CONFIG_BT_NIMBLE_ROLE_CENTRAL=1` — verify in `platformio.ini` (currently `=0`)
- `NimBLEScan` + `NimBLEAdvertisedDeviceCallbacks` to discover knob by MAC
- `NimBLEClient` + `NimBLERemoteService` + `NimBLERemoteCharacteristic` to connect to HID service
- `subscribe()` with callback for notify reports
- **Heap watch** continuous — dual-role NimBLE eats memory
- **Knob handler must NOT call `idle_consume_wake_press()`** — by design, knob input is silent (changes Sonos without waking display)

## Interaction with B3a display sleep

The display can be off (after long-press PWR or eventual auto-sleep). Knob handler should:
- Read Sonos state, send commands, no display interaction
- Not call `idle_*` functions — knob input is "silent input"
- Optionally: if `wifi_is_connected()` is false, skip silently and don't queue

Result: user listens to music with display dark, turns knob → volume changes immediately, display stays off.
