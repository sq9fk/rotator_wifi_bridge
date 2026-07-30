#include "Net.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

#include "Config.h"

namespace net {
namespace {

// Long enough for a slow router and DHCP, short enough that a genuinely
// missing network does not leave the operator staring at nothing.
const uint32_t kConnectTimeoutMs = 20000;

// After dropping to AP mode, keep trying the configured network in the
// background - a router that rebooted should not require the bridge to reboot.
const uint32_t kRetryIntervalMs = 120000;

const char* kApPassword = "rotator123";  // AP-only, changed at setup

// UTC only, no DST - the panel's own clock is UTC and labelled as such (a
// rotator is used against schedules, beacons and other stations' logs, all in
// UTC), so the bridge's wall clock has no reason to know about time zones.
// Two servers, not one: this is the same "do not depend on a single resource"
// reasoning as the AP fallback below, just for NTP instead of the router.
const char* kNtpServer1 = "pool.ntp.org";
const char* kNtpServer2 = "time.google.com";

// Before a first successful sync, time(nullptr) returns a small number near
// the 1970 epoch; anything before this sentinel (2024-01-01T00:00:00Z) is
// treated as "not synced yet" rather than trusted as a real timestamp.
const time_t kMinValidEpoch = 1704067200;

// SNTP resyncs itself in the background once started, but re-issuing this
// periodically makes "keep it fresh" an explicit, documented behaviour here
// rather than relying on a default nobody in this codebase chose on purpose.
const uint32_t kTimeResyncIntervalMs = 6UL * 60UL * 60UL * 1000UL;  // 6 h
uint32_t lastTimeSyncAt = 0;

Mode currentMode = Mode::Connecting;
uint32_t attemptStarted = 0;
uint32_t lastRetry = 0;

// Only meaningful in station mode: the AP-only fallback has no route to the
// internet, so starting SNTP there would just be a client with nowhere to
// send its request.
void syncTime() {
  configTime(0, 0, kNtpServer1, kNtpServer2);
  lastTimeSyncAt = millis();
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(config.hostname, kApPassword);
  currentMode = Mode::AccessPoint;
}

void startStation() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(config.hostname);
  WiFi.begin(config.wifiSsid, config.wifiPassword);
  attemptStarted = millis();
  currentMode = Mode::Connecting;
}

void announce() {
  MDNS.end();
  if (MDNS.begin(config.hostname)) {
    MDNS.addService("http", "tcp", 80);
  }
}

}  // namespace

void begin() {
  WiFi.persistent(false);   // credentials live in our config, not in NVS
  WiFi.setAutoReconnect(true);

  if (!config.hasWifi()) {
    startAccessPoint();
    announce();
    return;
  }
  startStation();
}

void poll() {
  switch (currentMode) {
    case Mode::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        currentMode = Mode::Station;
        announce();
        syncTime();
      } else if (millis() - attemptStarted > kConnectTimeoutMs) {
        startAccessPoint();
        announce();
        lastRetry = millis();
      }
      break;

    case Mode::Station:
      if (WiFi.status() != WL_CONNECTED) {
        startStation();
      } else if (millis() - lastTimeSyncAt > kTimeResyncIntervalMs) {
        syncTime();
      }
      break;

    case Mode::AccessPoint:
      if (config.hasWifi() && (millis() - lastRetry > kRetryIntervalMs)) {
        lastRetry = millis();
        startStation();
      }
      break;
  }
}

Mode mode() {
  return currentMode;
}

const char* modeName() {
  switch (currentMode) {
    case Mode::Station: return "station";
    case Mode::AccessPoint: return "ap";
    default: return "connecting";
  }
}

String address() {
  return (currentMode == Mode::Station) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

String ssid() {
  return (currentMode == Mode::AccessPoint) ? String(config.hostname) : WiFi.SSID();
}

int rssi() {
  return (currentMode == Mode::Station) ? WiFi.RSSI() : 0;
}

bool timeSynced() {
  return time(nullptr) > kMinValidEpoch;
}

}  // namespace net
