#include "Updater.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <atomic>

#include "BuildVersion.h"
#include "Config.h"
#include "Net.h"
#include "Semver.h"

// Embedded by board_build.embed_files (platformio.ini); see
// tools/gen_ca_bundle.py for the format and why it is the full Mozilla set.
extern const uint8_t kCaBundle[] asm("_binary_certs_x509_crt_bundle_bin_start");

// Replaces the core's weak default (false), with which initArduino() marks a
// freshly installed image valid before setup() even runs - leaving the
// bootloader's rollback nothing to protect. Returning true defers that to
// updater::poll(), once the new firmware has actually been running a while.
// C linkage: the weak default lives in esp32-hal-misc.c.
extern "C" bool verifyRollbackLater() { return true; }

namespace updater {
namespace {

const char* kLatestReleaseUrl = "https://api.github.com/repos/sq9fk/rotator_wifi_bridge/releases/latest";
const char* kFirmwareAsset = "firmware.bin";
const char* kPanelAsset = "littlefs.bin";

// WiFi and NTP need a moment after boot, and TLS certificate validation needs
// the real date (net::timeSynced()).
const uint32_t kFirstCheckDelayMs = 60UL * 1000;
const uint32_t kCheckIntervalMs = 12UL * 60 * 60 * 1000;
const uint32_t kRetryIntervalMs = 60UL * 60 * 1000;
// GitHub allows 60 unauthenticated API calls an hour per public IP, shared by
// everything behind the same NAT - a button held down must not spend them.
const uint32_t kManualCheckGapMs = 10UL * 1000;
// How long a new image has to run before it is marked valid. A crash, a
// watchdog reset or a power cut before that boots the previous image instead.
// Short on purpose: long enough to catch a boot loop or a hang in setup(),
// short enough that an unlucky power cut rarely undoes a good update.
const uint32_t kMarkValidAfterMs = 20UL * 1000;
// Long enough for the "rebooting" state to reach every open panel first.
const uint32_t kRebootDelayMs = 1500;
const uint32_t kDownloadStallMs = 20UL * 1000;
const uint32_t kHttpTimeoutMs = 15UL * 1000;
const uint32_t kUploadStaleMs = 60UL * 1000;
// TLS plus HTTPClient plus a filtered JSON parse; measured headroom is in DESIGN.md.
const uint32_t kTaskStack = 12 * 1024;
const size_t kChunk = 4096;

enum class Phase : uint8_t { Idle, Checking, Panel, Firmware, Writing, Rebooting, Failed };

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Checking: return "checking";
    case Phase::Panel: return "panel";
    case Phase::Firmware: return "firmware";
    case Phase::Writing: return "writing";
    case Phase::Rebooting: return "rebooting";
    case Phase::Failed: return "failed";
    default: return "idle";
  }
}

bool isInstallPhase(Phase p) {
  return p == Phase::Panel || p == Phase::Firmware || p == Phase::Writing || p == Phase::Rebooting;
}

struct Asset {
  String url;
  uint32_t size = 0;
  char sha256[65] = "";  // lowercase hex from GitHub's "sha256:..." digest; empty if not published
};

struct Release {
  bool known = false;
  char tag[32] = "";
  char name[64] = "";
  char publishedAt[24] = "";
  String htmlUrl;
  String notes;
  Asset firmware;
  Asset panel;  // url empty: the release ships firmware only
};

// Everything the task and loop()/web handlers both touch, behind one mutex.
// The task never holds it across I/O - only for the copies in and out.
struct Shared {
  Phase phase = Phase::Idle;
  Release release;
  bool noReleases = false;  // the repo has no published release yet (HTTP 404)
  bool checked = false;
  uint32_t checkedAt = 0;   // millis() of the last successful check
  String error;             // last failure, cleared when the next action starts
  uint32_t done = 0;        // progress within the current install phase
  uint32_t total = 0;
  uint32_t rebootAt = 0;
  char startedBy[Config::kStrLen] = "";
};

Shared shared;
SemaphoreHandle_t mutex = nullptr;

class Lock {
 public:
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};

Rotator* rotator = nullptr;
std::atomic<bool> taskRunning{false};
std::atomic<bool> installRequested{false};

