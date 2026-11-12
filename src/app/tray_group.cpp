#include "app/tray_group.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>

#include "app/resource.h"
#include "app/vitool_tray.h"

namespace vitool {
namespace {

struct Colors {
    COLORREF background, hover, text, muted, border;
};

Colors ColorsFor(bool dark) {
    return dark ? Colors{RGB(43, 43, 43), RGB(56, 56, 56), RGB(243, 243, 243), RGB(176, 176, 176), RGB(58, 58, 58)}
                : Colors{RGB(255, 255, 255), RGB(240, 240, 240), RGB(27, 27, 27), RGB(95, 95, 95), RGB(229, 229, 229)};
}

void PostChanged(bool on) {
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(vitool_tray::kChangedMessage), on ? 1 : 0, 0);
}

std::wstring OneLine(std::wstring text) {
    std::replace(text.begin(), text.end(), L'\r', L' ');
    for (size_t at = text.find(L'\n'); at != std::wstring::npos; at = text.find(L'\n', at)) text.replace(at, 1, L" · ");
    return text;
}

}  // namespace

bool TrayGroup::Start(HINSTANCE instance, bool darkMode) {
    if (window_) return true;
    instance_ = instance;
    darkMode_ = darkMode;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = vitool_tray::kHostClass;
        registered = RegisterClassExW(&windowClass) != 0;
        if (!registered) return false;
    }
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, vitool_tray::kHostClass, L"Vitool Box", WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!window_) return false;
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    SetDarkMode(darkMode);
    PostChanged(true);
    return true;
}

void TrayGroup::Stop() {
    if (!window_) return;
    DestroyWindow(window_);
    window_ = nullptr;
    for (auto& entry : entries_) DestroyIcon(entry.icon);
    entries_.clear();
    balloonOwner_ = nullptr;
    // Las apps vuelven a poner su propio icono.
    PostChanged(false);
}

bool TrayGroup::Empty() {
    RemoveDeadEntries();
    return entries_.empty();
}

void TrayGroup::SetDarkMode(bool darkMode) {
    darkMode_ = darkMode;
    if (!window_) return;
    const BOOL dark = darkMode;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    InvalidateRect(window_, nullptr, FALSE);
}

void TrayGroup::SetTexts(std::wstring open, std::wstring hint) {
    if (!open.empty()) openText_ = std::move(open);
    if (!hint.empty()) hintText_ = std::move(hint);
}

