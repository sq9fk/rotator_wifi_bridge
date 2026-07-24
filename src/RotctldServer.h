// Hamlib net rotator server (rotctl -m 2), on a configurable TCP port.
//
// Commands reach the controller through the same Rotator object as every other
// source, so rotctld cannot bypass the queue - and the position it reports is
// the shared cache, which keeps being polled no matter who is in control.

#pragma once

#include <WiFi.h>

#include "Rotator.h"

class RotctldServer {
 public:
  // Compile-time ceiling: the session array is this big, sized for the ESP32's
  // BSD-socket pool shared with the raw server. The configured limit is clamped
  // to it. rotctld is the multi-client face (loggers), so it gets the larger.
  static const size_t kClientCeiling = 4;
  static const size_t kLineLen = 64;

  RotctldServer(Rotator& rotator, uint16_t port, uint8_t maxClients);

  void begin();
  void poll();

  uint16_t port() const { return port_; }
  size_t maxClients() const { return maxClients_; }
  static size_t clientCeiling() { return kClientCeiling; }
  size_t clientCount() const;
  String clientAddresses() const;

 private:
  struct Session {
    WiFiClient client;
    char line[kLineLen];
    size_t len = 0;
  };

  void handleLine(Session& session, const char* line);
  void sendDumpState(Session& session);

  Rotator& rotator_;
  uint16_t port_;
  size_t maxClients_;
  WiFiServer server_;
  Session sessions_[kClientCeiling];
};
