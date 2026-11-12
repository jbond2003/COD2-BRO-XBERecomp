// The update check (update_check.h). Off in v0.1: the release asks nothing of the internet. Its WinHTTP request
// to the GitHub releases page got the executable flagged by Windows Defender's machine learning scan
// (Trojan:Win32/Bearfoos.A!ml) and stopped mid-load, so the menus show the version alone. The request is in the
// history (commit 5f60442) for when the release can be signed or cleared with Microsoft.
#include "common/update_check.h"

namespace app::update {

State Check() { return State::kUnknown; }

}  // namespace app::update
