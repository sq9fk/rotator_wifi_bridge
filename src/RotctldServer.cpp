#include "RotctldServer.h"

#include <string.h>

#include "Rotctl.h"

namespace {

// Hamlib return codes, as its client parses them out of a "RPRT n" line.
const int kOk = 0;
const int kInvalidParam = -1;   // RIG_EINVAL
const int kNotImplemented = -4; // RIG_ENIMPL
const int kIoError = -6;        // RIG_EIO

}  // namespace

RotctldServer::RotctldServer(Rotator& rotator, uint16_t port)
    : rotator_(rotator), port_(port), server_(port) {
  for (size_t i = 0; i < kMaxClients; i++) {
    sessions_[i].line[0] = '\0';
  }
}

void RotctldServer::begin() {
  server_.begin();
  server_.setNoDelay(true);
}

size_t RotctldServer::clientCount() const {
  size_t count = 0;
  for (size_t i = 0; i < kMaxClients; i++) {
    if (const_cast<WiFiClient&>(sessions_[i].client).connected()) {
      count++;
    }
  }
  return count;
}

String RotctldServer::clientAddresses() const {
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

void RotctldServer::poll() {
  // Accept into a free slot; refuse beyond the limit rather than thrashing.
  if (server_.hasClient()) {
    bool placed = false;
    for (size_t i = 0; i < kMaxClients && !placed; i++) {
      if (!sessions_[i].client.connected()) {
        sessions_[i].client = server_.accept();
        sessions_[i].len = 0;
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

    while (session.client.available() > 0) {
      const int c = session.client.read();
      if (c < 0) {
        break;
      }

      if (c == '\n' || c == '\r') {
        if (session.len == 0) {
          continue;
        }
        session.line[session.len] = '\0';
        session.len = 0;
        handleLine(session, session.line);
        if (!session.client.connected()) {
          break;
        }
        continue;
      }

      if (session.len < kLineLen - 1) {
        session.line[session.len++] = static_cast<char>(c);
      }
      // Overlong lines are truncated; the parser will report them unsupported.
    }
  }
}

void RotctldServer::sendDumpState(Session& session) {
  // Protocol version 1 - see Rotctl.h. Version 0 would leave the client using
  // its built-in ±180° range and put the overlap zone out of reach.
  session.client.print("1\n");
  session.client.print("0\n");  // rot_model, which the client explicitly ignores

  // Reported in real azimuth, 0..360: that is the coordinate logging software
  // thinks in. The overlap zone is not exposed as extra degrees; the bridge
  // picks the raw target with the shorter travel.
  session.client.print("min_az=0.000000\n");
  session.client.print("max_az=360.000000\n");
  session.client.print("min_el=0.000000\n");
  session.client.print("max_el=0.000000\n");
  session.client.print("south_zero=0\n");
  session.client.print("rot_type=Az\n");
  session.client.print("done\n");
}

void RotctldServer::handleLine(Session& session, const char* line) {
  const rotctl::Command command = rotctl::parse(line);
  char buf[48];

  switch (command.cmd) {
    case rotctl::Cmd::Empty:
      break;

    case rotctl::Cmd::GetPos:
      if (!rotator_.positionIsFresh()) {
        // Reporting the last known heading as if it were live is how an
        // operator ends up trusting a number the rotator has left behind.
        snprintf(buf, sizeof(buf), "RPRT %d\n", kIoError);
        session.client.print(buf);
        break;
      }
      snprintf(buf, sizeof(buf), "%.6f\n0.000000\n", rotator_.realAzimuth());
      session.client.print(buf);
      break;

    case rotctl::Cmd::SetPos: {
      const float az = rotctl::normalizeAzimuth(command.az);
      const bool accepted = rotator_.gotoAzimuth(az, RotatorLink::Source::Rotctld);
      snprintf(buf, sizeof(buf), "RPRT %d\n", accepted ? kOk : kIoError);
      session.client.print(buf);
      break;
    }

    case rotctl::Cmd::Stop:
      snprintf(buf, sizeof(buf), "RPRT %d\n", rotator_.stop(RotatorLink::Source::Rotctld) ? kOk : kIoError);
      session.client.print(buf);
      break;

    case rotctl::Cmd::Move: {
      bool accepted = false;
      if (command.direction == rotctl::kMoveRight) {
        accepted = rotator_.jog(true, RotatorLink::Source::Rotctld);
      } else if (command.direction == rotctl::kMoveLeft) {
        accepted = rotator_.jog(false, RotatorLink::Source::Rotctld);
      } else {
        // Up and down on an azimuth-only rotator.
        snprintf(buf, sizeof(buf), "RPRT %d\n", kInvalidParam);
        session.client.print(buf);
        break;
      }
      snprintf(buf, sizeof(buf), "RPRT %d\n", accepted ? kOk : kIoError);
      session.client.print(buf);
      break;
    }

    case rotctl::Cmd::GetInfo:
      session.client.print("Info: K3NG GS-232B bridge\n");
      break;

    case rotctl::Cmd::DumpState:
      sendDumpState(session);
      break;

    case rotctl::Cmd::Quit:
      session.client.stop();
      break;

    case rotctl::Cmd::Park:
    case rotctl::Cmd::Reset:
    case rotctl::Cmd::Unsupported:
    default:
      snprintf(buf, sizeof(buf), "RPRT %d\n", kNotImplemented);
      session.client.print(buf);
      break;
  }
}
