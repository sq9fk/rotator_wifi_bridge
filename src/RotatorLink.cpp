#include "RotatorLink.h"

#include <string.h>

namespace {

// The controller can stall its own loop() for a dozen milliseconds while it
// saves settings to EEPROM, so a tight timeout would produce phantom retries -
// and a retried motion command is a second rotation, not a harmless repeat.
const uint32_t kReplyTimeoutMs = 300;

// How long to wait for a possible "?>" after a command that says nothing on
// success. Long enough for the controller to parse and answer, short enough
// not to throttle the poller.
const uint32_t kErrorGraceMs = 80;

// The controller silently discards rotation and configuration commands for
// this long after boot (ROTATIONAL_AND_CONFIGURATION_CMD_IGNORE_TIME_MS).
// A little margin on top, since we detect the restart after the fact.
const uint32_t kBootLockoutMs = 5500;

}  // namespace

RotatorLink::RotatorLink(Stream& port, const gs232::AzimuthRange& range)
    : port_(port), range_(range) {
  memset(&active_, 0, sizeof(active_));
  replyBuf_[0] = '\0';
}

void RotatorLink::begin() {
  // Assume the controller may have just been powered up alongside us.
  lockoutUntil_ = millis() + kBootLockoutMs;
}

void RotatorLink::setReplyHandler(ReplyHandler handler, void* ctx) {
  handler_ = handler;
  handlerCtx_ = ctx;
}

bool RotatorLink::inBootLockout() const {
  return static_cast<int32_t>(millis() - lockoutUntil_) < 0;
}

bool RotatorLink::isMotionCommand(const char* command) const {
  switch (toupper(static_cast<unsigned char>(command[0]))) {
    case 'M':
    case 'L':
    case 'R':
      return true;
    default:
      return false;
  }
}

uint32_t RotatorLink::submit(const char* command, Source source) {
  if (command == nullptr || command[0] == '\0') {
    return 0;
  }
  if (queueCount_ >= kQueueLen) {
    return 0;
  }

  Request req;
  memset(&req, 0, sizeof(req));
  strncpy(req.command, command, kMaxCommandLen - 1);
  req.id = nextId_++;
  req.source = source;
  req.kind = gs232::classify(req.command);

  // A stop must not queue behind a position poll.
  const char first = toupper(static_cast<unsigned char>(req.command[0]));
  const bool urgent = (first == 'S' || first == 'A');

  if (urgent) {
    queueHead_ = (queueHead_ + kQueueLen - 1) % kQueueLen;
    queue_[queueHead_] = req;
  } else {
    queue_[(queueHead_ + queueCount_) % kQueueLen] = req;
  }
  queueCount_++;

  return req.id;
}

void RotatorLink::poll() {
  readIncoming();

  switch (state_) {
    case State::Idle:
      startNext();
      break;

    case State::AwaitingReply:
      if (millis() - stateSince_ > kReplyTimeoutMs) {
        finish(Result::Timeout, nullptr);
      }
      break;

    case State::Grace:
      if (millis() - stateSince_ > kErrorGraceMs) {
        // Nothing came back, which for this command class means success.
        finish(Result::NoReply, nullptr);
      }
      break;
  }
}

void RotatorLink::startNext() {
  if (queueCount_ == 0) {
    return;
  }

  active_ = queue_[queueHead_];
  queueHead_ = (queueHead_ + 1) % kQueueLen;
  queueCount_--;

  if (inBootLockout() && isMotionCommand(active_.command)) {
    // Sending it would be worse than refusing: the controller drops it without
    // a word, and the caller would wait for a reply that cannot arrive.
    finish(Result::Rejected, nullptr);
    return;
  }

  replyLen_ = 0;
  replyBuf_[0] = '\0';

  port_.print(active_.command);
  port_.print('\r');

  stateSince_ = millis();
  state_ = (active_.kind == gs232::ReplyKind::Immediate) ? State::AwaitingReply : State::Grace;

  if (active_.kind == gs232::ReplyKind::None) {
    finish(Result::NoReply, nullptr);
  }
}

void RotatorLink::readIncoming() {
  while (port_.available() > 0) {
    const int c = port_.read();
    if (c < 0) {
      break;
    }

    if (c == '\r' || c == '\n') {
      if (replyLen_ == 0) {
        continue;  // controller terminates with CR LF; ignore the empty half
      }
      replyBuf_[replyLen_] = '\0';

      if (state_ == State::Idle) {
        // Unsolicited - the controller emits "AZ Rotation Stall Detected" on
        // its own. Surface it without attributing it to a transaction.
        if (handler_ != nullptr) {
          handler_(0, Source::Poller, Result::Reply, replyBuf_, handlerCtx_);
        }
      } else {
        finish(gs232::isErrorReply(replyBuf_) ? Result::Rejected : Result::Reply, replyBuf_);
      }

      replyLen_ = 0;
      continue;
    }

    if (replyLen_ < kMaxReplyLen - 1) {
      replyBuf_[replyLen_++] = static_cast<char>(c);
    }
    // Overlong lines are truncated rather than dropped; the parser will reject
    // them and the transaction will be reported as rejected.
  }
}

void RotatorLink::finish(Result result, const char* reply) {
  state_ = State::Idle;
  if (handler_ != nullptr) {
    handler_(active_.id, active_.source, result, reply, handlerCtx_);
  }
}
