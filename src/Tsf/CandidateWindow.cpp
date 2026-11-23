// C++/WinRT needs the classic COM declarations before its own headers so
// com_ptr/as<> can interoperate with the composition interop interfaces.
#include <unknwn.h>

#include "Tsf/CandidateWindow.h"

#include "assets/tekito_resource.h"

#include <DispatcherQueue.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <windows.foundation.h>
#include <windows.graphics.effects.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.interop.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <winrt/Windows.UI.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "Tsf/CandidateWindowLook.h"

namespace tekito::tsf {
namespace {

using namespace candidate_look;

}  // namespace


// Host thread -> UI thread hand-off. Only the latest frame matters, so
// repeated Show calls overwrite `pending` and the UI thread coalesces them.
struct CandidateWindow::Channel {
    ~Channel() {
        if (ready) CloseHandle(ready);
    }

    std::mutex mutex;
    Frame pending;
    bool hasPending{false};
    std::atomic<HWND> controlHwnd{nullptr};
    // Initialize stopped waiting for the UI thread: it ends as soon as it
    // has started, rather than run on with nobody to tell it to stop.
    std::atomic<bool> abandoned{false};
    HWND notifyHwnd{nullptr};
    HINSTANCE instance{nullptr};
    DPI_AWARENESS_CONTEXT dpiContext{nullptr};
    HANDLE ready{nullptr};
};

namespace {

// A shadow drawn into a click-through layered window under a panel.
struct ShadowWindow {
    HWND hwnd{nullptr};
    HDC dc{nullptr};
    HBITMAP bitmap{nullptr};
    HGDIOBJ old{nullptr};
    void* bits{nullptr};
    SIZE size{0, 0};
    std::array<float, 5> key{};
    POINT position{};
};

class CandidateView final {
public:
    explicit CandidateView(std::shared_ptr<CandidateWindow::Channel> channel)
        : channel_(std::move(channel)) {}
    CandidateView(const CandidateView&) = delete;
    CandidateView& operator=(const CandidateView&) = delete;

    bool Start() {
        // Isolated: the private font loader below must not land in the
        // host application's shared DirectWrite factory.
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwrite_.put()));
        brandFont_ = LoadBrandFont(dwrite_.get(), channel_->instance);
        if (!RegisterWindowClass(kControlClassName) || !RegisterWindowClass(kWindowClassName) ||
            !RegisterWindowClass(kShadowClassName) || !RegisterWindowClass(kNoteClassName)) {
            return false;
        }
        control_ = CreateWindowExW(0, kControlClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   channel_->instance, this);
        return control_ != nullptr && dwrite_ != nullptr;
    }

    [[nodiscard]] HWND Control() const noexcept { return control_; }

