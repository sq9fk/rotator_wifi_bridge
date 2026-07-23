#include "Rotator.h"

#include <string.h>

namespace {
// Fast enough for a responsive dial, slow enough to leave the line free for
// commands from the control sources.
const uint32_t kPollIntervalMs = 300;

// Beyond this the cached position is stale and must not be presented as live.
const uint32_t kPositionStaleMs = 2000;
}  // namespace

Rotator::Rotator(Stream& port, const gs232::AzimuthRange& range) : range_(range), link_(port, range) {}

void Rotator::begin() {
  link_.setReplyHandler(replyTrampoline, this);
  link_.begin();
}

void Rotator::poll() {
  link_.poll();

  if (millis() - lastPoll_ >= kPollIntervalMs) {
    lastPoll_ = millis();
    // I, not C: one command yields the raw position, and the real azimuth
    // follows from it locally.
    link_.submit("I", RotatorLink::Source::Poller);
  }
}

bool Rotator::positionIsFresh() const {
  return valid_ && (millis() - updatedAt_) < kPositionStaleMs;
}

void Rotator::setRawHandler(RawHandler handler, void* ctx) {
  rawHandler_ = handler;
  rawHandlerCtx_ = ctx;
}

uint32_t Rotator::submitRaw(const char* command) {
  const uint32_t id = link_.submit(command, RotatorLink::Source::Raw);

  // A raw client moving the rotator must show up as the last motion source,
  // otherwise the panel would attribute the movement to whoever moved it last
  // through the API - exactly the wrong answer to "why is it turning".
  if (id != 0 && command != nullptr) {
    switch (toupper(static_cast<unsigned char>(command[0]))) {
      case 'M':
      case 'L':
      case 'R':
        noteMotion(RotatorLink::Source::Raw);
        break;
      default:
        break;
    }
  }

  return id;
}

void Rotator::replyTrampoline(uint32_t id, RotatorLink::Source source, RotatorLink::Result result, const char* reply,
                              void* ctx) {
  Rotator* self = static_cast<Rotator*>(ctx);

  if (source == RotatorLink::Source::Raw && self->rawHandler_ != nullptr) {
    self->rawHandler_(id, result, reply, self->rawHandlerCtx_);
  }

  self->onReply(source, result, reply);
}

void Rotator::onReply(RotatorLink::Source source, RotatorLink::Result result, const char* reply) {
  // A raw client's own position query still refreshes the shared cache: the
  // reply is a fact about the rotator regardless of who asked for it.
  if (source == RotatorLink::Source::Raw && result == RotatorLink::Result::Reply) {
    float raw = 0.0f;
    if (gs232::parseRawReply(reply, raw)) {
      rawAz_ = raw;
      updatedAt_ = millis();
      valid_ = true;
      return;
    }
  }

  switch (result) {
    case RotatorLink::Result::Reply: {
      float raw = 0.0f;
      if (gs232::parseRawReply(reply, raw)) {
        rawAz_ = raw;
        updatedAt_ = millis();
        valid_ = true;
      } else {
        strncpy(notice_, reply, sizeof(notice_) - 1);
        notice_[sizeof(notice_) - 1] = '\0';
      }
      break;
    }

    case RotatorLink::Result::Timeout:
      // The controller went quiet; the cached position is no longer evidence
      // of anything.
      valid_ = false;
      break;

    case RotatorLink::Result::Rejected:
    case RotatorLink::Result::NoReply:
      break;
  }
}

void Rotator::noteMotion(RotatorLink::Source source) {
  lastMotionSource_ = source;
  lastMotionAt_ = millis();
}

bool Rotator::gotoAzimuth(float realAz, RotatorLink::Source source) {
  if (!positionIsFresh() || link_.inBootLockout()) {
    return false;
  }

  const int target = gs232::chooseRawTarget(realAz, rawAz_, range_);
  if (target < 0) {
    return false;
  }

  char command[8];
  if (gs232::buildGoto(command, sizeof(command), target) == 0) {
    return false;
  }
  if (link_.submit(command, source) == 0) {
    return false;
  }

  noteMotion(source);
  return true;
}

bool Rotator::jog(bool clockwise, RotatorLink::Source source) {
  if (link_.inBootLockout()) {
    return false;
  }
  if (link_.submit(clockwise ? "R" : "L", source) == 0) {
    return false;
  }
  noteMotion(source);
  return true;
}

bool Rotator::stop(RotatorLink::Source source) {
  // Deliberately not gated on the boot lockout: refusing a stop is never the
  // safer choice, and the controller ignores it harmlessly if it is not moving.
  return link_.submit("S", source) != 0;
}

bool Rotator::syncRaw(int raw, RotatorLink::Source source) {
  if (raw < range_.rawMin || raw > range_.rawMax) {
    return false;
  }
  char command[8];
  if (snprintf(command, sizeof(command), "I%03d", raw) < 4) {
    return false;
  }
  return link_.submit(command, source) != 0;
}
