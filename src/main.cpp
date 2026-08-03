// WiFi bridge to a K3NG GS-232B rotator controller.
//
// Phase 2 of the plan in DESIGN.md: the serial layer from phase 1 plus WiFi
// with an AP fallback, configuration in LittleFS and a REST API. rotctld, the
// raw passthrough socket and the web panel come next; they all plug into the
// same Rotator object, so no source bypasses the command queue.

#include <Arduino.h>  // explicit and first, so IntelliSense resolves Arduino types before anything else here
#include <esp_task_wdt.h>

#include "AntennaSwitch.h"
#include "Auth.h"
#include "Config.h"
#include "Gs232.h"
#include "Heartbeat.h"
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

// If loop() ever hangs - a bug in any of the servers/clients below, not just
// the serial link - the bridge should recover on its own rather than needing
// a power cycle at the mast. Mirrors the always-on AVR watchdog in the
// sibling ant-sw-2x6 firmware. 8 s is generous next to every actual operation
// in loop() (all non-blocking; the one exception, a LittleFS config/favourites
// write, takes milliseconds) while still recovering promptly from a real hang.
const uint32_t kWatchdogTimeoutS = 8;

gs232::AzimuthRange azRange;
Rotator rotator(Serial1, azRange);

// Constructed in setup(), once the configured ports have been read.
RotctldServer* rotctld = nullptr;
RawServer* rawServer = nullptr;

// Recovery commands, deliberately independent of WiFi and the panel - the
// point is that these still work when the network is the very thing that's
// broken. None of them ask for a password of their own: physical access to
// the USB port already implies trust, the same as it would for reflashing
// the device outright - the bar here is "works with no network at all", not
// "works for an untrusted holder of the cable".
//
//   help                         list these commands
//   passwd <user> <newpassword>  reset an account's password (>= 8 chars)
//   apssid <name>                fallback AP's SSID (and mDNS hostname)
//   appass <password>            fallback AP's password (empty = open)
//   apip <ip> [gateway]          fallback AP's address (gateway defaults to ip)
//   restart                      reboot now
//   s                            stop
//   ?                            status report
//   a bare number                goto that azimuth
//
// All the "ap*"/"passwd" commands only take effect once AP mode is actually
// (re)entered - restart to apply immediately, exactly like the same fields
// in the panel's own Settings.
void handleConsoleCommand(char* line) {
  char* save = nullptr;
  const char* cmd = strtok_r(line, " ", &save);
  if (cmd == nullptr) {
    return;
  }

  if (strcmp(cmd, "help") == 0) {
    Serial.println(
        "123                       goto azimuth\n"
        "s                         stop\n"
        "?                         status report\n"
        "passwd <user> <password> reset an account's password (>= 8 chars)\n"
        "apssid <name>             fallback AP's SSID (and mDNS hostname)\n"
        "appass <password>         fallback AP's password (empty = open)\n"
        "apip <ip> [gateway]       fallback AP's address (gateway defaults to ip)\n"
        "restart                   reboot now\n"
        "passwd/apssid/appass/apip take effect after restart");
    return;
  }
  if (strcmp(cmd, "s") == 0 || strcmp(cmd, "S") == 0) {
    rotator.stop(RotatorLink::Source::Web);
    return;
  }
  if (strcmp(cmd, "?") == 0) {
    Serial.printf("az=%.0f raw=%.0f fresh=%d lockout=%d net=%s addr=%s heap=%u\n", rotator.realAzimuth(),
                  rotator.rawAzimuth(), rotator.positionIsFresh(), rotator.inBootLockout(), net::modeName(),
                  net::address().c_str(), ESP.getFreeHeap());
    return;
  }
  if (strcmp(cmd, "restart") == 0) {
    Serial.println("restarting");
    delay(100);
    ESP.restart();
  }
  if (strcmp(cmd, "passwd") == 0) {
    const char* user = strtok_r(nullptr, " ", &save);
    const char* pass = strtok_r(nullptr, " ", &save);
    if (user == nullptr || pass == nullptr) {
      Serial.println("usage: passwd <user> <newpassword>");
      return;
    }
    if (!auth::upsertUser(user, pass)) {
      Serial.println("failed - unknown account, or password under 8 characters");
      return;
    }
    Serial.println("ok");
    return;
  }
  if (strcmp(cmd, "apssid") == 0) {
    const char* name = strtok_r(nullptr, " ", &save);
    if (name == nullptr) {
      Serial.println("usage: apssid <name>");
      return;
    }
    strncpy(config.hostname, name, Config::kStrLen - 1);
    config.hostname[Config::kStrLen - 1] = '\0';
    config.save();
    Serial.println("ok - takes effect after restart");
    return;
  }
  if (strcmp(cmd, "appass") == 0) {
    const char* pass = strtok_r(nullptr, " ", &save);
    const char* value = (pass != nullptr) ? pass : "";
    if (value[0] != '\0' && strlen(value) < 8) {
      Serial.println("password must be empty (open) or at least 8 characters");
      return;
    }
    strncpy(config.apPassword, value, Config::kStrLen - 1);
    config.apPassword[Config::kStrLen - 1] = '\0';
    config.save();
    Serial.println("ok - takes effect after restart");
    return;
  }
  if (strcmp(cmd, "apip") == 0) {
    const char* ip = strtok_r(nullptr, " ", &save);
    const char* gw = strtok_r(nullptr, " ", &save);
    IPAddress parsed;
    if (ip == nullptr || !parsed.fromString(ip)) {
      Serial.println("usage: apip <ip> [gateway]");
      return;
    }
    IPAddress parsedGw;
    if (!parsedGw.fromString((gw != nullptr) ? gw : ip)) {
      Serial.println("bad gateway address");
      return;
    }
    strncpy(config.apIp, ip, sizeof(config.apIp) - 1);
    config.apIp[sizeof(config.apIp) - 1] = '\0';
    strncpy(config.apGateway, (gw != nullptr) ? gw : ip, sizeof(config.apGateway) - 1);
    config.apGateway[sizeof(config.apGateway) - 1] = '\0';
    config.save();
    Serial.println("ok - takes effect after restart");
    return;
  }

  if (!rotator.gotoAzimuth(atof(cmd), RotatorLink::Source::Web)) {
    Serial.println("rejected");
  }
}

