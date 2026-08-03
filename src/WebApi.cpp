#include "WebApi.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

#include "AntennaSwitch.h"
#include "Auth.h"
#include "Config.h"
#include "DebugLog.h"
#include "Favorites.h"
#include "Net.h"

namespace webapi {
namespace {

AsyncWebServer server(80);
AsyncWebSocket socket("/ws");

Rotator* rotator = nullptr;
RotctldServer* rotctld = nullptr;
RawServer* raw = nullptr;

// Jog is held, not latched: the panel repeats the command while the key or
// button is down, and silence means stop. GS-232 L and R rotate until
// something stops them, so a dropped WebSocket during a held jog would
// otherwise drive the rotator into its limit.
const uint32_t kJogKeepaliveMs = 500;
bool jogActive = false;
uint32_t lastJogAt = 0;

// Which account last successfully caused a motion command through this API,
// so "last motion" can name an operator instead of just "web" - the same
// "why is it turning" reasoning as attributing raw/rotctld clients (see
// Rotator::submitRaw). Stays stale if the temporary USB console
// (main.cpp's serviceConsole(), which also submits as Source::Web) causes a
// motion instead - accepted: that console is a dev aid, not an operator path.
char lastWebActor[Config::kStrLen] = "";

void noteWebActor(const char* user) {
  if (user == nullptr) {
    return;
  }
  strncpy(lastWebActor, user, sizeof(lastWebActor) - 1);
  lastWebActor[sizeof(lastWebActor) - 1] = '\0';
}

// A jog message carries no cookie of its own (unlike a plain POST), so the
// account behind an open WebSocket is recorded once at connect time and
// looked up again here by client id.
struct WsUser {
  uint32_t clientId = 0;
  char name[Config::kStrLen] = "";
  bool used = false;
};
WsUser wsUsers[Config::kMaxUsers * 2];  // generous: more than one tab per account is normal

void setWsUser(uint32_t clientId, const char* name) {
  if (name == nullptr) {
    return;
  }
  for (WsUser& w : wsUsers) {
    if (w.used && w.clientId == clientId) {
      strncpy(w.name, name, sizeof(w.name) - 1);
      w.name[sizeof(w.name) - 1] = '\0';
      return;
    }
  }
  for (WsUser& w : wsUsers) {
    if (!w.used) {
      w.used = true;
      w.clientId = clientId;
      strncpy(w.name, name, sizeof(w.name) - 1);
      w.name[sizeof(w.name) - 1] = '\0';
      return;
    }
  }
  // Every slot taken (unusually many tabs open at once) - the jog just goes
  // unattributed rather than displacing another client's record.
}

const char* wsUserName(uint32_t clientId) {
  for (const WsUser& w : wsUsers) {
    if (w.used && w.clientId == clientId) {
      return w.name;
    }
  }
  return nullptr;
}

void clearWsUser(uint32_t clientId) {
  for (WsUser& w : wsUsers) {
    if (w.used && w.clientId == clientId) {
      w.used = false;
      return;
    }
  }
}

const uint32_t kBroadcastIntervalMs = 250;
uint32_t lastBroadcast = 0;

const char* sourceName(RotatorLink::Source source) {
  switch (source) {
    case RotatorLink::Source::Web: return "web";
    case RotatorLink::Source::Rotctld: return "rotctld";
    case RotatorLink::Source::Raw: return "raw";
    default: return "poller";
  }
}

void buildStatus(JsonDocument& doc) {
  JsonObject position = doc["position"].to<JsonObject>();
  position["fresh"] = rotator->positionIsFresh();
  position["azimuth"] = rotator->realAzimuth();
  position["raw"] = rotator->rawAzimuth();
  position["overlap"] = rotator->inOverlap();
  position["ageMs"] = rotator->positionAgeMs();
  position["hasTarget"] = rotator->hasTarget();
  if (rotator->hasTarget()) {
    position["target"] = rotator->targetReal();
  }

  JsonObject controller = doc["controller"].to<JsonObject>();
  controller["bootLockout"] = rotator->inBootLockout();
  controller["linkHealthy"] = rotator->linkHealthy();
  controller["rawMin"] = rotator->range().rawMin;
  controller["rawMax"] = rotator->range().rawMax;
  controller["overlapFrom"] = config.overlapFrom;
  controller["overlapTo"] = config.overlapTo;
  if (rotator->lastNotice()[0] != '\0') {
    controller["notice"] = rotator->lastNotice();
  }

  if (rotator->hasMoved()) {
    JsonObject motion = doc["lastMotion"].to<JsonObject>();
    motion["source"] = sourceName(rotator->lastMotionSource());
    motion["ageMs"] = rotator->lastMotionAgeMs();
    if (rotator->lastMotionSource() == RotatorLink::Source::Web && lastWebActor[0] != '\0') {
      motion["user"] = lastWebActor;
    }
    // An absolute timestamp, not just an age - only once the bridge actually
    // knows the real date (see Net.h); before that, the panel falls back to
    // showing the relative age alone.
    if (net::timeSynced()) {
      motion["epochS"] = static_cast<uint32_t>(time(nullptr)) - (rotator->lastMotionAgeMs() / 1000);
    }
  }

  JsonObject sources = doc["sources"].to<JsonObject>();
  JsonObject rotctldInfo = sources["rotctld"].to<JsonObject>();
  rotctldInfo["port"] = rotctld->port();
  rotctldInfo["clients"] = rotctld->clientCount();
  rotctldInfo["max"] = rotctld->maxClients();
  if (rotctld->clientCount() > 0) {
    rotctldInfo["addresses"] = rotctld->clientAddresses();
  }

  JsonObject rawInfo = sources["raw"].to<JsonObject>();
  rawInfo["port"] = raw->port();
  rawInfo["clients"] = raw->clientCount();
  rawInfo["max"] = raw->maxClients();
  if (raw->clientCount() > 0) {
    rawInfo["addresses"] = raw->clientAddresses();
  }

  sources["remoteConnected"] = (rotctld->clientCount() + raw->clientCount()) > 0;

  // Every configured panel account's session state - who else is logged in,
  // and from where. "Is this me" is left for the client to work out (it
  // knows its own account from /api/session); this same JSON goes out to
  // every connected WebSocket at once, so there is no single "current
  // request" here to compare against.
  JsonArray sessions = doc["sessions"].to<JsonArray>();
  for (size_t i = 0; i < Config::kMaxUsers; i++) {
    if (config.users[i].name[0] == '\0') {
      continue;
    }
    const auth::SessionInfo* info = auth::session(config.users[i].name);
    JsonObject s = sessions.add<JsonObject>();
    s["name"] = config.users[i].name;
    s["active"] = info != nullptr && info->active;
    if (info != nullptr && info->active) {
      s["address"] = info->address.toString();
      s["ageMs"] = millis() - info->startedAt;
    }
  }

  JsonObject network = doc["network"].to<JsonObject>();
  network["mode"] = net::modeName();
  network["ssid"] = net::ssid();
  network["address"] = net::address();
  network["rssi"] = net::rssi();

  // ant-sw-2x6, a separate device (see AntennaSwitch.h) - folded into the same
  // status stream rather than a second poll loop in the panel.
  JsonObject antennaObj = doc["antenna"].to<JsonObject>();
  antennaObj["enabled"] = antswitch::enabled();
  antennaObj["connected"] = antswitch::connected();
  antennaObj["fresh"] = antswitch::fresh();
  JsonArray banks = antennaObj["banks"].to<JsonArray>();
  for (uint8_t i = 0; i < 2; i++) {
    JsonObject bank = banks.add<JsonObject>();
    const int ant = antswitch::antenna(i);
    if (ant >= 0) {
      bank["ant"] = ant;
    }
    bank["pwr"] = antswitch::power(i);
  }
  // From the switch's own EEPROM (see AntennaSwitch.h), not a second,
  // independently-typed copy - the legend can never disagree with that panel.
  JsonArray namesOut = antennaObj["names"].to<JsonArray>();
  for (uint8_t i = 0; i < 6; i++) {
    namesOut.add(antswitch::antennaName(i));
  }
  // Empty until the first successful /?K fetch (also true for older ant-sw-2x6
  // firmware that doesn't send it yet) - omitted rather than sent as "" so the
  // panel's own placeholder heading stays in charge of that case.
  if (antswitch::deviceName()[0] != '\0') {
    antennaObj["deviceName"] = antswitch::deviceName();
  }

  doc["jogging"] = jogActive;
  doc["heapFree"] = ESP.getFreeHeap();
  doc["uptimeMs"] = millis();
}

void sendJson(AsyncWebServerRequest* request, int code, const JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  request->send(code, "application/json", body);
}

void sendError(AsyncWebServerRequest* request, int code, const char* reason) {
  JsonDocument doc;
  doc["error"] = reason;
  sendJson(request, code, doc);
}

// Walks each "name=value" pair delimited by "; ", rather than searching the
// whole header for the substring "sid=" - a naive indexOf("sid=") would match
// inside any OTHER cookie whose name happens to end in "sid" (e.g. a stray
// "xsid=..." from some other script/proxy on the same origin) or even inside
// a value, silently returning the wrong cookie's contents as the session
// token instead of the real "sid" one.
String cookieToken(AsyncWebServerRequest* request) {
  if (!request->hasHeader("Cookie")) {
    return String();
  }
  const String cookies = request->header("Cookie");
  const int len = cookies.length();
  int pos = 0;
  while (pos < len) {
    while (pos < len && cookies[pos] == ' ') {
      pos++;
    }
    const int eq = cookies.indexOf('=', pos);
    int sep = cookies.indexOf(';', pos);
    if (sep < 0) {
      sep = len;
    }
    if (eq >= 0 && eq < sep && cookies.substring(pos, eq) == "sid") {
      return cookies.substring(eq + 1, sep);
    }
    pos = sep + 1;
  }
  return String();
}

bool requireAuth(AsyncWebServerRequest* request) {
  if (auth::validate(cookieToken(request).c_str(), request->client()->remoteIP())) {
    return true;
  }
  sendError(request, 401, "not authenticated");
  return false;
}

// True when the browser reached us over TLS. The bridge terminates plain HTTP;
// a reverse proxy in front (the recommended way to get modern TLS on a
// self-hosted LAN) tells us the real scheme via X-Forwarded-Proto. Only then
// is the session cookie marked Secure, so it is never withheld on a genuinely
// plain-HTTP LAN install where withholding it would just break login.
bool requestIsHttps(AsyncWebServerRequest* request) {
  return request->hasHeader("X-Forwarded-Proto") &&
         request->header("X-Forwarded-Proto").equalsIgnoreCase("https");
}

// The session cookie. HttpOnly because nothing in the panel reads it - the
// WebSocket authenticates from the handshake cookie, not a copy in JS - which
// keeps the token out of reach of any script. SameSite=Strict blocks it from
// cross-site requests. Secure is added behind a TLS proxy.
//
// No Max-Age on the active cookie (a session cookie: it lives only as long as
// the browser stays open) - the real expiry is server-side and sliding
// (Auth.cpp's kIdleTimeoutMs, renewed on every validate()). Login is the only
// place this header is sent, so a fixed Max-Age here would never be renewed:
// a continuously-active session (a multi-hour contest run, exactly the "sit
// through a slow QSO" case this bridge is meant for) would hit that fixed
// mark and force a re-login despite the server considering it still valid.
String sessionCookie(AsyncWebServerRequest* request, const String& token, bool clearing) {
  String c = "sid=";
  c += clearing ? "" : token;
  c += "; Path=/; HttpOnly; SameSite=Strict";
  if (clearing) {
    c += "; Max-Age=0";
  }
  if (requestIsHttps(request)) {
    c += "; Secure";
  }
  return c;
}

void handleStatus(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  JsonDocument doc;
  buildStatus(doc);
  sendJson(request, 200, doc);
}

// A goto now only fails for two reasons - the controller is still in its
// post-boot lockout, or the command queue is full - and each is reported so
// the operator is not left guessing. Any real azimuth is reachable and the
// bridge no longer needs a fresh cached position to command one, so those
// former refusals are gone.
void sendRefusal(AsyncWebServerRequest* request) {
  if (rotator->inBootLockout()) {
    sendError(request, 503, "controller in post-boot lockout");
  } else {
    sendError(request, 503, "command queue full");
  }
}

void handleGoto(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  if (!request->hasParam("az", true)) {
    sendError(request, 400, "missing az");
    return;
  }
  const float az = request->getParam("az", true)->value().toFloat();
  // isnan() first: az < 0.0f || az >= 360.0f is false for NaN either way (all
  // comparisons against NaN are), so that check alone would let it through -
  // and gotoAzimuth() rejecting it would then be misreported by sendRefusal()
  // below as "command queue full" instead of the actual reason.
  if (isnan(az) || az < 0.0f || az >= 360.0f) {
    sendError(request, 400, "az out of range");
    return;
  }
  if (!rotator->gotoAzimuth(az, RotatorLink::Source::Web)) {
    sendRefusal(request);
    return;
  }
  noteWebActor(auth::userForToken(cookieToken(request).c_str()));
  handleStatus(request);
}

void handleStop(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  jogActive = false;
  if (!rotator->stop(RotatorLink::Source::Web)) {
    sendError(request, 503, "queue full");
    return;
  }
  handleStatus(request);
}

void handleSync(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  // az (the real bearing the operator reads off the antenna, 0..359) is what
  // the panel sends - friendlier than the raw pulse count, and Rotator derives
  // the raw value itself. raw stays accepted too for scripts/tools that track
  // it directly.
  bool ok;
  if (request->hasParam("az", true)) {
    const float az = request->getParam("az", true)->value().toFloat();
    ok = rotator->syncReal(az, RotatorLink::Source::Web);
  } else if (request->hasParam("raw", true)) {
    const int rawValue = request->getParam("raw", true)->value().toInt();
    ok = rotator->syncRaw(rawValue, RotatorLink::Source::Web);
  } else {
    sendError(request, 400, "missing az or raw");
    return;
  }
  if (!ok) {
    sendError(request, 400, "azimuth/raw out of range");
    return;
  }
  handleStatus(request);
}

// --- antenna switch (ant-sw-2x6) ---------------------------------------------

void handleAntenna(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  if (!request->hasParam("bank", true) || !request->hasParam("ant", true)) {
    sendError(request, 400, "missing bank or ant");
    return;
  }
  const int bank = request->getParam("bank", true)->value().toInt();
  const int ant = request->getParam("ant", true)->value().toInt();
  if (bank < 1 || bank > 2 || ant < 0 || ant > 6) {
    sendError(request, 400, "bank must be 1..2, ant must be 0..6");
    return;
  }
  if (!antswitch::setAntenna(static_cast<uint8_t>(bank - 1), static_cast<uint8_t>(ant))) {
    sendError(request, 503, "antenna switch disabled");
    return;
  }
  handleStatus(request);
}

void handleAntennaPower(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  if (!request->hasParam("bank", true) || !request->hasParam("on", true)) {
    sendError(request, 400, "missing bank or on");
    return;
  }
  const int bank = request->getParam("bank", true)->value().toInt();
  const int on = request->getParam("on", true)->value().toInt();
  if (bank < 1 || bank > 2 || (on != 0 && on != 1)) {
    sendError(request, 400, "bank must be 1..2, on must be 0 or 1");
    return;
  }
  if (!antswitch::setPower(static_cast<uint8_t>(bank - 1), on != 0)) {
    sendError(request, 503, "antenna switch disabled");
    return;
  }
  handleStatus(request);
}

// --- session ---------------------------------------------------------------

void handleSession(AsyncWebServerRequest* request) {
  JsonDocument doc;
  const String token = cookieToken(request);
  doc["authenticated"] = auth::validate(token.c_str(), request->client()->remoteIP());
  const char* me = auth::userForToken(token.c_str());
  if (me != nullptr) {
    doc["user"] = me;  // which of possibly several accounts this cookie belongs to
  }
  doc["siteName"] = config.siteName;  // so the login screen can show the name

  JsonArray users = doc["users"].to<JsonArray>();
  for (size_t i = 0; i < Config::kMaxUsers; i++) {
    if (config.users[i].name[0] == '\0') {
      continue;
    }
    JsonObject u = users.add<JsonObject>();
    u["name"] = config.users[i].name;
    u["needsSetup"] = Config::needsSetup(config.users[i]);
  }
  sendJson(request, 200, doc);
}

void handleSetup(AsyncWebServerRequest* request) {
  if (!request->hasParam("user", true) || !request->hasParam("password", true)) {
    sendError(request, 400, "missing user or password");
    return;
  }
  const String user = request->getParam("user", true)->value();
  Config::User* u = config.findUser(user.c_str());
  if (u == nullptr) {
    sendError(request, 400, "unknown user");
    return;
  }
  // Only available before this specific account has a password; otherwise it
  // would be a way to reset it without knowing it.
  if (!Config::needsSetup(*u)) {
    sendError(request, 409, "already configured");
    return;
  }
  if (!auth::upsertUser(user.c_str(), request->getParam("password", true)->value().c_str())) {
    sendError(request, 400, "password must be at least 8 characters");
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(request, 200, doc);
}

void handleLogin(AsyncWebServerRequest* request) {
  if (auth::throttled()) {
    JsonDocument doc;
    doc["error"] = "too many attempts";
    doc["retryAfterMs"] = auth::throttleRemainingMs();
    sendJson(request, 429, doc);
    return;
  }
  if (!request->hasParam("user", true) || !request->hasParam("password", true)) {
    sendError(request, 400, "missing user or password");
    return;
  }
  const String user = request->getParam("user", true)->value();
  const bool force = request->hasParam("force", true) && request->getParam("force", true)->value() == "1";

  String token;
  const auth::LoginResult result = auth::login(user.c_str(), request->getParam("password", true)->value().c_str(),
                                               request->client()->remoteIP(), force, token);
  if (result == auth::LoginResult::SessionHeld) {
    // Only reached once auth::login() has already verified the password -
    // never derived from account/session state alone, which an
    // unauthenticated caller could probe by merely naming an account.
    const auth::SessionInfo* info = auth::session(user.c_str());
    JsonDocument doc;
    doc["error"] = "session held";
    doc["sessionAddress"] = info != nullptr ? info->address.toString() : String();
    doc["canForce"] = true;
    sendJson(request, 409, doc);
    return;
  }
  if (result != auth::LoginResult::Ok) {
    sendError(request, 401, "invalid credentials");
    return;
  }

  AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"ok\":true}");
  response->addHeader("Set-Cookie", sessionCookie(request, token, false));
  request->send(response);
}

void handleLogout(AsyncWebServerRequest* request) {
  auth::logout(cookieToken(request).c_str());
  AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"ok\":true}");
  response->addHeader("Set-Cookie", sessionCookie(request, "", true));
  request->send(response);
}

// --- account management -----------------------------------------------------
// Flat, like the rest of Settings: any authenticated session can manage
// accounts, there is no separate admin role.

void handleGetUsers(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (size_t i = 0; i < Config::kMaxUsers; i++) {
    if (config.users[i].name[0] == '\0') {
      continue;
    }
    JsonObject u = array.add<JsonObject>();
    u["name"] = config.users[i].name;
    u["needsSetup"] = Config::needsSetup(config.users[i]);
  }
  sendJson(request, 200, doc);
}

void handleUpsertUser(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  if (!request->hasParam("name", true) || !request->hasParam("password", true)) {
    sendError(request, 400, "missing name or password");
    return;
  }
  const String name = request->getParam("name", true)->value();
  if (!auth::upsertUser(name.c_str(), request->getParam("password", true)->value().c_str())) {
    sendError(request, 400, "password must be at least 8 characters, or every account slot is full");
    return;
  }
  handleGetUsers(request);
}

void handleDeleteUser(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  if (!request->hasParam("name", true)) {
    sendError(request, 400, "missing name");
    return;
  }
  if (!auth::deleteUser(request->getParam("name", true)->value().c_str())) {
    sendError(request, 400, "unknown account, or the last remaining account");
    return;
  }
  handleGetUsers(request);
}

// --- favourites ------------------------------------------------------------

void handleGetFavorites(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (size_t i = 0; i < favorites::count(); i++) {
    JsonObject item = array.add<JsonObject>();
    item["name"] = favorites::at(i).name;
    item["az"] = favorites::at(i).azimuth;
  }
  sendJson(request, 200, doc);
}

void handleSetFavorites(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  if (!requireAuth(request)) {
    return;
  }
  if (index != 0 || len != total) {
    sendError(request, 413, "body too large");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    sendError(request, 400, "invalid json");
    return;
  }

  favorites::Entry entries[favorites::kMax];
  size_t n = 0;
  for (JsonObjectConst item : doc.as<JsonArrayConst>()) {
    if (n >= favorites::kMax) {
      break;
    }
    strncpy(entries[n].name, item["name"] | "", favorites::kNameLen - 1);
    entries[n].name[favorites::kNameLen - 1] = '\0';
    entries[n].azimuth = item["az"] | 0.0f;
    n++;
  }

  if (!favorites::replaceAll(entries, n)) {
    sendError(request, 500, "could not save");
    return;
  }
  handleGetFavorites(request);
}

// --- WiFi networks -----------------------------------------------------------

// Async (WiFi.scanNetworks(true) below): a blocking scan takes seconds, and
// this bridge never blocks loop() for anything an operator can trigger from
// the panel - the jog dead-man timer runs off the same loop().
void handleWifiScan(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  const int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    JsonDocument doc;
    doc["status"] = "scanning";
    sendJson(request, 200, doc);
    return;
  }
  if (result == WIFI_SCAN_FAILED) {
    // No scan in flight, and any previous results were already read and
    // cleared below - start one now rather than reporting an empty list as
    // if nothing were in range.
    WiFi.scanNetworks(true);
    JsonDocument doc;
    doc["status"] = "scanning";
    sendJson(request, 200, doc);
    return;
  }