    void Shutdown() noexcept {
        DestroyNote();
        if (shadow_.hwnd) DestroyWindow(shadow_.hwnd);
        if (panel_) DestroyWindow(panel_);
        shadow_.hwnd = panel_ = nullptr;
        ReleaseComposition();
        ReleaseShadowBitmap(shadow_);
        ReleaseShadowBitmap(noteShadow_);
        hwndTarget_ = nullptr;
        noteHwndTarget_ = nullptr;
        formats_ = {};
        brandFont_.collection = nullptr;
        if (brandFont_.loader && dwrite_) {
            if (const auto factory5 = dwrite_.try_as<IDWriteFactory5>()) {
                factory5->UnregisterFontFileLoader(brandFont_.loader.get());
            }
        }
        brandFont_.loader = nullptr;
        ShutdownDispatcherQueue();
        for (const auto* name : {kNoteClassName, kShadowClassName, kWindowClassName, kControlClassName}) {
            UnregisterClassW(name, channel_->instance);
        }
    }

private:
    bool RegisterWindowClass(const wchar_t* name) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = channel_->instance;
        wc.lpfnWndProc = &CandidateView::WindowProc;
        wc.lpszClassName = name;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<CandidateView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<CandidateView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
        case WM_MOUSEACTIVATE:
            // Never take focus from the application being typed into.
            return MA_NOACTIVATE;
        case kMsgUpdate:
            self->OnUpdate();
            return 0;
        case kMsgQuit:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (hwnd == self->control_) PostQuitMessage(0);
            break;
        case WM_LBUTTONDOWN:
            if (hwnd == self->panel_) {
                self->OnClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            break;
        case WM_PAINT:
            if (hwnd == self->panel_ && !self->composition_) {
                self->PaintFallback();
                return 0;
            }
            if (hwnd == self->notePanel_ && !self->composition_) {
                self->PaintNoteFallback();
                return 0;
            }
            break;
        case WM_DPICHANGED:
            // Size and position are recomputed from the caret on every update.
            return 0;
        case WM_TIMER:
            if (hwnd == self->panel_ && wParam == kBadgeTimer) {
                self->OnBadgeTimer();
                return 0;
            }
            if (hwnd == self->panel_ && wParam == kRainbowTimer) {
                self->OnRainbowTimer();
                return 0;
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void OnUpdate() {
        Frame frame;
        {
            std::lock_guard<std::mutex> lock(channel_->mutex);
            if (!channel_->hasPending) return;
            frame = std::move(channel_->pending);
            channel_->hasPending = false;
        }
        try {
            if (!frame.visible) {
                HidePopup();
                return;
            }
            Present(std::move(frame));
        } catch (...) {
            // A composition failure must never take the host down; drop to
            // the HWND renderer and try again once.
            if (composition_) {
                SwitchToFallback();
                try {
                    Present(Frame(frame_));
                } catch (...) {
                    HidePopup();
                }
            } else {
                HidePopup();
            }
        }
    }

    void OnClick(int x, int y) {
        if (!visible_ || layout_.rowPitch <= 0.0f) return;
        const float fy = static_cast<float>(y) - layout_.padding - layout_.captionHeight;
        const float fx = static_cast<float>(x);
        if (fy < 0.0f || fx < layout_.padding || fx > layout_.rowRight) return;
        // Rows are separated by a gap, so the hit test uses the same pitch
        // the rows are drawn with and ignores clicks between rows.
        auto row = static_cast<std::size_t>(fy / layout_.rowPitch);
        if (fy - static_cast<float>(row) * layout_.rowPitch > layout_.rowHeight) return;
        if (layout_.columnRows > 0 && layout_.columnPitch > 0.0f) {
            if (row >= layout_.columnRows) return;
            const float cx = fx - layout_.padding;
            const auto column = static_cast<std::size_t>(cx / layout_.columnPitch);
            if (cx - static_cast<float>(column) * layout_.columnPitch > layout_.columnWidth) return;
            row += column * layout_.columnRows;
        }
        if (row >= frame_.rows.size()) return;
        PostMessageW(channel_->notifyHwnd, kMsgSelect, static_cast<WPARAM>(row),
                     static_cast<LPARAM>(frame_.generation));
    }

    void Present(Frame frame) {
        t_zoomPercent = frame.zoom;
        if (!EnsureWindows()) return;
        KillTimer(panel_, kBadgeTimer);
        badgeFade_ = 0;
        if (!visible_ || frame.style != frame_.style) {
            style_ = ReadStyle(composition_ && hostBackdrop_, frame.style == 1);
        }
        if (frame.rainbow && !frame.badge) TintRainbow();

        // DPI follows the monitor under the caret; move there first so
        // GetDpiForWindow reports the DPI the panel is about to render at.
        if (MonitorFromRect(&frame.caret, MONITOR_DEFAULTTONEAREST) !=
            MonitorFromWindow(panel_, MONITOR_DEFAULTTONEAREST)) {
            SetWindowPos(panel_, nullptr, frame.caret.left, frame.caret.bottom, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        EnsureTextFormats();

        const Layout layout = ComputeLayout(frame);
        const POINT position = CandidatePosition(frame.caret, layout.width, layout.height,
                                                 ScaleForWindow(panel_, kCaretGap));
        int noteOffset = 0;
        Layout note = ComputeNote(frame, layout, noteOffset);
        const bool animate = style_.animations && visible_ && frame_.selectedRow < frame_.rows.size() &&
                             frame.selectedRow < frame.rows.size() &&
                             frame.firstIndex == frame_.firstIndex &&
                             frame.selectedRow != frame_.selectedRow;
        frame_ = std::move(frame);
        layout_ = layout;
        note_ = std::move(note);
        const bool withNote = note_.detailText && notePanel_;
        const POINT notePosition = withNote ? NotePosition(frame_.caret, position, noteOffset) : POINT{};

        if (composition_) {
            HRESULT hr = RenderComposition(animate);
            if (IsDeviceLost(hr) && RecreateDevices()) hr = RenderComposition(false);
            winrt::check_hresult(hr);
            if (withNote) {
                hr = RenderNote();
                if (IsDeviceLost(hr) && RecreateDevices()) hr = RenderNote();
                winrt::check_hresult(hr);
                UpdateShadow(noteShadow_, note_, notePosition);
            }
            UpdateShadow(shadow_, layout_, position);
            HDWP batch = BeginDeferWindowPos(4);
            if (batch && shadow_.hwnd) {
                batch = DeferWindowPos(batch, shadow_.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            if (batch) {
                // Directly below the (click-through) shadow window.
                batch = DeferWindowPos(batch, panel_, shadow_.hwnd ? shadow_.hwnd : HWND_TOPMOST, position.x,
                                       position.y, layout.width, layout.height,
                                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            if (batch && withNote && noteShadow_.hwnd) {
                batch = DeferWindowPos(batch, noteShadow_.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            if (batch && withNote) {
                batch = DeferWindowPos(batch, notePanel_, noteShadow_.hwnd ? noteShadow_.hwnd : HWND_TOPMOST,
                                       notePosition.x, notePosition.y, note_.width, note_.height,
                                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            if (batch) EndDeferWindowPos(batch);
        } else {
            SetWindowPos(panel_, HWND_TOPMOST, position.x, position.y, layout.width, layout.height,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (hwndTarget_) {
                hwndTarget_->Resize(D2D1::SizeU(static_cast<UINT32>(layout.width),
                                                static_cast<UINT32>(layout.height)));
            }
            InvalidateRect(panel_, nullptr, FALSE);
            if (withNote) {
                SetWindowPos(notePanel_, HWND_TOPMOST, notePosition.x, notePosition.y, note_.width, note_.height,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (noteHwndTarget_) {
                    noteHwndTarget_->Resize(
                        D2D1::SizeU(static_cast<UINT32>(note_.width), static_cast<UINT32>(note_.height)));
                }
                InvalidateRect(notePanel_, nullptr, FALSE);
            }
        }
        if (withNote) {
            noteShown_ = true;
        } else {
            HideNote();
        }
        visible_ = true;
        if (frame_.badge) SetTimer(panel_, kBadgeTimer, kBadgeHoldMs, nullptr);
        if (frame_.rainbow && !frame_.badge) {
            SetTimer(panel_, kRainbowTimer, kRainbowTickMs, nullptr);
        } else {
            KillTimer(panel_, kRainbowTimer);
        }
    }

    // The accent (the light under the selection, the simple list's rail) in
    // the rainbow's colour now.
    void TintRainbow() {
        const bool dark = IsDarkThemeActive();
        const auto color = RainbowColor(dark);
        style_.light = color;
        style_.rail = color;
    }

    // Each tick: the list drawn again in the next colour.
    void OnRainbowTimer() {
        if (!visible_ || !frame_.rainbow || IsHighContrast()) {
            KillTimer(panel_, kRainbowTimer);
            return;
        }
        TintRainbow();
        if (composition_) {
            HRESULT hr = RenderComposition(false);
            if (IsDeviceLost(hr) && RecreateDevices()) hr = RenderComposition(false);
        } else {
            InvalidateRect(panel_, nullptr, FALSE);
        }
    }

    // The badge's first tick ends its hold; the rest fade it out.
    void OnBadgeTimer() {
        if (badgeFade_ == 0) SetTimer(panel_, kBadgeTimer, kBadgeFadeTickMs, nullptr);
        ++badgeFade_;
        if (badgeFade_ >= kBadgeFadeSteps || !composition_) {
            HidePopup();
            return;
        }
        const float opacity = 1.0f - static_cast<float>(badgeFade_) / kBadgeFadeSteps;
        try {
            if (root_) root_.Opacity(opacity);
        } catch (...) {
        }
        if (shadow_.hwnd && shadow_.dc) {
            POINT source{0, 0};
            SIZE size = shadow_.size;
            BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255.0f * opacity)), AC_SRC_ALPHA};
            UpdateLayeredWindow(shadow_.hwnd, nullptr, &shadow_.position, &size, shadow_.dc, &source, 0, &blend,
                                ULW_ALPHA);
        }
    }

    void HidePopup() noexcept {
        if (panel_) KillTimer(panel_, kBadgeTimer);
        if (panel_) KillTimer(panel_, kRainbowTimer);
        if (!visible_) return;
        visible_ = false;
        try {
            // Hidden with zero opacity, so a stale tree never flashes for a
            // frame when the window is shown again before the next commit.
            if (root_) root_.Opacity(0.0f);
        } catch (...) {
        }
        HideNote();
        if (shadow_.hwnd) ShowWindow(shadow_.hwnd, SW_HIDE);
        if (panel_) ShowWindow(panel_, SW_HIDE);
    }

    Layout ComputeLayout(const Frame& frame) const {
        Layout l;
        l.scale = ScaleFactor(panel_);
        l.radius = static_cast<float>(
            ScaleForWindow(panel_, !composition_                ? kFallbackCornerRadius
                                   : frame.style == 1 ? kSimpleCornerRadius
                                                      : kCornerRadius));
        l.padding = static_cast<float>(ScaleForWindow(panel_, kPanelPadding));
        l.rowHeight = static_cast<float>(ScaleForWindow(panel_, kRowHeight));
        l.rowPitch = l.rowHeight + static_cast<float>(ScaleForWindow(panel_, kRowGap));
        l.rowPaddingLeft = static_cast<float>(ScaleForWindow(panel_, kRowPaddingLeft));
        l.numberWidth = static_cast<float>(ScaleForWindow(panel_, kNumberWidth));
        l.numberGap = static_cast<float>(ScaleForWindow(panel_, kNumberGap));
        l.rowPaddingRight = static_cast<float>(ScaleForWindow(panel_, kRowPaddingRight));
        l.tagGap = static_cast<float>(ScaleForWindow(panel_, kTagGap));
        l.tagPaddingX = static_cast<float>(ScaleForWindow(panel_, kTagPaddingX));
        l.tagHeight = static_cast<float>(ScaleForWindow(panel_, kTagHeight));
        l.tagTracking = kTagTracking * l.scale;
        l.iconWidth = static_cast<float>(ScaleForWindow(panel_, kIconWidth));
        l.iconGap = static_cast<float>(ScaleForWindow(panel_, kIconGap));
        if (frame.pages > 1 && !frame.rows.empty() && formats_.page) {
            l.footerHeight = static_cast<float>(ScaleForWindow(panel_, kFooterHeight));
        }
        if (frame.badge) {
            const int side = static_cast<int>(std::lround(2.0f * l.padding + l.rowHeight));
            l.width = l.height = side;
            l.footerHeight = 0.0f;
            if (composition_ && frame.style != 1) l.radius = static_cast<float>(side) / 2.0f;
            l.rowRight = static_cast<float>(side) - l.padding;
            l.listWidth = l.listHeight = static_cast<float>(side);
            return l;
        }

        // Columns carry no numbers, as digits choose from the first page.
        const bool columns = frame.columnRows > 0 && frame.rows.size() > 0;
        if (columns) {
            l.columnRows = frame.columnRows;
            l.numberWidth = 0.0f;
            l.numberGap = 0.0f;
        }
        // Candidate length varies a lot (a single emoji vs. a long phrase), so
        // the panel is sized to the widest visible row. Measured in the
        // selected weight so moving the selection never changes the width.
        float widest = 0.0f;
        for (const Row& row : frame.rows) {
            float content = l.rowPaddingLeft + l.numberWidth + l.numberGap +
                            MeasureTextWidth(dwrite_.get(), formats_.textSelected.get(), row.text) +
                            l.rowPaddingRight;
            if (!row.label.empty()) {
                const auto tag = TagLayout(dwrite_.get(), formats_.tag.get(), row.label, l.tagHeight,
                                           l.tagTracking);
                content += l.tagGap + LayoutWidth(tag.get()) + 2.0f * l.tagPaddingX;
            }
            if (row.meaning && formats_.icon) content += l.iconGap + l.iconWidth;
            widest = std::max(widest, content);
        }
        if (!frame.caption.empty() && formats_.caption) {
            l.captionHeight = static_cast<float>(ScaleForWindow(panel_, kCaptionHeight));
            widest = std::max(widest, l.rowPaddingLeft + MeasureTextWidth(dwrite_.get(), formats_.caption.get(), frame.caption) +
                                          l.rowPaddingRight);
        }
        const int gutter = 0;
        std::size_t rowsDown = frame.rows.size();
        if (columns) {
            const std::size_t count = (frame.rows.size() + l.columnRows - 1) / l.columnRows;
            rowsDown = std::min(frame.rows.size(), l.columnRows);
            l.columnWidth = std::clamp(std::ceil(widest), static_cast<float>(ScaleForWindow(panel_, kColumnMinWidth)),
                                       static_cast<float>(ScaleForWindow(panel_, kColumnMaxWidth)));
            l.columnPitch = l.columnWidth + static_cast<float>(ScaleForWindow(panel_, kColumnGap));
            const float across = static_cast<float>(count) * l.columnPitch - (l.columnPitch - l.columnWidth);
            l.width = std::max(static_cast<int>(std::ceil(across + 2.0f * l.padding)) + gutter,
                               ScaleForWindow(panel_, kMinWidth));
        } else {
            l.width = std::clamp(static_cast<int>(std::ceil(widest + 2.0f * l.padding)) + 1 + gutter,
                                 ScaleForWindow(panel_, kMinWidth), ScaleForWindow(panel_, kMaxWidth));
        }
        const auto count = static_cast<float>(std::max<std::size_t>(rowsDown, 1));
        l.height = frame.rows.empty()
                       ? static_cast<int>(std::lround(2.0f * l.padding + l.captionHeight))
                       : static_cast<int>(std::lround(2.0f * l.padding + l.captionHeight + count * l.rowHeight +
                                                      (count - 1.0f) * (l.rowPitch - l.rowHeight) + l.footerHeight));
        l.rowRight = static_cast<float>(l.width) - l.padding - static_cast<float>(gutter);
        l.listWidth = static_cast<float>(l.width);
        l.listHeight = static_cast<float>(l.height);
        return l;
    }

    // The meaning's window: as wide as its text (up to kDetailWidth), and
    // `offset` down from the list's top so its headword sits on the
    // selected row's baseline. No text when the frame has no meaning.
    Layout ComputeNote(const Frame& frame, const Layout& list, int& offset) const {
        Layout l;
        l.scale = list.scale;
        l.padding = list.padding;
        offset = 0;
        if (frame.detailSenses.empty() || frame.badge || !formats_.detail || !dwrite_) return l;
        const float pad = static_cast<float>(ScaleForWindow(panel_, kDetailPadding));
        const float inner = static_cast<float>(ScaleForWindow(panel_, kDetailWidth)) - 2.0f * pad;
        const float maxText = static_cast<float>(ScaleForWindow(panel_, kDetailMaxHeight)) - 2.0f * pad;
        static constexpr wchar_t kMarks[] = L"\u2460\u2461\u2462\u2463\u2464\u2465\u2466\u2467\u2468";  // circled 1 to 9
        winrt::com_ptr<IDWriteTextLayout> text;
        float textHeight = 0.0f;
        // Later senses give way until the text fits.
        for (std::size_t count = frame.detailSenses.size(); count > 0; --count) {
            // The headword, then what form of another word it is.
            std::wstring content = frame.detailHead;
            if (!frame.detailForm.empty()) content += L"\u3000" + frame.detailForm;
            for (std::size_t i = 0; i < count; ++i) {
                content += L'\n';
                if (frame.detailSenses.size() > 1 && i < 9) {
                    content += kMarks[i];
                    content += L' ';
                }
                content += frame.detailSenses[i];
            }
            text = nullptr;
            if (FAILED(dwrite_->CreateTextLayout(content.c_str(), static_cast<UINT32>(content.size()),
                                                 formats_.detail.get(), inner, maxText, text.put()))) {
                return l;
            }
            const DWRITE_TEXT_RANGE head{0, static_cast<UINT32>(frame.detailHead.size())};
            text->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, head);
            text->SetFontSize(kFontDetailHead * list.scale, head);
            DWRITE_TEXT_METRICS metrics{};
            text->GetMetrics(&metrics);
            textHeight = metrics.height;
            if (textHeight <= maxText) break;
        }
        if (!text) return l;
        DWRITE_TEXT_METRICS metrics{};
        text->GetMetrics(&metrics);
        const float shown = std::min(textHeight, maxText);
        l.width = static_cast<int>(std::ceil(std::min(metrics.width, inner) + 2.0f * pad));
        l.height = static_cast<int>(std::ceil(shown + 2.0f * pad));
        l.radius = std::min(list.radius, static_cast<float>(l.height) / 2.0f);
        l.listWidth = static_cast<float>(l.width);
        l.listHeight = static_cast<float>(l.height);
        // The headword on the selected row's baseline.
        if (frame.selectedRow < frame.rows.size()) {
            // The first line's metrics (asking for all, as one is too few
            // for text that wraps).
            const auto lineOf = [](IDWriteTextLayout* layout) {
                UINT32 count = 0;
                if (!layout) return DWRITE_LINE_METRICS{};
                layout->GetLineMetrics(nullptr, 0, &count);
                std::vector<DWRITE_LINE_METRICS> lines(count);
                if (count == 0 || FAILED(layout->GetLineMetrics(lines.data(), count, &count))) return DWRITE_LINE_METRICS{};
                return lines.front();
            };
            winrt::com_ptr<IDWriteTextLayout> sample;
            dwrite_->CreateTextLayout(L"\x3042", 1, formats_.text.get(), 1000.0f, 1000.0f, sample.put());
            const auto rowLine = lineOf(sample.get());
            const auto headLine = lineOf(text.get());
            const D2D1_RECT_F row = list.RowRect(frame.selectedRow);
            const float baseline = row.top + (list.rowHeight - rowLine.height) / 2.0f + rowLine.baseline;
            offset = static_cast<int>(std::lround(baseline - pad - headLine.baseline));
        }
        // Its text set out to the width it takes.
        text->SetMaxWidth(static_cast<float>(l.width) - 2.0f * pad);
        l.detail = D2D1::RectF(pad, pad, static_cast<float>(l.width) - pad, pad + shown);
        l.detailText = std::move(text);
        return l;
    }

    // Beside the list (right, or left where the screen ends), level with
    // the selected row, within the work area.
    POINT NotePosition(const RECT& caret, POINT list, int offset) const {
        const RECT work = WorkAreaNear(caret);
        const int gap = ScaleForWindow(panel_, kNoteGap);
        int x = list.x + layout_.width + gap;
        if (x + note_.width > work.right) x = list.x - gap - note_.width;
        x = std::max<int>(x, work.left);
        int y = list.y + offset;
        y = std::min<int>(y, work.bottom - note_.height);
        y = std::max<int>(y, work.top);
        return {x, y};
    }

    void EnsureTextFormats() {
        const UINT dpi = ZoomedDpi(panel_);
        if (formats_.dpi == dpi && formats_.text) return;
        const float scale = static_cast<float>(dpi == 0 ? kBaseDpi : dpi) / static_cast<float>(kBaseDpi);
        IDWriteFactory* f = dwrite_.get();
        IDWriteFontCollection* brand = brandFont_.collection.get();
        formats_.dpi = dpi;
        formats_.text = CreateFormat(f, brand, kFontCandidate * scale, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
        formats_.textSelected = CreateFormat(f, brand, kFontCandidate * scale, DWRITE_FONT_WEIGHT_MEDIUM,
                                             DWRITE_TEXT_ALIGNMENT_LEADING, true);
        formats_.number = CreateFormat(f, brand, kFontNumber * scale, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_TEXT_ALIGNMENT_TRAILING, false);
        // The numbers sit on the candidates' baseline, not each centred in
        // the row by its own size.
        formats_.numberShift = 0.0f;
        const auto lineOf = [&](IDWriteTextFormat* format) -> std::pair<float, float> {
            winrt::com_ptr<IDWriteTextLayout> layout;
            DWRITE_LINE_METRICS line{};
            UINT32 lines = 0;
            if (!format || FAILED(f->CreateTextLayout(L"1\x3042", 2, format, 1000.0f, 1000.0f, layout.put())) ||
                FAILED(layout->GetLineMetrics(&line, 1, &lines)) || lines == 0) {
                return {0.0f, 0.0f};
            }
            return {line.height, line.baseline};
        };
        const auto [textHeight, textBaseline] = lineOf(formats_.text.get());
        const auto [numberHeight, numberBaseline] = lineOf(formats_.number.get());
        if (textHeight > 0.0f && numberHeight > 0.0f) {
            formats_.numberShift = (textBaseline - textHeight / 2.0f) - (numberBaseline - numberHeight / 2.0f);
        }
        formats_.tag = CreateFormat(f, brand, kFontTag * scale, DWRITE_FONT_WEIGHT_BOLD,
                                    DWRITE_TEXT_ALIGNMENT_LEADING, false);
        formats_.page = CreateFormat(f, brand, kFontCaption * scale, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_TEXT_ALIGNMENT_TRAILING, true);
        formats_.caption = CreateFormat(f, brand, kFontCaption * scale, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_TEXT_ALIGNMENT_LEADING, true);
        formats_.detail = CreateFormat(f, brand, kFontDetail * scale, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_TEXT_ALIGNMENT_LEADING, false);
        if (formats_.detail) {
            formats_.detail->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            formats_.detail->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        formats_.icon = nullptr;
        if (const wchar_t* family = IconFontFamily(f)) {
            if (SUCCEEDED(f->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, kFontIcon * scale, L"",
                                              formats_.icon.put()))) {
                formats_.icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                formats_.icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                formats_.icon->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
    }

    // --- windows -----------------------------------------------------------

    bool EnsureWindows() {
        if (panel_) return true;
        if (windowsFailed_) return false;

        if (!compositionBroken_ && InitComposition()) {
            panel_ = CreatePanel(WS_EX_NOREDIRECTIONBITMAP);
            if (panel_) {
                BOOL enable = TRUE;
                hostBackdrop_ = SUCCEEDED(DwmSetWindowAttribute(panel_, DWMWA_USE_HOSTBACKDROPBRUSH,
                                                                &enable, sizeof(enable)));
                const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
                DwmSetWindowAttribute(panel_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
                const COLORREF noBorder = DWMWA_COLOR_NONE;
                DwmSetWindowAttribute(panel_, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
                composition_ = BuildVisualTree();
                if (composition_) {
                    shadow_.hwnd = CreateShadowWindow();
                    CreateNote();
                    return true;
                }
                DestroyWindow(panel_);
                panel_ = nullptr;
            }
            ReleaseComposition();
            compositionBroken_ = true;
        }

        composition_ = false;
        panel_ = CreatePanel(0);
        if (!panel_) {
            windowsFailed_ = true;
            return false;
        }
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(panel_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        // Extending the frame gives the popup a DWM shadow.
        const MARGINS margins{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(panel_, &margins);
        CreateNote();
        return true;
    }

    HWND CreatePanel(DWORD extraStyle, const wchar_t* className = kWindowClassName) {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | extraStyle,
                               className, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               channel_->instance, this);
    }

    HWND CreateShadowWindow() {
        return CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                                   WS_EX_TOPMOST,
                               kShadowClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               channel_->instance, this);
    }

    // The meaning's window, in the list's kind: glass with its own shadow,
    // or a plain one with the system's shadow. None when it cannot be made
    // (the list shows without its meaning).
    void CreateNote() {
        if (composition_) {
            notePanel_ = CreatePanel(WS_EX_NOREDIRECTIONBITMAP, kNoteClassName);
            if (!notePanel_) return;
            BOOL enable = TRUE;
            DwmSetWindowAttribute(notePanel_, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
            const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
            DwmSetWindowAttribute(notePanel_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
            const COLORREF noBorder = DWMWA_COLOR_NONE;
            DwmSetWindowAttribute(notePanel_, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
            if (!BuildNoteTree()) {
                DestroyWindow(notePanel_);
                notePanel_ = nullptr;
                return;
            }
            noteShadow_.hwnd = CreateShadowWindow();
            return;
        }
        notePanel_ = CreatePanel(0, kNoteClassName);
        if (!notePanel_) return;
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(notePanel_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        const MARGINS margins{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(notePanel_, &margins);
    }

    void DestroyNote() noexcept {
        if (noteShadow_.hwnd) DestroyWindow(noteShadow_.hwnd);
        if (notePanel_) DestroyWindow(notePanel_);
        noteShadow_.hwnd = notePanel_ = nullptr;
        noteShown_ = false;
    }

    void HideNote() noexcept {
        if (!noteShown_) return;
        noteShown_ = false;
        try {
            if (noteRoot_) noteRoot_.Opacity(0.0f);
        } catch (...) {
        }
        if (noteShadow_.hwnd) ShowWindow(noteShadow_.hwnd, SW_HIDE);
        if (notePanel_) ShowWindow(notePanel_, SW_HIDE);
    }

    void SwitchToFallback() noexcept {
        DestroyNote();
        if (shadow_.hwnd) DestroyWindow(shadow_.hwnd);
        if (panel_) DestroyWindow(panel_);
        shadow_.hwnd = panel_ = nullptr;
        visible_ = false;
        ReleaseComposition();
        compositionBroken_ = true;
        composition_ = false;
    }

    // --- composition ---------------------------------------------------------

    bool InitComposition() {
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
        if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_NO_COMPOSITION", nullptr, 0) > 0) return false;
#endif
        try {
            if (!dispatcherQueue_) {
                DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
                winrt::check_hresult(CreateDispatcherQueueController(
                    options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
                                 winrt::put_abi(dispatcherQueue_))));
            }
            compositor_ = wuc::Compositor();
            if (!CreateDevices()) {
                ReleaseComposition();
                return false;
            }
            winrt::check_hresult(compositor_.as<abi::ICompositorInterop>()->CreateGraphicsDevice(
                d2dDevice_.get(),
                reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(graphics_))));
            return true;
        } catch (...) {
            ReleaseComposition();
            return false;
        }
    }

    bool CreateDevices() {
        d2dDevice_ = nullptr;
        d3dDevice_ = nullptr;
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                       D3D11_SDK_VERSION, d3dDevice_.put(), nullptr, nullptr);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, d3dDevice_.put(), nullptr, nullptr);
        }
        if (FAILED(hr)) return false;
        if (!d2dFactory_) {
            // Multithreaded: the compositor may touch the device off this thread.
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
                                         nullptr, d2dFactory_.put_void()))) {
                return false;
            }
        }
        const auto dxgi = d3dDevice_.try_as<IDXGIDevice>();
        return dxgi && SUCCEEDED(d2dFactory_->CreateDevice(dxgi.get(), d2dDevice_.put()));
    }

    bool RecreateDevices() {
        try {
            if (!CreateDevices()) return false;
            winrt::check_hresult(graphics_.as<abi::ICompositionGraphicsDeviceInterop>()->SetRenderingDevice(
                d2dDevice_.get()));
            return true;
        } catch (...) {
            return false;
        }
    }

    wuc::SpriteVisual SurfaceVisual(Surface& surface) {
        surface.surface = graphics_.CreateDrawingSurface(
            {1.0f, 1.0f}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        surface.size = {1, 1};
        auto brush = compositor_.CreateSurfaceBrush(surface.surface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0.0f);
        brush.VerticalAlignmentRatio(0.0f);
        auto visual = compositor_.CreateSpriteVisual();
        visual.Brush(brush);
        return visual;
    }

    // root
    //  +- glass       backdrop blur (or solid color), clipped to the panel shape
    //  +- chrome      veil, inner glow, specular rim
    //  +- content     rows in normal ink, the page line
    //  +- selection   moves between rows (implicitly animated)
    //      +- capsule    tinted glass pill with its lift shadow
    //      +- window     clipped to the pill
    //          +- selectedText   rows in selected ink, held still in panel space
    bool BuildVisualTree() {
        try {
            winrt::check_hresult(
                compositor_.as<abi::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
                    panel_, TRUE,
                    reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target_))));
            root_ = compositor_.CreateContainerVisual();
            target_.Root(root_);

            glassVisual_ = compositor_.CreateSpriteVisual();
            glassClip_ = compositor_.CreateRectangleClip();
            glassVisual_.Clip(glassClip_);
            solidBrush_ = compositor_.CreateColorBrush();
            if (hostBackdrop_) backdropBrush_ = VibrantBackdrop();

            chromeVisual_ = SurfaceVisual(chrome_);
            contentVisual_ = SurfaceVisual(content_);
            selectionVisual_ = compositor_.CreateContainerVisual();
            capsuleVisual_ = SurfaceVisual(capsule_);
            textWindow_ = compositor_.CreateContainerVisual();
            textClip_ = compositor_.CreateRectangleClip();
            textWindow_.Clip(textClip_);
            selectedTextVisual_ = SurfaceVisual(selectedText_);

            // The light lives inside the glass visual so the panel clip
            // applies, and its anchor tracks the selection's (animated) offset.
            lightAnchor_ = compositor_.CreateContainerVisual();
            lightVisual_ = SurfaceVisual(light_);
            lightAnchor_.Children().InsertAtTop(lightVisual_);
            glassVisual_.Children().InsertAtTop(lightAnchor_);
            root_.Children().InsertAtTop(glassVisual_);
            root_.Children().InsertAtTop(chromeVisual_);
            root_.Children().InsertAtTop(contentVisual_);
            root_.Children().InsertAtTop(selectionVisual_);
            selectionVisual_.Children().InsertAtTop(capsuleVisual_);
            selectionVisual_.Children().InsertAtTop(textWindow_);
            textWindow_.Children().InsertAtTop(selectedTextVisual_);

            // The selected-ink text stays fixed in panel space while the
            // capsule moves, so the capsule reveals it like a lens sliding
            // over the list instead of the text jumping row to row.
            auto counter = compositor_.CreateExpressionAnimation(
                L"Vector3(-selection.Offset.X, -selection.Offset.Y, 0)");
            counter.SetReferenceParameter(L"selection", selectionVisual_);
            selectedTextVisual_.StartAnimation(L"Offset", counter);
            auto follow = compositor_.CreateExpressionAnimation(L"selection.Offset");
            follow.SetReferenceParameter(L"selection", selectionVisual_);
            lightAnchor_.StartAnimation(L"Offset", follow);

            // A quick glide with a hint of overshoot.
            auto easing = compositor_.CreateCubicBezierEasingFunction(float2{0.25f, 1.1f},
                                                                      float2{0.4f, 1.0f});
            auto glide = compositor_.CreateVector3KeyFrameAnimation();
            glide.Target(L"Offset");
            glide.InsertExpressionKeyFrame(1.0f, L"this.FinalValue", easing);
            glide.Duration(kGlideDuration);
            selectionAnimations_ = compositor_.CreateImplicitAnimationCollection();
            selectionAnimations_.Insert(L"Offset", glide);
            selectionVisual_.ImplicitAnimations(selectionAnimations_);
            return true;
        } catch (...) {
            return false;
        }
    }

    // The note: glass, its chrome, and the meaning's text.
    bool BuildNoteTree() {
        try {
            winrt::check_hresult(
                compositor_.as<abi::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
                    notePanel_, TRUE,
                    reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(noteTarget_))));
            noteRoot_ = compositor_.CreateContainerVisual();
            noteTarget_.Root(noteRoot_);
            noteGlass_ = compositor_.CreateSpriteVisual();
            noteGlassClip_ = compositor_.CreateRectangleClip();
            noteGlass_.Clip(noteGlassClip_);
            noteSolid_ = compositor_.CreateColorBrush();
            if (hostBackdrop_) noteBackdrop_ = VibrantBackdrop();
            noteChromeVisual_ = SurfaceVisual(noteChrome_);
            noteContentVisual_ = SurfaceVisual(noteContent_);
            noteRoot_.Children().InsertAtTop(noteGlass_);
            noteRoot_.Children().InsertAtTop(noteChromeVisual_);
            noteRoot_.Children().InsertAtTop(noteContentVisual_);
            noteRoot_.Opacity(0.0f);
            return true;
        } catch (...) {
            ReleaseNoteComposition();
            return false;
        }
    }

    void ReleaseNoteComposition() noexcept {
        noteContentVisual_ = nullptr;
        noteChromeVisual_ = nullptr;
        noteBackdrop_ = nullptr;
        noteSolid_ = nullptr;
        noteGlassClip_ = nullptr;
        noteGlass_ = nullptr;
        noteRoot_ = nullptr;
        noteTarget_ = nullptr;
        noteChrome_ = {};
        noteContent_ = {};
    }

    // The blurred desktop behind the window, with its color boosted. Falls
    // back to the plain backdrop if the effect cannot be compiled.
    wuc::CompositionBrush VibrantBackdrop() {
        auto backdrop = compositor_.CreateHostBackdropBrush();
        try {
            const auto effect = winrt::make<SaturationEffect>(
                wuc::CompositionEffectSourceParameter(L"backdrop"), kBackdropSaturation);
            auto brush = compositor_.CreateEffectFactory(effect).CreateBrush();
            brush.SetSourceParameter(L"backdrop", backdrop);
            return brush;
        } catch (...) {
            return backdrop;
        }
    }

    void ReleaseComposition() noexcept {
        ReleaseNoteComposition();
        selectionAnimations_ = nullptr;
        lightVisual_ = nullptr;
        lightAnchor_ = nullptr;
        light_ = {};
        selectedTextVisual_ = nullptr;
        textClip_ = nullptr;
        textWindow_ = nullptr;
        capsuleVisual_ = nullptr;
        selectionVisual_ = nullptr;
        contentVisual_ = nullptr;
        chromeVisual_ = nullptr;
        backdropBrush_ = nullptr;
        solidBrush_ = nullptr;
        glassClip_ = nullptr;
        glassVisual_ = nullptr;
        root_ = nullptr;
        target_ = nullptr;
        chrome_ = {};
        content_ = {};
        capsule_ = {};
        selectedText_ = {};
        graphics_ = nullptr;
        compositor_ = nullptr;
        d2dDevice_ = nullptr;
        d3dDevice_ = nullptr;
    }

    void ShutdownDispatcherQueue() noexcept {
        if (!dispatcherQueue_) return;
        try {
            const auto operation = dispatcherQueue_.ShutdownQueueAsync();
            const auto deadline = GetTickCount64() + 1000;
            while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started &&
                   GetTickCount64() < deadline) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } catch (...) {
        }
        dispatcherQueue_ = nullptr;
    }

    template <typename Draw>
    HRESULT PaintSurface(Surface& surface, int width, int height, Draw&& draw) {
        width = std::max(width, 1);
        height = std::max(height, 1);
        const auto interop = surface.surface.as<abi::ICompositionDrawingSurfaceInterop>();
        if (surface.size.cx != width || surface.size.cy != height) {
            const HRESULT hr = interop->Resize({width, height});
            if (FAILED(hr)) return hr;
            surface.size = {width, height};
        }
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT offset{};
        HRESULT hr = interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset);
        if (FAILED(hr)) return hr;
        dc->SetDpi(96.0f, 96.0f);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                       static_cast<float>(offset.y)));
        dc->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                                            static_cast<float>(height)),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        // No brush can be made: nothing is drawn with a null brush.
        if (SolidBrush(dc.get(), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f))) draw(dc.get());
        dc->PopAxisAlignedClip();
        return interop->EndDraw();
    }

    static winrt::Windows::UI::Color ToUiColor(const D2D1_COLOR_F& color) {
        const auto byte = [](float channel) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
        };
        return {byte(color.a), byte(color.r), byte(color.g), byte(color.b)};
    }

    HRESULT RenderComposition(bool animate) {
        const float width = static_cast<float>(layout_.width);
        const float height = static_cast<float>(layout_.height);
        const float2 radius{layout_.radius, layout_.radius};

        root_.Opacity(1.0f);
        glassVisual_.Size({width, height});
        glassClip_.Right(width);
        glassClip_.Bottom(height);
        glassClip_.TopLeftRadius(radius);
        glassClip_.TopRightRadius(radius);
        glassClip_.BottomRightRadius(radius);
        glassClip_.BottomLeftRadius(radius);
        solidBrush_.Color(ToUiColor(style_.palette.solid));
        if (style_.glass && backdropBrush_) {
            glassVisual_.Brush(backdropBrush_);
        } else {
            glassVisual_.Brush(solidBrush_);
        }

        chromeVisual_.Size({width, height});
        HRESULT hr = PaintSurface(chrome_, layout_.width, layout_.height,
                                  [&](ID2D1RenderTarget* dc) { DrawChrome(dc, layout_, style_); });
        if (FAILED(hr)) return hr;

        contentVisual_.Size({width, height});
        hr = PaintSurface(content_, layout_.width, layout_.height, [&](ID2D1RenderTarget* dc) {
            if (frame_.badge) {
                DrawBadge(dc, layout_, frame_.badge, channel_->instance);
                return;
            }
            DrawCaption(dc, formats_, layout_, frame_, style_);
            DrawRows(dc, dwrite_.get(), formats_, layout_, frame_, style_, Ink::Normal, kNoRow, kNoRow);
            DrawPage(dc, formats_, layout_, frame_, style_);
        });
        if (FAILED(hr)) return hr;

        if (frame_.selectedRow >= frame_.rows.size()) {
            selectionVisual_.IsVisible(false);
            lightAnchor_.IsVisible(false);
            return S_OK;
        }

        const D2D1_RECT_F row = layout_.RowRect(frame_.selectedRow);
        const float rowWidth = row.right - row.left;
        const float rowHeight = row.bottom - row.top;
        const float pad = static_cast<float>(ScaleForWindow(panel_, kSelectionPad));
        const int capsuleWidth = static_cast<int>(std::ceil(rowWidth + 2.0f * pad));
        const int capsuleHeight = static_cast<int>(std::ceil(rowHeight + 2.0f * pad));
        const float spread = static_cast<float>(ScaleForWindow(panel_, kLightSpread) + 2);
        const int lightWidth = static_cast<int>(std::ceil(rowWidth + 2.0f * spread));
        const int lightHeight = static_cast<int>(std::ceil(rowHeight + 2.0f * spread));
        lightVisual_.Offset({-spread, -spread, 0.0f});
        lightVisual_.Size({static_cast<float>(lightWidth), static_cast<float>(lightHeight)});
        hr = PaintSurface(light_, lightWidth, lightHeight, [&](ID2D1RenderTarget* dc) {
            DrawLight(dc, D2D1::RectF(spread, spread, spread + rowWidth, spread + rowHeight), layout_.scale,
                      style_);
        });
        if (FAILED(hr)) return hr;
        lightAnchor_.IsVisible(true);

        capsuleVisual_.Offset({-pad, -pad, 0.0f});
        capsuleVisual_.Size({static_cast<float>(capsuleWidth), static_cast<float>(capsuleHeight)});
        hr = PaintSurface(capsule_, capsuleWidth, capsuleHeight, [&](ID2D1RenderTarget* dc) {
            DrawSelection(dc, D2D1::RectF(pad, pad, pad + rowWidth, pad + rowHeight), layout_.scale,
                          style_);
        });
        if (FAILED(hr)) return hr;

        const float corner = style_.simple
                                 ? std::min(static_cast<float>(ScaleForWindow(panel_, kSimpleRowRadius)), rowHeight / 2.0f)
                                 : rowHeight / 2.0f;
        const float2 pill{corner, corner};
        textWindow_.Size({rowWidth, rowHeight});
        textClip_.Right(rowWidth);
        textClip_.Bottom(rowHeight);
        textClip_.TopLeftRadius(pill);
        textClip_.TopRightRadius(pill);
        textClip_.BottomRightRadius(pill);
        textClip_.BottomLeftRadius(pill);
        selectedTextVisual_.Size({width, height});
        hr = PaintSurface(selectedText_, layout_.width, layout_.height, [&](ID2D1RenderTarget* dc) {
            DrawRows(dc, dwrite_.get(), formats_, layout_, frame_, style_, Ink::Selected, kNoRow, kNoRow);
        });
        if (FAILED(hr)) return hr;

        const float3 offset{row.left, row.top, 0.0f};
        if (animate) {
            selectionVisual_.Offset(offset);
        } else {
            selectionVisual_.ImplicitAnimations(nullptr);
            selectionVisual_.Offset(offset);
            selectionVisual_.ImplicitAnimations(selectionAnimations_);
        }
        selectionVisual_.IsVisible(true);
        return S_OK;
    }

    HRESULT RenderNote() {
        const float width = static_cast<float>(note_.width);
        const float height = static_cast<float>(note_.height);
        const float2 radius{note_.radius, note_.radius};
        noteRoot_.Opacity(1.0f);
        noteGlass_.Size({width, height});
        noteGlassClip_.Right(width);
        noteGlassClip_.Bottom(height);
        noteGlassClip_.TopLeftRadius(radius);
        noteGlassClip_.TopRightRadius(radius);
        noteGlassClip_.BottomRightRadius(radius);
        noteGlassClip_.BottomLeftRadius(radius);
        noteSolid_.Color(ToUiColor(style_.palette.solid));
        if (style_.glass && noteBackdrop_) {
            noteGlass_.Brush(noteBackdrop_);
        } else {
            noteGlass_.Brush(noteSolid_);
        }
        noteChromeVisual_.Size({width, height});
        HRESULT hr = PaintSurface(noteChrome_, note_.width, note_.height,
                                  [&](ID2D1RenderTarget* dc) { DrawChrome(dc, note_, style_); });
        if (FAILED(hr)) return hr;
        noteContentVisual_.Size({width, height});
        return PaintSurface(noteContent_, note_.width, note_.height,
                            [&](ID2D1RenderTarget* dc) { DrawNote(dc, note_, style_); });
    }

    void PaintNoteFallback() {
        PAINTSTRUCT ps{};
        if (!BeginPaint(notePanel_, &ps)) return;
        if (!d2dFactory_) {
            D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), nullptr, d2dFactory_.put_void());
        }
        if (!noteHwndTarget_ && note_.width > 0 && d2dFactory_) {
            const auto properties = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            d2dFactory_->CreateHwndRenderTarget(
                properties,
                D2D1::HwndRenderTargetProperties(
                    notePanel_, D2D1::SizeU(static_cast<UINT32>(note_.width), static_cast<UINT32>(note_.height))),
                noteHwndTarget_.put());
        }
        if (noteHwndTarget_ && noteShown_) {
            ID2D1RenderTarget* target = noteHwndTarget_.get();
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            target->BeginDraw();
            target->Clear(style_.palette.solid);
            if (SolidBrush(target, style_.palette.text)) {
                DrawChrome(target, note_, style_);
                DrawNote(target, note_, style_);
            }
            if (target->EndDraw() == D2DERR_RECREATE_TARGET) noteHwndTarget_ = nullptr;
        }
        EndPaint(notePanel_, &ps);
    }

    // --- shadow window -------------------------------------------------------

    static void ReleaseShadowBitmap(ShadowWindow& shadow) noexcept {
        if (shadow.dc) {
            if (shadow.old) SelectObject(shadow.dc, shadow.old);
            DeleteDC(shadow.dc);
        }
        if (shadow.bitmap) DeleteObject(shadow.bitmap);
        shadow.dc = nullptr;
        shadow.bitmap = nullptr;
        shadow.old = nullptr;
        shadow.bits = nullptr;
        shadow.size = {0, 0};
        shadow.key = {};
    }

    void UpdateShadow(ShadowWindow& shadow, const Layout& layout, POINT panelPosition) {
        if (!shadow.hwnd) return;
        const int marginX = ScaleForWindow(panel_, kShadowMarginX);
        const int marginTop = ScaleForWindow(panel_, kShadowMarginTop);
        const int marginBottom = ScaleForWindow(panel_, kShadowMarginBottom);
        const SIZE size{layout.width + 2 * marginX, layout.height + marginTop + marginBottom};
        const std::array<float, 5> key{static_cast<float>(layout.width),
                                       static_cast<float>(layout.height), layout.radius,
                                       style_.palette.shadowKey, style_.palette.shadowAmbient};

        if (key != shadow.key || !shadow.bits) {
            if (size.cx != shadow.size.cx || size.cy != shadow.size.cy || !shadow.bits) {
                ReleaseShadowBitmap(shadow);
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(info.bmiHeader);
                info.bmiHeader.biWidth = size.cx;
                info.bmiHeader.biHeight = -size.cy;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                shadow.dc = CreateCompatibleDC(nullptr);
                shadow.bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &shadow.bits, nullptr, 0);
                if (!shadow.dc || !shadow.bitmap || !shadow.bits) {
                    ReleaseShadowBitmap(shadow);
                    return;
                }
                shadow.old = SelectObject(shadow.dc, shadow.bitmap);
                shadow.size = size;
            }
            const float s = layout.scale;
            RasterizeShadow(static_cast<std::uint32_t*>(shadow.bits), size.cx, size.cy,
                            static_cast<float>(marginX), static_cast<float>(marginTop),
                            static_cast<float>(layout.width), static_cast<float>(layout.height),
                            layout.radius,
                            {kShadowKeyBlur * s, kShadowKeyOffset * s, style_.palette.shadowKey},
                            {kShadowAmbientBlur * s, kShadowAmbientOffset * s, style_.palette.shadowAmbient});
            shadow.key = key;
        }

        POINT destination{panelPosition.x - marginX, panelPosition.y - marginTop};
        shadow.position = destination;
        SIZE windowSize = shadow.size;
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(shadow.hwnd, nullptr, &destination, &windowSize, shadow.dc, &source, 0, &blend,
                            ULW_ALPHA);
    }

    // --- HWND fallback -------------------------------------------------------

    void PaintFallback() {
        PAINTSTRUCT ps{};
        if (!BeginPaint(panel_, &ps)) return;
        if (!hwndTarget_ && layout_.width > 0) {
            if (!d2dFactory_) {
                D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                  d2dFactory_.put_void());
            }
            if (d2dFactory_) {
                const auto properties = D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
                    96.0f);
                d2dFactory_->CreateHwndRenderTarget(
                    properties,
                    D2D1::HwndRenderTargetProperties(
                        panel_, D2D1::SizeU(static_cast<UINT32>(layout_.width),
                                            static_cast<UINT32>(layout_.height))),
                    hwndTarget_.put());
            }
        }
        if (hwndTarget_ && visible_) {
            ID2D1RenderTarget* target = hwndTarget_.get();
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            target->BeginDraw();
            target->Clear(style_.palette.solid);
            // No brush can be made (a lost device, no memory): nothing is
            // drawn with a null brush, and the target is made again.
            if (!SolidBrush(target, style_.palette.text)) {
                (void)target->EndDraw();
                hwndTarget_ = nullptr;
                EndPaint(panel_, &ps);
                return;
            }
            if (frame_.selectedRow < frame_.rows.size()) {
                DrawLight(target, layout_.RowRect(frame_.selectedRow), layout_.scale, style_);
            }
            DrawChrome(target, layout_, style_);
            const std::size_t selected = frame_.selectedRow;
            if (selected < frame_.rows.size()) {
                DrawSelection(target, layout_.RowRect(selected), layout_.scale, style_);
            }
            DrawCaption(target, formats_, layout_, frame_, style_);
            DrawRows(target, dwrite_.get(), formats_, layout_, frame_, style_, Ink::Normal, kNoRow, selected);
            if (selected < frame_.rows.size()) {
                DrawRows(target, dwrite_.get(), formats_, layout_, frame_, style_, Ink::Selected, selected,
                         kNoRow);
            }
            DrawPage(target, formats_, layout_, frame_, style_);
            if (frame_.badge) DrawBadge(target, layout_, frame_.badge, channel_->instance);
            if (target->EndDraw() == D2DERR_RECREATE_TARGET) hwndTarget_ = nullptr;
        }
        EndPaint(panel_, &ps);
    }

    std::shared_ptr<CandidateWindow::Channel> channel_;
    HWND control_{nullptr};
    HWND panel_{nullptr};
    bool composition_{false};
    bool compositionBroken_{false};
    bool windowsFailed_{false};
    bool hostBackdrop_{false};
    bool visible_{false};
    int badgeFade_{0};

    Frame frame_;
    Layout layout_;
    // The meaning's window and its layout (no text: hidden).
    HWND notePanel_{nullptr};
    bool noteShown_{false};
    Layout note_;
    wuc::Desktop::DesktopWindowTarget noteTarget_{nullptr};
    wuc::ContainerVisual noteRoot_{nullptr};
    wuc::SpriteVisual noteGlass_{nullptr};
    wuc::RectangleClip noteGlassClip_{nullptr};
    wuc::CompositionColorBrush noteSolid_{nullptr};
    wuc::CompositionBrush noteBackdrop_{nullptr};
    wuc::SpriteVisual noteChromeVisual_{nullptr};
    wuc::SpriteVisual noteContentVisual_{nullptr};
    Surface noteChrome_;
    Surface noteContent_;
    winrt::com_ptr<ID2D1HwndRenderTarget> noteHwndTarget_;
    Style style_;
    TextFormats formats_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    BrandFont brandFont_;
    winrt::com_ptr<ID2D1Factory1> d2dFactory_;

    winrt::Windows::System::DispatcherQueueController dispatcherQueue_{nullptr};
    wuc::Compositor compositor_{nullptr};
    wuc::Desktop::DesktopWindowTarget target_{nullptr};
    wuc::CompositionGraphicsDevice graphics_{nullptr};
    winrt::com_ptr<ID3D11Device> d3dDevice_;
    winrt::com_ptr<ID2D1Device> d2dDevice_;
    wuc::ContainerVisual root_{nullptr};
    wuc::SpriteVisual glassVisual_{nullptr};
    wuc::ContainerVisual lightAnchor_{nullptr};
    wuc::SpriteVisual lightVisual_{nullptr};
    wuc::RectangleClip glassClip_{nullptr};
    wuc::CompositionColorBrush solidBrush_{nullptr};
    wuc::CompositionBrush backdropBrush_{nullptr};
    wuc::SpriteVisual chromeVisual_{nullptr};
    wuc::SpriteVisual contentVisual_{nullptr};
    wuc::ContainerVisual selectionVisual_{nullptr};
    wuc::SpriteVisual capsuleVisual_{nullptr};
    wuc::ContainerVisual textWindow_{nullptr};
    wuc::RectangleClip textClip_{nullptr};
    wuc::SpriteVisual selectedTextVisual_{nullptr};
    wuc::ImplicitAnimationCollection selectionAnimations_{nullptr};
    Surface light_;
    Surface chrome_;
    Surface content_;
    Surface capsule_;
    Surface selectedText_;

    // The list's shadow and the meaning's.
    ShadowWindow shadow_;
    ShadowWindow noteShadow_;

    winrt::com_ptr<ID2D1HwndRenderTarget> hwndTarget_;
};

