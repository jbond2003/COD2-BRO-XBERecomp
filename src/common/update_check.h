// Is there a newer release? Asked once, in the background, of the repository's releases page on GitHub.
#pragma once

namespace app::update {

inline constexpr char kVersion[] = "0.1";  // this build's; a release's tag is "v0.1" or "0.1"

enum class State { kUnknown, kLatest, kAvailable };  // unknown: not answered yet, offline, or no release

// the first call starts the question; never waits
State Check();

}  // namespace app::update
