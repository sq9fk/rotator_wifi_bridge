#include "WebApi.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>

#include "Auth.h"
#include "Config.h"
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
    position["targetRaw"] = rotator->targetRaw();
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
  }

  JsonObject sources = doc["sources"].to<JsonObject>();
  JsonObject rotctldInfo = sources["rotctld"].to<JsonObject>();
  rotctldInfo["port"] = rotctld->port();
  rotctldInfo["clients"] = rotctld->clientCount();
  if (rotctld->clientCount() > 0) {
    rotctldInfo["addresses"] = rotctld->clientAddresses();
  }

  JsonObject rawInfo = sources["raw"].to<JsonObject>();
  rawInfo["port"] = raw->port();
  rawInfo["clients"] = raw->clientCount();
  if (raw->clientCount() > 0) {
    rawInfo["addresses"] = raw->clientAddresses();
  }

  sources["remoteConnected"] = (rotctld->clientCount() + raw->clientCount()) > 0;

  JsonObject network = doc["network"].to<JsonObject>();
  network["mode"] = net::modeName();
  network["ssid"] = net::ssid();
  network["address"] = net::address();
  network["rssi"] = net::rssi();

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

String cookieToken(AsyncWebServerRequest* request) {
  if (!request->hasHeader("Cookie")) {
    return String();
  }
  const String cookies = request->header("Cookie");
  const int start = cookies.indexOf("sid=");
  if (start < 0) {
    return String();
  }
  int end = cookies.indexOf(';', start);
  if (end < 0) {
    end = cookies.length();
  }
  return cookies.substring(start + 4, end);
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
String sessionCookie(AsyncWebServerRequest* request, const String& token, bool clearing) {
  String c = "sid=";
  c += clearing ? "" : token;
  c += "; Path=/; HttpOnly; SameSite=Strict";
  c += clearing ? "; Max-Age=0" : "; Max-Age=3600";
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

// Why a command was refused is reported specifically: a bare rejection sends
// the operator looking at the wrong thing.
void sendRefusal(AsyncWebServerRequest* request) {
  if (rotator->inBootLockout()) {
    sendError(request, 503, "controller in post-boot lockout");
  } else if (!rotator->positionIsFresh()) {
    sendError(request, 503, "position unknown");
  } else {
    sendError(request, 400, "azimuth unreachable");
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
  if (az < 0.0f || az >= 360.0f) {
    sendError(request, 400, "az out of range");
    return;
  }
  if (!rotator->gotoAzimuth(az, RotatorLink::Source::Web)) {
    sendRefusal(request);
    return;
  }
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
  if (!request->hasParam("raw", true)) {
    sendError(request, 400, "missing raw");
    return;
  }
  const int rawValue = request->getParam("raw", true)->value().toInt();
  if (!rotator->syncRaw(rawValue, RotatorLink::Source::Web)) {
    sendError(request, 400, "raw out of range");
    return;
  }
  handleStatus(request);
}

// --- session ---------------------------------------------------------------

void handleSession(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["setupRequired"] = config.needsPasswordSetup();
  doc["authenticated"] = auth::validate(cookieToken(request).c_str(), request->client()->remoteIP());
  doc["user"] = config.webUser;

  const auth::SessionInfo& info = auth::session();
  doc["sessionActive"] = info.active;
  if (info.active) {
    doc["sessionAddress"] = info.address.toString();
    doc["sessionAgeMs"] = millis() - info.startedAt;
  }
  sendJson(request, 200, doc);
}

void handleSetup(AsyncWebServerRequest* request) {
  // Only available while no password exists; otherwise it would be a way to
  // reset the password without knowing it.
  if (!config.needsPasswordSetup()) {
    sendError(request, 409, "already configured");
    return;
  }
  if (!request->hasParam("password", true)) {
    sendError(request, 400, "missing password");
    return;
  }
  if (request->hasParam("user", true)) {
    const String user = request->getParam("user", true)->value();
    strncpy(config.webUser, user.c_str(), Config::kStrLen - 1);
    config.webUser[Config::kStrLen - 1] = '\0';
  }
  if (!auth::setPassword(request->getParam("password", true)->value().c_str())) {
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
  if (!request->hasParam("password", true)) {
    sendError(request, 400, "missing password");
    return;
  }
  const String user = request->hasParam("user", true) ? request->getParam("user", true)->value() : String(config.webUser);
  const bool force = request->hasParam("force", true) && request->getParam("force", true)->value() == "1";

  const String token = auth::login(user.c_str(), request->getParam("password", true)->value().c_str(),
                                   request->client()->remoteIP(), force);
  if (token.isEmpty()) {
    const auth::SessionInfo& info = auth::session();
    if (info.active && !force) {
      // Distinguish "wrong password" from "someone else is logged in", and say
      // where from - otherwise the only recovery is a reboot.
      JsonDocument doc;
      doc["error"] = "session held";
      doc["sessionAddress"] = info.address.toString();
      doc["canForce"] = true;
      sendJson(request, 409, doc);
      return;
    }
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

// --- configuration ---------------------------------------------------------

void handleGetConfig(AsyncWebServerRequest* request) {
  if (!requireAuth(request)) {
    return;
  }
  JsonDocument doc;
  doc["hostname"] = config.hostname;
  doc["wifiSsid"] = config.wifiSsid;
  doc["wifiConfigured"] = config.hasWifi();
  doc["passwordSet"] = !config.needsPasswordSetup();
  doc["rotctldPort"] = config.rotctldPort;
  doc["rawPort"] = config.rawPort;
  doc["serialBaud"] = config.serialBaud;
  doc["rawMin"] = config.rawMin;
  doc["rawMax"] = config.rawMax;
  doc["overlapFrom"] = config.overlapFrom;
  doc["overlapTo"] = config.overlapTo;
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

  copyParam(request, "wifiSsid", config.wifiSsid, Config::kStrLen);
  copyParam(request, "wifiPassword", config.wifiPassword, Config::kStrLen);
  copyParam(request, "hostname", config.hostname, Config::kStrLen);

  if (!readPort(request, "rotctldPort", config.rotctldPort) || !readPort(request, "rawPort", config.rawPort)) {
    sendError(request, 400, "port must be 1..65535");
    return;
  }
  if (config.rotctldPort == config.rawPort) {
    sendError(request, 400, "rotctld and raw ports must differ");
    return;
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

  if (request->hasParam("password", true)) {
    if (!auth::setPassword(request->getParam("password", true)->value().c_str())) {
      sendError(request, 400, "password must be at least 8 characters");
      return;
    }
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
      if (!auth::validate(cookieToken(request).c_str(), client->remoteIP())) {
        client->close(1008, "unauthorised");
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

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/goto", HTTP_POST, handleGoto);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/sync", HTTP_POST, handleSync);

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
      String body;
      serializeJson(doc, body);
      socket.textAll(body);
    }
  }
}

}  // namespace webapi
