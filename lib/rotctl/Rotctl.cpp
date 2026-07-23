#include "Rotctl.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace rotctl {
namespace {

const char* skipSpace(const char* p) {
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  return p;
}

bool matchesLong(const char* p, const char* name) {
  const size_t len = strlen(name);
  if (strncmp(p, name, len) != 0) {
    return false;
  }
  const char after = p[len];
  return after == '\0' || after == ' ' || after == '\t';
}

}  // namespace

Command parse(const char* line) {
  Command out;
  if (line == nullptr) {
    return out;
  }

  const char* p = skipSpace(line);

  // Extended response mode: the client asks for verbose, delimited output.
  if (*p == '+' || *p == ';' || *p == '|' || *p == ',') {
    out.extended = true;
    p = skipSpace(p + 1);
  }

  if (*p == '\0') {
    out.cmd = Cmd::Empty;
    return out;
  }

  // Long forms, with or without the leading backslash.
  const char* word = (*p == '\\') ? p + 1 : p;
  if (matchesLong(word, "dump_state")) {
    out.cmd = Cmd::DumpState;
    return out;
  }
  if (matchesLong(word, "get_pos")) {
    out.cmd = Cmd::GetPos;
    return out;
  }
  if (matchesLong(word, "get_info")) {
    out.cmd = Cmd::GetInfo;
    return out;
  }
  if (matchesLong(word, "stop")) {
    out.cmd = Cmd::Stop;
    return out;
  }
  if (matchesLong(word, "park")) {
    out.cmd = Cmd::Park;
    return out;
  }
  if (matchesLong(word, "set_pos") || matchesLong(word, "move") || matchesLong(word, "reset")) {
    const bool isSet = matchesLong(word, "set_pos");
    const bool isMove = matchesLong(word, "move");
    const char* args = skipSpace(word + strlen(isSet ? "set_pos" : (isMove ? "move" : "reset")));
    if (isSet) {
      out.cmd = Cmd::SetPos;
      out.az = strtod(args, const_cast<char**>(&args));
      out.el = strtod(skipSpace(args), nullptr);
    } else if (isMove) {
      out.cmd = Cmd::Move;
      out.direction = strtol(args, const_cast<char**>(&args), 10);
      out.speed = strtol(skipSpace(args), nullptr, 10);
    } else {
      out.cmd = Cmd::Reset;
    }
    return out;
  }

  switch (*p) {
    case 'p':
      out.cmd = Cmd::GetPos;
      break;

    case 'P': {
      out.cmd = Cmd::SetPos;
      const char* args = skipSpace(p + 1);
      out.az = strtod(args, const_cast<char**>(&args));
      out.el = strtod(skipSpace(args), nullptr);
      break;
    }

    case 'S':
      out.cmd = Cmd::Stop;
      break;

    case 'K':
      out.cmd = Cmd::Park;
      break;

    case 'R':
      out.cmd = Cmd::Reset;
      break;

    case 'M': {
      out.cmd = Cmd::Move;
      const char* args = skipSpace(p + 1);
      out.direction = strtol(args, const_cast<char**>(&args), 10);
      out.speed = strtol(skipSpace(args), nullptr, 10);
      break;
    }

    case '_':
      out.cmd = Cmd::GetInfo;
      break;

    case 'q':
    case 'Q':
      out.cmd = Cmd::Quit;
      break;

    default:
      out.cmd = Cmd::Unsupported;
      break;
  }

  return out;
}

float normalizeAzimuth(float az) {
  float result = fmodf(az, 360.0f);
  if (result < 0.0f) {
    result += 360.0f;
  }
  return result;
}

}  // namespace rotctl