struct ThreadStart {
    std::shared_ptr<CandidateWindow::Channel> channel;
    HMODULE module{nullptr};
};

void RunUiThread(const std::shared_ptr<CandidateWindow::Channel>& channel) {
    if (channel->dpiContext) SetThreadDpiAwarenessContext(channel->dpiContext);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        CandidateView view(channel);
        const bool started = view.Start();
        channel->controlHwnd = started ? view.Control() : nullptr;
        SetEvent(channel->ready);
        if (started && !channel->abandoned) {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        view.Shutdown();
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
}

DWORD WINAPI UiThreadMain(void* parameter) {
    auto* start = static_cast<ThreadStart*>(parameter);
    const HMODULE module = start->module;
    try {
        RunUiThread(start->channel);
    } catch (...) {
    }
    delete start;
    // The thread holds its own reference on the module, so the DLL cannot be
    // unloaded while this code is still running.
    FreeLibraryAndExitThread(module, 0);
}

}  // namespace

CandidateWindow::CandidateWindow() = default;

CandidateWindow::~CandidateWindow() {
    Shutdown();
}

bool CandidateWindow::Initialize(HINSTANCE instance,
                                 std::function<void(std::size_t)> onSelection) {
    onSelection_ = std::move(onSelection);
    if (channel_) {
        return true;
    }
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = &CandidateWindow::NotifyWindowProc;
    wc.lpszClassName = kNotifyClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    notifyHwnd_ = CreateWindowExW(0, kNotifyClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  instance, this);
    if (!notifyHwnd_) {
        return false;
    }

    auto channel = std::make_shared<Channel>();
    channel->instance = instance;
    channel->notifyHwnd = notifyHwnd_;
    // Caret rectangles arrive in the host thread's coordinate space, so the
    // UI thread's windows must share its DPI awareness.
    channel->dpiContext = GetThreadDpiAwarenessContext();
    channel->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HMODULE module = nullptr;
    if (!channel->ready ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&UiThreadMain), &module)) {
        Shutdown();
        return false;
    }
    auto* start = new ThreadStart{channel, module};
    uiThread_ = CreateThread(nullptr, 0, &UiThreadMain, start, 0, nullptr);
    if (!uiThread_) {
        delete start;
        FreeLibrary(module);
        Shutdown();
        return false;
    }
    if (WaitForSingleObject(channel->ready, 5000) != WAIT_OBJECT_0) {
        // Too slow: the thread ends once it starts (or, started just now, is
        // told to by Shutdown).
        channel->abandoned = true;
    }
    channel_ = std::move(channel);
    if (channel_->abandoned || !channel_->controlHwnd) {
        Shutdown();
        return false;
    }
    return true;
}

