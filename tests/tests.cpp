// Vitool Box's automated tests: a minimal runner without dependencies.
//
//   VitoolBoxTests.exe

#include "core/backup.h"
#include "core/download.h"
#include "core/json.h"
#include "core/packages.h"
#include "core/text.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace vitool;
namespace fs = std::filesystem;

namespace {

struct TestCase {
    const char* name;
    std::function<void()> run;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> run) { Tests().push_back({name, std::move(run)}); }
};

struct Failure {
    std::string message;
};

#define TEST(name) \
    void name(); \
    Registrar name##_registrar(#name, name); \
    void name()

#define CHECK(condition) \
    do { \
        if (!(condition)) throw Failure{std::string(#condition) + " (line " + std::to_string(__LINE__) + ")"}; \
    } while (false)

TEST(JsonParsesFlatObjects) {
    const auto object = ParseFlatObject(R"( {"type": "install", "url":"https:\/\/x", "name":"caf\u00e9 \ud83d\ude00", "empty":""} )");
    CHECK(object.has_value());
    CHECK(object->at("type") == "install");
    CHECK(object->at("url") == "https://x");
    CHECK(object->at("name") == "caf\xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(object->at("empty").empty());
    CHECK(ParseFlatObject("{}").has_value());
}

TEST(JsonRejectsOtherShapes) {
    CHECK(!ParseFlatObject("").has_value());
    CHECK(!ParseFlatObject(R"({"a":1})").has_value());
    CHECK(!ParseFlatObject(R"({"a":"b"} trailing)").has_value());
    CHECK(!ParseFlatObject(R"({"a":"unterminated)").has_value());
    CHECK(!ParseFlatObject(R"(["a"])").has_value());
}

TEST(JsonQuotesStrings) {
    CHECK(JsonQuote("plain") == "\"plain\"");
    CHECK(JsonQuote("a\"b\\c\nd") == "\"a\\\"b\\\\c\\nd\"");
    CHECK(JsonQuote(std::string("\x01", 1)) == "\"\\u0001\"");
    // Round trip.
    CHECK(ParseFlatObject("{\"k\":" + JsonQuote("tab\there \"q\"") + "}")->at("k") == "tab\there \"q\"");
}

TEST(AppIdsAreGuidsInBraces) {
    CHECK(IsValidAppId(L"{B3E5A1C7-2D84-4F6B-9A0E-7C1D5F38E462}"));
    CHECK(IsValidAppId(L"{b3e5a1c7-2d84-4f6b-9a0e-7c1d5f38e462}"));
    CHECK(!IsValidAppId(L"{{B3E5A1C7-2D84-4F6B-9A0E-7C1D5F38E462}"));
    CHECK(!IsValidAppId(L"B3E5A1C7-2D84-4F6B-9A0E-7C1D5F38E462"));
    CHECK(!IsValidAppId(L"{B3E5A1C7-2D84-4F6B-9A0E-7C1D5F38E46Z}"));
    CHECK(!IsValidAppId(L"{..\\..\\Run}"));
}

TEST(OnlyVitomDevReleaseAssetsAreDownloaded) {
    CHECK(IsAllowedDownloadUrl(L"https://github.com/VitomDev/Kodo-QR-Instant/releases/download/v1.0.0/KodoSetup.exe"));
    CHECK(!IsAllowedDownloadUrl(L"http://github.com/VitomDev/Kodo-QR-Instant/releases/download/v1.0.0/KodoSetup.exe"));
    CHECK(!IsAllowedDownloadUrl(L"https://github.com/Someone/Repo/releases/download/v1/Setup.exe"));
    CHECK(!IsAllowedDownloadUrl(L"https://github.com/VitomDev/Repo/archive/main.zip"));
    CHECK(!IsAllowedDownloadUrl(L"https://github.com/VitomDev//releases/download/v1/Setup.exe"));
    CHECK(!IsAllowedDownloadUrl(L"https://github.com/VitomDev/Repo/releases/download/../../x"));
    CHECK(!IsAllowedDownloadUrl(L"https://github.com.evil.example/VitomDev/Repo/releases/download/v1/Setup.exe"));
}

TEST(Sha256AndFileNamesAreValidated) {
    CHECK(IsValidSha256("4b78e2ac0a4cfca38c09343dc96fad0ec9c7e209dc7dd7fa144a606e81157db8"));
    CHECK(!IsValidSha256("4B78E2AC0A4CFCA38C09343DC96FAD0EC9C7E209DC7DD7FA144A606E81157DB8"));
    CHECK(!IsValidSha256("abc"));
    CHECK(IsSafeFileName(L"KodoSetup.exe"));
    CHECK(IsSafeFileName(L"MuraConverter-Setup-2.2.0.exe"));
    CHECK(!IsSafeFileName(L"..\\evil.exe"));
    CHECK(!IsSafeFileName(L"sub/evil.exe"));
    CHECK(!IsSafeFileName(L"C:evil.exe"));
    CHECK(!IsSafeFileName(L""));
    CHECK(!IsSafeFileName(L"trailing."));
}

TEST(UninstallerIsFoundInTheCommand) {
    CHECK(UninstallerFromCommand(L"\"C:\\Program Files\\Kodo\\unins000.exe\"") == fs::path(L"C:\\Program Files\\Kodo\\unins000.exe"));
    CHECK(UninstallerFromCommand(L"\"C:\\A B\\unins000.exe\" /x") == fs::path(L"C:\\A B\\unins000.exe"));
    CHECK(UninstallerFromCommand(L"C:\\Apps\\unins000.exe /SILENT") == fs::path(L"C:\\Apps\\unins000.exe"));
    CHECK(UninstallerFromCommand(L"").empty());
    CHECK(UninstallerFromCommand(L"\"unterminated").empty());
}

TEST(UnknownAppsAreNotInstalled) {
    CHECK(!FindInstalled(L"{00000000-0000-0000-0000-000000000000}").has_value());
    CHECK(!FindInstalled(L"not an id").has_value());
}

TEST(Sha256OfAFile) {
    const auto file = fs::temp_directory_path() / L"vitoolbox-test-sha.txt";
    std::ofstream(file, std::ios::binary) << "abc";
    CHECK(Sha256File(file) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fs::remove(file);
    CHECK(Sha256File(file).empty());
}

TEST(InstallRejectsBadReleaseData) {
    const auto id = L"{00000000-0000-0000-0000-000000000000}";
    const std::string sha(64, 'a');
    CHECK(!InstallApp(id, L"https://example.com/Setup.exe", sha, L"Setup.exe", nullptr).empty());
    CHECK(!InstallApp(id, L"https://github.com/VitomDev/R/releases/download/v1/Setup.exe", "bad", L"Setup.exe", nullptr).empty());
    CHECK(!InstallApp(id, L"https://github.com/VitomDev/R/releases/download/v1/Setup.zip", sha, L"Setup.zip", nullptr).empty());
}

TEST(SettingsEntriesStayInTheirPlace) {
    CHECK(IsValidSettingsEntry(L"%APPDATA%\\MuraConverter"));
    CHECK(IsValidSettingsEntry(L"%LOCALAPPDATA%\\Kodo QR Instant"));
    CHECK(IsValidSettingsEntry(L"HKCU\\Software\\Vela System Monitor"));
    CHECK(!IsValidSettingsEntry(L"%APPDATA%\\..\\Startup"));
    CHECK(!IsValidSettingsEntry(L"%APPDATA%\\Microsoft\\Windows"));
    CHECK(!IsValidSettingsEntry(L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"));
    CHECK(!IsValidSettingsEntry(L"HKLM\\Software\\Vela"));
    CHECK(!IsValidSettingsEntry(L"C:\\Windows"));
}

TEST(BackupSpecIsParsed) {
    const auto apps = ParseBackupSpec(L"Vela\t{9E3B6A14-2C7D-4F58-B1A9-6D0E8F2C4B37}\tVela.exe\tHKCU\\Software\\Vela System Monitor\t%APPDATA%\\..\n"
                                      L"..\\bad\t{9E3B6A14-2C7D-4F58-B1A9-6D0E8F2C4B37}\tx.exe\t%APPDATA%\\X\n"
                                      L"NoEntries\t{9E3B6A14-2C7D-4F58-B1A9-6D0E8F2C4B37}\tx.exe");
    CHECK(apps.size() == 1);
    CHECK(apps[0].name == L"Vela");
    CHECK(apps[0].exe == L"Vela.exe");
    CHECK(apps[0].entries.size() == 1);
}

TEST(RestoredRegistryFilesAreChecked) {
    const std::wstring key = L"HKEY_CURRENT_USER\\Software\\Vela System Monitor";
    CHECK(RegFileOnlyTouches(L"Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Vela System Monitor]\r\n\"A\"=dword:00000001\r\n"
                             L"[HKEY_CURRENT_USER\\Software\\Vela System Monitor\\Sub]\r\n", key));
    CHECK(!RegFileOnlyTouches(L"Windows Registry Editor Version 5.00\r\n[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run]\r\n", key));
    CHECK(!RegFileOnlyTouches(L"Windows Registry Editor Version 5.00\r\n[HKEY_CURRENT_USER\\Software\\Vela System Monitor Evil]\r\n", key));
    CHECK(!RegFileOnlyTouches(L"Windows Registry Editor Version 5.00\r\n[-HKEY_CURRENT_USER\\Software]\r\n", key));
    CHECK(!RegFileOnlyTouches(L"[HKEY_CURRENT_USER\\Software\\Vela System Monitor]\r\n", key));
}

// Round trip through a real archive with tar.exe and reg.exe, on a test
// registry key and a test folder in %LOCALAPPDATA%.
TEST(SettingsBackupRoundTrip) {
    const wchar_t* key = L"Software\\VitoolBox Backup Test";
    wchar_t localAppData[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    const auto folder = fs::path(localAppData) / L"VitoolBox Backup Test";
    const auto zip = fs::temp_directory_path() / L"vitoolbox-backup-test.zip";
    const DWORD value = 42;
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"Answer", REG_DWORD, &value, sizeof(value));
    fs::create_directories(folder / L"sub");
    std::ofstream(folder / L"sub" / L"notes.txt") << "hello";

    const std::vector<BackupApp> apps{{L"TestApp", L"{00000000-0000-0000-0000-000000000000}", L"",
                                       {L"%LOCALAPPDATA%\\VitoolBox Backup Test", L"HKCU\\Software\\VitoolBox Backup Test"}}};
    std::vector<std::wstring> saved;
    CHECK(BackupSettings(apps, zip, saved).empty());
    CHECK(saved.size() == 1);

    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    fs::remove_all(folder);
    int before = 0, after = 0;
    std::vector<std::wstring> restored;
    CHECK(RestoreSettings(zip, apps, [&](const BackupApp&) { ++before; }, [&](const BackupApp&) { ++after; }, restored).empty());
    CHECK(restored.size() == 1 && before == 1 && after == 1);

    DWORD read = 0;
    DWORD size = sizeof(read);
    CHECK(RegGetValueW(HKEY_CURRENT_USER, key, L"Answer", RRF_RT_REG_DWORD, nullptr, &read, &size) == ERROR_SUCCESS && read == 42);
    std::string text;
    std::ifstream(folder / L"sub" / L"notes.txt") >> text;
    CHECK(text == "hello");

    std::vector<std::wstring> none;
    CHECK(RestoreSettings(folder / L"sub" / L"notes.txt", apps, nullptr, nullptr, none) == L"notABackup");

    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    fs::remove_all(folder);
    fs::remove(zip);
}

TEST(TextHelpers) {
    CHECK(Utf8ToWide("caf\xC3\xA9") == L"caf\u00e9");
    CHECK(WideToUtf8(L"caf\u00e9") == "caf\xC3\xA9");
    CHECK(ToLower(L"ABC.Exe") == L"abc.exe");
    CHECK(StartsWith(L"https://hub.vitool/x", L"https://hub.vitool/"));
    CHECK(!StartsWith(L"https://hub", L"https://hub.vitool/"));
}

}  // namespace

int main() {
    int failed = 0;
    for (const auto& test : Tests()) {
        try {
            test.run();
            std::cout << "PASS " << test.name << "\n";
        } catch (const Failure& failure) {
            ++failed;
            std::cout << "FAIL " << test.name << ": " << failure.message << "\n";
        }
    }
    std::cout << Tests().size() - failed << "/" << Tests().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}
