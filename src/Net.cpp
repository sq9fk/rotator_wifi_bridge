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

// Fixed instead of the ESP32 default (192.168.4.1) so it never collides with
// a station-mode subnet the operator's own router might already use.
const IPAddress kApIp(10, 10, 10, 1);
const IPAddress kApSubnet(255, 255, 255, 0);

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

// Index into config.wifiNetworks currently being attempted - list order is
// the operator's own priority, not signal strength, so this always resumes
// the search from the top (index 0) rather than remembering where a dropped
// connection left off.
size_t attemptIndex = 0;

// First configured slot at or after `from`, or -1 if none remain - kMax is
// small (5), so a linear scan every attempt costs nothing worth avoiding.
int nextConfiguredNetwork(size_t from) {
  for (size_t i = from; i < Config::kMaxWifiNetworks; i++) {
    if (config.wifiNetworks[i].ssid[0] != '\0') {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Only meaningful in station mode: the AP-only fallback has no route to the
// internet, so starting SNTP there would just be a client with nowhere to
// send its request.
void syncTime() {
  configTime(0, 0, kNtpServer1, kNtpServer2);
  lastTimeSyncAt = millis();
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  // softAPConfig before softAP: the DHCP server it starts automatically reads
  // this IP/subnet, so client leases land in 10.10.10.0/24 without any
  // separate DHCP setup.
  WiFi.softAPConfig(kApIp, kApIp, kApSubnet);
  WiFi.softAP(config.hostname, config.apPassword);
  currentMode = Mode::AccessPoint;
}

void startStation(size_t networkIndex) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(config.hostname);
  WiFi.begin(config.wifiNetworks[networkIndex].ssid, config.wifiNetworks[networkIndex].password);
  attemptIndex = networkIndex;
  attemptStarted = millis();
  currentMode = Mode::Connecting;
}

// Always restarts the priority search from index 0 - a network higher up the
// list than whatever just dropped should get first refusal again, not be
// skipped because the connection happened to fail past it last time.
void startFirstConfiguredNetwork() {
  const int idx = nextConfiguredNetwork(0);
  if (idx < 0) {
    startAccessPoint();
    return;
  }
  startStation(static_cast<size_t>(idx));
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
  startFirstConfiguredNetwork();
}

void poll() {
  switch (currentMode) {
    case Mode::Connecting:
      if (WiFi.status() == WL_CONNECTED) {
        currentMode = Mode::Station;
        announce();
        syncTime();
      } else if (millis() - attemptStarted > kConnectTimeoutMs) {
        // Move on to the next network on the list rather than giving up
        // straight to AP mode - only once every configured slot has had its
        // turn does this attempt actually fall back to AP.
        const int nextIdx = nextConfiguredNetwork(attemptIndex + 1);
        if (nextIdx >= 0) {
          startStation(static_cast<size_t>(nextIdx));
        } else {
          startAccessPoint();
          announce();
          lastRetry = millis();
        }
      }
      break;

    case Mode::Station:
      if (WiFi.status() != WL_CONNECTED) {
        startFirstConfiguredNetwork();
      } else if (millis() - lastTimeSyncAt > kTimeResyncIntervalMs) {
        syncTime();
      }
      break;

    case Mode::AccessPoint:
      if (config.hasWifi() && (millis() - lastRetry > kRetryIntervalMs)) {
        lastRetry = millis();
        startFirstConfiguredNetwork();
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

void reassertMode() {
  // Never touches AccessPoint - see the doc comment in Net.h for why forcing
  // WiFi.mode(WIFI_AP) back here is worse than the merge it would be undoing.
  if (currentMode != Mode::AccessPoint) {
    WiFi.mode(WIFI_STA);
  }
}

}  // namespace net
