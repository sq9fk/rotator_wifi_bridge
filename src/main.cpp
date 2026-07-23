// WiFi bridge to a K3NG GS-232B rotator controller.
//
// This is phase 1 of the plan in DESIGN.md: the serial transaction layer and
// the position cache, exercised over the USB serial monitor. WiFi, the web
// panel, rotctld and the raw passthrough socket come next; the structure here
// is what they all plug into.

#include <Arduino.h>

#include "Gs232.h"
#include "RotatorLink.h"

namespace {

// A real hardware UART on arbitrary pins, courtesy of the ESP32 GPIO matrix -
// no SoftwareSerial. UART0 stays on USB for the console, so its boot-time
// output never reaches the controller as garbage commands.
const int8_t kControllerRxPin = 18;  // to the controller TX, via a divider
const int8_t kControllerTxPin = 17;  // to the controller RX
const uint32_t kControllerBaud = 9600;

// Fast enough for a responsive display, slow enough to leave the line free.
const uint32_t kPollIntervalMs = 300;

// Beyond this the cached azimuth is stale and must not be presented as live.
const uint32_t kPositionStaleMs = 2000;

HardwareSerial& controllerPort = Serial1;
gs232::AzimuthRange azRange;
RotatorLink rotatorLink(controllerPort, azRange);

// Cached as raw, not real. The controller's I command reports which turn the
// rotator is on; a real azimuth of 0..359 cannot express that, and inferring it
// is guesswork that is wrong half the time in the overlap zone.
struct {
  float rawAz = 0.0f;
  uint32_t updatedAt = 0;
  bool valid = false;
} position;

RotatorLink::Source lastMotionSource = RotatorLink::Source::Poller;
uint32_t lastMotionAt = 0;

const char* sourceName(RotatorLink::Source source) {
  switch (source) {
    case RotatorLink::Source::Web: return "web";
    case RotatorLink::Source::Rotctld: return "rotctld";
    case RotatorLink::Source::Raw: return "raw";
    default: return "poller";
  }
}

void onReply(uint32_t id, RotatorLink::Source source, RotatorLink::Result result, const char* reply, void* ctx) {
  (void)id;
  (void)ctx;

  switch (result) {
    case RotatorLink::Result::Reply: {
      float raw = 0.0f;
      if (gs232::parseRawReply(reply, raw)) {
        position.rawAz = raw;
        position.updatedAt = millis();
        position.valid = true;
      } else {
        // Stall reports and command acknowledgements land here.
        Serial.printf("[%s] %s\n", sourceName(source), reply);
      }
      break;
    }

    case RotatorLink::Result::Rejected:
      Serial.printf("[%s] rejected\n", sourceName(source));
      break;

    case RotatorLink::Result::Timeout:
      Serial.printf("[%s] timeout\n", sourceName(source));
      position.valid = false;
      break;

    case RotatorLink::Result::NoReply:
      break;
  }
}

bool positionIsFresh() {
  return position.valid && (millis() - position.updatedAt) < kPositionStaleMs;
}

// Entry point for every control source. Keeping the coordinate mapping here
// rather than in the callers is deliberate: the controller reports a real
// azimuth but accepts a raw one, and that asymmetry should be handled once.
bool requestAzimuth(float desiredReal, RotatorLink::Source source) {
  if (!positionIsFresh()) {
    return false;  // choosing a target needs a known current position
  }

  const int target = gs232::chooseRawTarget(desiredReal, position.rawAz, azRange);
  if (target < 0) {
    return false;
  }

  char command[8];
  if (gs232::buildGoto(command, sizeof(command), target) == 0) {
    return false;
  }

  if (rotatorLink.submit(command, source) == 0) {
    return false;
  }

  lastMotionSource = source;
  lastMotionAt = millis();
  return true;
}

void servicePolling() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < kPollIntervalMs) {
    return;
  }
  lastPoll = millis();
  // I rather than C: one command yields the raw position, and the real azimuth
  // follows from it locally.
  rotatorLink.submit("I", RotatorLink::Source::Poller);
}

// Temporary console for phase 1: "123" rotates, "s" stops, "?" reports.
void serviceConsole() {
  static char buf[16];
  static size_t len = 0;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (len == 0) {
        continue;
      }
      buf[len] = '\0';
      len = 0;

      if (buf[0] == 's' || buf[0] == 'S') {
        rotatorLink.submit("S", RotatorLink::Source::Web);
      } else if (buf[0] == '?') {
        Serial.printf("az=%.0f raw=%.0f fresh=%d lockout=%d last motion=%s\n", gs232::rawToReal(position.rawAz),
                      position.rawAz, positionIsFresh(), rotatorLink.inBootLockout(), sourceName(lastMotionSource));
      } else {
        const float target = atof(buf);
        if (!requestAzimuth(target, RotatorLink::Source::Web)) {
          Serial.println("rejected");
        }
      }
      continue;
    }
    if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("rotator_wifi_bridge");

  controllerPort.begin(kControllerBaud, SERIAL_8N1, kControllerRxPin, kControllerTxPin);
  rotatorLink.setReplyHandler(onReply, nullptr);
  rotatorLink.begin();
}

void loop() {
  rotatorLink.poll();
  servicePolling();
  serviceConsole();
  yield();
}
