#include "DebugLog.h"

#include <string.h>

#include "Config.h"

namespace debuglog {
namespace {

// One broadcast tick's worth (see WebApi.cpp's kBroadcastIntervalMs, 250 ms) -
// this is a short-lived relay to the next WS push, not a scrollback buffer;
// the panel itself keeps the scrollback once lines arrive.
const size_t kCap = 24;

struct Entry {
  Proto proto;
  uint8_t session;
  char label[24];
  bool tx;
  // 104: comfortably fits ant-sw-2x6's "K=" line (6 antenna names + its own
  // site name, each up to 11 chars) with room to spare - 80 was tight enough
  // that the antenna stream's "A="/"K=" marker (see AntennaSwitch.cpp's
  // finish()) could still run past the end on a long site/antenna name.
  char text[104];
};
Entry buf[kCap];
size_t count = 0;
bool overflowed = false;

bool protoEnabled(Proto proto) {
  switch (proto) {
    case Proto::Rotctld: return config.debugRotctld;
    case Proto::Raw: return config.debugRaw;
    case Proto::Antenna: return config.debugAntenna;
    case Proto::Controller: return config.debugController;
    default: return false;
  }
}

const char* protoName(Proto proto) {
  switch (proto) {
    case Proto::Rotctld: return "rotctld";
    case Proto::Raw: return "raw";
    case Proto::Antenna: return "antenna";
    default: return "controller";
  }
}

}  // namespace

void log(Proto proto, uint8_t session, const char* label, bool tx, const char* text) {
  if (!config.debugEnabled || !protoEnabled(proto) || text == nullptr) {
    return;
  }
  if (count >= kCap) {
    overflowed = true;
    return;
  }
  Entry& e = buf[count++];
  e.proto = proto;
  e.session = session;
  strncpy(e.label, label != nullptr ? label : "", sizeof(e.label) - 1);
  e.label[sizeof(e.label) - 1] = '\0';
  e.tx = tx;
  strncpy(e.text, text, sizeof(e.text) - 1);
  e.text[sizeof(e.text) - 1] = '\0';
}

bool drain(JsonArray& arr) {
  for (size_t i = 0; i < count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["proto"] = protoName(buf[i].proto);
    o["session"] = buf[i].session;
    o["label"] = buf[i].label;
    o["dir"] = buf[i].tx ? "tx" : "rx";
    o["text"] = buf[i].text;
  }
  const bool didOverflow = overflowed;
  count = 0;
  overflowed = false;
  return didOverflow;
}

}  // namespace debuglog
