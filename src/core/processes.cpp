#include "core/processes.h"

#include <windows.h>
#include <tlhelp32.h>

#include <vector>

#include "core/text.h"

namespace vitool {
namespace {

std::vector<DWORD> ProcessesIn(const std::filesystem::path& folder) {
    std::vector<DWORD> result;
    if (folder.empty()) return result;
    auto prefix = ToLower(folder.wstring());
    if (prefix.back() != L'\\') prefix += L'\\';

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t path[MAX_PATH * 4];
        DWORD size = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(process, 0, path, &size) && StartsWith(ToLower(std::wstring(path, size)), prefix)) {
            result.push_back(entry.th32ProcessID);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return result;
}

BOOL CALLBACK CloseWindowOf(HWND window, LPARAM lParam) {
    const auto& ids = *reinterpret_cast<const std::vector<DWORD>*>(lParam);
    DWORD id = 0;
    GetWindowThreadProcessId(window, &id);
    for (const DWORD wanted : ids) {
        if (id == wanted) PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

}  // namespace

bool IsRunningFrom(const std::filesystem::path& folder) {
    return !ProcessesIn(folder).empty();
}

bool CloseProgramsIn(const std::filesystem::path& folder) {
    auto ids = ProcessesIn(folder);
    if (ids.empty()) return true;
    // Tray apps keep hidden top-level windows; message-only ones are reached
    // through FindWindowEx(HWND_MESSAGE).
    EnumWindows(CloseWindowOf, reinterpret_cast<LPARAM>(&ids));
    for (HWND window = nullptr; (window = FindWindowExW(HWND_MESSAGE, window, nullptr, nullptr)) != nullptr;) {
        CloseWindowOf(window, reinterpret_cast<LPARAM>(&ids));
    }
    for (int attempt = 0; attempt < 50 && IsRunningFrom(folder); ++attempt) Sleep(100);

    for (const DWORD id : ProcessesIn(folder)) {
        if (HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, id)) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 3000);
            CloseHandle(process);
        }
    }
    return !IsRunningFrom(folder);
}

}  // namespace vitool
