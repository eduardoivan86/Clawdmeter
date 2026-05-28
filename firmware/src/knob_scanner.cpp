#include "knob_scanner.h"
#include "wifi_manager.h"
#include "sonos_controller.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_log.h>
#include <freertos/queue.h>

static const char* TAG = "knob";

// ANTICATER VK-01 (Eduardo's unit) — see docs/anticater-vk01-dossier.md.
// Knob reports as PUBLIC despite the EC:E1:67 prefix (the chipset spoofs
// the type — confirmed via the diagnostic adv: ... type=0 log in B2.1).
static const NimBLEAddress KNOB_ADDR("EC:E1:67:F7:98:1F", BLE_ADDR_PUBLIC);

// Standard BLE HID over GATT UUIDs.
static const NimBLEUUID HID_SERVICE_UUID ((uint16_t)0x1812);
static const NimBLEUUID HID_REPORT_UUID  ((uint16_t)0x2A4D);
static const NimBLEUUID HID_CONTROL_UUID ((uint16_t)0x2A4C);  // exit-suspend control
static const NimBLEUUID HID_PROTOCOL_UUID((uint16_t)0x2A4E);  // Boot vs Report mode

// VK-01 Consumer Control usage codes (verified via B2.1 dump).
// Reports are 2 bytes: [usage_code, 0x00] on press, [0x00, 0x00] on release.
// The press button emits the SAME code for short and long press — no
// distinction at the HID level. Long-press cannot be mapped separately.
static const uint8_t HID_VOL_UP    = 0xE9;
static const uint8_t HID_VOL_DOWN  = 0xEA;
static const uint8_t HID_MUTE      = 0xE2;

static const int     KNOB_VOLUME_STEP = 2;
static const uint32_t KNOB_VOL_THROTTLE_MS = 80;
static const uint32_t KNOB_TAP_WINDOW_MS   = 350;  // multi-tap detection window
static const int     KNOB_QUEUE_DEPTH = 16;

struct KnobEvent { uint8_t code; };
static QueueHandle_t knob_queue = nullptr;

static NimBLEClient*                  client          = nullptr;
static const NimBLEAdvertisedDevice*  found_device    = nullptr;
static bool                           connected       = false;
static bool                           connect_pending = false;

class KnobAdvertisedCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        // Diagnostic at INFO so we can verify the scanner is receiving any
        // advertisements at all. Bumps log volume — drop to LOGD after B2.1.
        ESP_LOGI(TAG, "adv: %s rssi=%d type=%d",
                 dev->getAddress().toString().c_str(),
                 dev->getRSSI(),
                 (int)dev->getAddressType());
        if (dev->getAddress() != KNOB_ADDR) return;
        ESP_LOGI(TAG, "found knob @ %s rssi=%d", dev->getAddress().toString().c_str(),
                 dev->getRSSI());
        NimBLEDevice::getScan()->stop();
        found_device = dev;
        connect_pending = true;
    }
};

class KnobClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
        ESP_LOGI(TAG, "connected");
        connected = true;
    }
    void onDisconnect(NimBLEClient* c, int reason) override {
        ESP_LOGW(TAG, "disconnected reason=%d", reason);
        connected = false;
        // Resume scanning so we auto-reconnect when the knob wakes up.
        NimBLEDevice::getScan()->start(0, false);
    }
};

static KnobAdvertisedCallbacks scan_cbs;
static KnobClientCallbacks     client_cbs;

static void knob_notify_cb(NimBLERemoteCharacteristic* chr,
                           uint8_t* data, size_t len, bool is_notify) {
    // Release events are [0x00, 0x00] — skip.
    if (len < 1 || data[0] == 0x00) return;
    if (!knob_queue) return;
    // Push to queue and return fast — SOAP calls happen in worker task so
    // we don't stall the NimBLE stack from a notify callback.
    KnobEvent ev = { data[0] };
    xQueueSend(knob_queue, &ev, 0);  // drop if full (best-effort)
}

