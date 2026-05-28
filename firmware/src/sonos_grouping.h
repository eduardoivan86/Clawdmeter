#pragma once

// Sonos zone-group ops. Custom SOAP via HTTPClient — kept in its own
// translation unit so the linker places this code separately from
// sonos_controller.cpp (plan C2: previous shared-TU layouts triggered an
// ipc1 stack canary panic during touch init).

bool sonos_grouping_join(const char* slave_ip, const char* coordinator_rincon);
bool sonos_grouping_leave(const char* slave_ip);
