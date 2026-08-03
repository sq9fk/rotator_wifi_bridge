// Named azimuth presets, stored in NVS (not LittleFS - see Config.h/.cpp for
// why: LittleFS also holds data/www/*, wholesale-replaced by uploadfs/the
// panel's own "Panel" update). Nine of them - enough for the beams that
// matter, few enough that the whole controller tab (dial, session card and
// this list) still fits a normal window without scrolling (see style.css's
// @media (max-height: ...) rules).

#pragma once

#include <Arduino.h>

namespace favorites {

static const size_t kMax = 9;
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
