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

  // How many clients each server accepts at once. Clamped at load time to the
  // compile-time ceilings (RotctldServer::kClientCeiling / RawServer::) which
  // are sized for the ESP32's BSD-socket pool - raising these beyond that just
  // clamps. rotctld allows several loggers; raw defaults to one because it
  // emulates a single serial cable, though 2 is safe (replies are routed per
  // transaction id, so there is no packet collision - only shared control).
  uint8_t rotctldMaxClients = 2;
  uint8_t rawMaxClients = 1;

  // One UART to the controller, so one baud rate: it governs both the rotctld
  // and the raw path. 9600 is CONTROL_PORT_BAUD_RATE in the controller's
  // rotator_settings.h; changing it here means changing it there too.
  uint32_t serialBaud = 9600;

  // The rotator's full-CCW mechanical stop is at bearing 180 and it carries 45
  // degrees of overlap, so it sweeps 180 -> 585 raw. These must match
  // AZIMUTH_STARTING_POINT and AZIMUTH_ROTATION_CAPABILITY in the controller;
  // they decide which way it turns to reach a bearing.
  int rawMin = 180;
  int rawMax = 585;

  // Bearings reachable two different ways, drawn as the red arc on the dial.
  // Sweeps clockwise from overlapFrom to overlapTo. Defaults follow from the
  // range above - raw 180..359 covers bearings 180..359 and raw 360..585
  // covers 0..225, so their intersection, 180..225, is the ambiguous band -
  // but they stay configurable, because the operator with the rotator in hand
  // is the authority on what it physically does.
  int overlapFrom = 180;
  int overlapTo = 225;

  bool load();      // false if the file was missing or unparseable (defaults kept)
  bool save() const;

  bool hasWifi() const { return wifiSsid[0] != '\0'; }
  bool needsPasswordSetup() const { return webPasswordHash[0] == '\0'; }
};

extern Config config;
