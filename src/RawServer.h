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
  static const size_t kMaxClients = 1;
  static const size_t kLineLen = 32;

  RawServer(Rotator& rotator, uint16_t port);

  void begin();
  void poll();

  uint16_t port() const { return port_; }
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
  WiFiServer server_;
  Session sessions_[kMaxClients];
};