  JsonDocument doc;
  doc["status"] = "done";
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (int i = 0; i < result; i++) {
    JsonObject n = networks.add<JsonObject>();
    n["ssid"] = WiFi.SSID(i);
    n["rssi"] = WiFi.RSSI(i);
    n["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  // Frees the scan result buffer and resets scanComplete() to
  // WIFI_SCAN_FAILED, so the next call to this endpoint starts a fresh scan
  // instead of replaying an ever-more-stale list.
  WiFi.scanDelete();
  // scanNetworks() above ORs WIFI_MODE_STA into whatever mode was already
  // active and never reverts it - left alone, a scan triggered while in AP
  // fallback would leave the radio broadcasting AP and STA at once for the
  // rest of that boot. Undo the merge now that the scan is actually done.
  net::reassertMode();
  sendJson(request, 200, doc);
}

void handleGetWifiNetworks(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (size_t i = 0; i < Config::kMaxWifiNetworks; i++) {
    if (config.wifiNetworks[i].ssid[0] == '\0') {
      continue;
    }
    // Never echoes the password back, same as /api/users with passwordHash -
    // the client only needs to know which networks are already saved.
    JsonObject n = array.add<JsonObject>();
    n["ssid"] = config.wifiNetworks[i].ssid;
  }
  sendJson(request, 200, doc);
}

void handleUpsertWifiNetwork(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  if (!request->hasParam("ssid", true)) {
    sendError(request, 400, "missing ssid");
    return;
  }
  const String ssid = request->getParam("ssid", true)->value();
  if (ssid.length() == 0 || ssid.length() >= Config::kStrLen) {
    sendError(request, 400, "ssid must be 1..39 characters");
    return;
  }
  const String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";

  Config::WifiNetwork* slot = config.findWifiNetwork(ssid.c_str());
  if (slot == nullptr) {
    for (size_t i = 0; i < Config::kMaxWifiNetworks; i++) {
      if (config.wifiNetworks[i].ssid[0] == '\0') {
        slot = &config.wifiNetworks[i];
        break;
      }
    }
  }
  if (slot == nullptr) {
    sendError(request, 400, "every network slot is full");
    return;
  }

  strncpy(slot->ssid, ssid.c_str(), Config::kStrLen - 1);
  slot->ssid[Config::kStrLen - 1] = '\0';
  strncpy(slot->password, password.c_str(), Config::kStrLen - 1);
  slot->password[Config::kStrLen - 1] = '\0';

  if (!config.save()) {
    sendError(request, 500, "could not save");
    return;
  }
  handleGetWifiNetworks(request);
}

void handleDeleteWifiNetwork(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  if (!request->hasParam("ssid", true)) {
    sendError(request, 400, "missing ssid");
    return;
  }
  Config::WifiNetwork* slot = config.findWifiNetwork(request->getParam("ssid", true)->value().c_str());
  if (slot == nullptr) {
    sendError(request, 400, "unknown network");
    return;
  }
  *slot = Config::WifiNetwork{};
  if (!config.save()) {
    sendError(request, 500, "could not save");
    return;
  }
  handleGetWifiNetworks(request);
}

// --- configuration ---------------------------------------------------------

void handleGetConfig(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  JsonDocument doc;
  doc["hostname"] = config.hostname;
  doc["siteName"] = config.siteName;
  doc["wifiConfigured"] = config.hasWifi();
  doc["rotctldPort"] = config.rotctldPort;
  doc["rawPort"] = config.rawPort;
  doc["rotctldMaxClients"] = config.rotctldMaxClients;
  doc["rawMaxClients"] = config.rawMaxClients;
  // The hard ceilings, so the UI can bound its inputs and show the resource cap.
  doc["rotctldCeiling"] = RotctldServer::clientCeiling();
  doc["rawCeiling"] = RawServer::clientCeiling();
  doc["serialBaud"] = config.serialBaud;
  doc["rawMin"] = config.rawMin;
  doc["rawMax"] = config.rawMax;
  doc["overlapFrom"] = config.overlapFrom;
  doc["overlapTo"] = config.overlapTo;
  doc["antEnabled"] = config.antEnabled;
  doc["antHost"] = config.antHost;
  doc["rotorAnt"] = config.rotorAnt;
  doc["debugEnabled"] = config.debugEnabled;
  doc["debugRotctld"] = config.debugRotctld;
  doc["debugRaw"] = config.debugRaw;
  doc["debugAntenna"] = config.debugAntenna;
  doc["debugController"] = config.debugController;
  sendJson(request, 200, doc);
}

void copyParam(AsyncWebServerRequest* request, const char* name, char* dest, size_t len) {
  if (!request->hasParam(name, true)) {
    return;
  }
  const String value = request->getParam(name, true)->value();
  strncpy(dest, value.c_str(), len - 1);
  dest[len - 1] = '\0';
}

// Ports are validated rather than clamped: silently moving a listener to a
// port the operator did not ask for is worse than refusing the change.
bool readPort(AsyncWebServerRequest* request, const char* name, uint16_t& target) {
  if (!request->hasParam(name, true)) {
    return true;
  }
  const long value = request->getParam(name, true)->value().toInt();
  if (value < 1 || value > 65535) {
    return false;
  }
  target = static_cast<uint16_t>(value);
  return true;
}

void handleSetConfig(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }

  copyParam(request, "hostname", config.hostname, Config::kStrLen);
  copyParam(request, "siteName", config.siteName, Config::kStrLen);

  // Write-only, like account passwords elsewhere in this file - never echoed
  // back by handleGetConfig, so the panel's field is always blank and an
  // untouched field must not be mistaken for "set the AP password to empty".
  // Empty is a deliberate choice (open fallback AP), not the absence of one -
  // hence the length check only when the field was actually submitted non-empty.
  if (request->hasParam("apPassword", true)) {
    const String apPassword = request->getParam("apPassword", true)->value();
    if (apPassword.length() > 0 && apPassword.length() < 8) {
      sendError(request, 400, "AP password must be empty (open) or at least 8 characters");
      return;
    }
    strncpy(config.apPassword, apPassword.c_str(), Config::kStrLen - 1);
    config.apPassword[Config::kStrLen - 1] = '\0';
  }

  // Parsed into locals first, not straight into config: readPort() writes its
  // target as soon as a value parses, so validating the cross-field "must
  // differ" rule against the live config fields afterwards would leave a
  // rejected request's bad value already sitting in config - silently
  // persisted by some later, unrelated save (the settings form always submits
  // the whole struct together), rather than merely refused.
  uint16_t newRotctldPort = config.rotctldPort;
  uint16_t newRawPort = config.rawPort;
  if (!readPort(request, "rotctldPort", newRotctldPort) || !readPort(request, "rawPort", newRawPort)) {
    sendError(request, 400, "port must be 1..65535");
    return;
  }
  if (newRotctldPort == newRawPort) {
    sendError(request, 400, "rotctld and raw ports must differ");
    return;
  }
  config.rotctldPort = newRotctldPort;
  config.rawPort = newRawPort;

  // Client limits: clamp to the resource ceiling rather than reject, so a value
  // over the cap still applies at the maximum the hardware can carry.
  if (request->hasParam("rotctldMaxClients", true)) {
    const long v = request->getParam("rotctldMaxClients", true)->value().toInt();
    if (v < 0) {
      sendError(request, 400, "rotctldMaxClients must be >= 0");
      return;
    }
    config.rotctldMaxClients = (v > (long)RotctldServer::clientCeiling()) ? RotctldServer::clientCeiling() : v;
  }
  if (request->hasParam("rawMaxClients", true)) {
    const long v = request->getParam("rawMaxClients", true)->value().toInt();
    if (v < 0) {
      sendError(request, 400, "rawMaxClients must be >= 0");
      return;
    }
    config.rawMaxClients = (v > (long)RawServer::clientCeiling()) ? RawServer::clientCeiling() : v;
  }

  if (request->hasParam("serialBaud", true)) {
    const long baud = request->getParam("serialBaud", true)->value().toInt();
    // A standard rate only. An arbitrary divisor would come back as a link
    // that looks configured but returns nothing but framing errors, and the
    // only way out would be a serial console the operator may not have.
    static const long kRates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    bool valid = false;
    for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++) {
      valid = valid || (baud == kRates[i]);
    }
    if (!valid) {
      sendError(request, 400, "serialBaud must be a standard rate 1200..115200");
      return;
    }
    config.serialBaud = static_cast<uint32_t>(baud);
  }