LRESULT CALLBACK TrayGroup::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* self = reinterpret_cast<TrayGroup*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self && self->window_ ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TrayGroup::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COPYDATA:
            return OnIconMessage(reinterpret_cast<HWND>(wParam), reinterpret_cast<const COPYDATASTRUCT*>(lParam));
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) Hide();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) Hide();
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
                tracking_ = TrackMouseEvent(&track) != FALSE;
            }
            const int row = RowAt(GET_Y_LPARAM(lParam));
            if (row != hover_) {
                hover_ = row;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            hover_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            Click(RowAt(GET_Y_LPARAM(lParam)), message == WM_RBUTTONUP);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window_, &paint);
            RECT client;
            GetClientRect(window_, &client);
            HDC buffer = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ old = SelectObject(buffer, bitmap);
            Paint(buffer);
            BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, old);
            DeleteObject(bitmap);
            DeleteDC(buffer);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_DESTROY:
            if (font_) DeleteObject(font_);
            if (smallFont_) DeleteObject(smallFont_);
            if (hubIcon_) DestroyIcon(hubIcon_);
            font_ = smallFont_ = nullptr;
            hubIcon_ = nullptr;
            return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT TrayGroup::OnIconMessage(HWND sender, const COPYDATASTRUCT* copy) {
    if (!copy || copy->dwData != vitool_tray::kCopyDataId || copy->cbData != sizeof(vitool_tray::IconMessage)) return 0;
    const auto& message = *static_cast<const vitool_tray::IconMessage*>(copy->lpData);
    DWORD owner = 0;
    GetWindowThreadProcessId(sender, &owner);
    if (reinterpret_cast<HWND>(message.window) != sender || owner != message.processId) return 0;

    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const Entry& entry) { return entry.window == sender && entry.id == message.id; });
    if (message.command == NIM_DELETE) {
        if (it != entries_.end()) {
            DestroyIcon(it->icon);
            entries_.erase(it);
        }
    } else {
        if (message.command == NIM_ADD) {
            RemoveDeadEntries();
            it = std::find_if(entries_.begin(), entries_.end(),
                              [&](const Entry& entry) { return entry.window == sender && entry.id == message.id; });
            if (it == entries_.end()) {
                Entry entry;
                entry.processId = owner;
                entry.window = sender;
                entry.id = message.id;
                wchar_t name[128]{};
                GetWindowTextW(sender, name, ARRAYSIZE(name));
                entry.name = name;
                entries_.push_back(std::move(entry));
                std::sort(entries_.begin(), entries_.end(),
                          [](const Entry& a, const Entry& b) { return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0; });
                it = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& entry) { return entry.window == sender && entry.id == message.id; });
            }
        } else if (it == entries_.end()) {
            return 0;
        }
        // Copia del icono: la app puede destruir el suyo después de enviarlo.
        if (message.command == NIM_ADD || (message.flags & NIF_ICON)) {
            if (it->icon) DestroyIcon(it->icon);
            it->icon = message.icon ? CopyIcon(reinterpret_cast<HICON>(message.icon)) : nullptr;
        }
        if (message.command == NIM_ADD || (message.flags & NIF_TIP)) {
            const auto tip = OneLine(message.tip);
            it->tip = tip == it->name ? std::wstring() : tip;
        }
        if (message.command == NIM_ADD || (message.flags & NIF_MESSAGE)) it->callbackMessage = message.callbackMessage;
        if ((message.flags & NIF_INFO) && message.info[0] && onBalloon) {
            balloonOwner_ = sender;
            balloonId_ = message.id;
            HICON icon = (message.infoFlags & NIIF_USER) ? (message.balloonIcon ? reinterpret_cast<HICON>(message.balloonIcon) : it->icon) : nullptr;
            onBalloon(message.infoTitle, message.info, message.infoFlags, icon);
        }
    }
    if (IsWindowVisible(window_)) {
        if (message.command == NIM_MODIFY) InvalidateRect(window_, nullptr, FALSE);
        else Hide();
    }
    return 1;
}

void TrayGroup::RemoveDeadEntries() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (IsWindow(it->window)) {
            ++it;
        } else {
            DestroyIcon(it->icon);
            it = entries_.erase(it);
        }
    }
}

bool TrayGroup::ForwardBalloon(UINT event) {
    if (!balloonOwner_) return false;
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& entry) { return entry.window == balloonOwner_ && entry.id == balloonId_; });
    if (it != entries_.end()) {
        if (event == NIN_BALLOONUSERCLICK) AllowSetForegroundWindow(it->processId);
        PostMessageW(it->window, it->callbackMessage, 0, MAKELPARAM(event, it->id));
    }
    if (event != NIN_BALLOONSHOW) balloonOwner_ = nullptr;
    return true;
}

// --- Panel ----------------------------------------------------------------------

namespace {

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

constexpr int kWidth = 280;
constexpr int kPadding = 6;
constexpr int kRow = 40;
constexpr int kSeparator = 9;
constexpr int kHint = 30;

}  // namespace

