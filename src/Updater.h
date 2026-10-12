// Over-the-network updates from this project's GitHub Releases.
//
// The bridge asks api.github.com for the latest release (automatically,
// shortly after boot and every 12 h, or when an operator presses "Sprawdź
// teraz"), and the panel tells whoever is logged in that a newer version is
// available. Nothing is installed until an operator accepts it; then, in one
// go: the panel image (littlefs.bin) is downloaded into PSRAM, the firmware
// (firmware.bin) is streamed into the inactive OTA slot, the panel image is
// written to the filesystem partition, and the bridge reboots.
//
// See DESIGN.md "Updates from GitHub Releases" for the reasoning. The parts
// that matter most:
//
//  - Network and flash work run in a FreeRTOS task on core 0, never in
//    loop(): a TLS handshake alone is long enough to delay the jog dead-man
//    timer, and a download takes tens of seconds. loop() keeps running
//    throughout; the task never touches the rotator, only shared state below.
//  - TLS is verified against Mozilla's root CAs (certs/x509_crt_bundle.bin),
//    and each image's SHA-256 against the digest GitHub publishes for it.
//  - The rotator is stopped and every source's motion commands are refused
//    (RotatorLink::setMotionLocked) for the duration, since a reboot ends it.
//  - The firmware slot is only committed once fully downloaded and verified;
//    the panel image is downloaded *before* that and written *after*, so a
//    dropped connection at any point leaves the running version untouched.
//  - A new firmware that crashes or hangs before it has run for 20 s is
//    rolled back to the previous one by the bootloader (verifyRollbackLater).

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "Rotator.h"

namespace updater {

void begin(Rotator& rotator);
void poll();

// Each returns nullptr when the action was started, otherwise why not.
const char* requestCheck();
// expectedTag, when given, must still be the latest known release - so an
// operator never installs a different version from the one they were shown.
const char* requestInstall(const char* user, const char* expectedTag);

// Compact state for the status broadcast every connected panel receives.
void fillStatus(JsonObject out);
// The same plus the release notes, for GET /api/update.
void fillDetails(JsonObject out);

// The Arduino Update object is a singleton shared with the panel's manual
// .bin upload (WebApi.cpp). The upload claims it for the length of a request;
// an abandoned upload's claim goes stale after a minute of silence.
bool claimFlashForUpload();
void touchFlashUpload();
void releaseFlashUpload();

}  // namespace updater
