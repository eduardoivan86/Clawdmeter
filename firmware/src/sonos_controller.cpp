#include "sonos_controller.h"
#include "secrets.h"
#include "wifi_manager.h"
#include "ui.h"
#include "screen_sonos.h"
#include "sonos_grouping.h"
#include <Arduino.h>
#include <Sonos.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// PLAN B — re-add grouping with PROGMEM XML templates + char[] buffers
// (no String concatenations in the SOAP helpers). Goal: keep grouping
// while minimising binary/heap pressure that triggered the ipc1 stack
// canary panic in the previous R2 layout.

static const char* TAG = "sonos";

struct Room {
    const char* name;
    const char* ip;
    const char* rincon;
};

static const Room rooms[] = {
    { SONOS_LIVING_ROOM_NAME, SONOS_LIVING_ROOM_IP, SONOS_LIVING_ROOM_RINCON },
    { SONOS_MOVE_NAME,        SONOS_MOVE_IP,        SONOS_MOVE_RINCON        },
};

static Sonos sonos;
static SemaphoreHandle_t sonos_mutex = nullptr;

static SonosMode active_mode  = MODE_SINGLE_LR;
static int       cached_volume = -1;
static bool      cached_mute   = false;

static inline bool ok(SonosResult r) {
    return r == SonosResult::SUCCESS;
}

static const char* active_target_ip() {
    switch (active_mode) {
        case MODE_SINGLE_LR:   return SONOS_LIVING_ROOM_IP;
        case MODE_SINGLE_MOVE: return SONOS_MOVE_IP;
        case MODE_BOTH:        return SONOS_LIVING_ROOM_IP;
    }
    return SONOS_LIVING_ROOM_IP;
}

// ---------- Polling task ----------
static void sonos_poll_task(void* arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (ui_get_current_screen() != SCREEN_SONOS) continue;
        if (!wifi_is_connected())                    continue;
        if (sonos_ctrl_refresh_volume()) {
            screen_sonos_update();
        }
    }
}

void sonos_ctrl_init() {
    if (!sonos_mutex) {
        sonos_mutex = xSemaphoreCreateMutex();
    }
    SonosResult r = sonos.begin();
    ESP_LOGI(TAG, "begin() -> %d, free heap=%u min=%u",
             (int)r, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());

    BaseType_t t = xTaskCreatePinnedToCore(
        sonos_poll_task, "sonos_poll", 4096, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "poll task create -> %d, free heap=%u",
             (int)t, (unsigned)ESP.getFreeHeap());
}

// ---------- Mode ----------

SonosMode sonos_ctrl_get_mode() {
    return active_mode;
}

const char* sonos_ctrl_get_mode_label() {
    switch (active_mode) {
        case MODE_SINGLE_LR:   return SONOS_LIVING_ROOM_NAME;
        case MODE_SINGLE_MOVE: return SONOS_MOVE_NAME;
        case MODE_BOTH:        return "Living Room + Move";
    }
    return "?";
}

bool sonos_ctrl_set_mode(SonosMode m) {
    if (m == active_mode) return true;
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "set_mode skipped: wifi down");
        return false;
    }

    bool need_join  = (m == MODE_BOTH)            && (active_mode != MODE_BOTH);
    bool need_leave = (active_mode == MODE_BOTH)  && (m != MODE_BOTH);
    bool soap_ok    = true;

    xSemaphoreTake(sonos_mutex, portMAX_DELAY);
    if (need_join) {
        soap_ok = sonos_grouping_join(SONOS_MOVE_IP, SONOS_LIVING_ROOM_RINCON);
    } else if (need_leave) {
        soap_ok = sonos_grouping_leave(SONOS_MOVE_IP);
    }
    xSemaphoreGive(sonos_mutex);

    if (!soap_ok) {
        ESP_LOGE(TAG, "set_mode(%d) SOAP failed, mode unchanged", (int)m);
        return false;
    }

    active_mode   = m;
    cached_volume = -1;
    cached_mute   = false;
    ESP_LOGI(TAG, "mode -> %d (%s), free heap=%u min=%u",
             (int)m, sonos_ctrl_get_mode_label(),
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    return true;
}

void sonos_ctrl_cycle_room() {
    SonosMode next;
    if (active_mode == MODE_SINGLE_LR) {
        next = MODE_SINGLE_MOVE;
    } else if (active_mode == MODE_SINGLE_MOVE) {
        next = MODE_SINGLE_LR;
    } else {
        ESP_LOGI(TAG, "cycle_room no-op in BOTH mode (use selector)");
        return;
    }
    sonos_ctrl_set_mode(next);
}

