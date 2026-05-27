#pragma once
#include <Arduino.h>

void wifi_init();
bool wifi_is_connected();
String wifi_get_ip();
void wifi_loop();
