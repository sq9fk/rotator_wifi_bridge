#include "AntennaSwitch.h"

#include <AsyncTCP.h>

#include "Config.h"
#include "DebugLog.h"

namespace antswitch {
namespace {

AsyncClient client;

enum class State : uint8_t { Idle, Connecting, WaitingReply };
State state = State::Idle;
uint32_t stateSince = 0;

enum class RequestKind : uint8_t { Status, Command, Names };

// Bumped on every new request and captured by that request's callbacks, so a
// callback arriving after we have already given up on it (timeout, or the
// feature got disabled mid-flight) is recognised as stale and ignored instead
// of mutating state that belongs to a request that no longer exists.
uint32_t requestSeq = 0;
RequestKind requestKind = RequestKind::Status;
char requestPath[16] = "";

char respBuf[200];
size_t respLen = 0;

// One slot per TRX bank, not a single shared one: setAntenna() can be called
// for both banks in quick succession (e.g. reconfiguring TRX1 and TRX2 at
// once), and a single shared slot would let the second call silently
// overwrite the first before poll() ever got to send it - the caller sees
// setAntenna() return true for both, but only the last one would ever reach
// the switch.
bool queuedCommand[2] = {false, false};
char queuedPath[2][16] = {"", ""};

int antennaVal[2] = {-1, -1};
bool linkConnected = false;
uint32_t lastOkAt = 0;  // millis() of the last successful exchange, any kind
uint32_t lastPollAt = 0;

// Past this, a link that is still nominally "connected" (no single request
// has failed yet) is shown as stale rather than solid green - the same
// ok/stale/dead idea as the controller link's own status dot. 2.5x the poll
// interval tolerates one slow round trip without flapping the dot.
const uint32_t kFreshMs = 5000;

// Names come from the device itself (GET /?K), not a second copy typed here -
// see AntennaSwitch.h. They change rarely (only when the operator edits them
// on the switch's own panel), so this is polled far less often than status.
const size_t kNameLen = 12;  // matches ant-sw-2x6's own ANT_MAXLEN (11 chars + nul)
char names[6][kNameLen] = {"1", "2", "3", "4", "5", "6"};  // placeholder until first fetch
char deviceNameBuf[kNameLen] = "";  // empty until the first successful fetch
bool namesFetched = false;
uint32_t lastNamesAt = 0;

const uint32_t kPollIntervalMs = 2000;
const uint32_t kNamesIntervalMs = 30000;
const uint32_t kTimeoutMs = 2000;

// The callbacks below run on the AsyncTCP task, not the loop() task that
// calls poll()/setAntenna() - the same cross-task pattern this project
// already accepts for the WebSocket's onSocketEvent (see WebApi.cpp,
// jogActive/lastJogAt), so this follows it rather than introducing a new,
// inconsistent locking scheme for one module.

void parseNames() {
  const char* marker = strstr(respBuf, "K=");
  if (!marker) {
    return;
  }
  const char* p = marker + 2;
  bool more = true;
  for (size_t i = 0; i < 6 && more; i++) {
    const char* comma = strchr(p, ',');
    const size_t fieldLen = comma ? static_cast<size_t>(comma - p) : strlen(p);
    const size_t n = (fieldLen < kNameLen - 1) ? fieldLen : kNameLen - 1;
    memcpy(names[i], p, n);
    names[i][n] = '\0';
    if (comma) {
      p = comma + 1;
    } else {
      more = false;
    }
  }
  // A 7th field - the switch's own site name - is a newer addition to /?K.
  // Tolerate older ant-sw-2x6 firmware that only sends the 6 antenna names by
  // leaving the device name at whatever it was (empty on first fetch) rather
  // than requiring the extra field.
  if (more) {
    const char* comma = strchr(p, ',');
    const size_t fieldLen = comma ? static_cast<size_t>(comma - p) : strlen(p);
    const size_t n = (fieldLen < kNameLen - 1) ? fieldLen : kNameLen - 1;
    memcpy(deviceNameBuf, p, n);
    deviceNameBuf[n] = '\0';
  }
  namesFetched = true;
}

void finish(bool ok, RequestKind kind) {
  linkConnected = ok;
  if (ok) {
    lastOkAt = millis();
  }
  if (ok && kind == RequestKind::Status) {
    const char* marker = strstr(respBuf, "A=");
    int a0 = -1, a1 = -1;
    if (marker && sscanf(marker + 2, "%d,%d", &a0, &a1) == 2) {
      antennaVal[0] = a0;
      antennaVal[1] = a1;
    }
  } else if (ok && kind == RequestKind::Names) {
    parseNames();
  }
  if (ok && respLen > 0) {
    debuglog::log(debuglog::Proto::Antenna, 0, config.antHost, false, respBuf);
  }
  state = State::Idle;
  if (ok && kind == RequestKind::Command) {
    // Refresh status immediately rather than waiting up to kPollIntervalMs,
    // so the panel reflects the change without a visible lag.
    lastPollAt = 0;
  }
}

void startRequest(const char* path, RequestKind kind) {
  requestSeq++;
  const uint32_t seq = requestSeq;
  requestKind = kind;
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
        debuglog::log(debuglog::Proto::Antenna, 0, config.antHost, true, requestPath);
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
        finish(true, requestKind);
      },
      nullptr);

  client.onError(
      [seq](void*, AsyncClient*, int8_t) {
        if (seq != requestSeq) return;
        finish(false, requestKind);
      },
      nullptr);

  client.onTimeout(
      [seq](void*, AsyncClient*, uint32_t) {
        if (seq != requestSeq) return;
        client.close();
        finish(false, requestKind);
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

  for (uint8_t bank = 0; bank < 2; bank++) {
    if (queuedCommand[bank]) {
      queuedCommand[bank] = false;
      startRequest(queuedPath[bank], RequestKind::Command);
      return;
    }
  }

  // Names rarely change and are cheap to be a little stale, but a first fetch
  // right after enabling (rather than waiting up to kNamesIntervalMs) means
  // the legend shows real names promptly instead of placeholders. Still
  // throttled by kPollIntervalMs even in that first-fetch case - client.
  // connect() can fail synchronously (e.g. an unreachable host), which never
  // sets namesFetched, so without a floor here this retries on every single
  // loop() iteration with no backoff at all, flooding the log and starving
  // everything else on the cooperative loop of CPU time.
  const uint32_t namesRetryMs = namesFetched ? kNamesIntervalMs : kPollIntervalMs;
  if (millis() - lastNamesAt >= namesRetryMs) {
    lastNamesAt = millis();
    startRequest("/?K", RequestKind::Names);
    return;
  }

  if (millis() - lastPollAt >= kPollIntervalMs) {
    lastPollAt = millis();
    startRequest("/?J", RequestKind::Status);
  }
}

bool enabled() {
  return config.antEnabled && config.antHost[0] != '\0';
}

bool connected() {
  return enabled() && linkConnected;
}

bool fresh() {
  return connected() && (millis() - lastOkAt < kFreshMs);
}

int antenna(uint8_t bank) {
  return (bank < 2) ? antennaVal[bank] : -1;
}

const char* antennaName(uint8_t index) {
  return (index < 6) ? names[index] : "";
}

const char* deviceName() {
  return deviceNameBuf;
}

bool setAntenna(uint8_t bank, uint8_t ant) {
  if (!enabled() || bank > 1 || ant > 6) {
    return false;
  }
  snprintf(queuedPath[bank], sizeof(queuedPath[bank]), "/?S%u%02u", bank + 1, ant);
  queuedCommand[bank] = true;
  return true;
}

}  // namespace antswitch
