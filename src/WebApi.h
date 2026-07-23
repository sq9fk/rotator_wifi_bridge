// REST API. The web panel (phase 5) will be served alongside it from LittleFS,
// and rotctld and the raw socket are separate servers - all of them reach the
// rotator through the same Rotator object, so no source can bypass the queue.
//
// Not authenticated yet: session handling lands with the panel in phase 5.

#pragma once

#include "Rotator.h"

namespace webapi {

void begin(Rotator& rotator);

}  // namespace webapi
