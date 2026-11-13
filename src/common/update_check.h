// The port's version, shown in the corner of the menus. The update check is off in v0.1 (update_check.cpp).
#pragma once

namespace app::update {

inline constexpr char kVersion[] = "0.1.1";  // this build's, shown in the menus

enum class State { kUnknown, kLatest, kAvailable };  // unknown: not answered yet, offline, or no release

// off in v0.1: always unknown, so the menus show the version alone
State Check();

}  // namespace app::update
