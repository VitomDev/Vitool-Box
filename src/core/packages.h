// Installing and removing Vitool Box apps through their own Inno Setup
// installers, and finding them in Windows' list of installed programs.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/download.h"

namespace vitool {

struct InstalledApp {
    std::wstring version;
    std::filesystem::path location;
    std::wstring uninstallString;
};

// "{GUID}", as written in the manifest (Inno's AppId without the doubled brace).
bool IsValidAppId(std::wstring_view appId);
// Only release assets of the VitomDev repositories are ever downloaded.
bool IsAllowedDownloadUrl(std::wstring_view url);
bool IsValidSha256(std::string_view hex);
// A plain file name: no folders, no "..", no reserved characters.
bool IsSafeFileName(std::wstring_view name);

// The executable of an UninstallString such as "C:\...\unins000.exe" /x.
std::filesystem::path UninstallerFromCommand(std::wstring_view command);

std::optional<InstalledApp> FindInstalled(std::wstring_view appId);

// Each returns an empty string on success, otherwise an error key for the UI.
std::wstring InstallApp(std::wstring_view appId, const std::wstring& url, const std::string& sha256, std::wstring_view fileName,
                        const DownloadProgress& progress);
std::wstring UninstallApp(std::wstring_view appId);
// Vitool Box's own update: download its installer, then run it silently and
// wait. The running executable has to be moved out of the way first.
std::wstring DownloadSelfUpdate(const std::wstring& url, const std::string& sha256, std::wstring_view fileName,
                                const DownloadProgress& progress, std::filesystem::path& setup);
std::wstring RunSelfUpdate(const std::filesystem::path& setup);
// Removes installers left over from earlier runs (a self-update cannot
// delete its own).
void CleanDownloads();

}  // namespace vitool
