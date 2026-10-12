#include "Semver.h"

#include <ctype.h>
#include <string.h>

namespace semver {
namespace {

// Reads a run of digits into out. False if there was none, or it is absurdly
// long (a hash that happens to start with digits is not a version number).
bool readNumber(const char*& p, int& out) {
  if (!isdigit(static_cast<unsigned char>(*p))) {
    return false;
  }
  int value = 0;
  int digits = 0;
  while (isdigit(static_cast<unsigned char>(*p))) {
    if (++digits > 6) {
      return false;
    }
    value = value * 10 + (*p - '0');
    p++;
  }
  out = value;
  return true;
}

bool isHex(char c) { return isxdigit(static_cast<unsigned char>(c)) != 0; }

// "-<n>-g<hex>" at p, as git describe appends. Advances p past it on success.
bool readDescribeSuffix(const char*& p, int& commits) {
  const char* q = p;
  if (*q != '-') {
    return false;
  }
  q++;
  int n = 0;
  if (!readNumber(q, n) || *q != '-' || q[1] != 'g' || !isHex(q[2])) {
    return false;
  }
  q += 2;
  while (isHex(*q)) {
    q++;
  }
  if (*q != '\0' && *q != '-') {
    return false;
  }
  commits = n;
  p = q;
  return true;
}

}  // namespace

Version parse(const char* text) {
  Version v;
  if (text == nullptr) {
    return v;
  }
  const char* p = text;
  if (*p == 'v' || *p == 'V') {
    p++;
  }
  // major.minor is the minimum; a bare "7" is too easily part of a hash.
  if (!readNumber(p, v.major) || *p != '.') {
    return v;
  }
  p++;
  if (!readNumber(p, v.minor)) {
    return v;
  }
  if (*p == '.') {
    p++;
    if (!readNumber(p, v.patch)) {
      return v;
    }
  }

  // Suffixes, in the order git describe produces them. "-dirty" on its own
  // (a tag checked out with local edits) is valid too.
  if (strcmp(p, "-dirty") != 0) {
    readDescribeSuffix(p, v.commitsAhead);
  }
  if (strcmp(p, "-dirty") == 0) {
    v.dirty = true;
    p += 6;
  }
  if (*p == '-' && p[1] != '\0') {
    // Anything else after a dash is a prerelease label ("-rc1", "-beta"),
    // possibly itself followed by a describe suffix.
    v.prerelease = true;
    p++;
    while (*p != '\0' && *p != '-') {
      p++;
    }
    readDescribeSuffix(p, v.commitsAhead);
    if (strcmp(p, "-dirty") == 0) {
      v.dirty = true;
      p += 6;
    }
  }
  if (*p != '\0') {
    return v;  // trailing junk - not something to trust an ordering to
  }
  v.valid = true;
  return v;
}

int compare(const Version& a, const Version& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.prerelease != b.prerelease) return a.prerelease ? -1 : 1;
  if (a.commitsAhead != b.commitsAhead) return a.commitsAhead < b.commitsAhead ? -1 : 1;
  return 0;
}

bool isDevBuild(const Version& v) { return !v.valid || v.prerelease || v.commitsAhead > 0 || v.dirty; }

}  // namespace semver
