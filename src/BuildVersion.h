// The running firmware's version - a release tag ("v1.4.0") for a CI release
// build, `git describe` output for a local one. Defined in BuildVersion.cpp,
// which tools/version.py generates on every build (see there).

#pragma once

extern const char* const kFirmwareVersion;
