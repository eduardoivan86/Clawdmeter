#include "wifi_manager.h"
#include "secrets.h"
#include <WiFi.h>
#include <esp_log.h>

static const char* TAG = "wifi";

static const uint32_t INIT_TIMEOUT_MS    = 30000;
static const uint32_t RECONNECT_PERIOD_MS = 10000;

static uint32_t last_reconnect_attempt_ms = 0;
static wl_status_t last_logged_status     = WL_NO_SHIELD;

void wifi_init() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    ESP_LOGI(TAG, "connecting to SSID '%s'", WIFI_SSID);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < INIT_TIMEOUT_MS) {
        delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {
        ESP_LOGI(TAG, "connected, IP: %s", WiFi.localIP().toString().c_str());
    } else {
        ESP_LOGW(TAG, "initial connect timed out after %lu ms (will retry in loop)",
                 (unsigned long)INIT_TIMEOUT_MS);
    }
    last_logged_status = WiFi.status();
    last_reconnect_attempt_ms = millis();
}

bool wifi_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}

String wifi_get_ip() {
    if (!wifi_is_connected()) return String("");
    return WiFi.localIP().toString();
}

void wifi_loop() {
    wl_status_t s = WiFi.status();

    if (s != last_logged_status) {
        if (s == WL_CONNECTED) {
            ESP_LOGI(TAG, "reconnected, IP: %s", WiFi.localIP().toString().c_str());
        } else {
            ESP_LOGW(TAG, "link down (status=%d)", (int)s);
        }
        last_logged_status = s;
    }

    if (s == WL_CONNECTED) return;

    const uint32_t now = millis();
    if (now - last_reconnect_attempt_ms < RECONNECT_PERIOD_MS) return;

    last_reconnect_attempt_ms = now;
    ESP_LOGI(TAG, "attempting reconnect");
    WiFi.reconnect();
}
