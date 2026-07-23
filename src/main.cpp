// WiFi bridge to a K3NG GS-232B rotator controller.
//
// Phase 2 of the plan in DESIGN.md: the serial layer from phase 1 plus WiFi
// with an AP fallback, configuration in LittleFS and a REST API. rotctld, the
// raw passthrough socket and the web panel come next; they all plug into the
// same Rotator object, so no source bypasses the command queue.

#include <Arduino.h>

#include "Config.h"
#include "Gs232.h"
#include "Net.h"
#include "RawServer.h"
#include "Rotator.h"
#include "RotctldServer.h"
#include "WebApi.h"

namespace {

// A real hardware UART on arbitrary pins, courtesy of the ESP32 GPIO matrix -
// no SoftwareSerial. UART0 stays on USB for the console, so its boot-time
// output never reaches the controller as garbage commands.
// The baud rate is configurable and shared: one UART means one rate for the
// rotctld path, the raw path and the poller alike.
const int8_t kControllerRxPin = 18;  // to the controller TX, via a divider
const int8_t kControllerTxPin = 17;  // to the controller RX

gs232::AzimuthRange azRange;
Rotator rotator(Serial1, azRange);

// Constructed in setup(), once the configured ports have been read.
RotctldServer* rotctld = nullptr;
RawServer* rawServer = nullptr;

// Temporary console: "123" rotates, "s" stops, "?" reports.
void serviceConsole() {
  static char buf[16];
  static size_t len = 0;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c != '\r' && c != '\n') {
      if (len < sizeof(buf) - 1) {
        buf[len++] = c;
      }
      continue;
    }
    if (len == 0) {
      continue;
    }
    buf[len] = '\0';
    len = 0;

    if (buf[0] == 's' || buf[0] == 'S') {
      rotator.stop(RotatorLink::Source::Web);
    } else if (buf[0] == '?') {
      Serial.printf("az=%.0f raw=%.0f fresh=%d lockout=%d net=%s addr=%s heap=%u\n", rotator.realAzimuth(),
                    rotator.rawAzimuth(), rotator.positionIsFresh(), rotator.inBootLockout(), net::modeName(),
                    net::address().c_str(), ESP.getFreeHeap());
    } else if (!rotator.gotoAzimuth(atof(buf), RotatorLink::Source::Web)) {
      Serial.println("rejected");
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("rotator_wifi_bridge");

  config.load();  // defaults are usable, so a missing file is not an error
  azRange.rawMin = config.rawMin;
  azRange.rawMax = config.rawMax;

  Serial1.begin(config.serialBaud, SERIAL_8N1, kControllerRxPin, kControllerTxPin);
  rotator.begin();

  net::begin();

  rotctld = new RotctldServer(rotator, config.rotctldPort);
  rotctld->begin();

  rawServer = new RawServer(rotator, config.rawPort);
  rawServer->begin();

  webapi::begin(rotator, *rotctld, *rawServer);

  Serial.printf("config: %s, az range %d..%d, %lu baud, rotctld %u, raw %u\n",
                config.hasWifi() ? config.wifiSsid : "(no wifi, AP mode)", azRange.rawMin, azRange.rawMax,
                static_cast<unsigned long>(config.serialBaud), config.rotctldPort, config.rawPort);
}

void loop() {
  rotator.poll();
  net::poll();
  rotctld->poll();
  rawServer->poll();
  serviceConsole();
}
