#include "screen_sonos.h"
#include "sonos_controller.h"
#include "wifi_manager.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <esp_log.h>

LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(lv_font_montserrat_28);

static const char* TAG = "ui-sonos";

static lv_obj_t* sonos_container = nullptr;
static lv_obj_t* lbl_room        = nullptr;
static lv_obj_t* lbl_wifi        = nullptr;
static lv_obj_t* lbl_vol_num     = nullptr;
static lv_obj_t* slider_vol      = nullptr;
static lv_obj_t* btn_play_pause  = nullptr;
static lv_obj_t* lbl_play_pause  = nullptr;
static lv_obj_t* lbl_mute        = nullptr;
static lv_obj_t* selector_overlay = nullptr;

static bool play_state         = false;
static bool slider_dragging    = false;

static void update_room_label() {
    if (!lbl_room) return;
    lv_label_set_text_fmt(lbl_room, "%s  %s",
                          sonos_ctrl_get_mode_label(),
                          LV_SYMBOL_DOWN);
}

static void update_wifi_label() {
    if (!lbl_wifi) return;
    if (wifi_is_connected()) {
        lv_label_set_text(lbl_wifi, "WiFi");
        lv_obj_set_style_text_color(lbl_wifi, THEME_GREEN, 0);
    } else {
        lv_label_set_text(lbl_wifi, "no wifi");
        lv_obj_set_style_text_color(lbl_wifi, THEME_RED, 0);
    }
}

static void update_volume_widgets() {
    int v = sonos_ctrl_get_volume();
    if (v < 0) v = 0;
    if (lbl_vol_num) lv_label_set_text_fmt(lbl_vol_num, "%d", v);
    if (slider_vol && !slider_dragging) lv_slider_set_value(slider_vol, v, LV_ANIM_OFF);
}

static void header_click_cb(lv_event_t*) {
    sonos_ctrl_cycle_room();   // no-op in BOTH mode; use selector to exit
    update_room_label();
    sonos_ctrl_refresh_volume();
    update_volume_widgets();
}

// ---- Speaker selector overlay (opened via swipe-down) ----
static void selector_close() {
    if (!selector_overlay) return;
    lv_obj_delete(selector_overlay);
    selector_overlay = nullptr;
}

static void selector_btn_cb(lv_event_t* e) {
    SonosMode m = (SonosMode)(intptr_t)lv_event_get_user_data(e);
    sonos_ctrl_set_mode(m);
    selector_close();
    update_room_label();
    sonos_ctrl_refresh_volume();
    update_volume_widgets();
}

static lv_obj_t* make_selector_btn(lv_obj_t* parent, const char* text, SonosMode m) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, 380, 80);
    lv_obj_set_style_bg_color(btn, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, THEME_TEXT, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, selector_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)m);
    return btn;
}

