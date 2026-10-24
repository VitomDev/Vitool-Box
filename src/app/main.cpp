#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <WebView2.h>

#include "app/hub_window.h"

namespace {

constexpr wchar_t kMutexName[] = L"VitomDev.VitoolBox.SingleInstance";
constexpr wchar_t kWebView2Download[] = L"https://developer.microsoft.com/microsoft-edge/webview2/";

bool HasWebView2Runtime() {
    LPWSTR version = nullptr;
    const bool found = SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version;
    CoTaskMemFree(version);
    return found;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(vitool::HubWindow::kClassName, nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    if (!HasWebView2Runtime()) {
        if (MessageBoxW(nullptr,
                        L"Vitool Box needs the Microsoft Edge WebView2 Runtime, which is not installed on this PC.\n\n"
                        L"Open the download page?",
                        L"Vitool Box", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            ShellExecuteW(nullptr, L"open", kWebView2Download, nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 1;
    }

    vitool::HubWindow window;
    if (!window.Create(instance)) return 1;

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
