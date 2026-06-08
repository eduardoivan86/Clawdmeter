# Clawdmeter fork — project status

Last update: 2026-06-08. Branch: `feat/sonos-control` (pushed to `origin`, fork: `eduardoivan86/Clawdmeter`).

This document is the canonical bookmark between sessions. Reading it should give a complete picture of the state of the fork without needing to dig through git log or chat history.

> ⚠️ **HARDWARE STATUS (2026-06-08): the 2.16 unit is DEAD.** Eduardo's
> Waveshare AMOLED-2.16 powered off the instant it was plugged into a faulty
> coil-whining USB-C wall charger and never came back. Diagnosis: screen dark,
> **zero USB enumeration even in BOOT/ROM download mode**, persists after a full
> PMU power-drain reset → electrical damage to the AXP2101 PMU / input regulator
> from an overvoltage event. Not software-recoverable. Needs a replacement board
> (same model) + reflash from this branch, then clear the daemon's cached BLE MAC
> (factory-burned per chip). **On-hardware verification of the 2026-06-08 commits
> below is deferred until a replacement board arrives.**

## Hardware

- Waveshare ESP32-S3-Touch-AMOLED-2.16 (PlatformIO env: `waveshare_amoled_216`)
- AMOLED 480×480 CO5300, touch CST9220, PMU AXP2101, IMU QMI8658
- 8 MB PSRAM, 16 MB Flash
- BLE MAC `28:84:85:55:65:59`, WiFi IP `192.168.86.40` (DHCP)

Eduardo's unit currently has **no battery connected** — only USB power.

## Stack

- pioarduino platform-espressif32 55.03.38-1 → Arduino-ESP32 ~3.x
- LVGL 9.5 (Montserrat 28 enabled for `LV_SYMBOL_*` glyphs)
- NimBLE-Arduino 2.1.1 (dual role: peripheral for OS HID + daemon, central for knob)
- rupakpoddar/Sonos 1.0.0
- WiFi + BLE coexist OK via Arduino-ESP32 default sdkconfig (`CONFIG_SW_COEXIST_ENABLE=y`)
- `CORE_DEBUG_LEVEL=3` for ESP_LOGI visibility
- `LV_FONT_MONTSERRAT_28=1` for transport button glyphs

## Phase status

### Fase 0 — Setup ✅ pushed
- Fork: `github.com/eduardoivan86/Clawdmeter`
- Working dir: `~/dev/Clawdmeter`
- Branch: `feat/sonos-control`
- `firmware/src/secrets.h` (gitignored) holds WiFi creds + Sonos IPs + RINCON UUIDs
- `firmware/src/secrets.example.h` committed as template

### Fase B1.1 — WiFi ✅ pushed (`3958016`)
- `firmware/src/wifi_manager.{h,cpp}`: `wifi_init`, `wifi_is_connected`, `wifi_get_ip`, `wifi_loop`
- Non-blocking auto-reconnect every 10 s if link drops
- WiFi status text label on Bluetooth screen (`"WiFi: 192.168.86.40"`)

### Fase B1.2 — Sonos ✅ pushed (`98e4622`)
- `firmware/src/sonos_controller.{h,cpp}`: wrapper over `rupakpoddar/Sonos` with:
  - `bool` public API (internally maps `SonosResult::SUCCESS` via `ok()` helper)
  - `SemaphoreHandle_t sonos_mutex` serializes every SOAP call (poll task + touch handlers + grouping ops would collide on internal HTTPClient state)
  - `SonosMode` enum: `MODE_SINGLE_LR`, `MODE_SINGLE_MOVE`, `MODE_BOTH`
  - `active_target_ip()` returns coordinator IP (Living Room) when `MODE_BOTH`
  - 5 s polling task pinned to core 0, refreshes volume when `SCREEN_SONOS` is active
- `firmware/src/sonos_grouping.{h,cpp}`: custom SOAP (AVTransport `SetAVTransportURI` join / `BecomeCoordinatorOfStandaloneGroup` leave). Kept in its own translation unit to avoid the `ipc1` stack canary panic that the rest of the grouping code triggered when colocated.
- `firmware/src/screen_sonos.{h,cpp}`: new LVGL screen
  - Tappable header cycles SINGLE_LR ↔ SINGLE_MOVE (BOTH only via long-press selector)
  - Long-press header (~400 ms) opens overlay with 3 buttons: Solo Living Room / Solo Sonos Move / Ambos (grupo)
  - Volume slider throttled — `set_volume` only fires on `LV_EVENT_RELEASED`
  - Mute button toggles `LV_SYMBOL_VOLUME_MAX` (dim) ↔ `LV_SYMBOL_MUTE` (red)
