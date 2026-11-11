// The update check (update_check.h): one GET of api.github.com/repos/<owner>/<repo>/releases/latest on a thread of
// its own, the "tag_name" in the answer compared with this build's version number by number. Nothing about the
// player or the PC is sent beyond the request itself. Windows only, like the menus that show the answer.
#include "common/update_check.h"

#include <atomic>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <thread>

#include <windows.h>
#include <winhttp.h>

#include "core/log.h"

#pragma comment(lib, "winhttp.lib")
#endif

namespace app::update {

namespace {

std::atomic<State> g_state{State::kUnknown};
std::atomic<bool> g_asked{false};

// the release's tag in GitHub's answer: "tag_name": "v0.2"
std::string TagOf(const std::string& json) {
    size_t at = json.find("\"tag_name\"");
    if (at == std::string::npos) return {};
    at = json.find(':', at);
    if (at == std::string::npos) return {};
    size_t open = json.find('"', at);
    if (open == std::string::npos) return {};
    size_t close = json.find('"', open + 1);
    if (close == std::string::npos) return {};
    return json.substr(open + 1, close - open - 1);
}

// a's numbers against b's, left to right ("v0.10" is after "0.9"): below zero, zero, above
int Compare(const char* a, const char* b) {
    auto next = [](const char*& p, bool& any) {
        while (*p && (*p < '0' || *p > '9')) p++;
        any = *p != 0;
        long n = 0;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        return n;
    };
    for (;;) {
        bool has_a = false, has_b = false;
        long na = next(a, has_a), nb = next(b, has_b);
        if (!has_a && !has_b) return 0;
        if (na != nb) return na < nb ? -1 : 1;
    }
}

#ifdef _WIN32
std::string Fetch() {
    std::string body;
    HINTERNET session = WinHttpOpen(L"CoD2BRO-Recomp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET",
                                                     L"/repos/jbond2003/COD2-BRO-XBERecomp/releases/latest", nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request &&
        WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0,
                           0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            char chunk[4096];
            DWORD got = 0;
            while (body.size() < (1u << 20) && WinHttpReadData(request, chunk, sizeof(chunk), &got) && got)
                body.append(chunk, got);
        } else {
            LOGR("update check: the releases page answered %lu", static_cast<unsigned long>(status));
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

void Ask() {
    std::string tag = TagOf(Fetch());
    if (tag.empty()) {
        LOGR("update check: no answer (offline, or no release yet)");
        return;
    }
    bool newer = Compare(tag.c_str(), kVersion) > 0;
    LOGR("update check: the latest release is %s, this is %s", tag.c_str(), kVersion);
    g_state.store(newer ? State::kAvailable : State::kLatest, std::memory_order_relaxed);
}
#endif

}  // namespace

State Check() {
#ifdef _WIN32
    if (!g_asked.exchange(true)) std::thread(Ask).detach();
#else
    (void)g_asked, (void)TagOf, (void)Compare;
#endif
    return g_state.load(std::memory_order_relaxed);
}

}  // namespace app::update
