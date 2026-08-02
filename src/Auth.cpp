#include "Auth.h"

#include <mbedtls/md.h>
#include <string.h>

#include "Config.h"

namespace auth {
namespace {

// Salted, iterated SHA-256 - not PBKDF2, and named honestly rather than
// claimed to be something it is not. The point here is that a dump of
// LittleFS must not hand over a reusable password; 10k iterations on a 240 MHz
// S3 costs a few milliseconds per login and makes an offline guessing run
// correspondingly slower.
const int kIterations = 10000;

// An idle tab must not hold its account forever, but the timeout has to be
// long enough to sit through a slow QSO without being logged out mid-rotation.
const uint32_t kIdleTimeoutMs = 15UL * 60UL * 1000UL;

// Five wrong guesses buys a minute of silence, shared across every account
// (see Auth.h - deliberately not per account). Enough to stop an automated
// run without locking out an operator who mistyped twice in the dark.
const int kMaxFailures = 5;
const uint32_t kThrottleMs = 60000;

// One slot per configured account, index-aligned with config.users[] (a
// Config::User and its Slot always share the same index). SessionInfo is the
// part of a slot the rest of the firmware is allowed to see; the token stays
// private to this file.
struct Slot {
  SessionInfo info;
  char token[33] = "";
};
Slot slots[Config::kMaxUsers];

int failures = 0;
uint32_t throttledUntil = 0;

void toHex(const uint8_t* bytes, size_t len, char* out) {
  static const char* kDigits = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = kDigits[bytes[i] >> 4];
    out[i * 2 + 1] = kDigits[bytes[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

void randomHex(char* out, size_t hexLen) {
  static const char* kDigits = "0123456789abcdef";
  for (size_t i = 0; i < hexLen; i++) {
    out[i] = kDigits[esp_random() & 0x0f];
  }
  out[hexLen] = '\0';
}

bool hashPassword(const char* password, const char* salt, char* hexOut) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) {
    return false;
  }

  uint8_t digest[32];

  // First round over salt || password, then iterate over the digest.
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 0) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, reinterpret_cast<const uint8_t*>(salt), strlen(salt));
  mbedtls_md_update(&ctx, reinterpret_cast<const uint8_t*>(password), strlen(password));
  mbedtls_md_finish(&ctx, digest);

  for (int i = 1; i < kIterations; i++) {
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, digest, sizeof(digest));
    mbedtls_md_finish(&ctx, digest);
  }
  mbedtls_md_free(&ctx);

  toHex(digest, sizeof(digest), hexOut);
  return true;
}

// Length-independent compare, so a wrong guess cannot be narrowed down by
// timing the response.
bool constantTimeEquals(const char* a, const char* b) {
  const size_t lenA = strlen(a);
  const size_t lenB = strlen(b);
  uint8_t diff = (lenA == lenB) ? 0 : 1;
  const size_t len = (lenA < lenB) ? lenA : lenB;
  for (size_t i = 0; i < len; i++) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

// -1 if `name` does not match a configured account.
int findUserIndex(const char* name) {
  if (name == nullptr) {
    return -1;
  }
  Config::User* u = config.findUser(name);
  return u ? static_cast<int>(u - config.users) : -1;
}

}  // namespace

void begin() {
  for (size_t i = 0; i < Config::kMaxUsers; i++) {
    slots[i] = Slot{};
  }
}

bool upsertUser(const char* user, const char* password) {
  if (user == nullptr || user[0] == '\0' || password == nullptr || strlen(password) < 8) {
    return false;
  }

  Config::User* u = config.findUser(user);
  if (u == nullptr) {
    for (size_t i = 0; i < Config::kMaxUsers; i++) {
      if (config.users[i].name[0] == '\0') {
        u = &config.users[i];
        break;
      }
    }
    if (u == nullptr) {
      return false;  // every slot taken
    }
    strncpy(u->name, user, Config::kStrLen - 1);
    u->name[Config::kStrLen - 1] = '\0';
  }

  char salt[33];
  randomHex(salt, 32);
  char hash[65];
  if (!hashPassword(password, salt, hash)) {
    return false;
  }
  strncpy(u->passwordSalt, salt, sizeof(u->passwordSalt) - 1);
  u->passwordSalt[sizeof(u->passwordSalt) - 1] = '\0';
  strncpy(u->passwordHash, hash, sizeof(u->passwordHash) - 1);
  u->passwordHash[sizeof(u->passwordHash) - 1] = '\0';

  return config.save();
}

