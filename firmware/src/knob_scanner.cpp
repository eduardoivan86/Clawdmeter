#include "knob_scanner.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_log.h>

static const char* TAG = "knob";

// ANTICATER VK-01 (Eduardo's unit) — see docs/anticater-vk01-dossier.md.
static const char* KNOB_MAC = "EC:E1:67:F7:98:1F";

// Standard BLE HID over GATT UUIDs.
static const NimBLEUUID HID_SERVICE_UUID((uint16_t)0x1812);
static const NimBLEUUID HID_REPORT_UUID ((uint16_t)0x2A4D);

static NimBLEClient*                  client          = nullptr;
static const NimBLEAdvertisedDevice*  found_device    = nullptr;
static bool                           connected       = false;
static bool                           connect_pending = false;

class KnobAdvertisedCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (dev->getAddress().toString() != KNOB_MAC) return;
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
    // B2.1 instrumentation: dump every report so we can decode the format
    // empirically before writing a parser. B2.2 replaces this body.
    ESP_LOG_BUFFER_HEX(TAG, data, len);
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
    ESP_LOGI(TAG, "scanner started, looking for %s", KNOB_MAC);

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
