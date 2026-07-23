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

// An idle tab must not hold the panel forever, but the timeout has to be long
// enough to sit through a slow QSO without being logged out mid-rotation.
const uint32_t kIdleTimeoutMs = 15UL * 60UL * 1000UL;

// Five wrong guesses buys a minute of silence. Enough to stop an automated
// run without locking out an operator who mistyped twice in the dark.
const int kMaxFailures = 5;
const uint32_t kThrottleMs = 60000;

SessionInfo current;
char token[33] = "";

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

}  // namespace

void begin() {
  current.active = false;
  token[0] = '\0';
}

bool setPassword(const char* password) {
  if (password == nullptr || strlen(password) < 8) {
    return false;
  }
  char salt[33];
  randomHex(salt, 32);

  char hash[65];
  if (!hashPassword(password, salt, hash)) {
    return false;
  }

  strncpy(config.webPasswordSalt, salt, sizeof(config.webPasswordSalt) - 1);
  config.webPasswordSalt[sizeof(config.webPasswordSalt) - 1] = '\0';
  strncpy(config.webPasswordHash, hash, sizeof(config.webPasswordHash) - 1);
  config.webPasswordHash[sizeof(config.webPasswordHash) - 1] = '\0';

  return config.save();
}

bool checkPassword(const char* password) {
  if (password == nullptr || config.needsPasswordSetup()) {
    return false;
  }
  char hash[65];
  if (!hashPassword(password, config.webPasswordSalt, hash)) {
    return false;
  }
  return constantTimeEquals(hash, config.webPasswordHash);
}

bool throttled() {
  return static_cast<int32_t>(millis() - throttledUntil) < 0;
}

uint32_t throttleRemainingMs() {
  return throttled() ? (throttledUntil - millis()) : 0;
}

String login(const char* user, const char* password, const IPAddress& address, bool force) {
  if (throttled()) {
    return String();
  }
  if (user == nullptr || strcmp(user, config.webUser) != 0 || !checkPassword(password)) {
    if (++failures >= kMaxFailures) {
      throttledUntil = millis() + kThrottleMs;
      failures = 0;
    }
    return String();
  }
  failures = 0;

  if (current.active && !force) {
    return String();
  }

  randomHex(token, 32);
  current.active = true;
  current.address = address;
  current.startedAt = millis();
  current.lastSeenAt = millis();
  return String(token);
}

void logout(const char* candidate) {
  if (candidate != nullptr && current.active && constantTimeEquals(candidate, token)) {
    current.active = false;
    token[0] = '\0';
  }
}

bool validate(const char* candidate, const IPAddress& address) {
  (void)address;  // the address is reported, not enforced: phones roam networks
  if (!current.active || candidate == nullptr || token[0] == '\0') {
    return false;
  }
  if (!constantTimeEquals(candidate, token)) {
    return false;
  }
  current.lastSeenAt = millis();
  return true;
}

void poll() {
  if (current.active && (millis() - current.lastSeenAt > kIdleTimeoutMs)) {
    current.active = false;
    token[0] = '\0';
  }
}

const SessionInfo& session() {
  return current;
}

}  // namespace auth
