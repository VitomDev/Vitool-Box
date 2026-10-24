#include "app/hub_window.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <thread>

#include "app/resource.h"
#include "core/json.h"
#include "core/packages.h"
#include "core/paths.h"
#include "core/text.h"
#include "version.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace vitool {
namespace {

constexpr UINT WM_APP_POST = WM_APP + 1;
constexpr UINT WM_APP_OPERATION_DONE = WM_APP + 2;
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

}  // namespace

bool HubWindow::Create(HINSTANCE instance) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hIconSm = windowClass.hIcon;
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
    ShowWindow(window_, SW_SHOWNORMAL);
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
    switch (message) {
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
        case WM_CLOSE:
            // Quitting would cut an installer off halfway.
            if (runningOperations_ > 0 &&
                MessageBoxW(window_, L"An app is still being installed or removed. Close Vitool Box anyway?", L"Vitool Box",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            if (controller_) controller_->Close();
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
        Post(R"({"type":"hello","version":)" + JsonQuote(VITOOL_VERSION_STRING) + "}");
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
    } else if (type == "install") {
        StartOperation(appId, true, Utf8ToWide(Field(*message, "url")), Field(*message, "sha256"), Utf8ToWide(Field(*message, "fileName")));
    } else if (type == "uninstall") {
        StartOperation(appId, false, {}, {}, {});
    } else if (type == "launch") {
        const auto installed = FindInstalled(Utf8ToWide(appId));
        const auto exe = Utf8ToWide(Field(*message, "exe"));
        if (installed && !installed->location.empty() && IsSafeFileName(exe)) {
            const auto program = installed->location / exe;
            ShellExecuteW(window_, L"open", program.c_str(), nullptr, installed->location.c_str(), SW_SHOWNORMAL);
        }
    } else if (type == "openUrl") {
        OpenInBrowser(Utf8ToWide(Field(*message, "url")));
    }
}

void HubWindow::StartOperation(const std::string& appId, bool install, std::wstring url, std::string sha256, std::wstring fileName) {
    ++runningOperations_;
    HWND window = window_;
    std::thread([=] {
        auto post = [window](std::string json) {
            auto* heap = new std::string(std::move(json));
            if (!PostMessageW(window, WM_APP_POST, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
        };
        const auto id = Utf8ToWide(appId);
        std::wstring error;
        if (install) {
            int lastPercent = -1;
            error = InstallApp(id, url, sha256, fileName, [&](uint64_t received, uint64_t total) {
                const int percent = total ? static_cast<int>(received * 100 / total) : -1;
                if (percent == lastPercent) return;
                lastPercent = percent;
                post(R"({"type":"progress","appId":)" + JsonQuote(appId) + R"(,"percent":")" + std::to_string(percent) + "\"}");
            });
        } else {
            error = UninstallApp(id);
        }
        post(R"({"type":"done","appId":)" + JsonQuote(appId) + R"(,"action":)" + JsonQuote(install ? "install" : "uninstall") +
             R"(,"error":)" + JsonQuote(WideToUtf8(error)) + "}");
        PostMessageW(window, WM_APP_OPERATION_DONE, 0, 0);
    }).detach();
}

}  // namespace vitool
