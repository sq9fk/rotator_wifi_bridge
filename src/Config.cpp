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

  // Cleared before reading, not just overwritten - same reasoning as users[]
  // below: a network deleted before the last save() must not reappear on
  // reboot just because a stale slot still holds its old ssid/password.
  for (size_t i = 0; i < kMaxWifiNetworks; i++) {
    wifiNetworks[i] = WifiNetwork{};
  }
  JsonArrayConst networksIn = doc["wifiNetworks"];
  size_t wifiIndex = 0;
  for (JsonObjectConst n : networksIn) {
    if (wifiIndex >= kMaxWifiNetworks) {
      break;
    }
    copyField(wifiNetworks[wifiIndex].ssid, kStrLen, n["ssid"], "");
    copyField(wifiNetworks[wifiIndex].password, kStrLen, n["password"], "");
    wifiIndex++;
  }

  copyField(hostname, kStrLen, doc["hostname"], hostname);
  copyField(apPassword, kStrLen, doc["apPassword"], apPassword);
  copyField(siteName, kStrLen, doc["siteName"], siteName);

  // Every slot is cleared before reading the file, not just overwritten: a
  // user deleted before the last save() must not reappear on reboot just
  // because Config's own default member initializers (see Config.h) still
  // have a non-empty name at that index. A missing config.json never reaches
  // this point at all (see the early return above), so first boot keeps the
  // seeded sq9fk/sq9um accounts untouched.
  for (size_t i = 0; i < kMaxUsers; i++) {
    users[i] = User{};
  }
  JsonArrayConst usersIn = doc["users"];
  size_t i = 0;
  for (JsonObjectConst u : usersIn) {
    if (i >= kMaxUsers) {
      break;
    }
    copyField(users[i].name, kStrLen, u["name"], "");
    copyField(users[i].passwordHash, sizeof(users[i].passwordHash), u["passwordHash"], "");
    copyField(users[i].passwordSalt, sizeof(users[i].passwordSalt), u["passwordSalt"], "");
    i++;
  }

  rotctldPort = doc["rotctldPort"] | rotctldPort;
  rawPort = doc["rawPort"] | rawPort;
  rotctldMaxClients = doc["rotctldMaxClients"] | rotctldMaxClients;
  rawMaxClients = doc["rawMaxClients"] | rawMaxClients;
  serialBaud = doc["serialBaud"] | serialBaud;
  rawMin = doc["rawMin"] | rawMin;
  rawMax = doc["rawMax"] | rawMax;
  overlapFrom = doc["overlapFrom"] | overlapFrom;
  overlapTo = doc["overlapTo"] | overlapTo;

  antEnabled = doc["antEnabled"] | antEnabled;
  copyField(antHost, kStrLen, doc["antHost"], antHost);
  rotorAnt = doc["rotorAnt"] | rotorAnt;

  debugEnabled = doc["debugEnabled"] | debugEnabled;
  debugRotctld = doc["debugRotctld"] | debugRotctld;
  debugRaw = doc["debugRaw"] | debugRaw;
  debugAntenna = doc["debugAntenna"] | debugAntenna;
  debugController = doc["debugController"] | debugController;

  return true;
}

bool Config::save() const {
  JsonDocument doc;

  JsonArray networksOut = doc["wifiNetworks"].to<JsonArray>();
  for (size_t i = 0; i < kMaxWifiNetworks; i++) {
    if (wifiNetworks[i].ssid[0] == '\0') {
      continue;  // unused slot - omit rather than round-trip an empty entry
    }
    JsonObject n = networksOut.add<JsonObject>();
    n["ssid"] = wifiNetworks[i].ssid;
    n["password"] = wifiNetworks[i].password;
  }

  doc["hostname"] = hostname;
  doc["apPassword"] = apPassword;
  doc["siteName"] = siteName;

  JsonArray usersOut = doc["users"].to<JsonArray>();
  for (size_t i = 0; i < kMaxUsers; i++) {
    if (users[i].name[0] == '\0') {
      continue;  // unused slot - omit rather than round-trip an empty entry
    }
    JsonObject u = usersOut.add<JsonObject>();
    u["name"] = users[i].name;
    u["passwordHash"] = users[i].passwordHash;
    u["passwordSalt"] = users[i].passwordSalt;
  }

  doc["rotctldPort"] = rotctldPort;
  doc["rawPort"] = rawPort;
  doc["rotctldMaxClients"] = rotctldMaxClients;
  doc["rawMaxClients"] = rawMaxClients;
  doc["serialBaud"] = serialBaud;
  doc["rawMin"] = rawMin;
  doc["rawMax"] = rawMax;
  doc["overlapFrom"] = overlapFrom;
  doc["overlapTo"] = overlapTo;
  doc["antEnabled"] = antEnabled;
  doc["antHost"] = antHost;
  doc["rotorAnt"] = rotorAnt;
  doc["debugEnabled"] = debugEnabled;
  doc["debugRotctld"] = debugRotctld;
  doc["debugRaw"] = debugRaw;
  doc["debugAntenna"] = debugAntenna;
  doc["debugController"] = debugController;

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
