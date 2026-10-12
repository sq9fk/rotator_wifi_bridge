#include "Heartbeat.h"

#include <Arduino.h>

namespace heartbeat {

namespace {

const uint32_t kHeartbeatPeriodMs = 1000;
const uint32_t kOnMs = 60;  // both the idle pulse's width and the activity flash's width

// The LOLIN S3 Mini's WS2812, driven through the RMT peripheral directly
// rather than neopixelWrite(RGB_BUILTIN, ...): the board variant changed what
// RGB_BUILTIN means between Arduino-ESP32 2.0.11 (plain 47) and 2.0.17
// (47 + SOC_GPIO_PIN_COUNT, a "virtual" pin neopixelWrite() maps back), and
// neopixelWrite()'s mapping only works with one of the two - so the LED went
// dark depending on which core a build happened to use. A real GPIO number
// means the same thing on every core.
const int kLedGpio = 47;

rmt_obj_t* rmt = nullptr;

uint32_t lastActivityMs = 0;
bool everActive = false;

// Avoids re-sending the WS2812 bit stream every loop() iteration when the
// colour hasn't changed - loop() runs far more often than the LED state does.
uint8_t lastG = 0;
uint8_t lastB = 0;

// WS2812 timing at a 100 ns RMT tick: a 1 is 0.8 us high + 0.4 us low, a 0 is
// 0.4 us high + 0.8 us low; colour order on the wire is green, red, blue.
void write(uint8_t r, uint8_t g, uint8_t b) {
  if (rmt == nullptr) {
    return;
  }
  rmt_data_t bits[24];
  const uint8_t colour[3] = {g, r, b};
  int i = 0;
  for (uint8_t c : colour) {
    for (int bit = 7; bit >= 0; bit--) {
      const bool one = (c >> bit) & 1;
      bits[i].level0 = 1;
      bits[i].duration0 = one ? 8 : 4;
      bits[i].level1 = 0;
      bits[i].duration1 = one ? 4 : 8;
      i++;
    }
  }
  rmtWriteBlocking(rmt, bits, 24);  // 30 us
}

}  // namespace

void begin() {
  rmt = rmtInit(kLedGpio, RMT_TX_MODE, RMT_MEM_64);
  if (rmt != nullptr) {
    rmtSetTick(rmt, 100);
  }
  write(0, 0, 0);
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
    write(0, g, b);
    lastG = g;
    lastB = b;
  }
}

}  // namespace heartbeat
