#include "core/download.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <fstream>
#include <memory>
#include <vector>

namespace vitool {
namespace {

struct InternetHandleDeleter {
    void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
};
using InternetHandle = std::unique_ptr<void, InternetHandleDeleter>;

}  // namespace

std::wstring DownloadFile(const std::wstring& url, const std::filesystem::path& target, const DownloadProgress& progress) {
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return L"Invalid download address.";

    InternetHandle session(WinHttpOpen(L"VitoolBox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return L"Could not start the download.";
    InternetHandle connection(WinHttpConnect(session.get(), host, parts.nPort, 0));
    if (!connection) return L"Could not connect to GitHub.";
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) return L"Could not start the download.";

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return L"Could not connect to GitHub. Check your internet connection.";
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return L"The download failed (HTTP " + std::to_wstring(status) + L").";

    uint64_t total = 0;
    wchar_t lengthText[32]{};
    size = sizeof(lengthText);
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &size, WINHTTP_NO_HEADER_INDEX)) {
        total = _wcstoui64(lengthText, nullptr, 10);
    }

    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) return L"Could not save the download.";

    std::vector<char> buffer(64 * 1024);
    uint64_t received = 0;
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) return L"The download was interrupted.";
        if (read == 0) break;
        file.write(buffer.data(), read);
        if (!file) return L"Could not save the download.";
        received += read;
        if (progress) progress(received, total);
    }
    if (total != 0 && received != total) return L"The download was interrupted.";
    return {};
}

std::string Sha256File(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0))) return {};

    std::vector<char> buffer(64 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<ULONG>(input.gcount());
        if (count > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), count, 0);
    }
    unsigned char digest[32];
    const bool ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    BCryptDestroyHash(hash);
    if (!ok) return {};

    static const char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : digest) {
        result += digits[byte >> 4];
        result += digits[byte & 0xF];
    }
    return result;
}

}  // namespace vitool