// Scheduling of checks. Atomic: written by loop(), the web handlers (an
// AsyncTCP task) and the check task itself.
std::atomic<bool> attempted{false};
std::atomic<uint32_t> lastAttemptAt{0};
std::atomic<bool> lastAttemptFailed{false};
std::atomic<uint32_t> lastManualCheckAt{0};
std::atomic<bool> manualChecked{false};

bool markedValid = false;
bool rolledBack = false;
bool currentIsDev = true;
semver::Version current;

// Who holds the Update object (see Updater.h).
enum class FlashOwner : uint8_t { None, Upload, Updater };
std::atomic<uint8_t> flashOwner{static_cast<uint8_t>(FlashOwner::None)};
std::atomic<uint32_t> lastUploadActivity{0};

bool claimFlash(FlashOwner who) {
  uint8_t expected = static_cast<uint8_t>(FlashOwner::None);
  if (flashOwner.compare_exchange_strong(expected, static_cast<uint8_t>(who))) {
    return true;
  }
  // An upload whose client vanished never reaches the completion handler that
  // would have released it - reclaim it once it has been silent long enough.
  if (expected == static_cast<uint8_t>(FlashOwner::Upload) &&
      millis() - lastUploadActivity.load() > kUploadStaleMs &&
      flashOwner.compare_exchange_strong(expected, static_cast<uint8_t>(who))) {
    Update.abort();
    return true;
  }
  return false;
}

void releaseFlash(FlashOwner who) {
  uint8_t expected = static_cast<uint8_t>(who);
  flashOwner.compare_exchange_strong(expected, static_cast<uint8_t>(FlashOwner::None));
}

const char* networkProblem() {
  if (net::mode() != net::Mode::Station) {
    return "no internet connection (the bridge is not on a WiFi network)";
  }
  if (!net::timeSynced()) {
    return "clock not set yet (NTP) - certificates cannot be checked without the date";
  }
  return nullptr;
}

// --- HTTP ---------------------------------------------------------------------

void prepare(HTTPClient& http) {
  // HTTP/1.0: no chunked transfer encoding, so the body can be read straight
  // off the stream (getStream() hands back the raw socket, chunk headers and all).
  http.useHTTP10(true);
  // Release downloads redirect from github.com to a signed URL on GitHub's
  // asset host - a different host, so "strict" (same-host) is not enough.
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setUserAgent(String("rotator_wifi_bridge/") + kFirmwareVersion);  // GitHub rejects requests without one
}

String httpError(int code) {
  if (code < 0) {
    return String("connection failed: ") + HTTPClient::errorToString(code);
  }
  if (code == 403 || code == 429) {
    return "GitHub API rate limit reached - try again in an hour";
  }
  return String("HTTP ") + code;
}

void copyString(char* dest, size_t len, const char* src) {
  strncpy(dest, src != nullptr ? src : "", len - 1);
  dest[len - 1] = '\0';
}

void readAsset(JsonObjectConst a, Asset& out) {
  out.url = a["browser_download_url"] | "";
  out.size = a["size"] | 0;
  const char* digest = a["digest"] | "";
  if (strncmp(digest, "sha256:", 7) == 0 && strlen(digest + 7) == 64) {
    copyString(out.sha256, sizeof(out.sha256), digest + 7);
    for (char* c = out.sha256; *c; c++) {
      *c = tolower(static_cast<unsigned char>(*c));
    }
  }
}

