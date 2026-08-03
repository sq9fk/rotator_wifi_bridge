#include "Favorites.h"

#include <ArduinoJson.h>
#include <Preferences.h>

namespace favorites {
namespace {

// NVS, not LittleFS - see Config.cpp's namespace comment for why: LittleFS
// also holds data/www/*, and uploadfs/the panel's own "Panel" update replace
// that filesystem's entire image, which would otherwise wipe the favourites
// list too every time the panel's HTML/CSS/JS gets updated.
const char* kNamespace = "rotorfav";
const char* kKey = "json";

Entry entries[kMax];
size_t stored = 0;

bool save() {
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (size_t i = 0; i < stored; i++) {
    JsonObject item = array.add<JsonObject>();
    item["name"] = entries[i].name;
    item["az"] = entries[i].azimuth;
  }

  String out;
  if (serializeJson(doc, out) == 0) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) {
    return false;
  }
  const size_t written = prefs.putString(kKey, out);
  prefs.end();
  return written > 0;
}

}  // namespace

void begin() {
  stored = 0;

  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) {  // read-only: never creates the namespace on a first boot
    return;
  }
  const String raw = prefs.getString(kKey, "");
  prefs.end();
  if (raw.length() == 0) {
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, raw);
  if (error) {
    return;  // an unreadable list is an empty list, not a boot failure
  }

  for (JsonObjectConst item : doc.as<JsonArrayConst>()) {
    if (stored >= kMax) {
      break;
    }
    const char* name = item["name"] | "";
    strncpy(entries[stored].name, name, kNameLen - 1);
    entries[stored].name[kNameLen - 1] = '\0';
    entries[stored].azimuth = item["az"] | 0.0f;
    stored++;
  }
}

size_t count() {
  return stored;
}

const Entry& at(size_t index) {
  // Every current caller already bounds index by count(), but this is the
  // only thing standing between a future caller's off-by-one and reading
  // past the array - worth the one comparison.
  static const Entry kEmpty{};
  return (index < stored) ? entries[index] : kEmpty;
}

bool replaceAll(const Entry* incoming, size_t n) {
  if (n > kMax) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    entries[i] = incoming[i];
  }
  stored = n;
  return save();
}

}  // namespace favorites