static void speaker_selector_open() {
    if (selector_overlay) return;
    if (!sonos_container) return;
    const int W = board_caps().width;
    const int H = board_caps().height;

    selector_overlay = lv_obj_create(sonos_container);
    lv_obj_set_size(selector_overlay, W, H);
    lv_obj_set_pos(selector_overlay, 0, 0);
    lv_obj_set_style_bg_color(selector_overlay, THEME_BG, 0);
    lv_obj_set_style_bg_opa(selector_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(selector_overlay, 0, 0);
    lv_obj_clear_flag(selector_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(selector_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(selector_overlay,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(selector_overlay, 18, 0);

    make_selector_btn(selector_overlay, "Solo Living Room", MODE_SINGLE_LR);
    make_selector_btn(selector_overlay, "Solo Sonos Move",  MODE_SINGLE_MOVE);
    make_selector_btn(selector_overlay, "Ambos (grupo)",    MODE_BOTH);

    ESP_LOGI(TAG, "selector opened, free heap=%u min=%u",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
}

// Long-press on the header is the way into the selector. LV_EVENT_GESTURE
// (swipe-down) did not fire reliably on the CST9220 touch driver — removed.
static void header_long_press_cb(lv_event_t*) {
    speaker_selector_open();
}

static void slider_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        slider_dragging = true;
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        int v = (int)lv_slider_get_value(slider_vol);
        if (lbl_vol_num) lv_label_set_text_fmt(lbl_vol_num, "%d", v);
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        slider_dragging = false;
        int v = (int)lv_slider_get_value(slider_vol);
        ESP_LOGI(TAG, "slider released -> set_volume(%d)", v);
        sonos_ctrl_set_volume(v);
        return;
    }
}

static void update_play_pause_visual() {
    if (!lbl_play_pause) return;
    bool p = sonos_ctrl_is_playing();
    lv_label_set_text(lbl_play_pause, p ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void play_pause_click_cb(lv_event_t*) {
    sonos_ctrl_toggle_play_pause();
    update_play_pause_visual();
}

static void update_mute_visual() {
    if (!lbl_mute) return;
    bool m = sonos_ctrl_is_muted();
    lv_label_set_text(lbl_mute, m ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_mute, m ? THEME_RED : THEME_DIM, 0);
}

static void prev_click_cb(lv_event_t*) { sonos_ctrl_previous(); }
static void next_click_cb(lv_event_t*) { sonos_ctrl_next(); }
static void mute_click_cb(lv_event_t*) {
    sonos_ctrl_toggle_mute();
    update_mute_visual();
}

static lv_obj_t* make_transport_btn(lv_obj_t* parent, const char* sym, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, 88, 72);
    lv_obj_set_style_bg_color(btn, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, THEME_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void screen_sonos_init(lv_obj_t* parent_scr) {
    const int W = board_caps().width;
    const int H = board_caps().height;

    sonos_container = lv_obj_create(parent_scr);
    lv_obj_set_size(sonos_container, W, H);
    lv_obj_set_pos(sonos_container, 0, 0);
    lv_obj_set_style_bg_opa(sonos_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sonos_container, 0, 0);
    lv_obj_set_style_pad_all(sonos_container, 0, 0);
    lv_obj_clear_flag(sonos_container, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header (tappable, room name + wifi mini) ----
    lv_obj_t* header = lv_obj_create(sonos_container);
    lv_obj_set_size(header, W - 40, 70);
    lv_obj_set_pos(header, 20, 30);
    lv_obj_set_style_bg_color(header, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 12, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(header, header_click_cb,      LV_EVENT_CLICKED,      NULL);
    lv_obj_add_event_cb(header, header_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    lbl_room = lv_label_create(header);
    lv_obj_set_style_text_font(lbl_room, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_room, THEME_TEXT, 0);
    lv_obj_align(lbl_room, LV_ALIGN_LEFT_MID, 16, 0);

    lbl_wifi = lv_label_create(header);
    lv_obj_set_style_text_font(lbl_wifi, &font_styrene_20, 0);
    lv_obj_align(lbl_wifi, LV_ALIGN_RIGHT_MID, -16, 0);

    // ---- Volume big number ----
    lbl_vol_num = lv_label_create(sonos_container);
    lv_obj_set_style_text_font(lbl_vol_num, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_vol_num, THEME_TEXT, 0);
    lv_label_set_text(lbl_vol_num, "--");
    lv_obj_align(lbl_vol_num, LV_ALIGN_TOP_MID, 0, 130);

    // ---- Slider ----
    slider_vol = lv_slider_create(sonos_container);
    lv_obj_set_size(slider_vol, W - 80, 22);
    lv_obj_align(slider_vol, LV_ALIGN_TOP_MID, 0, 230);
    lv_slider_set_range(slider_vol, 0, 100);
    lv_slider_set_value(slider_vol, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_vol, THEME_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_vol, THEME_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_vol, THEME_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_vol, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_vol, slider_event_cb, LV_EVENT_PRESSED,       NULL);
    lv_obj_add_event_cb(slider_vol, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider_vol, slider_event_cb, LV_EVENT_RELEASED,      NULL);

    // ---- Transport row (prev / play_pause / next) ----
    lv_obj_t* row = lv_obj_create(sonos_container);
    lv_obj_set_size(row, W - 40, 90);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_transport_btn(row, LV_SYMBOL_PREV, prev_click_cb);
    btn_play_pause = make_transport_btn(row, LV_SYMBOL_PLAY, play_pause_click_cb);
    lbl_play_pause = lv_obj_get_child(btn_play_pause, 0);
    make_transport_btn(row, LV_SYMBOL_NEXT, next_click_cb);

    // ---- Mute (bottom) ----
    lv_obj_t* btn_mute = lv_button_create(sonos_container);
    lv_obj_set_size(btn_mute, 72, 56);
    lv_obj_align(btn_mute, LV_ALIGN_TOP_MID, 0, 400);
    lv_obj_set_style_bg_color(btn_mute, THEME_PANEL, 0);
    lv_obj_set_style_bg_opa(btn_mute, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_mute, 12, 0);
    lv_obj_set_style_border_width(btn_mute, 0, 0);
    lbl_mute = lv_label_create(btn_mute);
    lv_label_set_text(lbl_mute, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_mute, THEME_DIM, 0);
    lv_obj_set_style_text_font(lbl_mute, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl_mute);
    lv_obj_add_event_cb(btn_mute, mute_click_cb, LV_EVENT_CLICKED, NULL);

    update_room_label();
    update_wifi_label();
    update_volume_widgets();

    lv_obj_add_flag(sonos_container, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "init done, free heap=%u min=%u",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
}

void screen_sonos_show() {
    if (!sonos_container) return;
    lv_obj_clear_flag(sonos_container, LV_OBJ_FLAG_HIDDEN);
}

void screen_sonos_hide() {
    if (!sonos_container) return;
    lv_obj_add_flag(sonos_container, LV_OBJ_FLAG_HIDDEN);
}

void screen_sonos_update() {
    update_room_label();
    update_wifi_label();
    update_volume_widgets();
    update_mute_visual();
    update_play_pause_visual();
}
