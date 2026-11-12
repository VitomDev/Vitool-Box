#include "core/startup.h"

#include <windows.h>

#include <string>

namespace vitool {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"Vitool Box";
constexpr wchar_t kSettingsKey[] = L"Software\\VitomDev\\Vitool Box";
constexpr wchar_t kGroupValue[] = L"GroupTrayIcons";

}  // namespace

bool StartsWithWindows() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool SetStartWithWindows(bool enabled) {
    if (!enabled) {
        const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    wchar_t exe[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    const std::wstring command = L"\"" + std::wstring(exe, length) + L"\" " + kBackgroundArgument;
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, command.c_str(),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool GroupsTrayIcons() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kGroupValue, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

bool SetGroupTrayIcons(bool enabled) {
    const DWORD value = enabled ? 1 : 0;
    return RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kGroupValue, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

}  // namespace vitool
