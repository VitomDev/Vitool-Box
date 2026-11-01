#include "app/update_window.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#include "app/resource.h"

namespace vitool {
namespace {

constexpr wchar_t kClassName[] = L"VitomDev.VitoolBox.Update";
constexpr UINT_PTR kTimer = 1;
constexpr int kCardWidth = 420, kCardHeight = 128, kMargin = 28;
constexpr double kPi = 3.14159265358979;

ULONG_PTR gdiplusToken = 0;

Gdiplus::Color Rgb(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return Gdiplus::Color(a, r, g, b); }

void RoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    const float d = std::min({r * 2, w, h});
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

// The app icon with its alpha channel (Bitmap::FromHICON drops it).
Gdiplus::Bitmap* LoadIconBitmap(HINSTANCE instance, int size) {
    auto icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, 0));
    if (!icon) return nullptr;
    ICONINFO info{};
    GetIconInfo(icon, &info);
    BITMAPINFO bmi{};
    bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    auto* bitmap = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, size, size);
    bitmap->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data);
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, info.hbmColor, 0, size, data.Scan0, &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    bitmap->UnlockBits(&data);
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    DestroyIcon(icon);
    return bitmap;
}

}  // namespace

UpdateWindow::~UpdateWindow() {
    if (window_) DestroyWindow(window_);
    delete static_cast<Gdiplus::Bitmap*>(icon_);
}

bool UpdateWindow::Create(HINSTANCE instance, bool darkMode, std::wstring title, std::wstring subtitle) {
    if (!gdiplusToken) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok) return false;
    }
    darkMode_ = darkMode;
    title_ = std::move(title);
    subtitle_ = std::move(subtitle);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    scale_ = GetDpiForSystem() / 96.0f;
    icon_ = LoadIconBitmap(instance, static_cast<int>(40 * scale_));
    const int width = static_cast<int>((kCardWidth + kMargin * 2) * scale_);
    const int height = static_cast<int>((kCardHeight + kMargin * 2) * scale_);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    baseX_ = work.left + (work.right - work.left - width) / 2;
    baseY_ = work.top + (work.bottom - work.top - height) / 2;

    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST, kClassName, title_.c_str(), WS_POPUP, baseX_, baseY_, width, height,
                              nullptr, nullptr, instance, this);
    if (!window_) return false;
    start_ = GetTickCount64();
    Paint();
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    SetTimer(window_, kTimer, 16, nullptr);
    return true;
}

void UpdateWindow::SetProgress(double percent, std::wstring status) {
    target_ = std::clamp(percent, 0.0, 100.0);
    creepUntil_ = -1;
    status_ = std::move(status);
}

void UpdateWindow::Creep(double from, double until, std::wstring status) {
    target_ = std::max(target_, from);
    creepUntil_ = until;
    status_ = std::move(status);
}

void UpdateWindow::Close() {
    if (window_ && !closingSince_) closingSince_ = GetTickCount64();
}

