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

}  // namespace net
