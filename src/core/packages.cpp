#include "core/packages.h"

#include <windows.h>
#include <shellapi.h>

#include "core/paths.h"
#include "core/text.h"

namespace vitool {
namespace {

constexpr wchar_t kUninstallRoot[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
constexpr wchar_t kSilentArgs[] = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-";

std::wstring ReadString(HKEY key, const wchar_t* name) {
    wchar_t buffer[2048]{};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

// Runs a program (asking for administrator rights if its manifest needs them)
// and waits for it to finish.
std::wstring RunAndWait(const std::filesystem::path& program, const wchar_t* arguments, DWORD& exitCode) {
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = program.c_str();
    info.lpParameters = arguments;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        if (GetLastError() == ERROR_CANCELLED) return L"Administrator permission was not granted.";
        return L"Could not start the installer.";
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    return {};
}

bool IsHexDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

}  // namespace

bool IsValidAppId(std::wstring_view appId) {
    // {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
    if (appId.size() != 38 || appId.front() != L'{' || appId.back() != L'}') return false;
    for (size_t i = 1; i < 37; ++i) {
        const bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        if (dash ? appId[i] != L'-' : !IsHexDigit(appId[i])) return false;
    }
    return true;
}

bool IsAllowedDownloadUrl(std::wstring_view url) {
    constexpr std::wstring_view prefix = L"https://github.com/VitomDev/";
    if (!StartsWith(url, prefix)) return false;
    const auto rest = url.substr(prefix.size());
    const auto slash = rest.find(L'/');
    if (slash == 0 || slash == std::wstring_view::npos) return false;
    return StartsWith(rest.substr(slash), L"/releases/download/") && rest.find(L"..") == std::wstring_view::npos;
}

bool IsValidSha256(std::string_view hex) {
    if (hex.size() != 64) return false;
    for (const char c : hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool IsSafeFileName(std::wstring_view name) {
    if (name.empty() || name.size() > 200 || name == L"." || name == L"..") return false;
    for (const wchar_t c : name) {
        if (c < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) return false;
    }
    return name.back() != L'.' && name.back() != L' ';
}

std::filesystem::path UninstallerFromCommand(std::wstring_view command) {
    while (!command.empty() && command.front() == L' ') command.remove_prefix(1);
    if (command.empty()) return {};
    if (command.front() == L'"') {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring_view::npos) return {};
        return std::filesystem::path(command.substr(1, end - 1));
    }
    const auto exe = ToLower(command).find(L".exe");
    if (exe == std::wstring::npos) return {};
    return std::filesystem::path(command.substr(0, exe + 4));
}

std::optional<InstalledApp> FindInstalled(std::wstring_view appId) {
    if (!IsValidAppId(appId)) return std::nullopt;
    const std::wstring keyName = kUninstallRoot + std::wstring(appId) + L"_is1";
    const struct { HKEY root; REGSAM view; } places[] = {
        {HKEY_CURRENT_USER, KEY_WOW64_64KEY},
        {HKEY_CURRENT_USER, KEY_WOW64_32KEY},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY},
    };
    for (const auto& place : places) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(place.root, keyName.c_str(), 0, KEY_READ | place.view, &key) != ERROR_SUCCESS) continue;
        InstalledApp app;
        app.version = ReadString(key, L"DisplayVersion");
        app.location = ReadString(key, L"InstallLocation");
        app.uninstallString = ReadString(key, L"UninstallString");
        RegCloseKey(key);
        return app;
    }
    return std::nullopt;
}

std::wstring InstallApp(std::wstring_view appId, const std::wstring& url, const std::string& sha256, std::wstring_view fileName,
                        const DownloadProgress& progress) {
    if (!IsValidAppId(appId) || !IsAllowedDownloadUrl(url) || !IsValidSha256(sha256) || !IsSafeFileName(fileName) ||
        ToLower(std::filesystem::path(fileName).extension().wstring()) != L".exe") {
        return L"This app's release information is not valid.";
    }

    const auto folder = DataDir() / L"downloads";
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    const auto setup = folder / fileName;

    auto error = DownloadFile(url, setup, progress);
    if (error.empty() && Sha256File(setup) != sha256) error = L"The downloaded installer is damaged (checksum mismatch).";

    DWORD exitCode = 0;
    if (error.empty()) error = RunAndWait(setup, kSilentArgs, exitCode);
    std::filesystem::remove(setup, ignored);
    if (!error.empty()) return error;
    if (exitCode != 0) return L"The installer stopped with code " + std::to_wstring(exitCode) + L".";
    if (!FindInstalled(appId)) return L"The installer finished, but the app was not found afterwards.";
    return {};
}

std::wstring UninstallApp(std::wstring_view appId) {
    const auto app = FindInstalled(appId);
    if (!app) return {};
    const auto uninstaller = UninstallerFromCommand(app->uninstallString);
    std::error_code ignored;
    if (uninstaller.empty() || !std::filesystem::exists(uninstaller, ignored)) return L"The app's uninstaller was not found.";

    DWORD exitCode = 0;
    if (auto error = RunAndWait(uninstaller, kSilentArgs, exitCode); !error.empty()) return error;
    if (exitCode != 0) return L"The uninstaller stopped with code " + std::to_wstring(exitCode) + L".";

    // Inno's uninstaller finishes its work from a copy of itself, so give it
    // a moment to remove its entry.
    for (int attempt = 0; attempt < 100 && FindInstalled(appId); ++attempt) Sleep(200);
    if (FindInstalled(appId)) return L"The app could not be fully uninstalled.";
    return {};
}

}  // namespace vitool
