// Persistent configuration, stored as JSON in LittleFS.
//
// Kept deliberately small and flat: every field has a working default, so a
// missing or corrupt file is not an error condition - the bridge boots with
// defaults and puts itself in AP mode for setup.

#pragma once

#include <Arduino.h>

struct Config {
  static const size_t kStrLen = 40;

  char wifiSsid[kStrLen] = "";
  char wifiPassword[kStrLen] = "";
  char hostname[kStrLen] = "rotator";

  char webUser[kStrLen] = "admin";
  char webPasswordHash[65] = "";   // PBKDF2-SHA256, hex; empty = setup required
  char webPasswordSalt[33] = "";

  uint16_t rotctldPort = 4533;
  uint16_t rawPort = 4532;

  // One UART to the controller, so one baud rate: it governs both the rotctld
  // and the raw path. 9600 is CONTROL_PORT_BAUD_RATE in the controller's
  // rotator_settings.h; changing it here means changing it there too.
  uint32_t serialBaud = 9600;

  int rawMin = 180;                // must match the controller's starting point
  int rawMax = 630;                // starting point + rotation capability

  bool load();      // false if the file was missing or unparseable (defaults kept)
  bool save() const;

  bool hasWifi() const { return wifiSsid[0] != '\0'; }
  bool needsPasswordSetup() const { return webPasswordHash[0] == '\0'; }
};

extern Config config;