  for (const char* name : {"overlapFrom", "overlapTo"}) {
    if (!request->hasParam(name, true)) {
      continue;
    }
    const long value = request->getParam(name, true)->value().toInt();
    if (value < 0 || value > 359) {
      sendError(request, 400, "overlap bearings must be 0..359");
      return;
    }
    (strcmp(name, "overlapFrom") == 0 ? config.overlapFrom : config.overlapTo) = static_cast<int>(value);
  }

  if (request->hasParam("antEnabled", true)) {
    config.antEnabled = request->getParam("antEnabled", true)->value() == "1";
  }
  copyParam(request, "antHost", config.antHost, Config::kStrLen);

  // Which antenna this rotator physically turns - purely a marker for the
  // panel (see Config.h), not fed back into AntennaSwitch at all, so 0 (not
  // configured) is as valid as 1..6.
  if (request->hasParam("rotorAnt", true)) {
    const long v = request->getParam("rotorAnt", true)->value().toInt();
    if (v < 0 || v > 6) {
      sendError(request, 400, "rotorAnt must be 0..6");
      return;
    }
    config.rotorAnt = static_cast<uint8_t>(v);
  }

  for (const char* name : {"debugEnabled", "debugRotctld", "debugRaw", "debugAntenna", "debugController"}) {
    if (!request->hasParam(name, true)) {
      continue;
    }
    const bool value = request->getParam(name, true)->value() == "1";
    if (strcmp(name, "debugEnabled") == 0) config.debugEnabled = value;
    else if (strcmp(name, "debugRotctld") == 0) config.debugRotctld = value;
    else if (strcmp(name, "debugRaw") == 0) config.debugRaw = value;
    else if (strcmp(name, "debugAntenna") == 0) config.debugAntenna = value;
    else config.debugController = value;
  }

