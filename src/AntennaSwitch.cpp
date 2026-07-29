#include "AntennaSwitch.h"

#include <AsyncTCP.h>

#include "Config.h"

namespace antswitch {
namespace {

AsyncClient client;

enum class State : uint8_t { Idle, Connecting, WaitingReply };
State state = State::Idle;
uint32_t stateSince = 0;

// Bumped on every new request and captured by that request's callbacks, so a
// callback arriving after we have already given up on it (timeout, or the
// feature got disabled mid-flight) is recognised as stale and ignored instead
// of mutating state that belongs to a request that no longer exists.
uint32_t requestSeq = 0;
bool requestIsCommand = false;
char requestPath[16] = "";

char respBuf[200];
size_t respLen = 0;

bool queuedCommand = false;
char queuedPath[16] = "";

int antennaVal[2] = {-1, -1};
bool linkConnected = false;
uint32_t lastPollAt = 0;

const uint32_t kPollIntervalMs = 2000;
const uint32_t kTimeoutMs = 2000;

// The callbacks below run on the AsyncTCP task, not the loop() task that
// calls poll()/setAntenna() - the same cross-task pattern this project
// already accepts for the WebSocket's onSocketEvent (see WebApi.cpp,
// jogActive/lastJogAt), so this follows it rather than introducing a new,
// inconsistent locking scheme for one module.

void finish(bool ok, bool isCommand) {
  linkConnected = ok;
  if (ok && !isCommand) {
    const char* marker = strstr(respBuf, "A=");
    int a0 = -1, a1 = -1;
    if (marker && sscanf(marker + 2, "%d,%d", &a0, &a1) == 2) {
      antennaVal[0] = a0;
      antennaVal[1] = a1;
    }
  }
  state = State::Idle;
  if (ok && isCommand) {
    // Refresh status immediately rather than waiting up to kPollIntervalMs,
    // so the panel reflects the change without a visible lag.
    lastPollAt = 0;
  }
}

void startRequest(const char* path, bool isCommand) {
  requestSeq++;
  const uint32_t seq = requestSeq;
  requestIsCommand = isCommand;
  strncpy(requestPath, path, sizeof(requestPath) - 1);
  requestPath[sizeof(requestPath) - 1] = '\0';
  respLen = 0;
  respBuf[0] = '\0';
  state = State::Connecting;
  stateSince = millis();

  client.onConnect(
      [seq](void*, AsyncClient* c) {
        if (seq != requestSeq) return;
        char req[128];
        snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", requestPath,
                 config.antHost);
        c->write(req);
        state = State::WaitingReply;
        stateSince = millis();
      },
      nullptr);

  client.onData(
      [seq](void*, AsyncClient*, void* data, size_t len) {
        if (seq != requestSeq) return;
        const size_t room = sizeof(respBuf) - 1 - respLen;
        const size_t n = (len < room) ? len : room;
        memcpy(respBuf + respLen, data, n);
        respLen += n;
        respBuf[respLen] = '\0';
      },
      nullptr);

  client.onDisconnect(
      [seq](void*, AsyncClient*) {
        if (seq != requestSeq) return;
        finish(true, requestIsCommand);
      },
      nullptr);

  client.onError(
      [seq](void*, AsyncClient*, int8_t) {
        if (seq != requestSeq) return;
        finish(false, requestIsCommand);
      },
      nullptr);

  client.onTimeout(
      [seq](void*, AsyncClient*, uint32_t) {
        if (seq != requestSeq) return;
        client.close();
        finish(false, requestIsCommand);
      },
      nullptr);

  if (!client.connect(config.antHost, 80)) {
    state = State::Idle;
    linkConnected = false;
  }
}

}  // namespace

void begin() {
  // Nothing to set up up front: callbacks are (re)installed per request in
  // startRequest(), and no connection is opened until poll() decides to.
}

void poll() {
  if (!enabled()) {
    if (state != State::Idle) {
      requestSeq++;  // invalidate whatever request is in flight before closing it
      client.close();
      state = State::Idle;
    }
    linkConnected = false;
    return;
  }

  if (state != State::Idle) {
    if (millis() - stateSince > kTimeoutMs) {
      requestSeq++;  // the eventual late callback must see itself as stale
      client.close();
      state = State::Idle;
      linkConnected = false;
    }
    return;
  }

  if (queuedCommand) {
    queuedCommand = false;
    startRequest(queuedPath, true);
    return;
  }

  if (millis() - lastPollAt >= kPollIntervalMs) {
    lastPollAt = millis();
    startRequest("/?J", false);
  }
}

bool enabled() {
  return config.antEnabled && config.antHost[0] != '\0';
}

bool connected() {
  return enabled() && linkConnected;
}

int antenna(uint8_t bank) {
  return (bank < 2) ? antennaVal[bank] : -1;
}

bool setAntenna(uint8_t bank, uint8_t ant) {
  if (!enabled() || bank > 1 || ant > 6) {
    return false;
  }
  snprintf(queuedPath, sizeof(queuedPath), "/?S%u%02u", bank + 1, ant);
  queuedCommand = true;
  return true;
}

}  // namespace antswitch