void CandidateWindow::Show(const RECT& caretRect,
                           const std::vector<Candidate>& candidates,
                           std::size_t selectedIndex,
                           std::size_t pageStart,
                           std::size_t visibleCount,
                           const CandidateDetail& detail,
                           const std::wstring& caption,
                           std::size_t columnRows,
                           std::size_t page,
                           std::size_t pages) {
    if (!channel_) {
        return;
    }

    // The keys typed alone, with no candidates to list.
    if ((candidates.empty() || visibleCount == 0) && !caption.empty()) {
        candidates_.clear();
        selectedIndex_ = 0;
        pageStart_ = 0;
        visibleCount_ = 0;
        Frame frame;
        frame.visible = true;
        frame.caret = caretRect;
        frame.generation = ++generation_;
        frame.style = style_;
    frame.zoom = zoom_;
        frame.caption = caption;
        {
            std::lock_guard<std::mutex> lock(channel_->mutex);
            channel_->pending = std::move(frame);
            channel_->hasPending = true;
        }
        PostMessageW(channel_->controlHwnd, kMsgUpdate, 0, 0);
        shown_ = true;
        return;
    }

    if (candidates.empty() || visibleCount == 0) {
        candidates_.clear();
        selectedIndex_ = 0;
        pageStart_ = 0;
        visibleCount_ = 0;
        Hide();
        return;
    }

    candidates_ = candidates;
    selectedIndex_ = selectedIndex == kNoSelection ? kNoSelection
                                                   : std::min(selectedIndex, candidates_.size() - 1);
    pageStart_ = std::min(pageStart, candidates_.size() - 1);
    visibleCount_ = std::min(visibleCount, candidates_.size() - pageStart_);
    if (visibleCount_ == 0) {
        candidates_.clear();
        selectedIndex_ = 0;
        pageStart_ = 0;
        Hide();
        return;
    }

    if (listShown_ && !listShown_(candidates_, selectedIndex_, pageStart_, visibleCount_)) {
        HideWindow();
        return;
    }

    Frame frame;
    frame.visible = true;
    frame.caret = caretRect;
    frame.generation = ++generation_;
    frame.firstIndex = pageStart_;
    frame.style = style_;
    frame.zoom = zoom_;
    frame.rainbow = rainbow_;
    frame.totalCount = candidates_.size();
    frame.columnRows = columnRows;
    frame.page = page;
    frame.pages = pages;
    frame.rows.reserve(visibleCount_);
    const bool originalFirst = candidates_.front().isOriginal;
    for (std::size_t row = 0; row < visibleCount_; ++row) {
        const auto index = pageStart_ + row;
        const auto& candidate = candidates_[index];
        const auto displayId = candidate.id == 0 ? index + 1 : candidate.id;
        frame.rows.push_back({columnRows > 0 ? std::wstring{} : std::to_wstring(displayId), candidate.text,
                              LabelText(candidate, japanese_, originalFirst), candidate.hasMeaning,
                              candidate.groupStart && row > 0});
    }
    if (selectedIndex_ >= pageStart_ && selectedIndex_ - pageStart_ < visibleCount_) {
        frame.selectedRow = selectedIndex_ - pageStart_;
    }
    if (!detail.senses.empty()) {
        frame.detailHead = detail.headword;
        frame.detailForm = detail.form;
        frame.detailSenses = detail.senses;
    }
    frame.caption = caption;

    {
        std::lock_guard<std::mutex> lock(channel_->mutex);
        channel_->pending = std::move(frame);
        channel_->hasPending = true;
    }
    PostMessageW(channel_->controlHwnd, kMsgUpdate, 0, 0);
    shown_ = true;
}

