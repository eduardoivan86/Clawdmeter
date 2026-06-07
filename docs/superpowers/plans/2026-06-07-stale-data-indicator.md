# Stale-Data / Lost-Connection Indicator — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show an unmissable on-screen banner when the ESP32 stops receiving fresh Claude-usage data (token expired) or the BLE link drops, so the user knows to reopen `claude` / check the daemon instead of trusting stale numbers behind a still-spinning animation.

**Architecture:** Firmware-only. `ui.cpp` records `lv_tick_get()` of the last parsed payload; a per-loop evaluator `ui_tick_status()` reads the BLE state + freshness and drives a global banner created on `lv_layer_top()` (floats over every screen). No daemon, BLE-protocol, or `data.h` changes. The banner is *armed* only after the first fresh payload, so a fresh boot never false-alarms.

**Tech Stack:** PlatformIO (pioarduino), Arduino-ESP32 3.x, LVGL 9.5, C/C++. Board env `waveshare_amoled_216`. Device on macOS at `/dev/cu.usbmodem11201`.

**Verification model (firmware — no unit-test harness):** each task's "test" is a clean build (`pio run`), and for UI tasks an on-device flash + `./screenshot.sh` visual check (per `CLAUDE.md`). Read the PNG with the Read tool to confirm.

**Spec:** `docs/superpowers/specs/2026-06-07-stale-data-indicator-design.md`

---

## File Structure

- `firmware/src/ui.h` — declare two new public functions.
- `firmware/src/ui.cpp` — banner widget (top layer), freshness/arming state, status evaluator. All new state is file-private `static`.
- `firmware/src/main.cpp` — call `ui_note_data_fresh()` on a successful parse; call `ui_tick_status()` each loop.

No new files. All additions follow existing patterns in `ui.cpp` (file-private `static` state, `lv_*` widget creation in/near `ui_init()`).

---

## Task 1: Banner widget + freshness state + arming

**Files:**
- Modify: `firmware/src/ui.h` (add 2 declarations)
- Modify: `firmware/src/ui.cpp` (add static state, `ui_note_data_fresh()`, banner creation in `ui_init()`)

- [ ] **Step 1: Declare the new public API in `ui.h`**

In `firmware/src/ui.h`, add these two lines immediately after the existing
`void ui_update_battery(int percent, bool charging);` line:

```c
void ui_note_data_fresh(void);   // call when a payload parses OK — resets freshness timer + arms the indicator
void ui_tick_status(void);        // call every loop — evaluates freshness/BLE and drives the status banner
```

- [ ] **Step 2: Add file-private status state in `ui.cpp`**

In `firmware/src/ui.cpp`, find the animation-state block near line 139
(`static uint32_t anim_last_ms = 0;`). Immediately after the related
`anim_*` static declarations (after `static uint32_t anim_msg_start = 0;` on
line 143), add:

```c
// ---- Connection / freshness status banner (top layer, all screens) ----
#define STALE_THRESHOLD_MS 180000   // 3 min = ~3 missed daemon polls (poll ~60s)

enum status_kind_t { STATUS_OK = 0, STATUS_STALE = 1, STATUS_DISCONNECTED = 2 };

static lv_obj_t* status_banner    = nullptr;
static uint32_t  last_fresh_ms    = 0;
static bool      ever_received    = false;   // arm only after first fresh payload
static int       last_banner_kind = -1;      // cache so we only touch LVGL on change
```

- [ ] **Step 3: Implement `ui_note_data_fresh()` in `ui.cpp`**

In `firmware/src/ui.cpp`, add this function immediately after the end of
`ui_update(...)` (the closing brace on line 555, right before
`void ui_tick_anim(void)`):

```c
void ui_note_data_fresh(void) {
    last_fresh_ms = lv_tick_get();
    ever_received = true;
}
```

- [ ] **Step 4: Create the banner in `ui_init()`**

In `firmware/src/ui.cpp`, inside `ui_init()`, add the banner creation at the
very end of the function — immediately before its closing brace (after the
`battery_img` block that ends on line 532 with
`lv_obj_set_pos(battery_img, ...)`):

```c
    // Global status banner on the top layer — floats above every screen,
    // persists across screen switches. Hidden until armed + stale/disconnected.
    status_banner = lv_label_create(lv_layer_top());
    lv_obj_set_width(status_banner, L.scr_w - 2 * L.margin);
    lv_obj_set_style_radius(status_banner, 8, 0);
    lv_obj_set_style_bg_opa(status_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_banner, COL_RED, 0);
    lv_obj_set_style_text_color(status_banner, lv_color_black(), 0);
    lv_obj_set_style_text_font(status_banner, &font_styrene_20, 0);
    lv_obj_set_style_text_align(status_banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(status_banner, 8, 0);
    lv_obj_set_style_pad_hor(status_banner, 8, 0);
    lv_label_set_long_mode(status_banner, LV_LABEL_LONG_WRAP);
    lv_obj_align(status_banner, LV_ALIGN_TOP_MID, 0, L.margin);
    lv_obj_add_flag(status_banner, LV_OBJ_FLAG_HIDDEN);
```

