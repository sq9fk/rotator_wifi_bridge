// Transparent GS-232 socket on a configurable TCP port, for software that
// expects a serial rotator.
//
// Transparent at the *line* level, not the byte level. A byte pipe straight to
// the UART would race the position poller: GS-232 carries no transaction ids,
// so two overlapping "C" commands produce two indistinguishable "AZ=" replies
// and each reader has even odds of taking the other's. Reading whole commands
// and pushing them through the shared queue keeps every reply attributable,
// and from the client's side it still behaves like a cable.

#pragma once

#include <WiFi.h>

#include "Rotator.h"

class RawServer {
 public:
  // Ceiling of 2: two raw clients is safe - each session has its own line
  // buffer and pending-transaction id, so replies are routed per client with
  // no packet collision. It defaults to 1 because raw emulates one serial
  // cable, but two loggers sharing control is a policy choice, not a hazard.
  static const size_t kClientCeiling = 2;
  static const size_t kLineLen = 32;

  RawServer(Rotator& rotator, uint16_t port, uint8_t maxClients);

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
    uint32_t pendingId = 0;  // transaction this client is waiting on
  };

  static void replyTrampoline(uint32_t id, RotatorLink::Result result, const char* reply, void* ctx);
  void onReply(uint32_t id, RotatorLink::Result result, const char* reply);

  Rotator& rotator_;
  uint16_t port_;
  size_t maxClients_;
  WiFiServer server_;
  Session sessions_[kClientCeiling];
};
