#include "Heartbeat.h"

#include <Arduino.h>

namespace heartbeat {

namespace {

const uint32_t kHeartbeatPeriodMs = 1000;
const uint32_t kOnMs = 60;  // both the idle pulse's width and the activity flash's width

uint32_t lastActivityMs = 0;
bool everActive = false;

// Avoids re-sending the WS2812 bit stream every loop() iteration when the
// colour hasn't changed - neopixelWrite() bit-bangs it, and loop() runs far
// more often than the LED state does.
uint8_t lastG = 0;
uint8_t lastB = 0;

}  // namespace

void begin() {
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
}

void noteActivity() {
  lastActivityMs = millis();
  everActive = true;
}

void poll() {
  const uint32_t now = millis();
  uint8_t g = 0;
  uint8_t b = 0;

  if (everActive && now - lastActivityMs < kOnMs) {
    g = 40;  // controller link just sent or received a line
  } else if (now % kHeartbeatPeriodMs < kOnMs) {
    b = 30;  // idle heartbeat: firmware is alive even with no traffic yet
  }

  if (g != lastG || b != lastB) {
    neopixelWrite(RGB_BUILTIN, 0, g, b);
    lastG = g;
    lastB = b;
  }
}

}  // namespace heartbeat