int sonos_ctrl_get_active_room_idx() {
    return (active_mode == MODE_SINGLE_MOVE) ? 1 : 0;
}

const char* sonos_ctrl_get_active_room_name() {
    return rooms[sonos_ctrl_get_active_room_idx()].name;
}

const char* sonos_ctrl_get_active_room_ip() {
    return active_target_ip();
}

// ---------- Volume ----------

bool sonos_ctrl_set_volume(int v) {
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "set_volume(%d) skipped: wifi down", v);
        return false;
    }
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    xSemaphoreTake(sonos_mutex, portMAX_DELAY);
    bool result;
    if (active_mode == MODE_BOTH) {
        bool a = ok(sonos.setVolume(SONOS_LIVING_ROOM_IP, v));
        bool b = ok(sonos.setVolume(SONOS_MOVE_IP, v));
        result = a && b;
    } else {
        result = ok(sonos.setVolume(active_target_ip(), v));
    }
    if (result) cached_volume = v;
    xSemaphoreGive(sonos_mutex);
    ESP_LOGI(TAG, "set_volume(%d) %s mode=%d", v, result ? "ok" : "FAIL", (int)active_mode);
    return result;
}

int sonos_ctrl_get_volume() {
    return cached_volume;
}

bool sonos_ctrl_refresh_volume() {
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "refresh_volume skipped: wifi down");
        return false;
    }
    int v = -1;
    xSemaphoreTake(sonos_mutex, portMAX_DELAY);
    bool result = ok(sonos.getVolume(active_target_ip(), v));
    if (result) cached_volume = v;
    xSemaphoreGive(sonos_mutex);
    ESP_LOGI(TAG, "refresh_volume -> %s (v=%d) mode=%d",
             result ? "ok" : "FAIL", v, (int)active_mode);
    return result;
}

// ---------- Transport ----------

bool sonos_ctrl_play() {
    if (!wifi_is_connected()) return false;
    xSemaphoreTake(sonos_mutex, portMAX_DELAY);
    bool result = ok(sonos.play(active_target_ip()));
    xSemaphoreGive(sonos_mutex);
    ESP_LOGI(TAG, "play %s", result ? "ok" : "FAIL");
    return result;
}

bool sonos_ctrl_pause() {
    if (!wifi_is_connected()) return false;
    xSemaphoreTake(sonos_mutex, portMAX_DELAY);
    bool result = ok(sonos.pause(active_target_ip()));
    xSemaphoreGive(sonos_mutex);
    ESP_LOGI(TAG, "pause %s", result ? "ok" : "FAIL");
    return result;
}

bool sonos_ctrl_next() {
    if (!wifi_is_connected()) return false;
    xSemaphoreTake(sonos_mutex, portMAX_DELAY);
    bool result = ok(sonos.next(active_target_ip()));
    xSemaphoreGive(sonos_mutex);
    ESP_LOGI(TAG, "next %s", result ? "ok" : "FAIL");
    return result;
}

bool sonos_ctrl_previous() {
    if (!wifi_is_connected()) return false;
    xSemaphoreTake(sonos_mutex, portMAX_DELAY);
    bool result = ok(sonos.previous(active_target_ip()));
    xSemaphoreGive(sonos_mutex);
    ESP_LOGI(TAG, "previous %s", result ? "ok" : "FAIL");
    return result;
}

bool sonos_ctrl_is_muted() {
    return cached_mute;
}

bool sonos_ctrl_toggle_mute() {
    if (!wifi_is_connected()) return false;
    bool target = !cached_mute;
    xSemaphoreTake(sonos_mutex, portMAX_DELAY);
    bool result;
    if (active_mode == MODE_BOTH) {
        bool a = ok(sonos.setMute(SONOS_LIVING_ROOM_IP, target));
        bool b = ok(sonos.setMute(SONOS_MOVE_IP, target));
        result = a && b;
    } else {
        result = ok(sonos.setMute(active_target_ip(), target));
    }
    if (result) cached_mute = target;
    xSemaphoreGive(sonos_mutex);
    ESP_LOGI(TAG, "toggle_mute -> %d %s mode=%d",
             (int)target, result ? "ok" : "FAIL", (int)active_mode);
    return result;
}
