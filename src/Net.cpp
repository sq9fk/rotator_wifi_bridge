#include "Net.h"

#include <ESPmDNS.h>
#include <WiFi.h>

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

Mode currentMode = Mode::Connecting;
uint32_t attemptStarted = 0;
uint32_t lastRetry = 0;

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
      } else if (millis() - attemptStarted > kConnectTimeoutMs) {
        startAccessPoint();
        announce();
        lastRetry = millis();
      }
      break;

    case Mode::Station:
      if (WiFi.status() != WL_CONNECTED) {
        startStation();
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

}  // namespace net