  if (!config.save()) {
    sendError(request, 500, "could not write config");
    return;
  }

  JsonDocument doc;
  doc["saved"] = true;
  doc["restartRequired"] = true;
  sendJson(request, 200, doc);
}

// --- firmware update -------------------------------------------------------

// Over the panel's own authenticated connection rather than ArduinoOTA. One
// authenticated surface is easier to reason about than two, and it means an
// update needs only a browser - no toolchain at the mast.
void handleUpdateUpload(AsyncWebServerRequest* request, const String& filename, size_t index, uint8_t* data,
                        size_t len, bool final) {
  if (!auth::validate(cookieToken(request).c_str(), request->client()->remoteIP())) {
    return;
  }

  if (index == 0) {
    // "filesystem" replaces the LittleFS image, anything else the firmware.
    const bool isFilesystem = request->hasParam("target", true) &&
                              request->getParam("target", true)->value() == "filesystem";
    const int command = isFilesystem ? U_SPIFFS : U_FLASH;

    // Stopping the rotator first: an update reboots the bridge, and a rotator
    // left turning by a jog or a long rotation would keep going with nothing
    // watching it.
    rotator->stop(RotatorLink::Source::Web);
    jogActive = false;

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
      return;
    }
  }

  if (Update.write(data, len) != len) {
    return;
  }

  if (final) {
    Update.end(true);
  }
}

