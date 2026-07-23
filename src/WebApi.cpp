#include "WebApi.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "Config.h"
#include "Net.h"

namespace webapi {
namespace {

AsyncWebServer server(80);
Rotator* rotator = nullptr;
RotctldServer* rotctld = nullptr;

const char* sourceName(RotatorLink::Source source) {
  switch (source) {
    case RotatorLink::Source::Web: return "web";
    case RotatorLink::Source::Rotctld: return "rotctld";
    case RotatorLink::Source::Raw: return "raw";
    default: return "poller";
  }
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

void handleStatus(AsyncWebServerRequest* request) {
  JsonDocument doc;

  // Position is reported with its freshness, never as a bare number: a frozen
  // value that looks live is worse than an explicit "unknown".
  JsonObject position = doc["position"].to<JsonObject>();
  position["fresh"] = rotator->positionIsFresh();
  position["azimuth"] = rotator->realAzimuth();
  position["raw"] = rotator->rawAzimuth();
  position["overlap"] = rotator->inOverlap();
  position["ageMs"] = rotator->positionAgeMs();

  JsonObject controller = doc["controller"].to<JsonObject>();
  controller["bootLockout"] = rotator->inBootLockout();
  controller["rawMin"] = rotator->range().rawMin;
  controller["rawMax"] = rotator->range().rawMax;
  if (rotator->lastNotice()[0] != '\0') {
    controller["notice"] = rotator->lastNotice();
  }

  // Which source last moved the rotator - the question that matters when the
  // antenna starts turning is not "is someone connected" but "why".
  if (rotator->hasMoved()) {
    JsonObject motion = doc["lastMotion"].to<JsonObject>();
    motion["source"] = sourceName(rotator->lastMotionSource());
    motion["ageMs"] = rotator->lastMotionAgeMs();
  }

  // Who else is connected. The panel turns this into a persistent banner:
  // the operator has to be able to see that something remote can move the
  // antenna, without having to go looking for it.
  JsonObject sources = doc["sources"].to<JsonObject>();
  JsonObject rotctldInfo = sources["rotctld"].to<JsonObject>();
  rotctldInfo["port"] = rotctld->port();
  rotctldInfo["clients"] = rotctld->clientCount();
  if (rotctld->clientCount() > 0) {
    rotctldInfo["addresses"] = rotctld->clientAddresses();
  }

  JsonObject network = doc["network"].to<JsonObject>();
  network["mode"] = net::modeName();
  network["ssid"] = net::ssid();
  network["address"] = net::address();
  network["rssi"] = net::rssi();

  doc["heapFree"] = ESP.getFreeHeap();
  doc["uptimeMs"] = millis();

  sendJson(request, 200, doc);
}

// Reasons a command can be refused are distinguished, because "rejected" alone
// sends the operator looking at the wrong thing.
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

void handleJog(AsyncWebServerRequest* request) {
  if (!request->hasParam("dir", true)) {
    sendError(request, 400, "missing dir");
    return;
  }
  const String dir = request->getParam("dir", true)->value();
  if (dir != "cw" && dir != "ccw") {
    sendError(request, 400, "dir must be cw or ccw");
    return;
  }
  if (!rotator->jog(dir == "cw", RotatorLink::Source::Web)) {
    sendRefusal(request);
    return;
  }
  handleStatus(request);
}

void handleStop(AsyncWebServerRequest* request) {
  if (!rotator->stop(RotatorLink::Source::Web)) {
    sendError(request, 503, "queue full");
    return;
  }
  handleStatus(request);
}

void handleSync(AsyncWebServerRequest* request) {
  if (!request->hasParam("raw", true)) {
    sendError(request, 400, "missing raw");
    return;
  }
  const int raw = request->getParam("raw", true)->value().toInt();
  if (!rotator->syncRaw(raw, RotatorLink::Source::Web)) {
    sendError(request, 400, "raw out of range");
    return;
  }
  handleStatus(request);
}

void handleGetConfig(AsyncWebServerRequest* request) {
  JsonDocument doc;
  // Credentials are never sent back out, only whether they are set.
  doc["hostname"] = config.hostname;
  doc["wifiSsid"] = config.wifiSsid;
  doc["wifiConfigured"] = config.hasWifi();
  doc["passwordSet"] = !config.needsPasswordSetup();
  doc["rotctldPort"] = config.rotctldPort;
  doc["rawPort"] = config.rawPort;
  doc["rawMin"] = config.rawMin;
  doc["rawMax"] = config.rawMax;
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

  if (!config.save()) {
    sendError(request, 500, "could not write config");
    return;
  }

  JsonDocument doc;
  doc["saved"] = true;
  // WiFi changes only take effect on restart; say so rather than leaving the
  // caller to wonder why nothing happened.
  doc["restartRequired"] = true;
  sendJson(request, 200, doc);
}

}  // namespace

void begin(Rotator& r, RotctldServer& rotctldServer) {
  rotator = &r;
  rotctld = &rotctldServer;

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/goto", HTTP_POST, handleGoto);
  server.on("/api/jog", HTTP_POST, handleJog);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/sync", HTTP_POST, handleSync);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handleSetConfig);

  server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"restarting\":true}");
    delay(100);
    ESP.restart();
  });

  // The panel itself lands here in phase 5; until then LittleFS may be empty.
  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest* request) { sendError(request, 404, "not found"); });

  server.begin();
}

}  // namespace webapi
