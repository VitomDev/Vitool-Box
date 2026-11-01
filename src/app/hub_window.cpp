#include "app/hub_window.h"

#include <dwmapi.h>
#include <shobjidl.h>

#include <cmath>
#include <thread>
#include <vector>

#include "app/resource.h"
#include "core/backup.h"
#include "core/packages.h"
#include "core/paths.h"
#include "core/processes.h"
#include "core/startup.h"
#include "core/text.h"
#include "version.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace vitool {
namespace {

constexpr UINT WM_APP_POST = WM_APP + 1;
constexpr UINT WM_APP_OPERATION_DONE = WM_APP + 2;
constexpr UINT WM_APP_QUIT_FOR_UPDATE = WM_APP + 3;
constexpr UINT WM_APP_TRAY = WM_APP + 4;
constexpr UINT WM_APP_UPDATE_PROGRESS = WM_APP + 5;
constexpr UINT WM_APP_UPDATE_DOWNLOADED = WM_APP + 6;
constexpr UINT WM_APP_UPDATE_INSTALLED = WM_APP + 7;
constexpr UINT kTrayId = 1;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuCheck = 2;
constexpr UINT kMenuQuit = 3;
constexpr wchar_t kSelfUpdatePrefix[] = L"https://github.com/VitomDev/Vitool-Box/releases/download/";
constexpr wchar_t kHost[] = L"hub.vitool";
constexpr wchar_t kStartPage[] = L"https://hub.vitool/index.html";

bool SystemUsesDarkMode() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

bool IsWebUrl(std::wstring_view url) {
    const auto lower = ToLower(url);
    return StartsWith(lower, L"https://") || StartsWith(lower, L"http://");
}

void OpenInBrowser(const std::wstring& url) {
    if (IsWebUrl(url)) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string Field(const JsonObject& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? std::string() : it->second;
}

// Posts JSON to the page from a worker thread.
auto PosterFor(HWND window) {
    return [window](std::string json) {
        auto* heap = new std::string(std::move(json));
        if (!PostMessageW(window, WM_APP_POST, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
    };
}

std::string JoinUtf8(const std::vector<std::wstring>& items) {
    std::string result;
    for (const auto& item : items) result += (result.empty() ? "" : ",") + WideToUtf8(item);
    return result;
}

// The app icon with a dot in the corner: updates are waiting.
HICON MakeBadgeIcon(HICON base) {
    const int size = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, color);
    DrawIconEx(dc, 0, 0, base, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    DeleteDC(dc);

    auto* pixels = static_cast<uint32_t*>(bits);
    const double radius = size * 0.24;
    const double center = size - radius - 0.5;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double distance = std::hypot(x - center, y - center);
            if (distance <= radius + 1.2) pixels[y * size + x] = distance <= radius ? 0xFFE8485Au : 0xFFFFFFFFu;
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO iconInfo{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

std::filesystem::path PickZip(HWND owner, bool save, const std::wstring& suggestedName) {
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    const COMDLG_FILTERSPEC filter[] = {{L"Zip", L"*.zip"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetDefaultExtension(L"zip");
    if (save) dialog->SetFileName(suggestedName.c_str());
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::filesystem::path OwnExecutable() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::filesystem::path MovedExecutable() {
    auto path = OwnExecutable();
    return path += L".old";
}

// Another Vitool Box process has its main window up.
bool NewHubIsRunning() {
    struct Search { DWORD self; bool found; } search{GetCurrentProcessId(), false};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto* search = reinterpret_cast<Search*>(data);
        wchar_t name[64]{};
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != search->self && GetClassNameW(window, name, 64) && wcscmp(name, HubWindow::kClassName) == 0) {
            search->found = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

}  // namespace

bool HubWindow::Create(HINSTANCE instance, bool hidden, HANDLE instanceMutex) {
    CleanDownloads();
    instanceMutex_ = instanceMutex;
    // Left behind by the last self-update.
    std::error_code ignored;
    std::filesystem::remove(MovedExecutable(), ignored);
    instance_ = instance;
    startedHidden_ = hidden;
    keepInTray_ = StartsWithWindows();
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    badgeIcon_ = MakeBadgeIcon(icon_);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hIconSm = icon_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    darkMode_ = SystemUsesDarkMode();
    windowClass.hbrBackground = CreateSolidBrush(darkMode_ ? RGB(32, 32, 32) : RGB(243, 243, 243));
    if (!RegisterClassExW(&windowClass)) return false;

    // Centered on the primary monitor's work area, scaled to its DPI.
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const UINT dpi = GetDpiForSystem();
    const int width = std::min<int>(MulDiv(1120, static_cast<int>(dpi), 96), workArea.right - workArea.left);
    const int height = std::min<int>(MulDiv(760, static_cast<int>(dpi), 96), workArea.bottom - workArea.top);
    const int x = workArea.left + (workArea.right - workArea.left - width) / 2;
    const int y = workArea.top + (workArea.bottom - workArea.top - height) / 2;

    window_ = CreateWindowExW(0, kClassName, L"Vitool Box", WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr, instance, this);
    if (!window_) return false;

    const BOOL dark = darkMode_;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    if (!hidden) ShowWindow(window_, SW_SHOWNORMAL);
    SetTrayVisible(keepInTray_);
    CreateWebView();
    return true;
}

LRESULT CALLBACK HubWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<HubWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<HubWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT HubWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreated_ && trayVisible_) {
        // Explorer restarted: the icon has to be added again.
        trayVisible_ = false;
        SetTrayVisible(true);
        return 0;
    }
    switch (message) {
        case kShowMessage:
            ShowHub();
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ShowHub();
                    break;
                case NIN_BALLOONUSERCLICK:
                    ShowHub();
                    Post(R"({"type":"showUpdates"})");
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu();
                    break;
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kMenuOpen) ShowHub();
            if (LOWORD(wParam) == kMenuCheck) Post(R"({"type":"checkUpdates"})");
            if (LOWORD(wParam) == kMenuQuit) Quit();
            return 0;
        case WM_SIZE:
            ResizeWebView();
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            const UINT dpi = GetDpiForWindow(window_);
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {MulDiv(560, static_cast<int>(dpi), 96), MulDiv(480, static_cast<int>(dpi), 96)};
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
                darkMode_ = SystemUsesDarkMode();
                const BOOL dark = darkMode_;
                DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            }
            return 0;
        case WM_APP_POST: {
            std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lParam));
            Post(*json);
            return 0;
        }
        case WM_APP_OPERATION_DONE:
            --runningOperations_;
            return 0;
        case WM_APP_QUIT_FOR_UPDATE:
            DestroyWindow(window_);
            return 0;
        case WM_APP_UPDATE_PROGRESS:
            updateWindow_.SetProgress(static_cast<int>(wParam) * 0.75, strings_["updDownload"]);
            return 0;
        case WM_APP_UPDATE_DOWNLOADED: {
            std::unique_ptr<std::filesystem::path> setup(reinterpret_cast<std::filesystem::path*>(lParam));
            InstallSelfUpdate(*setup);
            return 0;
        }
        case WM_APP_UPDATE_INSTALLED:
            FinishSelfUpdate(wParam != 0);
            return 0;
        case WM_CLOSE:
            if (keepInTray_) {
                ShowWindow(window_, SW_HIDE);
                if (controller_) controller_->put_IsVisible(FALSE);
                return 0;
            }
            Quit();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            // Windows is shutting down, or an installer asked Vitool Box to
            // close (Restart Manager): quit even when kept in the tray.
            if (wParam) DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            SetTrayVisible(false);
            if (controller_) controller_->Close();
            if (instanceMutex_) CloseHandle(instanceMutex_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

void HubWindow::CreateWebView() {
    const auto userData = DataDir() / L"WebView2";
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment) {
                    MessageBoxW(window_, L"Vitool Box could not start its window (WebView2).", L"Vitool Box", MB_ICONERROR);
                    return S_OK;
                }
                return environment->CreateCoreWebView2Controller(window_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) return S_OK;
                            controller_ = controller;
                            controller_->get_CoreWebView2(&webView_);

                            ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(controller_.As(&controller2))) {
                                const COREWEBVIEW2_COLOR background = darkMode_ ? COREWEBVIEW2_COLOR{255, 32, 32, 32}
                                                                                : COREWEBVIEW2_COLOR{255, 243, 243, 243};
                                controller2->put_DefaultBackgroundColor(background);
                            }

                            ComPtr<ICoreWebView2Settings> settings;
                            webView_->get_Settings(&settings);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
                            settings->put_AreDevToolsEnabled(FALSE);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
#endif

                            ComPtr<ICoreWebView2_3> webView3;
                            if (SUCCEEDED(webView_.As(&webView3))) {
                                webView3->SetVirtualHostNameToFolderMapping(kHost, (ExecutableDir() / L"ui").c_str(),
                                                                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                            }

                            // Anything outside the hub page opens in the user's browser.
                            webView_->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        args->get_Uri(&uri);
                                        const std::wstring target = uri ? uri : L"";
                                        CoTaskMemFree(uri);
                                        if (!StartsWith(target, L"https://hub.vitool/")) {
                                            args->put_Cancel(TRUE);
                                            OpenInBrowser(target);
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);
                            webView_->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        args->get_Uri(&uri);
                                        args->put_Handled(TRUE);
                                        if (uri) OpenInBrowser(uri);
                                        CoTaskMemFree(uri);
                                        return S_OK;
                                    }).Get(),
                                nullptr);
                            webView_->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR source = nullptr;
                                        args->get_Source(&source);
                                        const bool fromHub = source && StartsWith(source, L"https://hub.vitool/");
                                        CoTaskMemFree(source);
                                        LPWSTR message = nullptr;
                                        if (fromHub && SUCCEEDED(args->get_WebMessageAsJson(&message)) && message) {
                                            OnWebMessage(WideToUtf8(message));
                                        }
                                        CoTaskMemFree(message);
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            // A hidden page lets the UI know it runs in the background.
                            controller_->put_IsVisible(IsWindowVisible(window_));
                            ResizeWebView();
                            webView_->Navigate(kStartPage);
                            return S_OK;
                        }).Get());
            }).Get());
}