void CandidateWindow::ShowModeBadge(const RECT& caretRect, InputMode mode) {
    if (!channel_) return;
    // The badge takes the list's place.
    if (listHidden_) {
        try {
            listHidden_();
        } catch (...) {
        }
    }
    candidates_.clear();
    selectedIndex_ = 0;
    pageStart_ = 0;
    visibleCount_ = 0;
    Frame frame;
    frame.visible = true;
    frame.caret = caretRect;
    frame.generation = ++generation_;
    frame.style = style_;
    frame.zoom = zoom_;
    frame.rainbow = rainbow_;
    frame.badge = mode == InputMode::Mixed ? kBadgeMixed
                  : TypesJapanese(mode)    ? kBadgeJapanese
                  : mode == InputMode::Direct ? kBadgeDirect
                                              : kBadgeAuto;
    {
        std::lock_guard<std::mutex> lock(channel_->mutex);
        channel_->pending = std::move(frame);
        channel_->hasPending = true;
    }
    PostMessageW(channel_->controlHwnd, kMsgUpdate, 0, 0);
    shown_ = true;
}

void CandidateWindow::SetStyle(int style) noexcept {
    style_ = style == 1 ? 1 : 0;
}

void CandidateWindow::SetZoom(int percent) noexcept {
    zoom_ = std::clamp(percent, 50, 200);
}

