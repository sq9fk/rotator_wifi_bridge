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

// Clears a STA mode-merge WiFi.scanNetworks() leaves behind (it calls
// enableSTA(true) internally, which ORs WIFI_MODE_STA into whatever mode is
// already active rather than replacing it, and never reverts it). Only acts
// while in Station/Connecting - deliberately a no-op in AccessPoint: forcing
// the radio back with WiFi.mode(WIFI_AP) calls esp_wifi_set_mode(), which
// restarts the AP interface and drops any already-associated client outright
// (confirmed - it needed a manual reconnect, not just a channel-hop blip
// during the scan itself). Leaving the harmless extra STA bit in place until
// the next natural mode transition is far cheaper than kicking whoever is
// using the fallback AP to configure WiFi in the first place. Call this once
// a scan is done (see WebApi.cpp's handleWifiScan).
void reassertMode();

}  // namespace net
