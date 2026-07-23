// The rotator as the rest of the firmware sees it: a position, a target, and
// three verbs. Every control source - REST, rotctld, raw, the console - goes
// through this object, which owns the RotatorLink queue underneath.
//
// Position is cached in raw coordinates. The controller's I command reports
// which turn the rotator is on; a real azimuth of 0..359 cannot express that.

#pragma once

#include <Arduino.h>

#include "Gs232.h"
#include "RotatorLink.h"

class Rotator {
 public:
  Rotator(Stream& port, const gs232::AzimuthRange& range);

  void begin();
  void poll();

  // Rejects and returns false when the position is stale, the azimuth is
  // unreachable, or the controller is still in its post-boot lockout.
  bool gotoAzimuth(float realAz, RotatorLink::Source source);
  bool jog(bool clockwise, RotatorLink::Source source);
  bool stop(RotatorLink::Source source);

  // Declares the rotator's true raw position (controller I command).
  bool syncRaw(int raw, RotatorLink::Source source);

  // Forwards a command line verbatim through the same queue, for the raw
  // passthrough socket. The reply is delivered to the handler with the id
  // returned here, so a client always gets the answer to its own command -
  // which is the whole reason the raw socket is framed rather than a byte pipe.
  typedef void (*RawHandler)(uint32_t id, RotatorLink::Result result, const char* reply, void* ctx);
  void setRawHandler(RawHandler handler, void* ctx);
  uint32_t submitRaw(const char* command);

  bool positionIsFresh() const;
  float rawAzimuth() const { return rawAz_; }
  float realAzimuth() const { return gs232::rawToReal(rawAz_); }
  bool inOverlap() const { return rawAz_ >= 360.0f; }
  uint32_t positionAgeMs() const { return millis() - updatedAt_; }

  bool inBootLockout() const { return link_.inBootLockout(); }
  RotatorLink::Source lastMotionSource() const { return lastMotionSource_; }
  uint32_t lastMotionAgeMs() const { return millis() - lastMotionAt_; }
  bool hasMoved() const { return lastMotionAt_ != 0; }

  // Last line from the controller that was not a position reply - stall
  // reports and command acknowledgements. Empty once read by the UI.
  const char* lastNotice() const { return notice_; }
  void clearNotice() { notice_[0] = '\0'; }

  const gs232::AzimuthRange& range() const { return range_; }

 private:
  static void replyTrampoline(uint32_t id, RotatorLink::Source source, RotatorLink::Result result, const char* reply,
                              void* ctx);
  void onReply(RotatorLink::Source source, RotatorLink::Result result, const char* reply);
  void noteMotion(RotatorLink::Source source);

  gs232::AzimuthRange range_;
  RotatorLink link_;

  float rawAz_ = 0.0f;
  uint32_t updatedAt_ = 0;
  bool valid_ = false;

  RotatorLink::Source lastMotionSource_ = RotatorLink::Source::Poller;
  uint32_t lastMotionAt_ = 0;

  uint32_t lastPoll_ = 0;
  char notice_[24] = "";

  RawHandler rawHandler_ = nullptr;
  void* rawHandlerCtx_ = nullptr;
};
