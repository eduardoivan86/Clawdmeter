#include "wifi_manager.h"
#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_log.h>

static const char* TAG = "wifi";

static const uint32_t INIT_TIMEOUT_MS     = 30000;
static const uint32_t RECONNECT_PERIOD_MS = 10000;

static uint32_t    last_reconnect_attempt_ms = 0;
static wl_status_t last_logged_status        = WL_NO_SHIELD;

static const char* SETUP_AP_SSID  = "Clawdmeter-Setup";
static const char* NVS_NAMESPACE  = "wifi";
static const char* NVS_KEY_SSID   = "ssid";
static const char* NVS_KEY_PASS   = "pass";

static bool has_real_creds(const String& ssid, const String& pass) {
    if (ssid.length() == 0 || pass.length() == 0) return false;
    if (ssid == "REPLACE_ME" || pass == "REPLACE_ME") return false;
    return true;
}

static const char* SETUP_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Clawdmeter Setup</title>"
    "<style>"
    "body{font-family:-apple-system,sans-serif;background:#000;color:#faf9f5;"
    "padding:20px;max-width:400px;margin:auto}"
    "h2{color:#d97757}"
    "label{display:block;margin-top:12px;color:#b0aea5;font-size:14px}"
    "input{width:100%;padding:10px;margin:4px 0;font-size:16px;"
    "background:#1f1f1e;color:#faf9f5;border:1px solid #2a2a28;border-radius:4px;"
    "box-sizing:border-box}"
    "button{width:100%;padding:14px;margin-top:18px;background:#d97757;"
    "color:white;border:0;font-size:16px;border-radius:4px;cursor:pointer}"
    "</style></head><body>"
    "<h2>Clawdmeter WiFi Setup</h2>"
    "<form method='POST' action='/save'>"
    "<label>SSID</label><input name='s' autocapitalize='off' autocorrect='off' required>"
    "<label>Password</label><input name='p' type='password' required>"
    "<button type='submit'>Save &amp; Reboot</button></form>"
    "</body></html>";

void wifi_force_setup_mode() {
    ESP_LOGW(TAG, "setup mode: SSID='%s', http://192.168.4.1", SETUP_AP_SSID);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(SETUP_AP_SSID);

    static WebServer server(80);
    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html", SETUP_HTML);
    });
    server.on("/save", HTTP_POST, []() {
        String ssid = server.arg("s");
        String pass = server.arg("p");
        if (ssid.length() == 0 || pass.length() == 0) {
            server.send(400, "text/plain", "Missing SSID or password");
            return;
        }
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putString(NVS_KEY_SSID, ssid);
        prefs.putString(NVS_KEY_PASS, pass);
        prefs.end();
        ESP_LOGI(TAG, "creds saved for '%s' -> rebooting", ssid.c_str());
        server.send(200, "text/html",
            "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:20px;background:#000;color:#faf9f5'>"
            "<h2 style='color:#788c5d'>Saved. Rebooting...</h2></body></html>");
        delay(800);
        ESP.restart();
    });
    server.begin();

    for (;;) {
        server.handleClient();
        delay(5);
    }
}

void wifi_init() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);  // read-only
    String ssid = prefs.getString(NVS_KEY_SSID, WIFI_SSID);
    String pass = prefs.getString(NVS_KEY_PASS, WIFI_PASS);
    prefs.end();

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);

    if (!has_real_creds(ssid, pass)) {
        ESP_LOGW(TAG, "no valid credentials (NVS empty + secrets.h placeholder)");
        wifi_force_setup_mode();  // never returns
    }

    WiFi.begin(ssid.c_str(), pass.c_str());
    ESP_LOGI(TAG, "connecting to SSID '%s'", ssid.c_str());

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
