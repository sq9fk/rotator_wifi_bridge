#include "ControllerPort.h"

#include "Config.h"

namespace controllerport {
namespace {

// A real hardware UART on arbitrary pins, courtesy of the ESP32 GPIO matrix -
// no SoftwareSerial. UART0 stays on USB for the console, so its boot-time
// output never reaches the controller as garbage commands.
const int8_t kControllerRxPin = 18;  // to the controller TX, via a divider
const int8_t kControllerTxPin = 17;  // to the controller RX

// Long enough for any flash of the controller, short enough that a forgotten
// release does not silently cut loggers off for the rest of a contest.
const uint32_t kAutoAttachMs = 15UL * 60 * 1000;

enum class State : uint8_t { Attached, Releasing, Released };

Rotator* rotator = nullptr;
State state = State::Attached;
uint32_t releasedAt = 0;
char releasedBy[Config::kStrLen] = "";

void open() {
  // The baud rate is configurable and shared: one UART means one rate for the
  // rotctld path, the raw path and the poller alike.
  Serial1.begin(config.serialBaud, SERIAL_8N1, kControllerRxPin, kControllerTxPin);
}

}  // namespace

void begin(Rotator& r) {
  rotator = &r;
  open();
}

void poll() {
  if (state == State::Releasing && rotator->lineClosed()) {
    Serial1.flush();  // the final stop must be on the wire, not in a FIFO
    Serial1.end();
    // Serial1.end() leaves the pad configured; an input with no pull is what
    // actually lets go of D0.
    pinMode(kControllerTxPin, INPUT);
    state = State::Released;
    releasedAt = millis();
    Serial.println("controller line released");
  }
  if (state == State::Released && millis() - releasedAt > kAutoAttachMs) {
    attach();
  }
}

bool release(const char* by) {
  if (state != State::Attached) {
    return false;
  }
  // Stop first: once the line is gone nothing could stop a rotation in progress.
  rotator->stop(RotatorLink::Source::Web);
  rotator->suspendLine();
  strncpy(releasedBy, by != nullptr ? by : "", sizeof(releasedBy) - 1);
  releasedBy[sizeof(releasedBy) - 1] = '\0';
  state = State::Releasing;
  return true;
}

bool attach() {
  if (state != State::Released) {
    return false;
  }
  open();
  while (Serial1.available() > 0) {
    Serial1.read();  // whatever the flash left on the line
  }
  rotator->resumeLine();
  state = State::Attached;
  Serial.println("controller line attached");
  return true;
}

bool released() { return state != State::Attached; }

void fillStatus(JsonObject controller) {
  controller["line"] = state == State::Attached ? "attached" : (state == State::Releasing ? "releasing" : "released");
  if (state == State::Released) {
    const uint32_t age = millis() - releasedAt;
    controller["releasedAgoS"] = age / 1000;
    controller["autoAttachInS"] = age < kAutoAttachMs ? (kAutoAttachMs - age) / 1000 : 0;
  }
  if (state != State::Attached && releasedBy[0] != '\0') {
    controller["releasedBy"] = releasedBy;
  }
}

}  // namespace controllerport