- `ui.cpp` cycle: USAGE → BT → SONOS → USAGE (no splash in PWR rotation; splash entered via tap)
- Logo + battery icon hidden on SCREEN_SONOS (was leaking from parent screen)

### Fase B3a — Display sleep + battery widget ✅ pushed (`737a6d1`)
- `power_hal_pwr_long_pressed()` API in `hal/power_hal.h`, implemented in `boards/waveshare_amoled_216/power.cpp`:
  - AXP2101 `XPOWERS_AXP2101_PKEY_LONG_IRQ` enabled
  - `setPowerKeyPressOnTime(XPOWERS_POWERON_2S)` — 2 s hold threshold
  - Long-press IRQ suppresses the short-press latch in the same poll tick (no double-event)
- `idle_force_sleep()` bypasses the `IDLE_SLEEP_WHEN_CHARGING=false` gate
- `main.cpp` PWR block: long-press → `idle_force_sleep()`
- Battery widget on BT screen (`p_info` panel, y=168, `font_styrene_20`), 30 s `lv_timer`, adaptive:
  - USB + no battery: `"USB +"` green
  - Charging: `"Bat: NN% +"` green
  - Battery only: `"Bat: NN%"` — dim ≥20 %, amber 10–19 %, red <10 %
- `bt_info_panel_h` bumped 160→200 (large) / 140→180 (compact) to fit the new label
- `docs/anticater-vk01-dossier.md` saved as reference for B2

### Phase A (B2 prep) — NimBLE CENTRAL + knob scanner skeleton ✅ pushed (`eb6bb9e`)
- `platformio.ini`: `CONFIG_BT_NIMBLE_ROLE_CENTRAL=1`, `OBSERVER=1`, `MAX_CONNECTIONS=3`
- `firmware/src/knob_scanner.{h,cpp}`: FreeRTOS task (4 KB stack, prio 1, core 0)
  - Continuous scan filtered by MAC `EC:E1:67:F7:98:1F`
  - Connect → discover `0x1812` HID Service → subscribe to every notify-able `0x2A4D` HID Report char
  - Notify callback dumps raw bytes via `ESP_LOG_BUFFER_HEX` (B2.1 instrumentation — replace with parser in B2.2)
  - Auto-reconnect on disconnect (resume scan)
- `knob_scanner_init()` called from `main.cpp setup()` after `sonos_ctrl_init()`
- Heap cost: ~5 KB (free heap dropped from 44 KB to 39 KB)

### Phase B (boot stack canary fix) ✅ pushed (`24fb623`)
- `gpio_install_isr_service(0)` called at the top of `setup()` (before any HAL init).
- This pre-allocates the heavy `esp_intr_alloc` → `heap_caps_malloc` path on the main task (4 KB stack), so the touch driver's later cross-core GPIO ISR registration on the `ipc1` task (1 KB stack baked into the precompiled IDF, `CONFIG_ESP_IPC_TASK_STACK_SIZE=1024`) no longer overflows the stack canary.
- Validated: **5/5 boot cycles clean, 0 panics** (was previously 0–21 panics per boot depending on binary layout).
- Root cause documented but not patched at IDF level (would require recompiling Arduino-ESP32 framework with `custom_sdkconfig CONFIG_ESP_IPC_TASK_STACK_SIZE=2048`; out of scope).

### Phase C (NVS WiFi credential storage + AP setup mode) ✅ pushed (`5b09a05`)
- `wifi_manager.{h,cpp}` extended:
  - `wifi_init()` now reads `Preferences` namespace `wifi` keys `ssid` / `pass`; falls back to `WIFI_SSID` / `WIFI_PASS` macros from `secrets.h` if NVS empty
  - `has_real_creds()` detects empty / `"REPLACE_ME"` placeholders
  - `wifi_force_setup_mode()`: WiFi softAP `"Clawdmeter-Setup"` + `WebServer` on `http://192.168.4.1` with a styled HTML form. POST `/save` writes NVS and reboots. Blocks forever (does not return).
  - Auto-trigger only if no valid credentials anywhere — Eduardo's existing flow preserved (his `secrets.h` macros are real)
