#include "Config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace {
const char* kPath = "/config.json";

void copyField(char* dest, size_t len, JsonVariantConst value, const char* fallback) {
  const char* text = value.is<const char*>() ? value.as<const char*>() : fallback;
  if (text == nullptr) {
    text = "";
  }
  strncpy(dest, text, len - 1);
  dest[len - 1] = '\0';
}
}  // namespace

Config config;

bool Config::load() {
  if (!LittleFS.begin(true)) {
    return false;
  }

  File file = LittleFS.open(kPath, "r");
  if (!file) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    return false;
  }

  copyField(wifiSsid, kStrLen, doc["wifiSsid"], wifiSsid);
  copyField(wifiPassword, kStrLen, doc["wifiPassword"], wifiPassword);
  copyField(hostname, kStrLen, doc["hostname"], hostname);
  copyField(webUser, kStrLen, doc["webUser"], webUser);
  copyField(webPasswordHash, sizeof(webPasswordHash), doc["webPasswordHash"], webPasswordHash);
  copyField(webPasswordSalt, sizeof(webPasswordSalt), doc["webPasswordSalt"], webPasswordSalt);

  rotctldPort = doc["rotctldPort"] | rotctldPort;
  rawPort = doc["rawPort"] | rawPort;
  serialBaud = doc["serialBaud"] | serialBaud;
  rawMin = doc["rawMin"] | rawMin;
  rawMax = doc["rawMax"] | rawMax;

  return true;
}

bool Config::save() const {
  JsonDocument doc;

  doc["wifiSsid"] = wifiSsid;
  doc["wifiPassword"] = wifiPassword;
  doc["hostname"] = hostname;
  doc["webUser"] = webUser;
  doc["webPasswordHash"] = webPasswordHash;
  doc["webPasswordSalt"] = webPasswordSalt;
  doc["rotctldPort"] = rotctldPort;
  doc["rawPort"] = rawPort;
  doc["serialBaud"] = serialBaud;
  doc["rawMin"] = rawMin;
  doc["rawMax"] = rawMax;

  // Write to a temporary file first: a power cut halfway through a direct
  // overwrite would leave an unparseable config and no WiFi credentials.
  File file = LittleFS.open("/config.tmp", "w");
  if (!file) {
    return false;
  }
  const bool written = serializeJson(doc, file) > 0;
  file.close();

  if (!written) {
    LittleFS.remove("/config.tmp");
    return false;
  }

  LittleFS.remove(kPath);
  return LittleFS.rename("/config.tmp", kPath);
}