LRESULT CALLBACK UpdateWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* self = reinterpret_cast<UpdateWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self) {
        switch (message) {
            case WM_TIMER:
                self->Tick();
                return 0;
            case WM_NCHITTEST:
                return HTCAPTION;   // drag it from anywhere
            case WM_CLOSE:
                return 0;           // closing it would not stop the update
            case WM_DESTROY:
                KillTimer(window, kTimer);
                self->window_ = nullptr;
                return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void UpdateWindow::Tick() {
    if (creepUntil_ >= 0 && target_ < creepUntil_) target_ = std::min(creepUntil_, target_ + (creepUntil_ - target_) * 0.004 + 0.002);
    shown_ += (target_ - shown_) * 0.12;
    if (closingSince_ && GetTickCount64() - closingSince_ > 260) {
        DestroyWindow(window_);
        return;
    }
    Paint();
}

void UpdateWindow::Paint() {
    using namespace Gdiplus;
    const ULONGLONG now = GetTickCount64();
    const double seconds = (now - start_) / 1000.0;
    // Fade and rise in, then float gently up and down.
    const double appear = std::min(1.0, seconds / 0.3);
    const double ease = 1 - std::pow(1 - appear, 3);
    double alpha = ease;
    if (closingSince_) alpha *= std::max(0.0, 1 - (now - closingSince_) / 250.0);
    const double offset = (1 - ease) * 12 + std::sin(seconds * 2 * kPi / 3.2) * 3;

    RECT bounds{};
    GetWindowRect(window_, &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous = SelectObject(memory, dib);
    {
        Bitmap canvas(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.ScaleTransform(scale_, scale_);
        g.TranslateTransform(static_cast<REAL>(kMargin), static_cast<REAL>(kMargin + offset - 3));

        const Color card = darkMode_ ? Rgb(28, 28, 31) : Rgb(255, 255, 255);
        const Color border = darkMode_ ? Rgb(52, 52, 58) : Rgb(224, 224, 230);
        const Color text = darkMode_ ? Rgb(242, 242, 245) : Rgb(26, 26, 30);
        const Color muted = darkMode_ ? Rgb(150, 150, 160) : Rgb(105, 105, 115);
        const Color track = darkMode_ ? Rgb(55, 55, 62) : Rgb(228, 228, 234);

        for (int i = 14; i > 0; --i) {
            GraphicsPath shadow;
            RoundedRect(shadow, -i * 0.8f, -i * 0.8f + 6, kCardWidth + i * 1.6f, kCardHeight + i * 1.6f, 14 + i * 0.8f);
            SolidBrush brush(Rgb(0, 0, 0, static_cast<BYTE>(darkMode_ ? 9 : 5)));
            g.FillPath(&brush, &shadow);
        }
        GraphicsPath body;
        RoundedRect(body, 0, 0, kCardWidth, kCardHeight, 14);
        SolidBrush cardBrush(card);
        g.FillPath(&cardBrush, &body);
        Pen borderPen(border, 1);
        g.DrawPath(&borderPen, &body);

        if (auto* icon = static_cast<Bitmap*>(icon_)) g.DrawImage(icon, RectF(20, 20, 40, 40));

        FontFamily family(L"Segoe UI");
        Font titleFont(&family, 16, FontStyleBold, UnitPixel);
        Font smallFont(&family, 12.5f, FontStyleRegular, UnitPixel);
        SolidBrush textBrush(text), mutedBrush(muted);
        StringFormat ellipsis;
        ellipsis.SetTrimming(StringTrimmingEllipsisCharacter);
        ellipsis.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(title_.c_str(), -1, &titleFont, RectF(72, 18, kCardWidth - 92, 24), &ellipsis, &textBrush);
        g.DrawString(subtitle_.c_str(), -1, &smallFont, RectF(72, 42, kCardWidth - 92, 20), &ellipsis, &mutedBrush);

        const int percent = static_cast<int>(std::lround(shown_));
        const std::wstring percentText = std::to_wstring(percent) + L" %";
        StringFormat right;
        right.SetAlignment(StringAlignmentFar);
        g.DrawString(status_.c_str(), -1, &smallFont, RectF(20, 84, kCardWidth - 110, 20), &ellipsis, &textBrush);
        g.DrawString(percentText.c_str(), -1, &smallFont, RectF(kCardWidth - 90, 84, 70, 20), &right, &mutedBrush);

        const REAL barWidth = kCardWidth - 40;
        GraphicsPath trackPath;
        RoundedRect(trackPath, 20, 106, barWidth, 6, 3);
        SolidBrush trackBrush(track);
        g.FillPath(&trackBrush, &trackPath);
        const REAL fill = static_cast<REAL>(barWidth * shown_ / 100);
        if (fill > 1) {
            GraphicsPath fillPath;
            RoundedRect(fillPath, 20, 106, std::max(fill, 6.0f), 6, 3);
            LinearGradientBrush gradient(PointF(20, 0), PointF(20 + barWidth, 0), Rgb(91, 94, 240), Rgb(6, 214, 160));
            g.FillPath(&gradient, &fillPath);
        }
    }

    POINT origin{bounds.left, bounds.top}, source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(alpha * 255), AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, screen, &origin, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, previous);
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

}  // namespace vitool