- Heap cost: ~5 KB (WebServer + Preferences linked even when not actively serving)

### Phase D (final validation + status doc) ✅ this commit
- 5/5 boot cycles clean (`/tmp/final_5cycle.log`)
- Heap stable at ~38 KB free after full init (above 25 KB threshold)
- This document

### Status indicator — stale-data / lost-connection banner ✅
- `ui.cpp`: global banner on `lv_layer_top()` (floats over all screens). Armed
  only after the first fresh payload (`ever_received`), so a fresh boot never
  false-alarms.
- `ui_note_data_fresh()` stamps `last_fresh_ms` on every OK parse (`main.cpp`).
- `ui_tick_status()` (per loop): BLE not connected → red `SIN CONEXION`;
  connected but >3 min since last payload → amber `DATOS VIEJOS - ABRI CLAUDE`;
  else hidden. Threshold `STALE_THRESHOLD_MS = 180000`.
- Verified on device: OK / STALE / DISCONNECTED / recovery screenshots.

## Current memory footprint (post-D)

```
[wifi] connected, IP: 192.168.86.40                  ~2.4 s
[sonos] begin() -> 0,         free heap=48 260       baseline
[sonos] poll task create -> 1,free heap=43 532       -4.7 KB (4 KB stack + overhead)
[knob] task create -> 1,      free heap=38 776       -4.7 KB (NimBLE central + 4 KB stack)
[ui-sonos] init done,         free heap=38 420       -0.3 KB (LVGL widgets)
```

Net free after init: **~38 KB**. Margin to 25 KB threshold: ~13 KB.

## Known issues

1. **`ipc1` stack canary** — still on the Phase B workaround
   (`gpio_install_isr_service(0)` pre-install in `setup()`, empirically 5/5 clean
   boots). The root-cause fix `custom_sdkconfig CONFIG_ESP_IPC_TASK_STACK_SIZE=2048`
   was attempted 2026-06-08 and **reverted**: enabling custom_sdkconfig forces a
   from-source ESP-IDF framework rebuild that fails to link on this platform
   (55.03.38-1) with `undefined reference to __wrap_log_printf` (Arduino WiFi log
   shim). To retry: resolve that link issue (likely needs a matching log/wrap
   sdkconfig entry or a newer pioarduino release) AND validate 5/5 boots on a live
   board. Config is left commented in `platformio.ini` with the full note.

2. **LV_EVENT_GESTURE doesn't fire on CST9220 driver in LVGL 9.5**: the swipe-down gesture intended for the Sonos selector never triggered. Replaced by long-press on the header. Gesture code removed (was dead). Re-evaluate on LVGL upgrade or touch driver swap.

3. ~~**Layout not responsive to AMOLED-1.8 board**~~ — RESOLVED 2026-06-08. The
   SONOS screen positions now live in a `SonosLayout` struct in `screen_sonos.cpp`
   with a compact (368×448) branch. The usage/BT screens were already responsive
   via `compute_layout()`. Compile-verified on both envs; visual QA on the 1.8
   panel still pending (no 1.8 board on hand).

4. ~~**Captive portal trigger is API-only**~~ — RESOLVED. A long-press on the WiFi
   label on the BT screen opens a "Cambiar WiFi?" confirm overlay that calls
   `wifi_force_setup_mode()` (ui.cpp). No longer requires wiping `secrets.h`.

5. **Knob press behaviour unknown**: until Eduardo unpairs the knob from his Mac and `nRF Connect`-dumps the HID reports (B2.1), we don't know if the press emits Mute (`0xE2`), Play/Pause (`0xCD`), or both. The B2.2 parser is blocked on that dump.

### 2026-06-08 session — backlog cleanup (committed locally, compile-verified, NOT yet flash-verified)

Done this session (the board was already dead, so all are verified via `pio run`
only — on-hardware QA pending the replacement):

- **Per-screen polling pause** ✅ — `sonos_poll_task` now `continue`s when
  `idle_is_asleep()`; no more 5 s `getVolume` SOAP round-trips behind a dark panel.
- **Responsive SCREEN_SONOS layout** ✅ — positions extracted into a
  `SonosLayout` struct in `screen_sonos.cpp`, picked by a height breakpoint. Large
  branch reproduces the 480×480 values exactly; compact branch fits 368×448 (the
  old mute at y=400+56 and 380px selector buttons overflowed the 1.8 panel).
