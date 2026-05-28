#pragma once
#include <Arduino.h>

void wifi_init();
bool wifi_is_connected();
String wifi_get_ip();
void wifi_loop();

// Captive-portal setup mode — opens a WiFi AP "Clawdmeter-Setup" and a web
// form on http://192.168.4.1 for the user to enter SSID/password. On submit,
// credentials are persisted to NVS and the device reboots. Blocks forever
// (does not return). Trigger from a future button combo or via the
// auto-fallback path inside wifi_init when no real credentials exist.
void wifi_force_setup_mode();