void TrayGroup::Layout() {
    if (font_) DeleteObject(font_);
    if (smallFont_) DeleteObject(smallFont_);
    if (hubIcon_) DestroyIcon(hubIcon_);
    font_ = CreateFontW(-Scale(14, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    smallFont_ = CreateFontW(-Scale(12, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi_);
    hubIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR));
}

int TrayGroup::RowAt(int y) const {
    const int padding = Scale(kPadding, dpi_);
    const int row = Scale(kRow, dpi_);
    const int count = static_cast<int>(entries_.size());
    if (y >= padding && y < padding + count * row) return (y - padding) / row;
    const int openTop = padding + count * row + Scale(kSeparator, dpi_);
    if (y >= openTop && y < openTop + row) return count;
    return -1;
}

void TrayGroup::ShowPanel(const RECT& anchor) {
    if (!window_) return;
    RemoveDeadEntries();
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpi_ = dpiX;
    Layout();

    const int count = static_cast<int>(entries_.size());
    const int width = Scale(kWidth, dpi_);
    const int height = Scale(kPadding * 2 + kSeparator + kHint, dpi_) + (count + 1) * Scale(kRow, dpi_);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const RECT work = info.rcWork;
    const int gap = Scale(12, dpi_);
    int x = (anchor.left + anchor.right) / 2 - width / 2;
    x = std::clamp<int>(x, work.left + gap, std::max<int>(work.left + gap, work.right - width - gap));
    // Encima del icono si la barra está abajo; debajo si está arriba.
    int y = anchor.top - height - gap;
    if (y < work.top) y = anchor.bottom + gap;
    y = std::clamp<int>(y, work.top + gap, std::max<int>(work.top + gap, work.bottom - height - gap));

    hover_ = -1;
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
    InvalidateRect(window_, nullptr, FALSE);
}

void TrayGroup::Hide() {
    if (window_ && IsWindowVisible(window_)) ShowWindow(window_, SW_HIDE);
}

void TrayGroup::Click(int row, bool menu) {
    const int count = static_cast<int>(entries_.size());
    if (row < 0) return;
    if (row == count) {
        Hide();
        if (onOpenHub) onOpenHub();
        return;
    }
    const Entry entry = entries_[row];
    if (!IsWindow(entry.window)) {
        Hide();
        return;
    }
    // Lo mismo que haría Windows al pinchar el icono de la app (NOTIFYICON_VERSION_4).
    POINT cursor{};
    GetCursorPos(&cursor);
    AllowSetForegroundWindow(entry.processId);
    Hide();
    PostMessageW(entry.window, entry.callbackMessage, MAKEWPARAM(cursor.x, cursor.y),
                 MAKELPARAM(menu ? WM_CONTEXTMENU : NIN_SELECT, entry.id));
}

void TrayGroup::Paint(HDC dc) {
    const auto colors = ColorsFor(darkMode_);
    RECT client;
    GetClientRect(window_, &client);
    HBRUSH background = CreateSolidBrush(colors.background);
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    const int padding = Scale(kPadding, dpi_);
    const int row = Scale(kRow, dpi_);
    const int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi_);
    const int textLeft = padding + Scale(14, dpi_) + iconSize + Scale(12, dpi_);
    const int textRight = client.right - padding - Scale(10, dpi_);
    const int count = static_cast<int>(entries_.size());

    auto drawRow = [&](int index, int top, HICON icon, const std::wstring& name, const std::wstring& tip) {
        if (index == hover_) {
            HBRUSH hover = CreateSolidBrush(colors.hover);
            HGDIOBJ oldBrush = SelectObject(dc, hover);
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            const int radius = Scale(8, dpi_);
            RoundRect(dc, padding, top, client.right - padding + 1, top + row + 1, radius, radius);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(hover);
        }
        if (icon) DrawIconEx(dc, padding + Scale(14, dpi_), top + (row - iconSize) / 2, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
        RECT nameRect{textLeft, top, textRight, top + row};
        SelectObject(dc, font_);
        SetTextColor(dc, colors.text);
        if (tip.empty()) {
            DrawTextW(dc, name.c_str(), -1, &nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            return;
        }
        nameRect.top += Scale(3, dpi_);
        nameRect.bottom = top + row / 2 + Scale(2, dpi_);
        DrawTextW(dc, name.c_str(), -1, &nameRect, DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT tipRect{textLeft, top + row / 2 + Scale(2, dpi_), textRight, top + row - Scale(3, dpi_)};
        SelectObject(dc, smallFont_);
        SetTextColor(dc, colors.muted);
        DrawTextW(dc, tip.c_str(), -1, &tipRect, DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX);
    };

    for (int i = 0; i < count; ++i) drawRow(i, padding + i * row, entries_[i].icon, entries_[i].name, entries_[i].tip);

    const int separator = padding + count * row + Scale(kSeparator, dpi_) / 2;
    RECT line{padding + Scale(8, dpi_), separator, client.right - padding - Scale(8, dpi_), separator + 1};
    HBRUSH border = CreateSolidBrush(colors.border);
    FillRect(dc, &line, border);
    DeleteObject(border);

    const int openTop = padding + count * row + Scale(kSeparator, dpi_);
    drawRow(count, openTop, hubIcon_, openText_, L"");

    RECT hint{padding + Scale(14, dpi_), openTop + row, textRight, client.bottom - padding};
    SelectObject(dc, smallFont_);
    SetTextColor(dc, colors.muted);
    DrawTextW(dc, hintText_.c_str(), -1, &hint, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

}  // namespace vitool
