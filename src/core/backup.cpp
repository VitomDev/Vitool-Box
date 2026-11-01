#include "core/backup.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <iterator>
#include <optional>

#include "core/packages.h"
#include "core/paths.h"
#include "core/text.h"

namespace vitool {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kMarker[] = L"vitool-backup.txt";

enum class Place { Roaming, Local, Registry };

struct Entry {
    Place place;
    std::wstring name;
};

std::optional<Entry> ParseEntry(std::wstring_view entry) {
    const auto lower = ToLower(entry);
    const struct { std::wstring_view prefix; Place place; } kinds[] = {
        {L"%appdata%\\", Place::Roaming},
        {L"%localappdata%\\", Place::Local},
        {L"hkcu\\software\\", Place::Registry},
    };
    for (const auto& kind : kinds) {
        if (!StartsWith(lower, kind.prefix)) continue;
        const auto name = entry.substr(kind.prefix.size());
        if (!IsSafeFileName(name)) return std::nullopt;
        return Entry{kind.place, std::wstring(name)};
    }
    return std::nullopt;
}

fs::path KnownFolder(Place place) {
    PWSTR path = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(place == Place::Roaming ? FOLDERID_RoamingAppData : FOLDERID_LocalAppData, 0, nullptr, &path))) {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

const wchar_t* FolderInArchive(Place place) {
    return place == Place::Roaming ? L"APPDATA" : L"LOCALAPPDATA";
}

fs::path SystemTool(const wchar_t* exe) {
    wchar_t folder[MAX_PATH];
    const UINT length = GetSystemDirectoryW(folder, ARRAYSIZE(folder));
    return fs::path(std::wstring(folder, length)) / exe;
}

// Runs one of Windows' command-line tools without a console window.
bool RunTool(const wchar_t* exe, const std::wstring& arguments) {
    const auto program = SystemTool(exe);
    std::wstring command = L"\"" + program.wstring() + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    DWORD exitCode = 1;
    if (WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode == 0;
}

std::wstring Quoted(const fs::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

fs::path FreshStaging() {
    const auto staging = DataDir() / L"backup-staging";
    std::error_code ignored;
    fs::remove_all(staging, ignored);
    fs::create_directories(staging, ignored);
    return staging;
}

bool CopyFolder(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::create_directories(to, error);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
    return !error;
}

std::wstring ReadRegFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
    }
    return Utf8ToWide(bytes);
}

}  // namespace

bool IsValidSettingsEntry(std::wstring_view entry) {
    return ParseEntry(entry).has_value();
}

std::vector<BackupApp> ParseBackupSpec(std::wstring_view spec) {
    std::vector<BackupApp> apps;
    while (!spec.empty()) {
        const auto lineEnd = spec.find(L'\n');
        auto line = spec.substr(0, lineEnd);
        spec = lineEnd == std::wstring_view::npos ? std::wstring_view() : spec.substr(lineEnd + 1);

        std::vector<std::wstring_view> fields;
        while (true) {
            const auto tab = line.find(L'\t');
            fields.push_back(line.substr(0, tab));
            if (tab == std::wstring_view::npos) break;
            line = line.substr(tab + 1);
        }
        if (fields.size() < 4 || !IsSafeFileName(fields[0]) || !IsValidAppId(fields[1])) continue;
        BackupApp app{std::wstring(fields[0]), std::wstring(fields[1]), IsSafeFileName(fields[2]) ? std::wstring(fields[2]) : L"", {}};
        for (size_t i = 3; i < fields.size(); ++i) {
            if (IsValidSettingsEntry(fields[i])) app.entries.emplace_back(fields[i]);
        }
        if (!app.entries.empty()) apps.push_back(std::move(app));
    }
    return apps;
}

