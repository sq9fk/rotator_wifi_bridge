// GS-232B protocol layer: encoding, parsing and azimuth coordinate mapping.
//
// Deliberately free of any Arduino dependency so it can be unit tested on the
// host (env:native). Nothing here talks to a UART or knows about time; it is
// all pure functions over buffers.

#pragma once

#include <stddef.h>

namespace gs232 {

// The controller is asymmetric: the C command reports a *real* azimuth of
// 0..359, while the M command takes a *raw* azimuth that runs from the
// rotator's starting point to starting point + rotation capability. On the
// target hardware that is 180..630 - the 360..450 stretch being the overlap
// zone reachable only by continuing clockwise past north.
//
// Anything crossing this boundary must go through rawToReal/chooseRawTarget.
struct AzimuthRange {
  int rawMin = 180;   // AZIMUTH_STARTING_POINT
  int rawMax = 630;   // starting point + AZIMUTH_ROTATION_CAPABILITY
};

// What the controller will send back after a given command. Several GS-232
// commands answer nothing at all on success, which the transaction layer has
// to know or every rotation would sit waiting for a reply that never comes.
enum class ReplyKind {
  None,        // never answers (H - the help text is compiled out)
  Immediate,   // always answers (C, D, X, and anything unrecognised -> "?>")
  ErrorOnly,   // silent on success, "?>" on a rejected value (M, L, R, A, S)
};

float rawToReal(float raw);

// Picks the raw target for a requested real azimuth, preferring the shorter
// travel from the current position. A real azimuth inside the overlap zone is
// reachable two ways (e.g. 10 deg is raw 10 or raw 370) and picking blindly is
// how a rotator ends up going 350 degrees the wrong way round.
// Returns -1 if the azimuth is unreachable within the range.
int chooseRawTarget(float desiredReal, float currentRaw, const AzimuthRange& range);

// Writes "M###" (always 4 characters, no terminator) for a raw target.
// Returns the number of characters written, or 0 if raw is out of 0..999.
size_t buildGoto(char* buf, size_t bufLen, int raw);

// Parses an "AZ=123" reply, tolerating the "AZ=123EL=000" form that C2 returns.
// Returns false if the buffer is not an azimuth reply at all.
bool parseAzimuthReply(const char* reply, float& realAzOut);

// Parses a "RAW=370" reply from the fork's I command. Preferred over
// parseAzimuthReply for position tracking: the raw value says which turn the
// rotator is on, which a real azimuth of 0..359 cannot express.
bool parseRawReply(const char* reply, float& rawAzOut);

// True if the reply is the controller's generic rejection.
bool isErrorReply(const char* reply);

// Classifies an arbitrary command line, used to drive the transaction timeout
// for commands forwarded verbatim from a raw client.
ReplyKind classify(const char* command);

}  // namespace gs232
