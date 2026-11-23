#include "apps/tekito-settings/CompanionBubble.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace tekito::companion {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"TekitoCompanionBubble";
// In DIPs: the widest the text runs, the card's padding and corners, the
// gap to the companion, and the soft shadow around the card.
constexpr float kTextWidth = 260.0f;
constexpr float kPadX = 14.0f;
constexpr float kPadY = 10.0f;
constexpr float kLineGap = 2.0f;
constexpr float kRadius = 10.0f;
constexpr float kGap = 6.0f;
constexpr float kShadow = 8.0f;
constexpr float kTitleSize = 13.0f;
constexpr float kBodySize = 12.0f;
// Fading in and out, in seconds; a frame every this many milliseconds.
constexpr double kFadeIn = 0.15;
constexpr double kFadeOut = 0.3;
constexpr UINT kFrameMs = 16;
constexpr UINT_PTR kFadeTimer = 1;

bool DarkApps() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value == 0;
}

D2D1_COLOR_F Rgb(unsigned rgb, float alpha = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha);
}

}  // namespace

struct Bubble::Impl {
    HWND hwnd{nullptr};
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1DCRenderTarget> target;
    HDC memory{nullptr};
    HBITMAP bitmap{nullptr};
    HGDIOBJ previous{nullptr};
    POINT place{};
    SIZE size{};
    double shownAt{0};
    double hideAt{0};
    bool fadingOut{false};
    LARGE_INTEGER frequency{};

