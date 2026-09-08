// Identifying details of the executable this build targets.
//
// MWDX is written against Need for Speed: Most Wanted (2005), PC retail v1.3,
// a 32-bit binary. Anything here that encodes an assumption about that
// specific build belongs in this header rather than scattered through the
// renderer, so that the assumptions are visible in one place if another
// version ever has to be supported.

#pragma once

namespace dx9to11::target {

inline constexpr const char* kExeName   = "speed.exe";
inline constexpr const char* kBuild     = "retail v1.3";
inline constexpr const char* kSha1      = "eb0a5d2c505ca0f18dee1c33988a1de413a0c824";
inline constexpr unsigned    kImageBase = 0x400000u;

}