void CandidateWindow::Hide() noexcept {
    if (listHidden_) {
        try {
            listHidden_();
        } catch (...) {
        }
    }
    HideWindow();
}

void CandidateWindow::HideWindow() noexcept {
    if (!channel_ || !shown_) {
        return;
    }
    shown_ = false;
    ++generation_;  // clicks already in flight refer to the old list
    try {
        std::lock_guard<std::mutex> lock(channel_->mutex);
        channel_->pending = Frame{};
        channel_->pending.generation = generation_;
        channel_->hasPending = true;
    } catch (...) {
        return;
    }
    PostMessageW(channel_->controlHwnd, kMsgUpdate, 0, 0);
}

void CandidateWindow::Shutdown() noexcept {
    if (channel_ && channel_->controlHwnd) {
        PostMessageW(channel_->controlHwnd, kMsgQuit, 0, 0);
    }
    if (uiThread_) {
        // Keep servicing sent messages while waiting so the UI thread can
        // never deadlock against this one; give up after a bounded time (the
        // thread holds its own module reference, so leaving it is safe).
        const auto deadline = GetTickCount64() + 3000;
        for (;;) {
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            const DWORD result = MsgWaitForMultipleObjects(
                1, &uiThread_, FALSE, static_cast<DWORD>(deadline - now), QS_SENDMESSAGE);
            if (result != WAIT_OBJECT_0 + 1) break;
            MSG msg;
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        CloseHandle(uiThread_);
        uiThread_ = nullptr;
    }
    if (notifyHwnd_) {
        DestroyWindow(notifyHwnd_);
        notifyHwnd_ = nullptr;
        UnregisterClassW(kNotifyClassName, instance_);
    }
    channel_.reset();
    shown_ = false;
}

LRESULT CALLBACK CandidateWindow::NotifyWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                                   LPARAM lParam) {
    auto* self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<CandidateWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == kMsgSelect && self) {
        // A click selects by row on the page that was on screen; ignore it if
        // the list has changed since (the generation moved on).
        const auto row = static_cast<std::size_t>(wParam);
        const auto index = self->pageStart_ + row;
        if (self->shown_ && lParam == static_cast<LPARAM>(self->generation_) &&
            row < self->visibleCount_ && index < self->candidates_.size() && self->onSelection_) {
            self->onSelection_(index);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace tekito::tsf
