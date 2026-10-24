// The main window: a WebView2 showing the web UI from the "ui" folder. The
// page finds the apps on GitHub; this side installs, removes and starts them.
#pragma once

#include <windows.h>
#include <wrl.h>

#include <WebView2.h>

#include <string>

namespace vitool {

class HubWindow {
public:
    static constexpr wchar_t kClassName[] = L"VitomDev.VitoolBox";

    bool Create(HINSTANCE instance);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateWebView();
    void ResizeWebView();
    void OnWebMessage(const std::string& json);
    void Post(const std::string& json);
    // Runs an install or uninstall on a worker thread.
    void StartOperation(const std::string& appId, bool install, std::wstring url, std::string sha256, std::wstring fileName);

    HWND window_ = nullptr;
    bool darkMode_ = false;
    int runningOperations_ = 0;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webView_;
};

}  // namespace vitool
