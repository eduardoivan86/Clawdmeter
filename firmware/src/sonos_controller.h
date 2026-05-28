#pragma once

enum SonosMode {
    MODE_SINGLE_LR,
    MODE_SINGLE_MOVE,
    MODE_BOTH,
};

void  sonos_ctrl_init();
void  sonos_ctrl_cycle_room();
int   sonos_ctrl_get_active_room_idx();
const char* sonos_ctrl_get_active_room_name();
const char* sonos_ctrl_get_active_room_ip();

bool  sonos_ctrl_set_mode(SonosMode m);
SonosMode sonos_ctrl_get_mode();
const char* sonos_ctrl_get_mode_label();

bool  sonos_ctrl_set_volume(int v);
int   sonos_ctrl_get_volume();
bool  sonos_ctrl_refresh_volume();

bool  sonos_ctrl_play();
bool  sonos_ctrl_pause();
bool  sonos_ctrl_next();
bool  sonos_ctrl_previous();
bool  sonos_ctrl_toggle_mute();
bool  sonos_ctrl_is_muted();

// Local play/pause cache + toggle. Knob 1-tap and screen play button both
// call this so the UI icon stays in sync with the user's last intent.
// (Sonos itself has no Transport state poll in the rupakpoddar lib, so this
// can desync if play state is changed from outside; acceptable trade-off.)
bool  sonos_ctrl_toggle_play_pause();
bool  sonos_ctrl_is_playing();
