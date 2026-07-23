// Named azimuth presets, stored in LittleFS. Ten of them, as in the reference
// interface - enough for the beams that matter, few enough to fit on a phone
// screen without scrolling.

#pragma once

#include <Arduino.h>

namespace favorites {

static const size_t kMax = 10;
static const size_t kNameLen = 20;

struct Entry {
  char name[kNameLen] = "";
  float azimuth = 0.0f;
};

void begin();
size_t count();
const Entry& at(size_t index);

// Replaces the whole list; the panel edits it as a set rather than one at a
// time, which avoids index races between the editor and the buttons.
bool replaceAll(const Entry* entries, size_t n);

}  // namespace favorites
