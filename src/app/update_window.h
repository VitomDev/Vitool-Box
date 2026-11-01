// The small floating card shown while Vitool Box updates itself. It lives on
// after the main window is gone, until the new version has started.
#pragma once

#include <windows.h>

#include <string>

namespace vitool {

class UpdateWindow {
public:
    UpdateWindow() = default;
    UpdateWindow(const UpdateWindow&) = delete;
    UpdateWindow& operator=(const UpdateWindow&) = delete;
    ~UpdateWindow();

    bool Create(HINSTANCE instance, bool darkMode, std::wstring title, std::wstring subtitle);
    // percent < 0 keeps the bar moving slowly towards "until".
    void SetProgress(double percent, std::wstring status);
    void Creep(double from, double until, std::wstring status);
    // Fades out and destroys the window.
    void Close();
    bool IsOpen() const { return window_ != nullptr; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Tick();
    void Paint();

    HWND window_ = nullptr;
    bool darkMode_ = true;
    std::wstring title_, subtitle_, status_;
    double target_ = 0, shown_ = 0, creepUntil_ = -1;
    ULONGLONG start_ = 0, closingSince_ = 0;
    int baseX_ = 0, baseY_ = 0;
    float scale_ = 1;
    void* icon_ = nullptr;   // Gdiplus::Bitmap
};

}  // namespace vitool