- **AMOLED-1.8 env was unbuildable** ✅ fixed — it had compiled the Sonos source
  (via `+<*>`) without the `rupakpoddar/Sonos` dep (`Sonos.h not found`) and
  without `-DLV_FONT_MONTSERRAT_28=1` (`undefined reference to lv_font_montserrat_28`).
  Both added to the 1.8 env. `pio run -e waveshare_amoled_18` now SUCCEEDS.
- **Stack-canary root-cause fix** ⚠️ **ATTEMPTED, BLOCKED — reverted.** Enabling
  `custom_sdkconfig CONFIG_ESP_IPC_TASK_STACK_SIZE=2048` forces pioarduino to
  rebuild Arduino from ESP-IDF source instead of the precompiled libs. On this
  platform (55.03.38-1) that build fails to link with `undefined reference to
  __wrap_log_printf` (the Arduino WiFi log shim isn't provided in the from-source
  build). The config is left commented in `platformio.ini` with the diagnosis.
  The **active fix remains the `gpio_install_isr_service(0)` workaround** in
  `main.cpp setup()` (empirically 5/5 clean boots). Revisit on the live board.

## Next steps (when the replacement board arrives)

1. **Flash + boot-verify the 2026-06-08 commits** — confirm 5/5 clean boots and
   eyeball the Sonos screen layout (especially on a 1.8 panel if available).
2. **Retry the stack-canary root-cause fix** — resolve the pioarduino
   `__wrap_log_printf` link error under `custom_sdkconfig` (commented in
   `platformio.ini`), then validate it gives 5/5 clean boots. Only then drop the
   `gpio_install_isr_service(0)` workaround in `main.cpp setup()`.
3. **Clear the daemon's cached BLE MAC** — the new chip has a new factory-burned
   address; the cache at `~/.config/claude-usage-monitor/ble-address` (or the
   bluez entry) points at the dead `28:84:85:55:65:59`.
4. **Knob battery indicator** (B2 polish) — show the knob battery % on the BT
   screen alongside the device battery. (Knob battery subscription already exists
   per commit `bea7492`; verify it still surfaces.)

Everything from the previous "Next steps" list is now done: B2 knob HID parser
(`2e41379`), captive-portal trigger UI (long-press WiFi label, `30841ce`),
stale-data banner, per-screen polling pause, and the 1.8 layout.

## Daemon (Mac side, not in this repo)

- LaunchAgent at `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist` (local change, not committed to fork)
- Python script at `~/dev/Clawdmeter/daemon/claude_usage_daemon.py`
- 60 s poll cycle: reads `~/.claude` credentials → polls api.anthropic.com → BLE GATT write to ESP32

### 401 auth-failure handling (committed)
- `poll_api()` now returns `(payload, http_status)` so the loop can distinguish a
  401 from a network error.
- On HTTP 401 the loop caches the access-token hash (`_token_hash`) and stops
  re-polling until the token changes (running `claude` refreshes the Keychain
  token → new hash → polling resumes; a success resets the cache).
- Prevents 5 s log spam + needless API hammering while the user re-authenticates.
- Pairs with the firmware status banner: on a 401 the daemon goes quiet → no
  fresh GATT writes → after 3 min the ESP shows amber `DATOS VIEJOS - ABRI CLAUDE`.

Status: working. This is the first daemon change committed to the fork (prior
local mods were environment-specific and intentionally untracked; this one is a
self-contained, secret-free improvement worth versioning).

## How to verify after pulling this branch

```bash
cd ~/dev/Clawdmeter
git pull
cd firmware
pio run -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem101
pio device monitor -p /dev/cu.usbmodem101 -b 115200
```

Expected at boot:
```
{"ready":true}
AXP2101 init OK
... touch init ...
Touch init OK
I NimBLEDevice: NimBLE Started!
[wifi] connecting to SSID 'Network-ESXYZ'
[wifi] connected, IP: 192.168.86.40
[sonos] begin() -> 0
[sonos] poll task create -> 1
[knob] task create -> 1
[knob] scanner started, looking for EC:E1:67:F7:98:1F
[ui-sonos] init done
Dashboard ready (Waveshare AMOLED 2.16, 480x480)
```
