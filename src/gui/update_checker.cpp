#include "update_checker.h"

#include <windows.h>
#include <winhttp.h>

namespace psvr2pt {

UpdateChecker::~UpdateChecker() { shutdown(); }

void UpdateChecker::start(const char* current_version) {
    thread_ = std::thread(&UpdateChecker::run, this, std::string(current_version));
}

void UpdateChecker::shutdown() {
    if (thread_.joinable())
        thread_.join();
}

UpdateChecker::State UpdateChecker::state() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return state_;
}

std::string UpdateChecker::latest_tag() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return latest_tag_;
}

namespace {

std::string http_get(const wchar_t* host, const wchar_t* path) {
    std::string body;

    HINTERNET hs = WinHttpOpen(L"PSVR2PT/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) return body;

    // 5-second timeouts so a slow/absent network doesn't stall shutdown.
    DWORD timeout_ms = 5000;
    WinHttpSetOption(hs, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(hs, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(hs, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout_ms, sizeof(timeout_ms));

    HINTERNET hc = WinHttpConnect(hs, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hc) { WinHttpCloseHandle(hs); return body; }

    HINTERNET hr = WinHttpOpenRequest(hc, L"GET", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hr) { WinHttpCloseHandle(hc); WinHttpCloseHandle(hs); return body; }

    if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hr, nullptr)) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hr, chunk.data(), avail, &read)) break;
            body.append(chunk, 0, read);
        }
    }

    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return body;
}

std::string extract_json_string(const std::string& json, const char* key) {
    for (const char* needle : { "\":\"", "\": \"" }) {
        std::string k = std::string("\"") + key + needle;
        auto pos = json.find(k);
        if (pos == std::string::npos) continue;
        pos += k.size();
        auto end = json.find('"', pos);
        if (end != std::string::npos)
            return json.substr(pos, end - pos);
    }
    return {};
}

}  // namespace

void UpdateChecker::run(std::string current_version) {
    const std::string body = http_get(
        L"api.github.com",
        L"/repos/Obsidiate/psvr2passthrough/releases/latest");

    const std::string tag = extract_json_string(body, "tag_name");

    std::lock_guard<std::mutex> lk(mutex_);
    if (tag.empty()) {
        state_ = State::Failed;
    } else if (tag == current_version) {
        state_ = State::UpToDate;
    } else {
        latest_tag_ = tag;
        state_      = State::Available;
    }
}

}  // namespace psvr2pt
