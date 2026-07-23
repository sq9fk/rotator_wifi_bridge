// Sole owner of the serial line to the rotator controller.
//
// Every command from every source - the web panel, rotctld, the raw
// passthrough socket and the position poller - goes through this queue. That
// is what keeps replies attributable: GS-232 has no transaction ids, so two
// overlapping "C" commands would produce two indistinguishable "AZ=" replies.
//
// Non-blocking throughout: poll() advances a state machine and returns.

#pragma once

#include <Arduino.h>

#include "Gs232.h"

class RotatorLink {
 public:
  static const size_t kMaxCommandLen = 16;
  static const size_t kMaxReplyLen = 24;
  static const size_t kQueueLen = 8;

  // Identifies who asked, so the reply can be routed back and so the UI can
  // say which source last moved the rotator.
  enum class Source : uint8_t { Poller, Web, Rotctld, Raw };

  enum class Result : uint8_t { Reply, NoReply, Timeout, Rejected };

  typedef void (*ReplyHandler)(uint32_t id, Source source, Result result, const char* reply, void* ctx);

  RotatorLink(Stream& port, const gs232::AzimuthRange& range);

  void begin();
  void poll();

  // Queues a raw command line (without terminator). Returns the transaction id,
  // or 0 if the queue is full. Stop commands jump the queue.
  uint32_t submit(const char* command, Source source);

  void setReplyHandler(ReplyHandler handler, void* ctx);

  // True while motion commands are being withheld because the controller was
  // seen restarting - it silently ignores them for the first
  // ROTATIONAL_AND_CONFIGURATION_CMD_IGNORE_TIME_MS after boot.
  bool inBootLockout() const;

  // False after a run of consecutive timeouts: the controller is unplugged,
  // unpowered, or the link is misconfigured. Distinct from a stale position,
  // which can also be caused by a single dropped reply.
  bool healthy() const { return healthy_; }
  uint32_t consecutiveTimeouts() const { return timeouts_; }

  const gs232::AzimuthRange& range() const { return range_; }

 private:
  struct Request {
    char command[kMaxCommandLen];
    uint32_t id;
    Source source;
    gs232::ReplyKind kind;
  };

  enum class State : uint8_t { Idle, AwaitingReply, Grace };

  bool isMotionCommand(const char* command) const;
  void startNext();
  void finish(Result result, const char* reply);
  void readIncoming();

  Stream& port_;
  gs232::AzimuthRange range_;

  Request queue_[kQueueLen];
  size_t queueHead_ = 0;
  size_t queueCount_ = 0;

  Request active_;
  State state_ = State::Idle;
  uint32_t stateSince_ = 0;
  uint32_t nextId_ = 1;

  char replyBuf_[kMaxReplyLen];
  size_t replyLen_ = 0;

  uint32_t lockoutUntil_ = 0;
  uint32_t timeouts_ = 0;
  bool healthy_ = true;

  ReplyHandler handler_ = nullptr;
  void* handlerCtx_ = nullptr;
};
