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

  // The name shown in the panel's top bar and on the login screen. Separate
  // from hostname, which is the technical mDNS name.
  char siteName[kStrLen] = "RotorBridge";

  // Panel accounts. A fixed-size table, like Favorites or the rotctld/raw
  // client limits elsewhere in this file - an empty name marks an unused
  // slot. Seeded with two accounts for the two operators of this station;
  // more can be added later from the panel's own Settings (see Auth.h), up
  // to kMaxUsers. Three: two operators plus one spare, not a round number -
  // rotctld's and raw's own client ceilings scale with this (see their
  // headers), and the ESP32's BSD socket pool is finite, so this is not a
  // dial to turn up casually. An empty passwordHash means the account exists
  // but has not been through first-run setup yet.
  struct User {
    char name[kStrLen] = "";
    char passwordHash[65] = "";
    char passwordSalt[33] = "";
  };
  static const size_t kMaxUsers = 3;
  User users[kMaxUsers];

  // Seeds the two starting accounts. Not a default member initializer on
  // `users` itself: this toolchain's C++ standard treats a struct with
  // default member initializers (User's name/passwordHash/passwordSalt) as
  // no longer an aggregate, so `User users[2] = {{"sq9fk"}, {"sq9um"}}` does
  // not compile - a constructor sidesteps that entirely.
  Config() {
    strncpy(users[0].name, "sq9fk", kStrLen - 1);
    strncpy(users[1].name, "sq9um", kStrLen - 1);
  }

  User* findUser(const char* name) {
    for (size_t i = 0; i < kMaxUsers; i++) {
      if (users[i].name[0] != '\0' && strcmp(users[i].name, name) == 0) {
        return &users[i];
      }
    }
    return nullptr;
  }

  size_t userCount() const {
    size_t n = 0;
    for (size_t i = 0; i < kMaxUsers; i++) {
      if (users[i].name[0] != '\0') {
        n++;
      }
    }
    return n;
  }

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

  // ant-sw-2x6 (a separate device: a 6-antenna x 2-TRX switch) reachable over
  // the LAN at its own fixed HTTP port 80 - not configurable, it is not this
  // bridge's port to choose. Off by default: nothing probes the network for
  // it until the operator turns it on and points it somewhere.
  bool antEnabled = false;
  char antHost[kStrLen] = "";

  bool load();      // false if the file was missing or unparseable (defaults kept)
  bool save() const;

  bool hasWifi() const { return wifiSsid[0] != '\0'; }

  // An account exists but has not been through first-run setup yet.
  static bool needsSetup(const User& user) { return user.passwordHash[0] == '\0'; }
};

extern Config config;
