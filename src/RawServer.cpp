#include "RawServer.h"

#include <string.h>

RawServer::RawServer(Rotator& rotator, uint16_t port) : rotator_(rotator), port_(port), server_(port) {
  for (size_t i = 0; i < kMaxClients; i++) {
    sessions_[i].line[0] = '\0';
  }
}

void RawServer::begin() {
  server_.begin();
  server_.setNoDelay(true);
  rotator_.setRawHandler(replyTrampoline, this);
}

size_t RawServer::clientCount() const {
  size_t count = 0;
  for (size_t i = 0; i < kMaxClients; i++) {
    if (const_cast<WiFiClient&>(sessions_[i].client).connected()) {
      count++;
    }
  }
  return count;
}

String RawServer::clientAddresses() const {
  String out;
  for (size_t i = 0; i < kMaxClients; i++) {
    WiFiClient& client = const_cast<WiFiClient&>(sessions_[i].client);
    if (!client.connected()) {
      continue;
    }
    if (out.length() > 0) {
      out += ", ";
    }
    out += client.remoteIP().toString();
  }
  return out;
}

void RawServer::replyTrampoline(uint32_t id, RotatorLink::Result result, const char* reply, void* ctx) {
  static_cast<RawServer*>(ctx)->onReply(id, result, reply);
}

void RawServer::onReply(uint32_t id, RotatorLink::Result result, const char* reply) {
  for (size_t i = 0; i < kMaxClients; i++) {
    Session& session = sessions_[i];
    if (session.pendingId != id || !session.client.connected()) {
      continue;
    }
    session.pendingId = 0;

    switch (result) {
      case RotatorLink::Result::Reply:
      case RotatorLink::Result::Rejected:
        // Both are real controller output, including "?>" - pass it through
        // unchanged, terminated the way the controller terminates.
        if (reply != nullptr) {
          session.client.print(reply);
          session.client.print("\r\n");
        }
        break;

      case RotatorLink::Result::NoReply:
        // The controller says nothing on a successful rotate. Saying nothing
        // back is what a real serial link would do.
        break;

      case RotatorLink::Result::Timeout:
        // Not silence by design - the controller failed to answer. A client
        // waiting forever is worse than one told the link is unhealthy.
        session.client.print("?>\r\n");
        break;
    }
    return;
  }
}

void RawServer::poll() {
  if (server_.hasClient()) {
    bool placed = false;
    for (size_t i = 0; i < kMaxClients && !placed; i++) {
      if (!sessions_[i].client.connected()) {
        sessions_[i].client = server_.accept();
        sessions_[i].len = 0;
        sessions_[i].pendingId = 0;
        placed = true;
      }
    }
    if (!placed) {
      server_.accept().stop();
    }
  }

  for (size_t i = 0; i < kMaxClients; i++) {
    Session& session = sessions_[i];
    if (!session.client.connected()) {
      continue;
    }

    // One outstanding command per client, so replies cannot be reordered
    // relative to the commands that produced them.
    while (session.pendingId == 0 && session.client.available() > 0) {
      const int c = session.client.read();
      if (c < 0) {
        break;
      }

      if (c == '\r' || c == '\n') {
        if (session.len == 0) {
          continue;
        }
        session.line[session.len] = '\0';
        session.len = 0;

        const uint32_t id = rotator_.submitRaw(session.line);
        if (id == 0) {
          session.client.print("?>\r\n");  // queue full
        } else {
          session.pendingId = id;
        }
        continue;
      }

      if (session.len < kLineLen - 1) {
        session.line[session.len++] = static_cast<char>(c);
      }
    }
  }
}