// GET /repos/.../releases/latest. "latest" already skips drafts and
// prereleases, so tagging an -rc and marking it a prerelease keeps it off
// every bridge in the field.
bool fetchLatest(Release& rel, bool& none, String& error) {
  WiFiClientSecure client;
  client.setCACertBundle(kCaBundle);
  HTTPClient http;
  prepare(http);
  if (!http.begin(client, kLatestReleaseUrl)) {
    error = "bad release URL";
    return false;
  }
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  const int code = http.GET();
  if (code == 404) {
    none = true;
    http.end();
    return true;
  }
  if (code != 200) {
    error = httpError(code);
    http.end();
    return false;
  }

  // Only what is used - the full response carries uploader/author objects
  // for every asset and is several times larger.
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["name"] = true;
  filter["body"] = true;
  filter["html_url"] = true;
  filter["published_at"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["size"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  filter["assets"][0]["digest"] = true;

  JsonDocument doc;
  const DeserializationError parseError =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (parseError) {
    error = String("unreadable response from GitHub: ") + parseError.c_str();
    return false;
  }

  copyString(rel.tag, sizeof(rel.tag), doc["tag_name"] | "");
  copyString(rel.name, sizeof(rel.name), doc["name"] | "");
  copyString(rel.publishedAt, sizeof(rel.publishedAt), doc["published_at"] | "");
  rel.htmlUrl = doc["html_url"] | "";
  rel.notes = doc["body"] | "";
  if (rel.notes.length() > 2000) {
    rel.notes = rel.notes.substring(0, 2000) + "\n…";
  }
  for (JsonObjectConst a : doc["assets"].as<JsonArrayConst>()) {
    const char* name = a["name"] | "";
    if (strcmp(name, kFirmwareAsset) == 0) {
      readAsset(a, rel.firmware);
    } else if (strcmp(name, kPanelAsset) == 0) {
      readAsset(a, rel.panel);
    }
  }
  if (rel.tag[0] == '\0') {
    error = "release has no tag";
    return false;
  }
  if (rel.firmware.url.length() == 0 || rel.firmware.size == 0) {
    error = String("release ") + rel.tag + " has no " + kFirmwareAsset;
    return false;
  }
  rel.known = true;
  return true;
}

void setProgress(uint32_t done) {
  Lock lock;
  shared.done = done;
}

void startPhase(Phase phase, uint32_t total) {
  Lock lock;
  shared.phase = phase;
  shared.done = 0;
  shared.total = total;
}

void sha256Start(mbedtls_sha256_context& ctx) {
  mbedtls_sha256_init(&ctx);
#if MBEDTLS_VERSION_MAJOR >= 3
  mbedtls_sha256_starts(&ctx, 0);
#else
  mbedtls_sha256_starts_ret(&ctx, 0);
#endif
}

void sha256Update(mbedtls_sha256_context& ctx, const uint8_t* data, size_t len) {
#if MBEDTLS_VERSION_MAJOR >= 3
  mbedtls_sha256_update(&ctx, data, len);
#else
  mbedtls_sha256_update_ret(&ctx, data, len);
#endif
}

void sha256Hex(mbedtls_sha256_context& ctx, char out[65]) {
  uint8_t digest[32];
#if MBEDTLS_VERSION_MAJOR >= 3
  mbedtls_sha256_finish(&ctx, digest);
#else
  mbedtls_sha256_finish_ret(&ctx, digest);
#endif
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) {
    snprintf(out + i * 2, 3, "%02x", digest[i]);
  }
}

// Where downloaded bytes go: PSRAM for the panel image, the OTA slot for the
// firmware. Returns false (with error set) to abort the download.
typedef bool (*Sink)(const uint8_t* data, size_t len, void* ctx, String& error);

bool download(const Asset& asset, Sink sink, void* ctx, String& error) {
  WiFiClientSecure client;
  client.setCACertBundle(kCaBundle);
  HTTPClient http;
  prepare(http);
  if (!http.begin(client, asset.url)) {
    error = "bad download URL";
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    error = httpError(code);
    http.end();
    return false;
  }
  const int length = http.getSize();
  if (length >= 0 && static_cast<uint32_t>(length) != asset.size) {
    error = String("size mismatch: ") + length + " bytes, release says " + asset.size;
    http.end();
    return false;
  }

  uint8_t* buf = static_cast<uint8_t*>(malloc(kChunk));
  if (buf == nullptr) {
    error = "out of memory";
    http.end();
    return false;
  }
  mbedtls_sha256_context sha;
  sha256Start(sha);

  WiFiClient* stream = http.getStreamPtr();
  uint32_t received = 0;
  uint32_t lastData = millis();
  while (received < asset.size) {
    const size_t available = stream->available();
    if (available == 0) {
      if (!stream->connected()) {
        break;
      }
      if (millis() - lastData > kDownloadStallMs) {
        error = "download stalled";
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    size_t want = available < kChunk ? available : kChunk;
    if (want > asset.size - received) {
      want = asset.size - received;
    }
    const int n = stream->read(buf, want);
    if (n <= 0) {
      vTaskDelay(1);
      continue;
    }
    sha256Update(sha, buf, n);
    if (!sink(buf, n, ctx, error)) {
      break;
    }
    received += n;
    lastData = millis();
    setProgress(received);
  }
  http.end();
  free(buf);

  char computed[65];
  sha256Hex(sha, computed);
  if (error.length() > 0) {
    return false;
  }
  if (received != asset.size) {
    error = String("download incomplete: ") + received + " of " + asset.size + " bytes";
    return false;
  }
  // Releases published before GitHub started computing digests have none;
  // the firmware image still carries its own SHA-256 that esp_ota checks.
  if (asset.sha256[0] != '\0' && strcmp(computed, asset.sha256) != 0) {
    error = "SHA-256 does not match the release";
    return false;
  }
  return true;
}

struct PanelBuffer {
  uint8_t* data;
  size_t used;
  size_t capacity;
};

bool panelSink(const uint8_t* data, size_t len, void* ctx, String& error) {
  PanelBuffer* b = static_cast<PanelBuffer*>(ctx);
  if (b->used + len > b->capacity) {
    error = "panel image larger than announced";
    return false;
  }
  memcpy(b->data + b->used, data, len);
  b->used += len;
  return true;
}

bool firmwareSink(const uint8_t* data, size_t len, void* ctx, String& error) {
  (void)ctx;
  if (Update.write(const_cast<uint8_t*>(data), len) != len) {
    error = String("flash write failed: ") + Update.errorString();
    return false;
  }
  return true;
}

// --- tasks --------------------------------------------------------------------

void checkTask(void*) {
  Release rel;
  bool none = false;
  String error;
  const bool ok = fetchLatest(rel, none, error);
  {
    Lock lock;
    if (ok) {
      shared.release = none ? Release() : rel;
      shared.noReleases = none;
      shared.checked = true;
      shared.checkedAt = millis();
    }
    shared.error = error;
    shared.phase = Phase::Idle;
  }
  lastAttemptFailed = !ok;
  Serial.printf("update check: %s\n", ok ? (none ? "no releases" : rel.tag) : error.c_str());
  taskRunning = false;
  vTaskDelete(nullptr);
}

void installTask(void*) {
  Release rel;
  {
    Lock lock;
    rel = shared.release;
  }
  String error;
  uint8_t* panelImage = nullptr;
  size_t panelSize = 0;
  bool firmwareCommitted = false;
  const esp_partition_t* running = esp_ota_get_running_partition();

  do {
    // 1. The panel image into PSRAM first. Nothing has been touched yet, so a
    //    failed or interrupted download here costs nothing.
    if (rel.panel.url.length() > 0) {
      const esp_partition_t* fs =
          esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
      // Exactly the partition's size: mklittlefs builds an image of the whole
      // partition and LittleFS records that block count in its superblock, so
      // an image from a different partitions.csv would write fine and then
      // fail to mount - leaving no panel at all. That needs a USB flash.
      if (fs == nullptr || rel.panel.size != fs->size) {
        error = String("panel image (") + rel.panel.size + " B) was built for a different partition layout than this " +
                "bridge's (" + (fs != nullptr ? fs->size : 0) + " B) - flash this version over USB once";
        break;
      }
      panelImage = static_cast<uint8_t*>(ps_malloc(rel.panel.size));
      if (panelImage == nullptr) {
        error = "not enough PSRAM to stage the panel image";
        break;
      }
      startPhase(Phase::Panel, rel.panel.size);
      PanelBuffer buffer{panelImage, 0, rel.panel.size};
      if (!download(rel.panel, panelSink, &buffer, error)) {
        break;
      }
      panelSize = buffer.used;
    }

    // 2. The firmware, straight into the inactive slot. Update.end() checks
    //    the image (esp_ota's own SHA-256 and layout) before marking it bootable.
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next == nullptr || rel.firmware.size > next->size) {
      error = "firmware does not fit the app partition";
      break;
    }
    startPhase(Phase::Firmware, rel.firmware.size);
    if (!Update.begin(rel.firmware.size, U_FLASH)) {
      error = String("cannot start firmware update: ") + Update.errorString();
      break;
    }
    if (!download(rel.firmware, firmwareSink, nullptr, error)) {
      Update.abort();
      break;
    }
    if (!Update.end()) {
      error = String("firmware rejected: ") + Update.errorString();
      break;
    }
    firmwareCommitted = true;

    // 3. The panel image from PSRAM - local flash writes only, no network left
    //    to fail.
    if (panelImage != nullptr) {
      startPhase(Phase::Writing, panelSize);
      if (!Update.begin(panelSize, U_SPIFFS)) {
        error = String("cannot start panel update: ") + Update.errorString();
        break;
      }
      for (size_t offset = 0; offset < panelSize; offset += kChunk) {
        const size_t n = (panelSize - offset < kChunk) ? panelSize - offset : kChunk;
        if (Update.write(panelImage + offset, n) != n) {
          error = String("panel write failed: ") + Update.errorString();
          break;
        }
        setProgress(offset + n);
        vTaskDelay(1);
      }
      if (error.length() > 0) {
        Update.abort();
        break;
      }
      if (!Update.end()) {
        error = String("panel write failed: ") + Update.errorString();
        break;
      }
    }
  } while (false);

  free(panelImage);
  if (error.length() > 0 && firmwareCommitted) {
    // The new firmware is already set to boot, but its panel did not make it -
    // stay on the running firmware rather than boot a mismatched pair. The
    // filesystem may be half-written by now (a local flash failure, not a
    // network one - the image was already in PSRAM), so say how to recover.
    esp_ota_set_boot_partition(running);
    error += " - the panel may be damaged; retry, or run \"update install\" on the USB console";
  }
  {
    Lock lock;
    if (error.length() > 0) {
      shared.phase = Phase::Failed;
      shared.error = error;
    } else {
      shared.phase = Phase::Rebooting;
      shared.rebootAt = millis();
    }
  }
  Serial.printf("update install %s: %s\n", rel.tag, error.length() > 0 ? error.c_str() : "ok, rebooting");
  releaseFlash(FlashOwner::Updater);
  taskRunning = false;
  vTaskDelete(nullptr);
}

// Core 0, alongside the WiFi stack: loop() runs on core 1 and keeps its
// timing. Priority 1, below lwIP/WiFi, so the network it depends on is never
// starved by it.
bool startTask(TaskFunction_t fn, const char* name) {
  bool expected = false;
  if (!taskRunning.compare_exchange_strong(expected, true)) {
    return false;
  }
  if (xTaskCreatePinnedToCore(fn, name, kTaskStack, nullptr, 1, nullptr, 0) != pdPASS) {
    taskRunning = false;
    return false;
  }
  return true;
}

bool startCheck() {
  Phase previous;
  {
    Lock lock;
    previous = shared.phase;
    shared.phase = Phase::Checking;
    shared.error = "";
  }
  if (!startTask(checkTask, "updcheck")) {
    Lock lock;
    shared.phase = previous;
    return false;
  }
  attempted = true;
  lastAttemptAt = millis();
  return true;
}

void scheduleChecks() {
  if (!config.updateAutoCheck || taskRunning || installRequested || networkProblem() != nullptr ||
      millis() < kFirstCheckDelayMs) {
    return;
  }
  {
    Lock lock;
    if (isInstallPhase(shared.phase)) {
      return;
    }
  }
  const uint32_t interval = lastAttemptFailed ? kRetryIntervalMs : kCheckIntervalMs;
  if (!attempted || millis() - lastAttemptAt >= interval) {
    startCheck();
  }
}

const char* relation(const Release& rel) {
  if (!current.valid) {
    return "unknown";
  }
  const int c = semver::compare(semver::parse(rel.tag), current);
  return c > 0 ? "newer" : (c == 0 ? "same" : "older");
}

}  // namespace

void begin(Rotator& r) {
  rotator = &r;
  mutex = xSemaphoreCreateMutex();
  current = semver::parse(kFirmwareVersion);
  currentIsDev = semver::isDevBuild(current);
  // A slot the bootloader gave up on - the last update did not survive its
  // first boot. Reported until a later update overwrites that slot.
  rolledBack = esp_ota_get_last_invalid_partition() != nullptr;
  esp_ota_img_states_t state;
  const bool pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                       state == ESP_OTA_IMG_PENDING_VERIFY;
  Serial.printf("firmware %s%s%s\n", kFirmwareVersion, pending ? " (new, pending verification)" : "",
                rolledBack ? " (previous update rolled back)" : "");
}

void poll() {
  if (!markedValid && millis() > kMarkValidAfterMs) {
    // A no-op unless this image is the freshly installed one awaiting it.
    esp_ota_mark_app_valid_cancel_rollback();
    markedValid = true;
  }

  // Started here rather than in the web handler: stopping and locking the
  // rotator belongs on loop()'s thread, like every other Rotator call.
  if (installRequested) {
    installRequested = false;
    rotator->stop(RotatorLink::Source::Web);
    rotator->setMotionLocked(true);
    {
      Lock lock;
      shared.phase = Phase::Panel;
      shared.done = 0;
      shared.total = 0;
    }
    if (!startTask(installTask, "updinstall")) {
      releaseFlash(FlashOwner::Updater);
      Lock lock;
      shared.phase = Phase::Failed;
      shared.error = "could not start the update task";
    }
  }

  Phase phase;
  uint32_t rebootAt;
  {
    Lock lock;
    phase = shared.phase;
    rebootAt = shared.rebootAt;
  }
  if (phase == Phase::Rebooting && millis() - rebootAt > kRebootDelayMs) {
    ESP.restart();
  }
  if (phase == Phase::Failed && !taskRunning && rotator->motionLocked()) {
    rotator->setMotionLocked(false);
  }

  scheduleChecks();
}

const char* requestCheck() {
  if (taskRunning || installRequested) {
    return "busy - a check or an update is already running";
  }
  if (const char* problem = networkProblem()) {
    return problem;
  }
  if (manualChecked && millis() - lastManualCheckAt < kManualCheckGapMs) {
    return "checked a moment ago";
  }
  if (!startCheck()) {
    return "busy - a check or an update is already running";
  }
  manualChecked = true;
  lastManualCheckAt = millis();
  return nullptr;
}

const char* requestInstall(const char* user, const char* expectedTag) {
  if (taskRunning || installRequested) {
    return "busy - a check or an update is already running";
  }
  {
    Lock lock;
    if (!shared.release.known) {
      return "no release known - check for updates first";
    }
    if (expectedTag != nullptr && expectedTag[0] != '\0' && strcmp(expectedTag, shared.release.tag) != 0) {
      return "the latest release has changed - review it again";
    }
  }
  if (const char* problem = networkProblem()) {
    return problem;
  }
  if (!claimFlash(FlashOwner::Updater)) {
    return "a manual firmware upload is in progress";
  }
  {
    Lock lock;
    copyString(shared.startedBy, sizeof(shared.startedBy), user);
    shared.error = "";
  }
  installRequested = true;
  return nullptr;
}

void fillStatus(JsonObject out) {
  Lock lock;
  out["current"] = kFirmwareVersion;
  out["dev"] = currentIsDev;
  out["phase"] = phaseName(shared.phase);
  if (shared.release.known) {
    const char* rel = relation(shared.release);
    out["latest"] = shared.release.tag;
    out["relation"] = rel;
    out["available"] = strcmp(rel, "newer") == 0 || strcmp(rel, "unknown") == 0;
  }
  if (shared.noReleases) {
    out["noReleases"] = true;
  }
  if (shared.checked) {
    out["checkedAgoS"] = (millis() - shared.checkedAt) / 1000;
  }
  if (shared.error.length() > 0) {
    out["error"] = shared.error;
  }
  if (isInstallPhase(shared.phase) || shared.phase == Phase::Failed) {
    out["done"] = shared.done;
    out["total"] = shared.total;
    out["by"] = shared.startedBy;
  }
  if (rolledBack) {
    out["rolledBack"] = true;
  }
}

void fillDetails(JsonObject out) {
  fillStatus(out);
  Lock lock;
  out["autoCheck"] = config.updateAutoCheck;
  if (shared.release.known) {
    out["name"] = shared.release.name;
    out["notes"] = shared.release.notes;
    out["url"] = shared.release.htmlUrl;
    out["publishedAt"] = shared.release.publishedAt;
    out["firmwareSize"] = shared.release.firmware.size;
    out["panelSize"] = shared.release.panel.size;
  }
}

bool claimFlashForUpload() {
  lastUploadActivity = millis();
  return claimFlash(FlashOwner::Upload);
}

void touchFlashUpload() { lastUploadActivity = millis(); }

void releaseFlashUpload() { releaseFlash(FlashOwner::Upload); }

}  // namespace updater
