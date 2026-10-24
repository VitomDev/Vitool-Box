#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>

namespace vitool {

std::filesystem::path ExecutableDir() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

std::filesystem::path DataDir() {
    std::filesystem::path result;
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        result = std::filesystem::path(localAppData) / L"Vitool Box";
    }
    CoTaskMemFree(localAppData);
    std::error_code ignored;
    std::filesystem::create_directories(result, ignored);
    return result;
}

}  // namespace vitool
