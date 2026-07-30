// A ring buffer of recent protocol traffic - rotctld, raw, the HTTP link to
// ant-sw-2x6, and the serial link to the K3NG controller itself - drained
// into the panel's existing WebSocket status stream once per broadcast tick.
// A live protocol monitor for debugging a logger, contest program, or the
// controller link, without a separate packet sniffer or a serial cable.
//
// Gated per-stream (config.debugEnabled plus one flag per Proto, see
// Config.h), not just filtered for display: the position poller alone talks
// to the controller roughly every 300 ms, which would flood the log whether
// or not anyone has the Monitor tab open, if capture itself were not opt-in
// per stream. An operator who captures nothing pays nothing - no capture, no
// extra broadcast payload, no exposure of client traffic.

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace debuglog {

enum class Proto : uint8_t { Rotctld, Raw, Antenna, Controller };

// `session` identifies which client slot within that stream this line
// belongs to (0-based, stable for the life of that connection; always 0 for
// Antenna/Controller, which have only one link each) - lets two simultaneous
// clients on the same server stay visually distinct even if one reconnects
// mid-session. `label` is the client address for rotctld/raw, the switch's
// host for Antenna, or a fixed name for Controller - whatever identifies the
// other end of that particular stream.
void log(Proto proto, uint8_t session, const char* label, bool tx, const char* text);

// Appends every entry buffered since the last drain to `arr` and clears the
// buffer. Returns true if the buffer filled up between two broadcast ticks
// (traffic burst) and some lines were dropped rather than queued forever.
bool drain(JsonArray& arr);

}  // namespace debuglog