- [ ] **Step 5: Build to verify it compiles**

Run: `pio run -d firmware -e waveshare_amoled_216`
Expected: `SUCCESS` (the new functions compile; `ui_tick_status` is declared in
`ui.h` but not yet defined — that's fine because nothing calls it and C++ allows
an undefined-but-declared function as long as it isn't referenced. If the linker
complains about an undefined reference to `ui_tick_status`, it means something
already calls it — it should not at this point. If so, proceed to Task 2 which
defines it.)

- [ ] **Step 6: Commit**

```bash
git add firmware/src/ui.h firmware/src/ui.cpp
git commit -m "feat(status): add freshness state + top-layer status banner widget

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Status evaluator (`ui_tick_status`)

**Files:**
- Modify: `firmware/src/ui.cpp` (define `ui_tick_status()`)

- [ ] **Step 1: Implement `ui_tick_status()` in `ui.cpp`**

In `firmware/src/ui.cpp`, add this function immediately after
`ui_note_data_fresh()` (added in Task 1, Step 3):

```c
void ui_tick_status(void) {
    if (!status_banner) return;

    int kind;
    if (!ever_received) {
        // Not armed yet: fresh boot before the first payload. Never alarm —
        // BLE is ADVERTISING/INIT during the normal connect window.
        kind = STATUS_OK;
    } else if (ble_get_state() != BLE_STATE_CONNECTED) {
        kind = STATUS_DISCONNECTED;
    } else if ((lv_tick_get() - last_fresh_ms) > STALE_THRESHOLD_MS) {
        kind = STATUS_STALE;
    } else {
        kind = STATUS_OK;
    }

    if (kind == last_banner_kind) return;   // only touch LVGL on a real change
    last_banner_kind = kind;

    switch (kind) {
        case STATUS_DISCONNECTED:
            lv_obj_set_style_bg_color(status_banner, COL_RED, 0);
            lv_label_set_text(status_banner, "SIN CONEXION");
            lv_obj_clear_flag(status_banner, LV_OBJ_FLAG_HIDDEN);
            break;
        case STATUS_STALE:
            lv_obj_set_style_bg_color(status_banner, COL_AMBER, 0);
            lv_label_set_text(status_banner, "DATOS VIEJOS - ABRI CLAUDE");
            lv_obj_clear_flag(status_banner, LV_OBJ_FLAG_HIDDEN);
            break;
        case STATUS_OK:
        default:
            lv_obj_add_flag(status_banner, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}
```

Note: banner text is ASCII-only (no `í`/accents, no `⚠` glyph) because the
custom `font_styrene_20` bitmap font is not guaranteed to contain those
glyphs — color + position carry the urgency. Step-3 screenshot QA confirms
legibility; a symbol glyph can be added later with a symbol-capable font if
wanted.

- [ ] **Step 2: Build to verify it compiles**

Run: `pio run -d firmware -e waveshare_amoled_216`
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add firmware/src/ui.cpp
git commit -m "feat(status): evaluate freshness/BLE and drive status banner

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Wire into `main.cpp` + on-device OK-state QA

**Files:**
- Modify: `firmware/src/main.cpp` (call `ui_note_data_fresh()` on parse, `ui_tick_status()` in loop)

- [ ] **Step 1: Call `ui_note_data_fresh()` on a successful parse**

In `firmware/src/main.cpp`, find the `ble_has_data()` block (around line 332).
The success branch currently reads:

```c
            ui_update(&usage);
            ble_send_ack();
```

Change it to:

```c
            ui_update(&usage);
            ui_note_data_fresh();
            ble_send_ack();
```

- [ ] **Step 2: Call `ui_tick_status()` every loop**

In `firmware/src/main.cpp`, in `loop()`, find the tick block near line 249:

```c
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
```

Insert `ui_tick_status();` right after `ui_tick_anim();`:

```c
    lv_timer_handler();
    ui_tick_anim();
    ui_tick_status();
    ble_tick();
```

- [ ] **Step 3: Build, flash, and screenshot the healthy (OK) state**

The daemon is running and delivering data, so after boot the device should be
in `STATUS_OK` (no banner).

Run:
```bash
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem11201
sleep 8
./screenshot.sh /tmp/status_ok.png /dev/cu.usbmodem11201
```
Then Read `/tmp/status_ok.png`.
Expected: usage screen renders normally, **no banner** visible at top. (If the
device sits on the splash screen, that's fine — the banner should still be
absent.)

- [ ] **Step 4: Commit**

```bash
git add firmware/src/main.cpp
git commit -m "feat(status): wire freshness tracking + status tick into main loop

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: On-device QA of STALE and DISCONNECTED, then finalize

This task uses **temporary build hacks** to force each alarm state
deterministically, screenshots each, then reverts. No production code changes
are committed from the hacks.

- [ ] **Step 1: Force and screenshot the STALE state**

Temporary hack (do NOT commit): in `firmware/src/ui.cpp` change the threshold
to 15 s and make `ui_note_data_fresh()` arm without refreshing the timer, so a
live BLE connection still goes stale:

- Change `#define STALE_THRESHOLD_MS 180000` → `#define STALE_THRESHOLD_MS 15000`
- In `ui_note_data_fresh()`, comment out `last_fresh_ms = lv_tick_get();` so the
  function body is only `ever_received = true;`

Then:
```bash
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem11201
sleep 25
./screenshot.sh /tmp/status_stale.png /dev/cu.usbmodem11201
```
Read `/tmp/status_stale.png`.
Expected: amber banner near the top reading `DATOS VIEJOS - ABRI CLAUDE`,
visible over whatever screen is active.

- [ ] **Step 2: Revert the STALE hack**

- Restore `#define STALE_THRESHOLD_MS 15000` → `#define STALE_THRESHOLD_MS 180000`
- Uncomment `last_fresh_ms = lv_tick_get();` in `ui_note_data_fresh()`

Verify the file is back to the committed state:
```bash
git diff firmware/src/ui.cpp
```
Expected: **no diff** (clean — matches the Task 2 commit).

- [ ] **Step 3: Force and screenshot the DISCONNECTED state**

Flash the clean firmware, let it connect + receive at least one payload (arms
`ever_received`), then drop the daemon so BLE disconnects:

```bash
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem11201
sleep 10   # let it connect + receive a payload (arms the indicator)
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
sleep 8    # let BLE drop
./screenshot.sh /tmp/status_disconnected.png /dev/cu.usbmodem11201
```
Read `/tmp/status_disconnected.png`.
Expected: red banner near the top reading `SIN CONEXION`.

- [ ] **Step 4: Verify recovery (banner clears) + restore daemon**

```bash
launchctl load ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
sleep 75   # daemon reconnects + delivers a fresh payload (poll ~60s)
./screenshot.sh /tmp/status_recovered.png /dev/cu.usbmodem11201
```
Read `/tmp/status_recovered.png`.
Expected: banner gone (back to `STATUS_OK`). Confirms `ui_note_data_fresh()`
clears the alarm on the next fresh payload.

- [ ] **Step 5: Update the project status doc**

In `docs/project-status.md`, under the existing phase list, add a new entry
documenting this feature (match the surrounding style):

```markdown
### Status indicator — stale-data / lost-connection banner ✅
- `ui.cpp`: global banner on `lv_layer_top()` (floats over all screens). Armed
  only after the first fresh payload (`ever_received`), so a fresh boot never
  false-alarms.
- `ui_note_data_fresh()` stamps `last_fresh_ms` on every OK parse (`main.cpp`).
- `ui_tick_status()` (per loop): BLE not connected → red `SIN CONEXION`;
  connected but >3 min since last payload → amber `DATOS VIEJOS - ABRI CLAUDE`;
  else hidden. Threshold `STALE_THRESHOLD_MS = 180000`.
- Verified on device: OK / STALE / DISCONNECTED / recovery screenshots.
```

- [ ] **Step 6: Final clean build + commit the doc**

```bash
pio run -d firmware -e waveshare_amoled_216
git add docs/project-status.md
git commit -m "docs: record stale-data/lost-connection status banner

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

Expected build: `SUCCESS`.

---

## Self-review notes

- **Spec coverage:** freshness tracking (Task 1), arming/`ever_received` gate
  (Tasks 1–2), banner on top layer over all screens (Task 1), priority state
  machine with precise BLE-aware messages (Task 2), `main.cpp` wiring (Task 3),
  sleep interaction (no special handling — covered by per-loop tick in Task 3),
  test plan OK/STALE/DISCONNECTED/recovery (Tasks 3–4). All spec sections mapped.
- **Type consistency:** `STATUS_OK/STATUS_STALE/STATUS_DISCONNECTED`,
  `status_banner`, `last_fresh_ms`, `ever_received`, `last_banner_kind`,
  `STALE_THRESHOLD_MS`, `ui_note_data_fresh`, `ui_tick_status` used identically
  across all tasks.
- **No placeholders:** every step has exact paths, full code, exact commands,
  expected output.
