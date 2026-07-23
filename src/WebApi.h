// REST API. The web panel (phase 5) will be served alongside it from LittleFS,
// and rotctld and the raw socket are separate servers - all of them reach the
// rotator through the same Rotator object, so no source can bypass the queue.
//
// Authenticated with a single session; see Auth.h for why takeover exists.

#pragma once

#include "RawServer.h"
#include "Rotator.h"
#include "RotctldServer.h"

namespace webapi {

void begin(Rotator& rotator, RotctldServer& rotctld, RawServer& raw);

// Must be called from loop(): expires idle sessions, runs the jog dead-man
// timer and broadcasts status to the panel.
void poll();

}  // namespace webapi
