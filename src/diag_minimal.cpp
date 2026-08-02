// Throwaway diagnostic build (env:diag_minimal) - isolates whether the board
// heats up from bare Arduino + PSRAM alone, or only once WiFi/AsyncTCP/the
// web server/the controller UART poll are added on top. Not part of the
// real firmware; not linked into env:lolin_s3_mini.
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  Serial.println("diag_minimal: no WiFi, no AsyncTCP, no UART polling");
}

void loop() {
  static uint32_t last = 0;
  static bool on = false;
  if (millis() - last >= 500) {
    last = millis();
    on = !on;
    neopixelWrite(RGB_BUILTIN, 0, on ? 30 : 0, 0);
    Serial.println(on ? "on" : "off");
  }
}