void handleUpdateDone(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  const bool ok = !Update.hasError();

  JsonDocument doc;
  doc["ok"] = ok;
  if (!ok) {
    doc["error"] = Update.errorString();
  }
  sendJson(request, ok ? 200 : 500, doc);

  if (ok) {
    delay(200);
    ESP.restart();
  }
}

// --- websocket -------------------------------------------------------------

void handleSocketMessage(AsyncWebSocketClient* client, const char* message) {
  JsonDocument doc;
  if (deserializeJson(doc, message)) {
    return;
  }

  if (!doc["jog"].is<const char*>()) {
    return;
  }
  const char* jog = doc["jog"];

  if (strcmp(jog, "stop") == 0) {
    jogActive = false;
    rotator->stop(RotatorLink::Source::Web);
    return;
  }

  const bool clockwise = (strcmp(jog, "cw") == 0);
  if (!clockwise && strcmp(jog, "ccw") != 0) {
    return;
  }

  lastJogAt = millis();
  if (!jogActive) {
    jogActive = rotator->jog(clockwise, RotatorLink::Source::Web);
    if (jogActive) {
      noteWebActor(wsUserName(client->id()));
    }
  }
}

void onSocketEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                   size_t len) {
  (void)ws;

  switch (type) {
    case WS_EVT_CONNECT: {
      // The handshake carries the session cookie (browsers send it on a
      // same-origin WebSocket, and the proxy forwards it), so authenticate
      // here rather than trusting a token echoed in the first message. An
      // unauthenticated socket is closed before it can drive anything.
      AsyncWebServerRequest* request = static_cast<AsyncWebServerRequest*>(arg);
      const String token = cookieToken(request);
      if (!auth::validate(token.c_str(), client->remoteIP())) {
        client->close(1008, "unauthorised");
      } else {
        setWsUser(client->id(), auth::userForToken(token.c_str()));
      }
      break;
    }

    case WS_EVT_DATA: {
      AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0;
        handleSocketMessage(client, reinterpret_cast<const char*>(data));
      }
      break;
    }

    case WS_EVT_DISCONNECT:
      // A panel that vanishes mid-jog must not leave the rotator turning.
      if (jogActive) {
        jogActive = false;
        rotator->stop(RotatorLink::Source::Web);
      }
      clearWsUser(client->id());
      break;

    default:
      break;
  }
}

}  // namespace

