// Vitool Box's automated tests: a minimal runner without dependencies.
//
//   VitoolBoxTests.exe

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
