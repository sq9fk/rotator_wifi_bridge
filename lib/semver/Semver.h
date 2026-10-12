// Firmware version strings: parsing and ordering, for deciding whether a
// GitHub release is newer than what is running.
//
// Pure and Arduino-free so it can be unit tested on the host - a mistake here
// either hides every update or offers the same one forever.
//
// Two shapes reach this code:
//   - a release tag, "v1.4.0" (also "1.4", "v1.4.0-rc1") - what CI stamps into
//     a release build and what GitHub reports as tag_name;
//   - `git describe --tags --always --dirty` from a local build, which adds
//     "-<commits>-g<hash>" after the last tag and "-dirty" for uncommitted
//     changes ("v1.4.0-3-gabc1234-dirty"), or is a bare hash before any tag
//     exists at all ("b04c149").

#pragma once

namespace semver {

struct Version {
  bool valid = false;      // false: not a version at all (a bare hash, "dev")
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool prerelease = false;  // "-rc1" style suffix: sorts below the plain release
  int commitsAhead = 0;     // git describe: commits since the tag, sorts above it
  bool dirty = false;       // git describe: uncommitted changes (not ordered)
};

Version parse(const char* text);

// <0, 0, >0 like strcmp. Same numbers: prerelease < release < release+N commits.
int compare(const Version& a, const Version& b);

// Anything other than an exact, clean release tag - a local or CI-less build.
// The panel shows such a build's update status in the Update tile but does not
// nag with a banner, since its "older than the release" is usually not true.
bool isDevBuild(const Version& v);

}  // namespace semver