void HubWindow::ResizeWebView() {
    if (!controller_) return;
    RECT bounds{};
    GetClientRect(window_, &bounds);
    controller_->put_Bounds(bounds);
}

void HubWindow::Post(const std::string& json) {
    if (webView_) webView_->PostWebMessageAsJson(Utf8ToWide(json).c_str());
}

void HubWindow::OnWebMessage(const std::string& json) {
    const auto message = ParseFlatObject(json);
    if (!message) return;
    const auto type = Field(*message, "type");
    const auto appId = Field(*message, "appId");

    if (type == "hello") {
        Post(R"({"type":"hello","version":)" + JsonQuote(VITOOL_VERSION_STRING) + R"(,"startup":)" + JsonQuote(keepInTray_ ? "1" : "0") +
             R"(,"hidden":)" + JsonQuote(startedHidden_ ? "1" : "0") + "}");
    } else if (type == "strings") {
        for (const char* key : {"closeBusy", "trayOpen", "trayCheck", "trayQuit", "trayTip"}) strings_[key] = Utf8ToWide(Field(*message, key));
        UpdateTrayIcon();
    } else if (type == "startup") {
        const bool enabled = Field(*message, "enabled") == "1";
        if (SetStartWithWindows(enabled)) keepInTray_ = enabled;
        SetTrayVisible(keepInTray_);
        Post(R"({"type":"startup","enabled":)" + JsonQuote(keepInTray_ ? "1" : "0") + "}");
    } else if (type == "updates") {
        updateCount_ = std::atoi(Field(*message, "count").c_str());
        strings_["trayTip"] = Utf8ToWide(Field(*message, "tip"));
        UpdateTrayIcon();
    } else if (type == "notify") {
        Notify(Utf8ToWide(Field(*message, "title")), Utf8ToWide(Field(*message, "text")));
    } else if (type == "state") {
        // "ids" is a comma-separated list of AppIds.
        std::string apps;
        const auto ids = Field(*message, "ids");
        size_t start = 0;
        while (start <= ids.size()) {
            auto end = ids.find(',', start);
            if (end == std::string::npos) end = ids.size();
            const auto id = ids.substr(start, end - start);
            if (const auto installed = FindInstalled(Utf8ToWide(id))) {
                if (!apps.empty()) apps += ',';
                apps += JsonQuote(id) + R"(:{"version":)" + JsonQuote(WideToUtf8(installed->version)) + "}";
            }
            start = end + 1;
        }
        Post(R"({"type":"state","apps":{)" + apps + "}}");
    } else if (type == "install" || type == "uninstall") {
        StartOperation(*message, type == "install");
    } else if (type == "cancel") {
        if (const auto it = cancels_.find(appId); it != cancels_.end()) *it->second = true;
    } else if (type == "selfUpdate") {
        for (const char* key : {"updTitle", "updSubtitle", "updDownload", "updInstall", "updOpen"}) strings_[key] = Utf8ToWide(Field(*message, key));
        StartSelfUpdateOperation(Utf8ToWide(Field(*message, "url")), Field(*message, "sha256"), Utf8ToWide(Field(*message, "fileName")));
    } else if (type == "launch") {
        const auto installed = FindInstalled(Utf8ToWide(appId));
        const auto exe = Utf8ToWide(Field(*message, "exe"));
        if (installed && !installed->location.empty() && IsSafeFileName(exe)) {
            const auto program = installed->location / exe;
            ShellExecuteW(window_, L"open", program.c_str(), nullptr, installed->location.c_str(), SW_SHOWNORMAL);
        }
    } else if (type == "openFolder") {
        const auto installed = FindInstalled(Utf8ToWide(appId));
        std::error_code ignored;
        if (installed && std::filesystem::is_directory(installed->location, ignored)) {
            ShellExecuteW(window_, L"open", installed->location.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (type == "backup" || type == "restore") {
        StartBackup(Field(*message, "spec"), type == "restore");
    } else if (type == "openUrl") {
        OpenInBrowser(Utf8ToWide(Field(*message, "url")));
    }
}

void HubWindow::StartOperation(const JsonObject& message, bool install) {
    const auto appId = Field(message, "appId");
    const auto url = Utf8ToWide(Field(message, "url"));
    const auto sha256 = Field(message, "sha256");
    const auto fileName = Utf8ToWide(Field(message, "fileName"));
    const auto exe = Utf8ToWide(Field(message, "exe"));
    const bool closeApp = Field(message, "closeApp") == "1";
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancels_[appId] = cancel;
    ++runningOperations_;
    HWND window = window_;
    std::thread([=] {
        const auto post = PosterFor(window);
        const auto id = Utf8ToWide(appId);
        const std::string action = install ? "install" : "uninstall";
        std::wstring error;

        // An installer can't replace the files of a program that is open.
        bool reopen = false;
        if (const auto installed = FindInstalled(id); installed && IsRunningFrom(installed->location)) {
            if (!closeApp) {
                post(R"({"type":"running","appId":)" + JsonQuote(appId) + R"(,"action":)" + JsonQuote(action) + "}");
                PostMessageW(window, WM_APP_OPERATION_DONE, 0, 0);
                return;
            }
            if (!CloseProgramsIn(installed->location)) error = L"closeFailed";
            reopen = install;
        }

        if (error.empty() && install) {
            int lastPercent = -1;
            error = InstallApp(id, url, sha256, fileName, [&](uint64_t received, uint64_t total) {
                const int percent = total ? static_cast<int>(received * 100 / total) : -1;
                if (percent != lastPercent) {
                    lastPercent = percent;
                    post(R"({"type":"progress","appId":)" + JsonQuote(appId) + R"(,"percent":")" + std::to_string(percent) + "\"}");
                }
                return !*cancel;
            });
        } else if (error.empty()) {
            error = UninstallApp(id);
        }

        if (error.empty() && reopen && IsSafeFileName(exe)) {
            // Some installers start their app again by themselves.
            Sleep(1500);
            if (const auto installed = FindInstalled(id); installed && !IsRunningFrom(installed->location)) {
                const auto program = installed->location / exe;
                ShellExecuteW(nullptr, L"open", program.c_str(), nullptr, installed->location.c_str(), SW_SHOWNORMAL);
            }
        }
        post(R"({"type":"done","appId":)" + JsonQuote(appId) + R"(,"action":)" + JsonQuote(action) + R"(,"error":)" +
             JsonQuote(WideToUtf8(error)) + "}");
        PostMessageW(window, WM_APP_OPERATION_DONE, 0, 0);
    }).detach();
}

void HubWindow::StartSelfUpdateOperation(std::wstring url, std::string sha256, std::wstring fileName) {
    if (!StartsWith(url, kSelfUpdatePrefix)) {
        Post(R"({"type":"done","appId":"hub","action":"selfUpdate","error":"notHubRelease"})");
        return;
    }
    // The floating card takes over from the main window until the new version is up.
    if (updateWindow_.Create(instance_, darkMode_, strings_["updTitle"], strings_["updSubtitle"])) {
        updateWindow_.SetProgress(0, strings_["updDownload"]);
        ShowWindow(window_, SW_HIDE);
    }
    HWND window = window_;
    std::thread([=] {
        std::filesystem::path setup;
        int lastPercent = -1;
        const auto error = DownloadSelfUpdate(url, sha256, fileName, [&](uint64_t received, uint64_t total) {
            const int percent = total ? static_cast<int>(received * 100 / total) : 0;
            if (percent != lastPercent) {
                lastPercent = percent;
                PostMessageW(window, WM_APP_UPDATE_PROGRESS, percent, 0);
            }
            return true;
        }, setup);
        if (error.empty()) {
            PostMessageW(window, WM_APP_UPDATE_DOWNLOADED, 0, reinterpret_cast<LPARAM>(new std::filesystem::path(setup)));
        } else {
            PosterFor(window)(R"({"type":"done","appId":"hub","action":"selfUpdate","error":)" + JsonQuote(WideToUtf8(error)) + "}");
            PostMessageW(window, WM_APP_UPDATE_INSTALLED, 0, 0);
        }
    }).detach();
}

void HubWindow::InstallSelfUpdate(const std::filesystem::path& setup) {
    updateWindow_.Creep(75, 96, strings_["updInstall"]);
    // Let go of everything the installer replaces or waits for: the UI files,
    // the single-instance mutex and, by renaming it, the running executable.
    if (controller_) controller_->Close();
    controller_.Reset();
    webView_.Reset();
    if (instanceMutex_) {
        ReleaseMutex(instanceMutex_);
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
    SetTrayVisible(false);
    MoveFileExW(OwnExecutable().c_str(), MovedExecutable().c_str(), MOVEFILE_REPLACE_EXISTING);
    HWND window = window_;
    std::thread([=] {
        const bool installed = RunSelfUpdate(setup).empty();
        PostMessageW(window, WM_APP_UPDATE_INSTALLED, installed ? 1 : 0, 0);
    }).detach();
}

void HubWindow::FinishSelfUpdate(bool installed) {
    if (webView_) {
        // The download failed: nothing was touched, carry on.
        updateWindow_.Close();
        ShowHub();
        return;
    }
    if (installed) {
        updateWindow_.SetProgress(100, strings_["updOpen"]);
    } else {
        // Put the old version back and start it again.
        std::error_code ignored;
        if (!std::filesystem::exists(OwnExecutable(), ignored)) MoveFileExW(MovedExecutable().c_str(), OwnExecutable().c_str(), 0);
        ShellExecuteW(nullptr, L"open", OwnExecutable().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    HWND window = window_;
    std::thread([=] {
        // The installer starts the new version; stay until its window is up.
        for (int attempt = 0; installed && attempt < 75 && !NewHubIsRunning(); ++attempt) Sleep(200);
        Sleep(installed ? 600 : 0);
        PostMessageW(window, WM_APP_QUIT_FOR_UPDATE, 0, 0);
    }).detach();
}

void HubWindow::StartBackup(const std::string& spec, bool restore) {
    const std::string type = restore ? "restoreDone" : "backupDone";
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[64];
    swprintf_s(name, L"Vitool Box settings %04u-%02u-%02u.zip", now.wYear, now.wMonth, now.wDay);
    const auto zip = PickZip(window_, !restore, name);
    if (zip.empty()) {
        Post(R"({"type":)" + JsonQuote(type) + R"(,"error":"","apps":"","cancelled":"1"})");
        return;
    }
    ++runningOperations_;
    HWND window = window_;
    std::thread([=] {
        const auto apps = ParseBackupSpec(Utf8ToWide(spec));
        std::vector<std::wstring> done;
        std::wstring error;
        if (!restore) {
            error = BackupSettings(apps, zip, done);
        } else {
            // An open app would write its old settings back when it quits:
            // close it first and start it again afterwards.
            bool reopen = false;
            error = RestoreSettings(zip, apps,
                [&](const BackupApp& app) {
                    const auto installed = FindInstalled(app.appId);
                    reopen = installed && IsRunningFrom(installed->location) && CloseProgramsIn(installed->location);
                },
                [&](const BackupApp& app) {
                    const auto installed = FindInstalled(app.appId);
                    if (!reopen || !installed || app.exe.empty()) return;
                    const auto program = installed->location / app.exe;
                    ShellExecuteW(nullptr, L"open", program.c_str(), nullptr, installed->location.c_str(), SW_SHOWNORMAL);
                },
                done);
        }
        PosterFor(window)(R"({"type":)" + JsonQuote(type) + R"(,"error":)" + JsonQuote(WideToUtf8(error)) + R"(,"apps":)" + JsonQuote(JoinUtf8(done)) + "}");
        PostMessageW(window, WM_APP_OPERATION_DONE, 0, 0);
    }).detach();
}

void HubWindow::ShowHub() {
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    if (controller_) controller_->put_IsVisible(TRUE);
    SetForegroundWindow(window_);
}

void HubWindow::Quit() {
    // Quitting would cut an installer off halfway.
    if (runningOperations_ > 0) {
        const auto text = strings_["closeBusy"].empty() ? std::wstring(L"An app is still being installed or removed. Close Vitool Box anyway?")
                                                        : strings_["closeBusy"];
        if (MessageBoxW(window_, text.c_str(), L"Vitool Box", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    DestroyWindow(window_);
}

void HubWindow::SetTrayVisible(bool visible) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = kTrayId;
    if (!visible) {
        if (trayVisible_) Shell_NotifyIconW(NIM_DELETE, &data);
        trayVisible_ = false;
        return;
    }
    if (trayVisible_) return;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = WM_APP_TRAY;
    data.hIcon = icon_;
    wcscpy_s(data.szTip, L"Vitool Box");
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        trayVisible_ = true;
        UpdateTrayIcon();
    }
}

void HubWindow::UpdateTrayIcon() {
    if (!trayVisible_) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = kTrayId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.hIcon = updateCount_ > 0 && badgeIcon_ ? badgeIcon_ : icon_;
    const auto& tip = strings_["trayTip"];
    wcsncpy_s(data.szTip, tip.empty() ? L"Vitool Box" : tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void HubWindow::ShowTrayMenu() {
    auto text = [this](const char* key, const wchar_t* fallback) {
        const auto& value = strings_[key];
        return value.empty() ? std::wstring(fallback) : value;
    };
    const auto open = text("trayOpen", L"Open Vitool Box");
    const auto check = text("trayCheck", L"Check for updates");
    const auto quit = text("trayQuit", L"Quit");
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, open.c_str());
    AppendMenuW(menu, MF_STRING, kMenuCheck, check.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, quit.c_str());
    SetMenuDefaultItem(menu, kMenuOpen, FALSE);
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, window_, nullptr);
    DestroyMenu(menu);
}

void HubWindow::Notify(const std::wstring& title, const std::wstring& text) {
    if (!trayVisible_) return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window_;
    data.uID = kTrayId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    data.hBalloonIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

}  // namespace vitool
