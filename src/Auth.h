// Password check and per-account session tracking for the web panel.
//
// One session per account, by explicit requirement - not a single global
// session, and not a fixed pool of session slots either. Each configured
// user (see Config::User) gets its own session, exactly like the original
// single-account design: logging the same account in twice is refused with
// the address of the holder and can take over deliberately (without a
// takeover path, a browser tab closed without logging out would lock that
// account until the bridge was rebooted). With two accounts configured this
// naturally allows two operators to be logged in at once, each from their
// own device.

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

// Account management. Available to any authenticated session - the panel has
// no admin/operator role split, so this follows the same flat model as the
// rest of Settings.
//
// Creates the account if `user` does not exist yet (subject to Config::kMaxUsers),
// otherwise resets its password. Returns false if the password is under 8
// characters, the name is empty, or (only when creating) every slot is full.
bool upsertUser(const char* user, const char* password);

// Refuses if `user` would be the last remaining account - the panel must
// always have somewhere to log in from. Logs out that account's session
// first if it has one active.
bool deleteUser(const char* user);

bool checkPassword(const char* user, const char* password);

// Distinguishes *why* a login attempt failed - the caller must not guess this
// from other state (e.g. whether the account has an active session) after the
// fact, because that state is true or false independently of whether the
// password just submitted was actually correct. Reporting "session held" for
// a wrong password would hand an unauthenticated caller the holder's address
// for any account they can merely name.
enum class LoginResult : uint8_t { Ok, InvalidCredentials, SessionHeld };

// On Ok, tokenOut holds the new session token; left untouched otherwise.
LoginResult login(const char* user, const char* password, const IPAddress& address, bool force, String& tokenOut);

// True while login is refused outright after repeated failures. Deliberately
// one shared counter across every account, not one per account - a per-account
// throttle would let repeated wrong guesses against different accounts run in
// parallel without ever tripping a limit. Guessing a password over HTTP is
// otherwise limited only by how fast the ESP can hash.
bool throttled();
uint32_t throttleRemainingMs();
void logout(const char* token);

// Validates the cookie and refreshes the idle timer.
bool validate(const char* token, const IPAddress& address);

// The account name that owns this token, or nullptr - used to tell a
// reloaded browser which of possibly several accounts its cookie belongs to.
const char* userForToken(const char* token);

void poll();  // expires idle sessions, independently per account

// nullptr if `user` does not exist.
const SessionInfo* session(const char* user);

}  // namespace auth