void serviceConsole() {
  // Sized for "passwd <39-char user> <39-char password>" plus the command
  // word - the longest line this console has to accept.
  static char buf[100];
  static size_t len = 0;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    // Local echo: this console has none otherwise (unlike a real terminal's
    // own line discipline), so typing here previously gave no feedback at
    // all that a keystroke had even reached the device - indistinguishable
    // from RX not working, which is exactly what a missing
    // ARDUINO_USB_CDC_ON_BOOT turned out to look like.
    Serial.write(c);
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
    handleConsoleCommand(buf);
  }
}

}  // namespace

void setup() {
  // First thing, before anything that could stall - this LED is the proof
  // the flash succeeded at all, even if a later begin() hangs.
  heartbeat::begin();

  Serial.begin(115200);
  Serial.println();
  Serial.println("rotator_wifi_bridge");

  config.load();  // defaults are usable, so a missing file is not an error
  azRange.rawMin = config.rawMin;
  azRange.rawMax = config.rawMax;

  Serial1.begin(config.serialBaud, SERIAL_8N1, kControllerRxPin, kControllerTxPin);
  rotator.begin();

  net::begin();

  rotctld = new RotctldServer(rotator, config.rotctldPort, config.rotctldMaxClients);
  rotctld->begin();

  rawServer = new RawServer(rotator, config.rawPort, config.rawMaxClients);
  rawServer->begin();

  webapi::begin(rotator, *rotctld, *rawServer);
  antswitch::begin();

  // Enabled last, once every begin() above (each fast and non-blocking) has
  // run - so nothing in normal startup is mistaken for a hang. The Arduino
  // core's own loop task feeds this automatically every iteration; nothing
  // else in this file needs to call esp_task_wdt_reset().
  esp_task_wdt_init(kWatchdogTimeoutS, true);  // panic=true: restarts on timeout
  enableLoopWDT();

  const char* firstSsid = "(no wifi, AP mode)";
  for (size_t i = 0; i < Config::kMaxWifiNetworks; i++) {
    if (config.wifiNetworks[i].ssid[0] != '\0') {
      firstSsid = config.wifiNetworks[i].ssid;
      break;
    }
  }
  Serial.printf("config: %s, az range %d..%d, %lu baud, rotctld %u, raw %u\n", firstSsid, azRange.rawMin,
                azRange.rawMax, static_cast<unsigned long>(config.serialBaud), config.rotctldPort, config.rawPort);
}

void loop() {
  rotator.poll();
  net::poll();
  rotctld->poll();
  rawServer->poll();
  webapi::poll();
  antswitch::poll();
  serviceConsole();
  heartbeat::poll();
}
