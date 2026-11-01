// Backing up and restoring the settings of the apps to a .zip file. Each app
// lists its settings in vitool.json:
//   "%APPDATA%\Name", "%LOCALAPPDATA%\Name"  a folder
//   "HKCU\Software\Name"                     a registry key
// The archive is made and read with Windows' own tar.exe and reg.exe.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vitool {

struct BackupApp {
    std::wstring name;      // folder inside the archive (the repository name)
    std::wstring appId;
    std::wstring exe;       // started again after a restore that closed it
    std::vector<std::wstring> entries;
};

bool IsValidSettingsEntry(std::wstring_view entry);

// "name\tappId\texe\tentry\tentry\nname\t..." as sent by the UI. Apps or entries
// that are not valid are left out.
std::vector<BackupApp> ParseBackupSpec(std::wstring_view spec);

// Whether a .reg file only writes to (or deletes) the key and its subkeys.
bool RegFileOnlyTouches(std::wstring_view content, std::wstring_view key);

// Both return an empty string on success, otherwise an error key for the UI.
std::wstring BackupSettings(const std::vector<BackupApp>& apps, const std::filesystem::path& zip, std::vector<std::wstring>& saved);
// before() runs ahead of restoring each app (to close it), after() once it is done.
std::wstring RestoreSettings(const std::filesystem::path& zip, const std::vector<BackupApp>& apps,
                             const std::function<void(const BackupApp&)>& before,
                             const std::function<void(const BackupApp&)>& after, std::vector<std::wstring>& restored);

}  // namespace vitool
