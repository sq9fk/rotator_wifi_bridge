#include "Gs232.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace gs232 {

float rawToReal(float raw) {
  float real = raw;
  while (real >= 360.0f) {
    real -= 360.0f;
  }
  while (real < 0.0f) {
    real += 360.0f;
  }
  return real;
}

int chooseRawTarget(float desiredReal, float currentRaw, const AzimuthRange& range) {
  const float real = rawToReal(desiredReal);

  int best = -1;
  float bestTravel = 0.0f;

  // A real azimuth maps to raw values spaced 360 apart; on a 450 degree
  // rotator at most two of them fall inside the reachable range.
  for (int turn = -1; turn <= 2; turn++) {
    const float candidate = real + (360.0f * turn);
    if (candidate < range.rawMin || candidate > range.rawMax) {
      continue;
    }
    const float travel = fabsf(candidate - currentRaw);
    if (best < 0 || travel < bestTravel) {
      best = static_cast<int>(lroundf(candidate));
      bestTravel = travel;
    }
  }

  return best;
}

size_t buildGoto(char* buf, size_t bufLen, int raw) {
  if (raw < 0 || raw > 999 || bufLen < 5) {
    return 0;
  }
  const int written = snprintf(buf, bufLen, "M%03d", raw);
  return (written == 4) ? 4 : 0;
}

bool parseAzimuthReply(const char* reply, float& realAzOut) {
  if (reply == nullptr) {
    return false;
  }

  const char* az = strstr(reply, "AZ=");
  if (az == nullptr) {
    return false;
  }
  az += 3;

  // Exactly three digits, per the GS-232B format.
  float value = 0.0f;
  for (int i = 0; i < 3; i++) {
    if (!isdigit(static_cast<unsigned char>(az[i]))) {
      return false;
    }
    value = (value * 10.0f) + (az[i] - '0');
  }

  realAzOut = value;
  return true;
}

bool isErrorReply(const char* reply) {
  return reply != nullptr && strncmp(reply, "?>", 2) == 0;
}

ReplyKind classify(const char* command) {
  if (command == nullptr || command[0] == '\0') {
    return ReplyKind::Immediate;
  }

  switch (toupper(static_cast<unsigned char>(command[0]))) {
    case 'C':
    case 'D':
    case 'X':
      return ReplyKind::Immediate;

    case 'H':
      // OPTION_SERIAL_HELP_TEXT is disabled on the controller, so H prints
      // nothing at all rather than a help screen.
      return ReplyKind::None;

    case 'M':
    case 'L':
    case 'R':
    case 'A':
    case 'S':
      return ReplyKind::ErrorOnly;

    default:
      // Everything else - including the stripped backslash and extended
      // commands - lands on the controller's default handler and answers "?>".
      return ReplyKind::Immediate;
  }
}

}  // namespace gs232