// Multi-tap dispatch for the press button — VK-01 fires the same 0xE2
// usage code for short or long press, so we count taps within a window.
//   1 tap  -> toggle play/pause (local state flag)
//   2 taps -> next track
//   3 taps -> previous track
static void fire_press_action(int taps) {
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "press %dx dropped: wifi down", taps);
        return;
    }
    switch (taps) {
        case 1:
            ESP_LOGI(TAG, "1x -> toggle play/pause");
            sonos_ctrl_toggle_play_pause();  // single source of truth
            break;
        case 2:
            ESP_LOGI(TAG, "2x -> next");
            sonos_ctrl_next();
            break;
        default:  // 3 or more
            ESP_LOGI(TAG, "%dx -> previous", taps);
            sonos_ctrl_previous();
            break;
    }
}

static void knob_action_task(void* arg) {
    (void)arg;
    KnobEvent ev;
    uint32_t last_vol_ms     = 0;
    uint32_t last_press_ms   = 0;
    int      pending_taps    = 0;

    for (;;) {
        TickType_t wait_ticks = portMAX_DELAY;
        if (pending_taps > 0) {
            const uint32_t elapsed = millis() - last_press_ms;
            if (elapsed >= KNOB_TAP_WINDOW_MS) {
                fire_press_action(pending_taps);
                pending_taps = 0;
                continue;
            }
            wait_ticks = pdMS_TO_TICKS(KNOB_TAP_WINDOW_MS - elapsed);
        }

        if (xQueueReceive(knob_queue, &ev, wait_ticks) != pdTRUE) {
            // Timeout — loop top will fire the accumulated press action.
            continue;
        }

        // Knob input is "silent" — never wakes the display.
        const uint32_t now = millis();
        switch (ev.code) {
            case HID_VOL_UP: {
                if (now - last_vol_ms < KNOB_VOL_THROTTLE_MS) break;
                last_vol_ms = now;
                if (!wifi_is_connected()) break;
                int v = sonos_ctrl_get_volume();
                if (v < 0) v = 50;
                v += KNOB_VOLUME_STEP;
                if (v > 100) v = 100;
                ESP_LOGI(TAG, "vol+ -> %d", v);
                sonos_ctrl_set_volume(v);
                break;
            }
            case HID_VOL_DOWN: {
                if (now - last_vol_ms < KNOB_VOL_THROTTLE_MS) break;
                last_vol_ms = now;
                if (!wifi_is_connected()) break;
                int v = sonos_ctrl_get_volume();
                if (v < 0) v = 50;
                v -= KNOB_VOLUME_STEP;
                if (v < 0) v = 0;
                ESP_LOGI(TAG, "vol- -> %d", v);
                sonos_ctrl_set_volume(v);
                break;
            }
            case HID_MUTE:  // VK-01 press button — multi-tap dispatched below
                pending_taps++;
                last_press_ms = now;
                break;
            default:
                ESP_LOGI(TAG, "unknown code 0x%02x", ev.code);
                break;
        }
    }
}

