// The main window: a WebView2 showing the web UI from the "ui" folder. The
// page finds the apps on GitHub; this side installs, removes and starts them,
// and keeps Vitool Box in the notification area when it starts with Windows.
#pragma once

#include <windows.h>
#include <shellapi.h>
#include <wrl.h>

#include <WebView2.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "app/tray_group.h"
#include "app/update_window.h"
#include "core/json.h"

namespace vitool {

class HubWindow {
public:
    static constexpr wchar_t kClassName[] = L"VitomDev.VitoolBox";
    // Sent by a second instance to bring this one to the front.
    static constexpr UINT kShowMessage = WM_APP + 10;

    // hidden: started with Windows, only the notification area icon shows.
    // Takes the single-instance mutex: a self-update lets go of it early.
    bool Create(HINSTANCE instance, bool hidden, HANDLE instanceMutex);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateWebView();
    void ResizeWebView();
    void OnWebMessage(const std::string& json);
    void Post(const std::string& json);
    void ShowHub();
    void Quit();
    // Runs an install or uninstall on a worker thread.
    void StartOperation(const JsonObject& message, bool install);
    void StartSelfUpdateOperation(std::wstring url, std::string sha256, std::wstring fileName);
    void InstallSelfUpdate(const std::filesystem::path& setup);
    void FinishSelfUpdate(bool installed);
    void StartBackup(const std::string& spec, bool restore);

    void SetTrayVisible(bool visible);
    void UpdateTrayIcon();
    void ShowTrayMenu();
    void Notify(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_USER | NIIF_LARGE_ICON, HICON icon = nullptr);
    void SetGroupIcons(bool enabled);
    void OnTrayClick();

    HWND window_ = nullptr;
    HANDLE instanceMutex_ = nullptr;
    UpdateWindow updateWindow_;
    TrayGroup trayGroup_;
    HINSTANCE instance_ = nullptr;
    bool darkMode_ = false;
    bool startedHidden_ = false;
    bool keepInTray_ = false;   // "Start with Windows": closing hides the window
    bool groupIcons_ = false;   // también se queda en segundo plano
    bool trayVisible_ = false;
    int updateCount_ = 0;
    int runningOperations_ = 0;
    UINT taskbarCreated_ = 0;
    HICON icon_ = nullptr;
    HICON badgeIcon_ = nullptr;
    std::map<std::string, std::wstring> strings_;   // tray and dialog texts in the UI language
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> cancels_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webView_;
};

}  // namespace vitool
