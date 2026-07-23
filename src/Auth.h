// Password check and single-session tracking for the web panel.
//
// One session at a time, by explicit requirement. A second login is refused
// with the address of the holder and can take over deliberately - without a
// takeover path, a browser tab closed without logging out would lock the panel
// until the bridge was rebooted.

#pragma once

#include <Arduino.h>

namespace auth {

struct SessionInfo {
  bool active = false;
  IPAddress address;
  uint32_t startedAt = 0;
  uint32_t lastSeenAt = 0;
};

void begin();

// Hashes with a fresh random salt and stores both in the config. Used by first
// run setup and by a password change.
bool setPassword(const char* password);
bool checkPassword(const char* password);

// Returns an empty string if the password is wrong, or if a different session
// holds the panel and force is false.
String login(const char* user, const char* password, const IPAddress& address, bool force);

// True while login is refused outright after repeated failures. Guessing a
// password over HTTP is otherwise limited only by how fast the ESP can hash.
bool throttled();
uint32_t throttleRemainingMs();
void logout(const char* token);

// Validates the cookie and refreshes the idle timer.
bool validate(const char* token, const IPAddress& address);

void poll();  // expires an idle session

const SessionInfo& session();

}  // namespace auth