static bool connect_to_knob() {
    if (!found_device) return false;
    if (!client) {
        client = NimBLEDevice::createClient();
        client->setClientCallbacks(&client_cbs, false);
    }
    ESP_LOGI(TAG, "connecting...");
    if (!client->connect(found_device)) {
        ESP_LOGE(TAG, "connect failed");
        return false;
    }
    // HID over GATT requires an encrypted link before the report char CCCD
    // writes are honoured. Without this the subscribe() succeeds at GATT
    // level but the knob suppresses notifications.
    if (!client->secureConnection()) {
        ESP_LOGE(TAG, "secureConnection failed — pairing did not complete");
        client->disconnect();
        return false;
    }
    ESP_LOGI(TAG, "link encrypted");

    NimBLERemoteService* svc = client->getService(HID_SERVICE_UUID);
    if (!svc) {
        ESP_LOGE(TAG, "no HID service");
        client->disconnect();
        return false;
    }
    // A HID device may expose several HID Report characteristics (input,
    // output, feature). Subscribe to every notify-capable one we find — the
    // VK-01 only has a single input report so this resolves cleanly.
    int subscribed = 0;
    std::vector<NimBLERemoteCharacteristic*> chars = svc->getCharacteristics(true);
    for (auto* c : chars) {
        if (c->getUUID() != HID_REPORT_UUID) continue;
        if (!c->canNotify()) continue;
        if (c->subscribe(true, knob_notify_cb)) {
            ESP_LOGI(TAG, "subscribed HID report handle=%u", c->getHandle());
            subscribed++;
        }
    }
    if (subscribed == 0) {
        ESP_LOGE(TAG, "no notify-able HID Report found");
        client->disconnect();
        return false;
    }

    // Force Report mode (0x01) — some HID devices default to Boot mode and
    // only emit minimal keyboard/mouse reports; Report mode unlocks the
    // Consumer Control input that we care about.
    NimBLERemoteCharacteristic* pm = svc->getCharacteristic(HID_PROTOCOL_UUID);
    if (pm) {
        uint8_t v = 0x01;
        if (pm->writeValue(&v, 1, false)) ESP_LOGI(TAG, "Protocol Mode = 1 (Report)");
        else                              ESP_LOGW(TAG, "Protocol Mode write failed");
    }
    // Exit suspend — write 0x00 to HID Control Point so the knob starts
    // sending reports if it was in a power-save state.
    NimBLERemoteCharacteristic* cp = svc->getCharacteristic(HID_CONTROL_UUID);
    if (cp) {
        uint8_t v = 0x00;
        if (cp->writeValue(&v, 1, false)) ESP_LOGI(TAG, "Control Point = 0 (exit suspend)");
        else                              ESP_LOGW(TAG, "Control Point write failed");
    }

    ESP_LOGI(TAG, "ready (%d report channel%s), free heap=%u",
             subscribed, subscribed == 1 ? "" : "s",
             (unsigned)ESP.getFreeHeap());
    return true;
}

static void knob_task(void* arg) {
    (void)arg;
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scan_cbs, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(0, false);  // continuous scan
    ESP_LOGI(TAG, "scanner started, looking for %s", KNOB_ADDR.toString().c_str());

    for (;;) {
        if (connect_pending) {
            connect_pending = false;
            if (!connect_to_knob()) {
                // Failed — resume scanning.
                NimBLEDevice::getScan()->start(0, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void knob_scanner_init() {
    // NimBLEDevice::init was already called by ble.cpp (peripheral side).
    // We just hop on the same stack as a central. Verify by checking init.
    if (!NimBLEDevice::isInitialized()) {
        ESP_LOGE(TAG, "NimBLE not initialised — ble_init must run first");
        return;
    }
    // BLE HID requires encrypted subscriptions on the HID Report char.
    // Configure just-works pairing: ESP32 has no I/O for a passkey, the knob
    // never displays a passkey (no screen). Bonding stored in NVS so we
    // don't re-pair on every reconnect.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    knob_queue = xQueueCreate(KNOB_QUEUE_DEPTH, sizeof(KnobEvent));
    // 8 KB stack — Sonos lib SOAP path uses ~5 KB with String formatting.
    // 4 KB overflows into _xt_alloca_exc on the first vol+ event.
    xTaskCreatePinnedToCore(knob_action_task, "knob_act", 8192, NULL, 1, NULL, 0);

    // Run scan + connect logic off the main loop so blocking NimBLE calls
    // don't stall LVGL. Stack 4096, priority 1, core 0 (mirrors sonos poll).
    BaseType_t t = xTaskCreatePinnedToCore(
        knob_task, "knob_scan", 4096, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "task create -> %d, free heap=%u", (int)t,
             (unsigned)ESP.getFreeHeap());
}

bool knob_is_connected() {
    return connected;
}