bool deleteUser(const char* user) {
  const int idx = findUserIndex(user);
  if (idx < 0 || config.userCount() <= 1) {
    return false;
  }
  slots[idx] = Slot{};             // logs out any session this account held
  config.users[idx] = Config::User{};
  return config.save();
}

bool checkPassword(const char* user, const char* password) {
  const int idx = findUserIndex(user);
  if (idx < 0 || password == nullptr || Config::needsSetup(config.users[idx])) {
    return false;
  }
  char hash[65];
  if (!hashPassword(password, config.users[idx].passwordSalt, hash)) {
    return false;
  }
  return constantTimeEquals(hash, config.users[idx].passwordHash);
}

bool throttled() {
  return static_cast<int32_t>(millis() - throttledUntil) < 0;
}

uint32_t throttleRemainingMs() {
  return throttled() ? (throttledUntil - millis()) : 0;
}

LoginResult login(const char* user, const char* password, const IPAddress& address, bool force, String& tokenOut) {
  if (throttled()) {
    return LoginResult::InvalidCredentials;
  }
  const int idx = findUserIndex(user);
  if (idx < 0 || !checkPassword(user, password)) {
    if (++failures >= kMaxFailures) {
      throttledUntil = millis() + kThrottleMs;
      failures = 0;
    }
    return LoginResult::InvalidCredentials;
  }
  failures = 0;

  Slot& slot = slots[idx];
  if (slot.info.active && !force) {
    // Only reachable once the password above has already been verified
    // correct - this must never be checked before that, or an unauthenticated
    // caller could learn whether an account is logged in (and from where)
    // just by naming it, without knowing its password at all.
    return LoginResult::SessionHeld;
  }

  randomHex(slot.token, 32);
  slot.info.active = true;
  slot.info.address = address;
  slot.info.startedAt = millis();
  slot.info.lastSeenAt = millis();
  tokenOut = String(slot.token);
  return LoginResult::Ok;
}

void logout(const char* candidate) {
  if (candidate == nullptr) {
    return;
  }
  for (size_t i = 0; i < Config::kMaxUsers; i++) {
    if (slots[i].info.active && constantTimeEquals(candidate, slots[i].token)) {
      slots[i] = Slot{};
      return;
    }
  }
}

bool validate(const char* candidate, const IPAddress& address) {
  (void)address;  // the address is reported, not enforced: phones roam networks
  if (candidate == nullptr || candidate[0] == '\0') {
    return false;
  }
  for (size_t i = 0; i < Config::kMaxUsers; i++) {
    if (slots[i].info.active && constantTimeEquals(candidate, slots[i].token)) {
      slots[i].info.lastSeenAt = millis();
      return true;
    }
  }
  return false;
}

const char* userForToken(const char* token) {
  if (token == nullptr || token[0] == '\0') {
    return nullptr;
  }
  for (size_t i = 0; i < Config::kMaxUsers; i++) {
    if (slots[i].info.active && constantTimeEquals(token, slots[i].token)) {
      return config.users[i].name;
    }
  }
  return nullptr;
}

void poll() {
  for (size_t i = 0; i < Config::kMaxUsers; i++) {
    if (slots[i].info.active && (millis() - slots[i].info.lastSeenAt > kIdleTimeoutMs)) {
      slots[i] = Slot{};
    }
  }
}

const SessionInfo* session(const char* user) {
  const int idx = findUserIndex(user);
  return (idx < 0) ? nullptr : &slots[idx].info;
}

}  // namespace auth
