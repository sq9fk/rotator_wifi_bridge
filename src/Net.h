// WiFi bring-up with an access-point fallback.
//
// The bridge must be reachable even when the configured network is gone,
// otherwise a moved antenna and a changed router password mean fetching a
// cable and a laptop. If the station connection fails or no credentials are
// stored, it brings up its own AP and serves the same web interface there.

#pragma once

#include <Arduino.h>

namespace net {

enum class Mode : uint8_t { Connecting, Station, AccessPoint };

void begin();
void poll();

Mode mode();
const char* modeName();
String address();      // the address the panel is reachable at
String ssid();
int rssi();

// True once NTP has produced a plausible wall-clock time (see Net.cpp) -
// checked before trusting time(nullptr) for anything shown to the operator,
// such as an absolute timestamp on the last motion command. Synced once the
// bridge first reaches the real internet (station mode, not the AP-only
// fallback, which has no route out), and resynced periodically after that.
bool timeSynced();

// Re-applies whatever single mode (AP or STA) this state machine currently
// intends. WiFi.scanNetworks() calls enableSTA(true) internally, which ORs
// WIFI_MODE_STA into whatever mode is already active rather than replacing
// it - after a scan triggered while in AP fallback, the radio is left
// broadcasting AP *and* STA at once, and nothing in the scan API ever
// reverts that on its own. Call this once a scan is done (see
// WebApi.cpp's handleWifiScan) to undo the merge.
void reassertMode();

}  // namespace net
