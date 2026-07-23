// Hamlib "net rotctl" protocol (rotctl -m 2), parsing only.
//
// Pure and Arduino-free so it can be unit tested on the host. Response
// formatting lives in RotctldServer, which has the rotator state to fill in.
//
// Verified against Hamlib's own client, rigs/dummy/netrotctl.c:
//   - a reply line beginning "RPRT " is a return code; 0 is success
//   - anything else is data, read line by line
//   - \dump_state must answer protocol version 1, then a model line, then
//     "setting=value" lines, then "done". Answering version 0 makes the client
//     fall back to its built-in ±180° azimuth range, which would put the whole
//     overlap zone out of reach.

#pragma once

namespace rotctl {

enum class Cmd {
  Empty,
  GetPos,      // p
  SetPos,      // P <az> <el>
  Stop,        // S
  Park,        // K
  Reset,       // R <n>
  Move,        // M <direction> <speed>
  GetInfo,     // _
  DumpState,   // \dump_state
  Quit,        // q / Q
  Unsupported,
};

// Hamlib rotator move directions.
enum : int {
  kMoveUp = 2,
  kMoveDown = 4,
  kMoveLeft = 8,    // CCW
  kMoveRight = 16,  // CW
};

struct Command {
  Cmd cmd = Cmd::Unsupported;
  float az = 0.0f;
  float el = 0.0f;
  int direction = 0;
  int speed = 0;
  bool extended = false;  // client prefixed the command with '+'
};

Command parse(const char* line);

// Wraps an azimuth from any convention a client might use (negative, or past
// 360) into 0..359.9. Rejecting instead would fail a request whose meaning is
// unambiguous.
float normalizeAzimuth(float az);

}  // namespace rotctl