void begin(Rotator& r, RotctldServer& rotctldServer, RawServer& rawServer) {
  rotator = &r;
  rotctld = &rotctldServer;
  raw = &rawServer;

  auth::begin();
  favorites::begin();

  socket.onEvent(onSocketEvent);
  server.addHandler(&socket);

  server.on("/api/session", HTTP_GET, handleSession);
  server.on("/api/setup", HTTP_POST, handleSetup);
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/logout", HTTP_POST, handleLogout);

  server.on("/api/users", HTTP_GET, handleGetUsers);
  server.on("/api/users", HTTP_POST, handleUpsertUser);
  server.on("/api/users/delete", HTTP_POST, handleDeleteUser);

  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/networks", HTTP_GET, handleGetWifiNetworks);
  server.on("/api/wifi/networks", HTTP_POST, handleUpsertWifiNetwork);
  server.on("/api/wifi/networks/delete", HTTP_POST, handleDeleteWifiNetwork);

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/goto", HTTP_POST, handleGoto);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/sync", HTTP_POST, handleSync);

  server.on("/api/antenna", HTTP_POST, handleAntenna);
  server.on("/api/antenna/power", HTTP_POST, handleAntennaPower);

  server.on("/api/favorites", HTTP_GET, handleGetFavorites);
  server.on(
      "/api/favorites", HTTP_POST, [](AsyncWebServerRequest* request) {}, nullptr, handleSetFavorites);

  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handleSetConfig);

  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!requireAuth(request)) {
      return;
    }
    request->send(200, "application/json", "{\"restarting\":true}");
    delay(100);
    ESP.restart();
  });

  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html").setCacheControl("max-age=600");
  server.onNotFound([](AsyncWebServerRequest* request) { sendError(request, 404, "not found"); });

  server.begin();
}

void poll() {
  auth::poll();

  // The dead-man timer. Silence from the panel stops the rotator; this is the
  // one place where "no news" must not mean "carry on".
  if (jogActive && (millis() - lastJogAt > kJogKeepaliveMs)) {
    jogActive = false;
    rotator->stop(RotatorLink::Source::Web);
  }

  if (millis() - lastBroadcast >= kBroadcastIntervalMs) {
    lastBroadcast = millis();
    socket.cleanupClients();
    if (socket.count() > 0) {
      JsonDocument doc;
      buildStatus(doc);
      // Drained only here, not inside buildStatus() itself: that function is
      // also called by action handlers (handleGoto/handleStop/...) to answer
      // their own HTTP request, and draining there would consume an entry
      // into a response the panel's JS never reads, losing it before this
      // broadcast - the one thing the Monitor tab actually renders - ever
      // sees it.
      if (config.debugEnabled) {
        JsonArray debugArr = doc["debugLog"].to<JsonArray>();
        if (debuglog::drain(debugArr)) {
          doc["debugOverflow"] = true;
        }
      }
      String body;
      serializeJson(doc, body);
      socket.textAll(body);
    }
  }
}

}  // namespace webapi