    double Now() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        switch (message) {
        case WM_TIMER:
            if (wParam == kFadeTimer) self->Tick();
            return 0;
        case WM_LBUTTONUP:
            self->StartFadeOut();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void StartFadeOut() {
        if (!hwnd || fadingOut) return;
        fadingOut = true;
        hideAt = Now();
    }

    // The card's opacity now; hidden once it has faded out.
    void Tick() {
        const double t = Now();
        float alpha = 1.0f;
        if (!fadingOut && t >= hideAt) {
            fadingOut = true;
            hideAt = t;
        }
        if (fadingOut) {
            alpha = static_cast<float>(1.0 - (t - hideAt) / kFadeOut);
            if (alpha <= 0.0f) {
                KillTimer(hwnd, kFadeTimer);
                ShowWindow(hwnd, SW_HIDE);
                return;
            }
        } else {
            alpha = static_cast<float>(std::min(1.0, (t - shownAt) / kFadeIn));
        }
        Blend(alpha);
    }

    void Blend(float alpha) {
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255)),
                            AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, nullptr, &place, &size, memory, &source, 0, &blend, ULW_ALPHA);
    }

    ComPtr<IDWriteTextLayout> Layout(const std::wstring& text, float points, DWRITE_FONT_WEIGHT weight, float scale) {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        if (text.empty() ||
            FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            points * scale, L"ja-jp", &format)) ||
            FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                                            kTextWidth * scale, 1000.0f * scale, &layout))) {
            return nullptr;
        }
        return layout;
    }

    void Show(const std::wstring& title, const std::wstring& body, const RECT& anchor, UINT dpi, double seconds) {
        const float scale = static_cast<float>(dpi ? dpi : 96) / 96.0f;
        const auto heading = Layout(title, kTitleSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, scale);
        const auto detail = Layout(body, kBodySize, DWRITE_FONT_WEIGHT_NORMAL, scale);
        if (!heading) return;
        DWRITE_TEXT_METRICS first{}, second{};
        heading->GetMetrics(&first);
        if (detail) detail->GetMetrics(&second);
        const float textWidth = std::max(first.widthIncludingTrailingWhitespace, second.widthIncludingTrailingWhitespace);
        const float textHeight = first.height + (detail ? kLineGap * scale + second.height : 0.0f);
        const float shadow = kShadow * scale;
        const float cardWidth = std::ceil(textWidth + 2 * kPadX * scale);
        const float cardHeight = std::ceil(textHeight + 2 * kPadY * scale);
        size = {static_cast<LONG>(cardWidth + 2 * shadow), static_cast<LONG>(cardHeight + 2 * shadow)};

        // Left of the companion, level with its middle; right of it when
        // there is no room on the left; kept on its screen.
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT& work = monitor.rcWork;
        const LONG gap = static_cast<LONG>(kGap * scale);
        LONG x = anchor.left - gap - size.cx + static_cast<LONG>(shadow);
        if (x < work.left) x = anchor.right + gap - static_cast<LONG>(shadow);
        LONG y = (anchor.top + anchor.bottom) / 2 - size.cy / 2;
        x = std::clamp(x, work.left, std::max(work.left, work.right - size.cx));
        y = std::clamp(y, work.top, std::max(work.top, work.bottom - size.cy));
        place = {x, y};

        if (bitmap) {
            SelectObject(memory, previous);
            DeleteObject(bitmap);
        }
        if (!memory) memory = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        previous = SelectObject(memory, bitmap);
        if (!target) {
            const auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(d2d->CreateDCRenderTarget(&properties, &target))) return;
        }
        const RECT bounds{0, 0, size.cx, size.cy};
        target->BindDC(memory, &bounds);
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0, 0, 0, 0));
        const bool dark = DarkApps();
        const D2D1_RECT_F card = D2D1::RectF(shadow, shadow, shadow + cardWidth, shadow + cardHeight);
        const float radius = kRadius * scale;
        // A soft shadow: rings going out, fainter each.
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &brush);
        for (int ring = 6; ring >= 1; --ring) {
            const float out = shadow * ring / 6.0f;
            brush->SetColor(D2D1::ColorF(0, 0, 0, (dark ? 0.06f : 0.025f) * (7 - ring) / 6.0f));
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(card.left - out, card.top - out + scale, card.right + out, card.bottom + out + scale),
                                  radius + out, radius + out),
                brush.Get());
        }
        brush->SetColor(dark ? Rgb(0x2c2c2c) : Rgb(0xffffff));
        target->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), brush.Get());
        brush->SetColor(dark ? Rgb(0xffffff, 0.09f) : Rgb(0x000000, 0.08f));
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(card.left + 0.5f, card.top + 0.5f, card.right - 0.5f,
                                                                   card.bottom - 0.5f),
                                                       radius, radius),
                                     brush.Get(), 1.0f);
        const D2D1_POINT_2F at = D2D1::Point2F(card.left + kPadX * scale, card.top + kPadY * scale);
        brush->SetColor(dark ? Rgb(0xffffff) : Rgb(0x1a1a1a));
        target->DrawTextLayout(at, heading.Get(), brush.Get());
        if (detail) {
            brush->SetColor(dark ? Rgb(0xffffff, 0.75f) : Rgb(0x1a1a1a, 0.7f));
            target->DrawTextLayout(D2D1::Point2F(at.x, at.y + first.height + kLineGap * scale), detail.Get(), brush.Get());
        }
        if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
            target.Reset();
            return;
        }
        SetWindowPos(hwnd, HWND_TOPMOST, place.x, place.y, size.cx, size.cy, SWP_NOACTIVATE);
        const bool wasShown = IsWindowVisible(hwnd) && !fadingOut;
        shownAt = wasShown ? Now() - kFadeIn : Now();
        hideAt = Now() + seconds;
        fadingOut = false;
        Blend(wasShown ? 1.0f : 0.0f);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetTimer(hwnd, kFadeTimer, kFrameMs, nullptr);
    }

    ~Impl() {
        if (hwnd) DestroyWindow(hwnd);
        if (bitmap) {
            SelectObject(memory, previous);
            DeleteObject(bitmap);
        }
        if (memory) DeleteDC(memory);
    }
};

Bubble::Bubble() : impl_(std::make_unique<Impl>()) {}
Bubble::~Bubble() = default;

bool Bubble::Create(HINSTANCE instance) {
    QueryPerformanceFrequency(&impl_->frequency);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, impl_->d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(impl_->dwrite.GetAddressOf())))) {
        return false;
    }
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = &Impl::Proc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);
    impl_->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kWindowClass,
                                  L"TEKITO", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, impl_.get());
    return impl_->hwnd != nullptr;
}

void Bubble::Show(const std::wstring& title, const std::wstring& body, const RECT& anchor, UINT dpi, double seconds) {
    if (impl_->hwnd) impl_->Show(title, body, anchor, dpi, seconds);
}

void Bubble::Hide() {
    if (impl_->hwnd && IsWindowVisible(impl_->hwnd)) impl_->StartFadeOut();
}

bool Bubble::Visible() const noexcept {
    return impl_->hwnd && IsWindowVisible(impl_->hwnd);
}

}  // namespace tekito::companion