bool RegFileOnlyTouches(std::wstring_view content, std::wstring_view key) {
    const auto allowed = ToLower(key);
    bool header = false;
    while (!content.empty()) {
        const auto end = content.find(L'\n');
        auto line = content.substr(0, end);
        content = end == std::wstring_view::npos ? std::wstring_view() : content.substr(end + 1);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.remove_suffix(1);
        while (!line.empty() && line.front() == L' ') line.remove_prefix(1);
        if (line.empty()) continue;
        if (!header) {
            if (line != L"Windows Registry Editor Version 5.00" && line != L"REGEDIT4") return false;
            header = true;
            continue;
        }
        if (line.front() != L'[') continue;
        line.remove_prefix(1);
        if (!line.empty() && line.front() == L'-') line.remove_prefix(1);
        const auto close = line.find(L']');
        if (close == std::wstring_view::npos) return false;
        const auto target = ToLower(line.substr(0, close));
        if (target != allowed && !StartsWith(target, allowed + L"\\")) return false;
    }
    return header;
}

std::wstring BackupSettings(const std::vector<BackupApp>& apps, const fs::path& zip, std::vector<std::wstring>& saved) {
    const auto staging = FreshStaging();
    std::ofstream(staging / kMarker) << "Vitool Box settings backup\n";

    for (const auto& app : apps) {
        bool any = false;
        for (const auto& text : app.entries) {
            const auto entry = ParseEntry(text);
            if (!entry) continue;
            if (entry->place == Place::Registry) {
                HKEY key = nullptr;
                const auto path = L"Software\\" + entry->name;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
                RegCloseKey(key);
                const auto folder = staging / app.name / L"registry";
                std::error_code ignored;
                fs::create_directories(folder, ignored);
                if (RunTool(L"reg.exe", L"export " + Quoted(L"HKCU\\" + path) + L" " + Quoted(folder / (entry->name + L".reg")) + L" /y")) any = true;
            } else {
                const auto root = KnownFolder(entry->place);
                std::error_code ignored;
                if (root.empty() || !fs::is_directory(root / entry->name, ignored)) continue;
                const auto source = root / entry->name;
                if (CopyFolder(source, staging / app.name / FolderInArchive(entry->place) / entry->name)) any = true;
            }
        }
        if (any) saved.push_back(app.name);
    }

    std::wstring error;
    if (saved.empty()) {
        error = L"nothingToBackup";
    } else {
        std::error_code ignored;
        fs::remove(zip, ignored);
        if (!RunTool(L"tar.exe", L"-a -c -f " + Quoted(zip) + L" -C " + Quoted(staging) + L" .")) error = L"backupFailed";
    }
    std::error_code ignored;
    fs::remove_all(staging, ignored);
    return error;
}

std::wstring RestoreSettings(const fs::path& zip, const std::vector<BackupApp>& apps, const std::function<void(const BackupApp&)>& before,
                             const std::function<void(const BackupApp&)>& after, std::vector<std::wstring>& restored) {
    const auto staging = FreshStaging();
    std::error_code ignored;
    if (!RunTool(L"tar.exe", L"-x -f " + Quoted(zip) + L" -C " + Quoted(staging)) || !fs::exists(staging / kMarker, ignored)) {
        fs::remove_all(staging, ignored);
        return L"notABackup";
    }

    for (const auto& app : apps) {
        const auto base = staging / app.name;
        if (!fs::is_directory(base, ignored)) continue;
        bool started = false;
        bool any = false;
        for (const auto& text : app.entries) {
            const auto entry = ParseEntry(text);
            if (!entry) continue;
            fs::path source;
            if (entry->place == Place::Registry) {
                source = base / L"registry" / (entry->name + L".reg");
                const auto key = L"HKEY_CURRENT_USER\\Software\\" + entry->name;
                if (!fs::is_regular_file(source, ignored) || !RegFileOnlyTouches(ReadRegFile(source), key)) continue;
            } else {
                source = base / FolderInArchive(entry->place) / entry->name;
                if (!fs::is_directory(source, ignored)) continue;
            }
            if (!started) {
                if (before) before(app);
                started = true;
            }
            if (entry->place == Place::Registry) {
                RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\" + entry->name).c_str());
                if (RunTool(L"reg.exe", L"import " + Quoted(source))) any = true;
            } else {
                const auto root = KnownFolder(entry->place);
                if (root.empty()) continue;
                const auto target = root / entry->name;
                fs::remove_all(target, ignored);
                if (CopyFolder(source, target)) any = true;
            }
        }
        if (any) restored.push_back(app.name);
        if (started && after) after(app);
    }
    fs::remove_all(staging, ignored);
    return restored.empty() ? L"nothingRestored" : L"";
}

}  // namespace vitool
