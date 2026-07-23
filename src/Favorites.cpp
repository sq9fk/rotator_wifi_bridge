#include "Favorites.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace favorites {
namespace {

const char* kPath = "/favorites.json";

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

  File file = LittleFS.open("/favorites.tmp", "w");
  if (!file) {
    return false;
  }
  const bool written = serializeJson(doc, file) > 0;
  file.close();
  if (!written) {
    LittleFS.remove("/favorites.tmp");
    return false;
  }
  LittleFS.remove(kPath);
  return LittleFS.rename("/favorites.tmp", kPath);
}

}  // namespace

void begin() {
  stored = 0;

  File file = LittleFS.open(kPath, "r");
  if (!file) {
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
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
  return entries[index];
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
