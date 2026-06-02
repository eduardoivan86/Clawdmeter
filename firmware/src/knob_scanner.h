#pragma once

// BLE central scanner + HID input client for the ANTICATER VK-01 knob.
//
// B2.1 (current state): scans for the knob by MAC, connects when found,
// discovers the HID Service (0x1812), subscribes to the HID Report
// characteristic (0x2A4D), and ESP_LOG_BUFFER_HEX-dumps every notification
// payload. The dump is the diagnostic we need before writing the parser.
//
// B2.2 (next): once Eduardo confirms the report format, replace the dump
// callback with a parser that maps Consumer Control usages to Sonos ops:
//   0xE9 (Volume Up)   -> sonos_ctrl_set_volume(cached + STEP)
//   0xEA (Volume Down) -> sonos_ctrl_set_volume(cached - STEP)
//   0xE2 (Mute)        -> sonos_ctrl_toggle_mute()
//   0xCD (Play/Pause)  -> sonos_ctrl_play / pause
// The press behaviour (mute vs play/pause) is determined empirically.
//
// IMPORTANT: by design the knob handler does NOT call idle_consume_wake_press
// or any idle_* function — knob input is "silent". Volume changes while the
// display is asleep should not wake the screen (Eduardo's "music in meeting"
// case from the B3a discussion).

void knob_scanner_init();
bool knob_is_connected();

// Knob battery percentage from BLE Battery Service (0x180F / 0x2A19).
// Returns -1 when knob is not connected or the battery value hasn't been
// reported yet.
int  knob_get_battery_pct();
