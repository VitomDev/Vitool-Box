// Los iconos de las apps agrupados dentro del de Vitool Box: los recibe de las
// apps (vitool_tray.h) y los muestra en un panel pequeño junto al icono.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace vitool {

class TrayGroup {
public:
    std::function<void(const std::wstring& title, const std::wstring& text, DWORD flags, HICON icon)> onBalloon;
    std::function<void()> onOpenHub;

    bool Start(HINSTANCE instance, bool darkMode);
    void Stop();
    bool Active() const { return window_ != nullptr; }
    bool Empty();
    void SetDarkMode(bool darkMode);
    void SetTexts(std::wstring open, std::wstring hint);
    void ShowPanel(const RECT& anchor);
    // Devuelve true si el aviso que se ha pulsado o cerrado era de una app.
    bool ForwardBalloon(UINT event);
    void ClearBalloon() { balloonOwner_ = nullptr; }

private:
    struct Entry {
        DWORD processId = 0;
        HWND window = nullptr;
        UINT id = 0;
        UINT callbackMessage = 0;
        HICON icon = nullptr;
        std::wstring name;
        std::wstring tip;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT OnIconMessage(HWND sender, const COPYDATASTRUCT* copy);
    void RemoveDeadEntries();
    void Paint(HDC dc);
    void Layout();
    int RowAt(int y) const;
    void Click(int row, bool menu);
    void Hide();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    HFONT smallFont_ = nullptr;
    HICON hubIcon_ = nullptr;
    bool darkMode_ = false;
    UINT dpi_ = 96;
    int hover_ = -1;
    bool tracking_ = false;
    HWND balloonOwner_ = nullptr;
    UINT balloonId_ = 0;
    std::wstring openText_ = L"Open Vitool Box";
    std::wstring hintText_ = L"Right-click an app for its menu";
    std::vector<Entry> entries_;
};

}  // namespace vitool
