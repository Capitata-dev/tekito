#pragma once

// The candidate window's look: its sizes and colours, the layout of a
// frame, and drawing it (D2D, the same for the composition surfaces and
// the plain window). Private to CandidateWindow.cpp, which shows it.

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

namespace tekito::tsf::candidate_look {

namespace wuc = winrt::Windows::UI::Composition;
namespace abi = ABI::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

constexpr wchar_t kWindowClassName[] = L"TekitoCandidateWindow";
constexpr wchar_t kShadowClassName[] = L"TekitoCandidateShadow";
// The meaning beside the list, a window of its own.
constexpr wchar_t kNoteClassName[] = L"TekitoCandidateNote";
constexpr wchar_t kControlClassName[] = L"TekitoCandidateControl";
constexpr wchar_t kNotifyClassName[] = L"TekitoCandidateNotify";

constexpr UINT kMsgUpdate = WM_APP + 1;
constexpr UINT kMsgQuit = WM_APP + 2;
constexpr UINT kMsgSelect = WM_APP + 3;

constexpr UINT kBaseDpi = 96;

// Layout, in DIPs. Every corner is concentric: the selection capsule is a
// full pill (radius = half the row height) and the panel corner is that
// radius plus the padding around the capsule, so the two curves stay
// parallel the way nested glass shapes do.
constexpr int kRowHeight = 28;
constexpr int kRowGap = 2;
constexpr int kPanelPadding = 6;
constexpr int kCornerRadius = kRowHeight / 2 + kPanelPadding;
// DWMWCP_ROUND radius, used when composition is unavailable and DWM clips.
constexpr int kFallbackCornerRadius = 8;
constexpr int kRowPaddingLeft = 12;
constexpr int kNumberWidth = 16;
constexpr int kNumberGap = 10;
constexpr int kRowPaddingRight = 14;
constexpr int kTagGap = 10;
constexpr int kTagPaddingX = 7;
constexpr int kTagHeight = 16;
constexpr float kTagTracking = 0.6f;
constexpr int kMinWidth = 200;
constexpr int kMaxWidth = 440;
// The pages after the first: columns of a fixed width each, as wide as the
// widest candidate on the page within these.
constexpr int kColumnMinWidth = 96;
constexpr int kColumnMaxWidth = 220;
constexpr int kColumnGap = 4;
// The meaning in a window of its own beside the list: as wide as its text
// up to kDetailWidth, its headword level with the selected row (up to
// kDetailMaxHeight, dropping later senses to fit), kNoteGap from the list.
constexpr int kDetailWidth = 250;
constexpr int kNoteGap = 8;
constexpr int kDetailPadding = 14;
constexpr int kDetailMaxHeight = 300;
constexpr int kCaretGap = 6;
// The mode badge: a row's height square (a circle in glass), held, then
// faded out.
constexpr int kBadgeIcon = 24;
constexpr UINT_PTR kBadgeTimer = 7;
// Rainbow: the accent's hue goes round while the list is shown.
constexpr UINT_PTR kRainbowTimer = 8;
constexpr UINT kRainbowTickMs = 50;
constexpr ULONGLONG kRainbowRoundMs = 4000;

// The accent at this moment of the rainbow (hue from the clock).
inline D2D1_COLOR_F RainbowColor(bool dark) {
    const float hue = static_cast<float>(GetTickCount64() % kRainbowRoundMs) / kRainbowRoundMs * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(hue)) {
    case 0: r = 1; g = x; break;
    case 1: r = x; g = 1; break;
    case 2: g = 1; b = x; break;
    case 3: g = x; b = 1; break;
    case 4: r = x; b = 1; break;
    default: r = 1; b = x; break;
    }
    // Lighter on dark glass, deeper on light, as the Windows accent shades are.
    const float low = dark ? 0.35f : 0.0f, high = dark ? 1.0f : 0.85f;
    const auto shade = [&](float c) { return low + (high - low) * c; };
    return {shade(r), shade(g), shade(b), 1.0f};
}
constexpr UINT kBadgeHoldMs = 850;
constexpr UINT kBadgeFadeTickMs = 25;
constexpr int kBadgeFadeSteps = 8;
constexpr int kSelectionPad = 8;
constexpr int kGlowDepth = 9;
constexpr int kShadeDepth = 4;
constexpr int kEdgeDepth = 2;
constexpr int kRecessDepth = 4;
constexpr int kLightSpread = 22;

constexpr float kFontCandidate = 14.0f;
constexpr float kFontNumber = 12.0f;
constexpr float kFontTag = 9.0f;
constexpr float kFontDetail = 12.5f;
// The keys typed, small above the list (UserSettings::showTypedKeys).
constexpr float kFontCaption = 12.0f;
constexpr int kCaptionHeight = 20;
// The page shown of how many ("1 / 3"), small under the rows, a gap apart
// from them so it does not read as one more row.
constexpr int kFooterGap = 10;
constexpr int kFooterHeight = 20 + kFooterGap;
// The dictionary sign on rows with a meaning: the open book ("ReadingMode")
// of the Windows icon font, which reads as a book even this small.
constexpr float kFontIcon = 13.0f;
constexpr int kIconWidth = 14;
constexpr int kIconGap = 6;
constexpr wchar_t kDictionaryGlyph[] = L"\xE736";
constexpr float kFontDetailHead = 15.0f;

// Floating shadow: a tight contact shadow for edge definition plus a soft
// ambient one for lift. It lives in a separate click-through window so the
// area it covers does not swallow clicks meant for the app underneath.
constexpr float kShadowKeyBlur = 1.2f;
constexpr float kShadowKeyOffset = 1.0f;
constexpr float kShadowAmbientBlur = 7.0f;
constexpr float kShadowAmbientOffset = 6.0f;
constexpr int kShadowMarginX = 16;
constexpr int kShadowMarginTop = 10;
constexpr int kShadowMarginBottom = 26;

constexpr auto kGlideDuration = std::chrono::milliseconds(180);

// A little extra color in the blurred backdrop keeps the frosted glass from
// looking gray and dead without making it a clear window.
constexpr float kBackdropSaturation = 1.3f;

// Where light catches a glass edge: a faint line all the way round, a hot
// spot on the top-left curve and a weaker one diagonally opposite, as if lit
// from the upper left.
struct Specular {
    float base;
    float topLeft;
    float bottomRight;
};

struct Palette {
    D2D1_COLOR_F solid;       // panel base where no backdrop blur is available
    D2D1_COLOR_F tintTop;     // frosted veil over the blur, top edge
    D2D1_COLOR_F tintBottom;  // ... and bottom edge
    D2D1_COLOR_F glow;        // luminous band just inside the edge
    D2D1_COLOR_F shade;       // darker band inside the bottom edge
    D2D1_COLOR_F edge;        // thin dark ring inside the rim: the thickness of the slab
    D2D1_COLOR_F gleam;       // soft light pooled in the top-left of the panel
    D2D1_COLOR_F rim;         // specular edge color
    Specular rimLight;
    float rimBottom;          // bright lip along the bottom edge
    D2D1_COLOR_F text;        // unselected rows sit back a little ...
    D2D1_COLOR_F mutedText;
    D2D1_COLOR_F tagFill;
    D2D1_COLOR_F indicator;
    float shadowKey;
    float shadowAmbient;
};

constexpr Palette kLightPalette{
    {0.953f, 0.957f, 0.965f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.56f},
    {1.0f, 1.0f, 1.0f, 0.42f},
    {1.0f, 1.0f, 1.0f, 0.50f},
    {0.0f, 0.0f, 0.0f, 0.05f},
    {0.0f, 0.0f, 0.0f, 0.06f},
    {1.0f, 1.0f, 1.0f, 0.20f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.30f, 1.0f, 0.60f},
    0.90f,
    {0.180f, 0.196f, 0.231f, 1.0f},
    {0.330f, 0.357f, 0.404f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.06f},
    {0.0f, 0.0f, 0.0f, 0.28f},
    0.14f,
    0.16f,
};

constexpr Palette kDarkPalette{
    {0.110f, 0.118f, 0.133f, 1.0f},
    {0.130f, 0.140f, 0.160f, 0.62f},
    {0.070f, 0.080f, 0.090f, 0.72f},
    {1.0f, 1.0f, 1.0f, 0.08f},
    {0.0f, 0.0f, 0.0f, 0.25f},
    {0.0f, 0.0f, 0.0f, 0.35f},
    {1.0f, 1.0f, 1.0f, 0.05f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.10f, 0.50f, 0.25f},
    0.40f,
    {0.780f, 0.792f, 0.816f, 1.0f},
    {0.530f, 0.557f, 0.604f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.08f},
    {1.0f, 1.0f, 1.0f, 0.36f},
    0.35f,
    0.40f,
};

// The selection is a pill pressed into the glass slab: a little darker than
// the panel, shadowed under its top edge and catching light on its lower
// lip. Under the frosted glass beneath it sits a soft pool of accent light,
// diffused by the frost, which follows the selection from row to row. The
// selected word is simply brighter (dark: whiter, light: blacker) than the
// rest.
struct SelectionStyle {
    D2D1_COLOR_F fill;
    D2D1_COLOR_F text;
    D2D1_COLOR_F mutedText;
    D2D1_COLOR_F tagFill;
    D2D1_COLOR_F recess;  // inner shadow below the pill's top edge
    float rimBottom;      // light on the pill's lower lip
    float light;          // peak of the accent light under the glass
    bool glass;
};

constexpr SelectionStyle kLightSelection{
    {0.10f, 0.12f, 0.16f, 0.07f},
    {0.020f, 0.024f, 0.031f, 1.0f},
    {0.200f, 0.216f, 0.251f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.07f},
    {0.0f, 0.0f, 0.0f, 0.10f},
    0.95f,
    0.16f,
    true,
};

constexpr SelectionStyle kDarkSelection{
    {0.0f, 0.0f, 0.0f, 0.30f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.800f, 0.816f, 0.840f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.10f},
    {0.0f, 0.0f, 0.0f, 0.45f},
    0.22f,
    0.50f,
    true,
};
// "Simple" style: the first Direct2D design. An opaque panel with a thin
// border, a flat tinted selection row marked by a short accent rail, and no
// glass effects.
constexpr Palette kSimpleLightPalette{
    {1.0f, 1.0f, 1.0f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {0.863f, 0.898f, 0.945f, 1.0f},
    {1.0f, 0.0f, 0.0f},
    0.0f,
    {0.110f, 0.137f, 0.184f, 1.0f},
    {0.400f, 0.439f, 0.502f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.28f},
    0.10f,
    0.12f,
};

constexpr Palette kSimpleDarkPalette{
    {0.118f, 0.125f, 0.141f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {0.235f, 0.247f, 0.278f, 1.0f},
    {1.0f, 0.0f, 0.0f},
    0.0f,
    {0.902f, 0.910f, 0.925f, 1.0f},
    {0.588f, 0.612f, 0.655f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.36f},
    0.35f,
    0.40f,
};

constexpr SelectionStyle kSimpleLightSelection{
    {0.933f, 0.969f, 1.0f, 1.0f},
    kSimpleLightPalette.text,
    kSimpleLightPalette.mutedText,
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    0.0f,
    0.0f,
    false,
};

constexpr SelectionStyle kSimpleDarkSelection{
    {0.176f, 0.227f, 0.314f, 1.0f},
    kSimpleDarkPalette.text,
    kSimpleDarkPalette.mutedText,
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    0.0f,
    0.0f,
    false,
};

constexpr int kSimpleCornerRadius = 12;
constexpr int kSimpleRowRadius = 8;
constexpr int kSimpleRailWidth = 3;
// Unnumbered rows (the columns): room between the rail and the text.
constexpr int kRailClearance = 8;

struct Style {
    Palette palette{kLightPalette};
    SelectionStyle selection{};
    D2D1_COLOR_F light{0.0f, 0.471f, 0.831f, 1.0f};  // color of the light under the selection
    D2D1_COLOR_F rail{0.024f, 0.396f, 1.0f, 1.0f};    // simple style: selection marker
    bool glass{false};
    bool simple{false};
    bool animations{true};
};

struct Row {
    std::wstring number;
    std::wstring text;
    std::wstring label;
    bool meaning{false};
    // A thin line above it (Candidate::groupStart).
    bool groupStart{false};
};

constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

struct Frame {
    bool visible{false};
    RECT caret{};
    std::vector<Row> rows;
    std::size_t selectedRow{kNoRow};
    std::size_t firstIndex{0};
    std::size_t totalCount{0};
    // Columns of this many rows, down then across (0: one column).
    std::size_t columnRows{0};
    // The page shown (from 0) of how many (0 or 1: no page line).
    std::size_t page{0};
    std::size_t pages{0};
    std::uint64_t generation{0};
    int style{0};  // CandidateWindow::Style
    int zoom{100};  // CandidateWindow::SetZoom, percent
    bool rainbow{false};  // CandidateWindow::SetRainbow
    std::wstring detailHead;
    std::wstring detailForm;
    std::vector<std::wstring> detailSenses;
    // A mode badge instead of rows: 0 none, else the mode.
    int badge{0};
    // The keys typed, small above the rows (alone when there are none).
    std::wstring caption;
};



inline DWORD ReadPersonalizeValue(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    const auto result = RegGetValueW(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return result == ERROR_SUCCESS ? value : fallback;
}

inline bool IsDarkThemeActive() {
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
    wchar_t theme[16]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_THEME", theme, 16) > 0) {
        return wcscmp(theme, L"dark") == 0;
    }
#endif
    return ReadPersonalizeValue(L"AppsUseLightTheme", 1) == 0;
}

// Settings > Personalization > Colors > Transparency effects.
inline bool IsTransparencyEnabled() {
    return ReadPersonalizeValue(L"EnableTransparency", 1) != 0;
}

inline bool IsHighContrast() {
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_HIGH_CONTRAST", nullptr, 0) > 0) return true;
#endif
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

// Settings > Accessibility > Visual effects > Animation effects.
inline bool AreAnimationsEnabled() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled != FALSE;
}

inline D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) {
    color.a = alpha;
    return color;
}

inline D2D1_COLOR_F SystemColor(int index) {
    const COLORREF color = GetSysColor(index);
    return {GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, 1.0f};
}

// The light under the selection takes the user's Windows accent color: a
// brighter shade on dark glass, the base shade on light glass.
inline D2D1_COLOR_F AccentLight(bool dark) {
    try {
        using winrt::Windows::UI::ViewManagement::UIColorType;
        const auto color = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
            dark ? UIColorType::AccentLight1 : UIColorType::Accent);
        return {color.R / 255.0f, color.G / 255.0f, color.B / 255.0f, 1.0f};
    } catch (...) {
        return dark ? D2D1_COLOR_F{0.298f, 0.694f, 1.0f, 1.0f} : D2D1_COLOR_F{0.0f, 0.471f, 0.831f, 1.0f};
    }
}

inline Style ReadStyle(bool compositionGlass, bool simple) {
    Style style;
    style.animations = AreAnimationsEnabled();
    style.simple = simple;
    if (IsHighContrast()) {
        // High contrast wants the user's exact system colors and no effects.
        Palette& p = style.palette;
        p.solid = SystemColor(COLOR_WINDOW);
        p.tintTop = p.tintBottom = p.glow = p.shade = p.edge = p.gleam = WithAlpha(p.solid, 0.0f);
        p.rim = SystemColor(COLOR_WINDOWTEXT);
        p.rimLight = {1.0f, 0.0f, 0.0f};
        p.rimBottom = 0.0f;
        p.text = p.mutedText = SystemColor(COLOR_WINDOWTEXT);
        p.tagFill = WithAlpha(p.solid, 0.0f);
        p.shadowKey = p.shadowAmbient = 0.0f;
        const auto highlightText = SystemColor(COLOR_HIGHLIGHTTEXT);
        const auto none = WithAlpha(highlightText, 0.0f);
        style.selection = {SystemColor(COLOR_HIGHLIGHT), highlightText, highlightText, none, none,
                           0.0f, 0.0f, false};
        style.glass = false;
        return style;
    }
    const bool dark = IsDarkThemeActive();
    if (simple) {
        style.palette = dark ? kSimpleDarkPalette : kSimpleLightPalette;
        style.selection = dark ? kSimpleDarkSelection : kSimpleLightSelection;
        style.rail = dark ? D2D1_COLOR_F{0.353f, 0.627f, 1.0f, 1.0f} : D2D1_COLOR_F{0.024f, 0.396f, 1.0f, 1.0f};
        style.glass = false;
        return style;
    }
    style.palette = dark ? kDarkPalette : kLightPalette;
    style.selection = dark ? kDarkSelection : kLightSelection;
    style.light = AccentLight(dark);
    style.glass = compositionGlass && IsTransparencyEnabled();
    return style;
}

// The list's size the user chose, in percent (CandidateWindow::SetZoom),
// for the frame the UI thread is drawing: it scales as the DPI does.
inline thread_local int t_zoomPercent = 100;

inline UINT ZoomedDpi(HWND hwnd) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : kBaseDpi;
    return static_cast<UINT>(MulDiv(static_cast<int>(dpi == 0 ? kBaseDpi : dpi), t_zoomPercent, 100));
}

inline int ScaleForWindow(HWND hwnd, int value) {
    return MulDiv(value, static_cast<int>(ZoomedDpi(hwnd)), kBaseDpi);
}

inline float ScaleFactor(HWND hwnd) {
    return static_cast<float>(ZoomedDpi(hwnd)) / static_cast<float>(kBaseDpi);
}

inline RECT WorkAreaNear(const RECT& rect) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }

    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        return work;
    }
    return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

inline POINT CandidatePosition(const RECT& caretRect, int width, int height, int caretGap) {
    const RECT work = WorkAreaNear(caretRect);

    int x = caretRect.left;
    if (x + width > work.right) x = work.right - width;
    if (x < work.left) x = work.left;

    int y = caretRect.bottom + caretGap;
    if (y + height > work.bottom) y = caretRect.top - height - caretGap;
    if (y < work.top) y = work.top;

    return {x, y};
}

// A row's tag, only where its text does not tell and knowing changes the
// pick; one a row. What a candidate is (emoji, a date, katakana) shows.
// `originalFirst`: what was typed leads the list, so it needs no tag.
inline std::wstring LabelText(const Candidate& candidate, bool japanese, bool originalFirst) {
    // The empty row that takes out a word typed twice: its tag is all it shows.
    if ((candidate.sourceFlags & CandidateSourceDoubledWord) != 0) return japanese ? L"重複を削除" : L"REMOVE REPEAT";
    // Not what was typed: the user should know before picking it.
    if (candidate.label == SemanticLabel::Suggestion) return japanese ? L"もしかして" : L"TYPO";
    // What was typed, when something else comes first.
    if (candidate.isOriginal) return originalFirst ? std::wstring{} : japanese ? L"そのまま" : L"ORIGINAL";
    // Slang offered for what was typed.
    if (candidate.label == SemanticLabel::Slang) return japanese ? L"スラング" : L"SLANG";
    // The same as another row but for width, which is hard to see.
    if (candidate.label == SemanticLabel::FullWidth) return japanese ? L"全角" : L"FULL-WIDTH";
    if (candidate.label == SemanticLabel::HalfWidth) return japanese ? L"半角" : L"HALF-WIDTH";
    return {};
}

// ---------------------------------------------------------------------------
// Layout and drawing. Everything is in physical pixels (D2D at 96 DPI), with
// DIP constants scaled by the window DPI, so the same routines serve the
// composition surfaces and the HWND fallback.

struct TextFormats {
    UINT dpi{0};
    winrt::com_ptr<IDWriteTextFormat> text;
    winrt::com_ptr<IDWriteTextFormat> textSelected;
    winrt::com_ptr<IDWriteTextFormat> number;
    winrt::com_ptr<IDWriteTextFormat> tag;
    winrt::com_ptr<IDWriteTextFormat> detail;
    winrt::com_ptr<IDWriteTextFormat> icon;
    winrt::com_ptr<IDWriteTextFormat> caption;
    winrt::com_ptr<IDWriteTextFormat> page;
    // How far down the numbers go to share the candidates' baseline.
    float numberShift{0.0f};
};

struct Layout {
    int width{0};
    int height{0};
    float scale{1.0f};
    float radius{0.0f};
    float padding{0.0f};
    float rowHeight{0.0f};
    float rowPitch{0.0f};
    float rowPaddingLeft{0.0f};
    float numberWidth{0.0f};
    float numberGap{0.0f};
    float rowPaddingRight{0.0f};
    float tagGap{0.0f};
    float tagPaddingX{0.0f};
    float tagHeight{0.0f};
    float tagTracking{0.0f};
    float rowRight{0.0f};
    float iconWidth{0.0f};
    float iconGap{0.0f};
    // The list part; the rest of the width is the meaning pane.
    float listWidth{0.0f};
    float listHeight{0.0f};
    D2D1_RECT_F detail{};
    winrt::com_ptr<IDWriteTextLayout> detailText;
    // The page line under the rows (0: none).
    float footerHeight{0.0f};
    // The caption's line above the rows (0: none).
    float captionHeight{0.0f};
    // Columns: rows in each, a column's width and its pitch (0: one column).
    std::size_t columnRows{0};
    float columnWidth{0.0f};
    float columnPitch{0.0f};

    [[nodiscard]] D2D1_RECT_F RowRect(std::size_t row) const {
        if (columnRows > 0) {
            const float left = padding + static_cast<float>(row / columnRows) * columnPitch;
            const float top = padding + captionHeight + static_cast<float>(row % columnRows) * rowPitch;
            return D2D1::RectF(left, top, left + columnWidth, top + rowHeight);
        }
        const float top = padding + captionHeight + static_cast<float>(row) * rowPitch;
        return D2D1::RectF(padding, top, rowRight, top + rowHeight);
    }
};

inline D2D1_RECT_F Inset(const D2D1_RECT_F& rect, float amount) {
    return D2D1::RectF(rect.left + amount, rect.top + amount, rect.right - amount,
                       rect.bottom - amount);
}

inline D2D1_ROUNDED_RECT Rounded(const D2D1_RECT_F& rect, float radius) {
    const float r = std::max(radius, 0.0f);
    return D2D1::RoundedRect(rect, r, r);
}

inline winrt::com_ptr<ID2D1SolidColorBrush> SolidBrush(ID2D1RenderTarget* target, const D2D1_COLOR_F& color) {
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(color, brush.put());
    return brush;
}

inline winrt::com_ptr<ID2D1LinearGradientBrush> GradientBrush(ID2D1RenderTarget* target,
                                                       D2D1_POINT_2F from, D2D1_POINT_2F to,
                                                       const D2D1_GRADIENT_STOP* stops,
                                                       UINT32 count) {
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1LinearGradientBrush> brush;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, count, collection.put()))) {
        target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to),
                                          collection.get(), brush.put());
    }
    return brush;
}

// The TEKITO typeface (M PLUS 1) ships inside this DLL. It is loaded into a
// private collection on the candidate window's own DirectWrite factory, so
// the host application's fonts are never touched.
struct BrandFont {
    winrt::com_ptr<IDWriteInMemoryFontFileLoader> loader;
    winrt::com_ptr<IDWriteFontCollection1> collection;
};

inline BrandFont LoadBrandFont(IDWriteFactory* factory, HINSTANCE module) {
    BrandFont font;
    winrt::com_ptr<IDWriteFactory5> factory5;
    if (factory) factory->QueryInterface(IID_PPV_ARGS(factory5.put()));
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_FONT_MPLUS1), RT_RCDATA);
    const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    const void* bytes = loaded && size ? LockResource(loaded) : nullptr;
    if (!factory5 || !bytes) return font;
    winrt::com_ptr<IDWriteFontFile> file;
    winrt::com_ptr<IDWriteFontSetBuilder1> builder;
    winrt::com_ptr<IDWriteFontSet> set;
    if (FAILED(factory5->CreateInMemoryFontFileLoader(font.loader.put())) ||
        FAILED(factory5->RegisterFontFileLoader(font.loader.get()))) {
        font.loader = nullptr;
        return font;
    }
    // The resource stays mapped for as long as the module is loaded, so the
    // loader can reference it without a copy.
    if (FAILED(font.loader->CreateInMemoryFontFileReference(factory5.get(), bytes, size, nullptr, file.put())) ||
        FAILED(factory5->CreateFontSetBuilder(builder.put())) ||
        FAILED(builder->AddFontFile(file.get())) || FAILED(builder->CreateFontSet(set.put())) ||
        FAILED(factory5->CreateFontCollectionFromFontSet(set.get(), font.collection.put()))) {
        font.collection = nullptr;
    }
    return font;
}

inline winrt::com_ptr<IDWriteTextFormat> CreateFormat(IDWriteFactory* factory, IDWriteFontCollection* brand,
                                               float size, DWRITE_FONT_WEIGHT weight,
                                               DWRITE_TEXT_ALIGNMENT alignment, bool ellipsis) {
    winrt::com_ptr<IDWriteTextFormat> format;
    // Japanese locale: kanji and kana that M PLUS 1 lacks fall back to a
    // Japanese font, with Japanese glyph shapes. Latin text is unaffected.
    if (FAILED(factory->CreateTextFormat(brand ? L"M PLUS 1" : L"Segoe UI Variable Text", brand, weight,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         size, L"ja-jp", format.put()))) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
    if (ellipsis) {
        // Over-long candidates end in "..." instead of being cut mid-glyph.
        winrt::com_ptr<IDWriteInlineObject> sign;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format.get(), sign.put()))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, sign.get());
        }
    }
    return format;
}

// The Windows icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2
// Assets (Windows 10); nullptr when neither is installed.
inline const wchar_t* IconFontFamily(IDWriteFactory* factory) {
    winrt::com_ptr<IDWriteFontCollection> fonts;
    if (!factory || FAILED(factory->GetSystemFontCollection(fonts.put(), FALSE))) return nullptr;
    for (const wchar_t* family : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists) return family;
    }
    return nullptr;
}

inline float MeasureTextWidth(IDWriteFactory* factory, IDWriteTextFormat* format, const std::wstring& text) {
    if (text.empty() || !factory || !format) return 0.0f;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                         10000.0f, 1000.0f, layout.put()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

// Small-caps style tag text with a little tracking, split evenly before and
// after each glyph so the word stays centered in its capsule.
inline winrt::com_ptr<IDWriteTextLayout> TagLayout(IDWriteFactory* factory, IDWriteTextFormat* format,
                                            const std::wstring& text, float height, float tracking) {
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!factory || !format ||
        FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                         1000.0f, height, layout.put()))) {
        return nullptr;
    }
    if (const auto layout1 = layout.try_as<IDWriteTextLayout1>()) {
        layout1->SetCharacterSpacing(tracking / 2.0f, tracking / 2.0f, 0.0f,
                                     {0, static_cast<UINT32>(text.size())});
    }
    return layout;
}

inline float LayoutWidth(IDWriteTextLayout* layout) {
    if (!layout) return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

// A soft band along the inside of a rounded shape, drawn as concentric 1px
// rings whose opacity falls off like a Gaussian away from the edge. `brush`
// carries the color and any directional weighting (e.g. top-heavy).
inline void InnerBand(ID2D1RenderTarget* target, const D2D1_RECT_F& shape, float radius, float scale,
               int depthDips, float peak, const winrt::com_ptr<ID2D1LinearGradientBrush>& brush) {
    if (!brush || peak <= 0.0f) return;
    const int depth = std::max(1, static_cast<int>(std::lround(static_cast<float>(depthDips) * scale)));
    const float sigma = static_cast<float>(depth) / 2.6f;
    for (int ring = 0; ring < depth; ++ring) {
        const float inset = static_cast<float>(ring) + 0.5f;
        const float falloff = std::exp(-static_cast<float>(ring * ring) / (2.0f * sigma * sigma));
        brush->SetOpacity(peak * falloff);
        target->DrawRoundedRectangle(Rounded(Inset(shape, inset), radius - inset), brush.get(), 1.0f);
    }
}

// A soft pool of light: `color` at its own alpha at `center`, fading to
// nothing at `reach`.
inline winrt::com_ptr<ID2D1RadialGradientBrush> Hotspot(ID2D1RenderTarget* target, D2D1_POINT_2F center,
                                                 float reach, const D2D1_COLOR_F& color) {
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, color},
                                        {0.4f, WithAlpha(color, color.a * 0.4f)},
                                        {1.0f, WithAlpha(color, 0.0f)}};
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1RadialGradientBrush> brush;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, 3, collection.put()))) {
        target->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0.0f, 0.0f), reach, reach),
            collection.get(), brush.put());
    }
    return brush;
}

// The edge of a glass shape catching light; see Specular.
inline void SpecularRim(ID2D1RenderTarget* target, const D2D1_RECT_F& shape, float radius, float scale,
                 const D2D1_COLOR_F& color, const Specular& light) {
    const float width = std::max(1.0f, std::floor(scale));
    const auto edge = Rounded(Inset(shape, width / 2.0f), radius - width / 2.0f);
    if (light.base > 0.0f) {
        auto base = SolidBrush(target, WithAlpha(color, light.base));
        target->DrawRoundedRectangle(edge, base.get(), width);
    }
    const float reach = 2.5f * radius + 0.35f * std::min(shape.right - shape.left, shape.bottom - shape.top);
    const float inset = 0.3f * radius;
    if (light.topLeft > 0.0f) {
        if (auto glint = Hotspot(target, D2D1::Point2F(shape.left + inset, shape.top + inset), reach,
                                 WithAlpha(color, light.topLeft))) {
            target->DrawRoundedRectangle(edge, glint.get(), width);
        }
    }
    if (light.bottomRight > 0.0f) {
        if (auto glint = Hotspot(target, D2D1::Point2F(shape.right - inset, shape.bottom - inset),
                                 0.8f * reach, WithAlpha(color, light.bottomRight))) {
            target->DrawRoundedRectangle(edge, glint.get(), width);
        }
    }
}
// Light caught along the lower edge of a glass shape, fading out up its sides.
inline void BottomLip(ID2D1RenderTarget* target, const D2D1_RECT_F& shape, float radius, float scale,
               const D2D1_COLOR_F& color) {
    if (color.a <= 0.0f) return;
    const float width = std::max(1.0f, std::floor(scale));
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, WithAlpha(color, 0.0f)},
                                        {0.55f, WithAlpha(color, 0.0f)},
                                        {1.0f, color}};
    if (auto lip = GradientBrush(target, D2D1::Point2F(0.0f, shape.top), D2D1::Point2F(0.0f, shape.bottom),
                                 stops, 3)) {
        target->DrawRoundedRectangle(Rounded(Inset(shape, width / 2.0f), radius - width / 2.0f), lip.get(),
                                     width);
    }
}

// The glass body: translucent veil, luminous inner edge and specular rim.
// `glass` is false when there is no blur underneath (transparency effects
// off, high contrast, or the HWND fallback), which drops the glow.
inline void DrawChrome(ID2D1RenderTarget* target, const Layout& layout, const Style& style) {
    const Palette& p = style.palette;
    const float width = static_cast<float>(layout.width);
    const float height = static_cast<float>(layout.height);
    const D2D1_RECT_F panel = D2D1::RectF(0.0f, 0.0f, width, height);

    const D2D1_GRADIENT_STOP tintStops[] = {{0.0f, p.tintTop}, {1.0f, p.tintBottom}};
    if (auto tint = GradientBrush(target, D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height),
                                  tintStops, 2)) {
        target->FillRoundedRectangle(Rounded(panel, layout.radius), tint.get());
    }

    if (style.glass) {
        // Thickness: a band of light just inside the edge, strongest at the
        // top, and a faint darker band inside the bottom edge.
        const D2D1_GRADIENT_STOP glowStops[] = {{0.0f, WithAlpha(p.glow, 1.0f)},
                                                {1.0f, WithAlpha(p.glow, 0.35f)}};
        const D2D1_GRADIENT_STOP shadeStops[] = {{0.0f, WithAlpha(p.shade, 0.0f)},
                                                 {0.55f, WithAlpha(p.shade, 0.0f)},
                                                 {1.0f, WithAlpha(p.shade, 1.0f)}};
        InnerBand(target, panel, layout.radius, layout.scale, kGlowDepth, p.glow.a,
                  GradientBrush(target, D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height),
                                glowStops, 2));
        InnerBand(target, panel, layout.radius, layout.scale, kShadeDepth, p.shade.a,
                  GradientBrush(target, D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height),
                                shadeStops, 3));
    }

    if (style.glass && p.gleam.a > 0.0f) {
        if (auto gleam = Hotspot(target, D2D1::Point2F(0.0f, 0.0f),
                                 0.9f * std::min(width, height) + layout.radius, p.gleam)) {
            target->FillRoundedRectangle(Rounded(panel, layout.radius), gleam.get());
        }
    }

    if (p.edge.a > 0.0f) {
        const D2D1_GRADIENT_STOP edgeStops[] = {{0.0f, WithAlpha(p.edge, 1.0f)},
                                                {1.0f, WithAlpha(p.edge, 1.0f)}};
        InnerBand(target, panel, layout.radius, layout.scale, kEdgeDepth, p.edge.a,
                  GradientBrush(target, D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height),
                                edgeStops, 2));
    }

    SpecularRim(target, panel, layout.radius, layout.scale, p.rim, p.rimLight);
    BottomLip(target, panel, layout.radius, layout.scale, WithAlpha(p.rim, p.rimBottom));
}

// Soft pool of accent light under the frosted glass, centered on the
// selected row. Drawn below the veil so the frost diffuses it.
inline void DrawLight(ID2D1RenderTarget* target, const D2D1_RECT_F& capsule, float scale, const Style& style) {
    const float peak = style.selection.light;
    if (peak <= 0.0f) return;
    const float radius = (capsule.bottom - capsule.top) / 2.0f;
    auto light = SolidBrush(target, WithAlpha(style.light, 1.0f));
    if (!light) return;
    light->SetOpacity(peak);
    target->FillRoundedRectangle(Rounded(capsule, radius), light.get());
    const int spread = std::max(1, static_cast<int>(std::lround(kLightSpread * scale)));
    const float sigma = static_cast<float>(spread) / 2.6f;
    for (int ring = 0; ring < spread; ++ring) {
        const float outset = static_cast<float>(ring) + 0.5f;
        const float falloff = std::exp(-static_cast<float>(ring * ring) / (2.0f * sigma * sigma));
        light->SetOpacity(peak * falloff);
        target->DrawRoundedRectangle(Rounded(Inset(capsule, -outset), radius + outset), light.get(), 1.0f);
    }
}

// The selection pill: see SelectionStyle.
inline void DrawSelection(ID2D1RenderTarget* target, const D2D1_RECT_F& capsule, float scale,
                   const Style& style) {
    const SelectionStyle& s = style.selection;
    const float radius = style.simple ? std::min(kSimpleRowRadius * scale, (capsule.bottom - capsule.top) / 2.0f)
                                      : (capsule.bottom - capsule.top) / 2.0f;

    auto fill = SolidBrush(target, s.fill);
    target->FillRoundedRectangle(Rounded(capsule, radius), fill.get());
    if (style.simple) {
        const float inset = std::round(8.0f * scale);
        const float rail = std::max(1.0f, std::round(kSimpleRailWidth * scale));
        const float vertical = std::round(6.0f * scale);
        auto accent = SolidBrush(target, style.rail);
        target->FillRoundedRectangle(
            Rounded(D2D1::RectF(capsule.left + inset, capsule.top + vertical, capsule.left + inset + rail,
                                capsule.bottom - vertical),
                    rail / 2.0f),
            accent.get());
        return;
    }
    if (!s.glass) return;

    const auto top = D2D1::Point2F(0.0f, capsule.top);
    const auto bottom = D2D1::Point2F(0.0f, capsule.bottom);
    const D2D1_GRADIENT_STOP recessStops[] = {{0.0f, WithAlpha(s.recess, 1.0f)},
                                              {0.65f, WithAlpha(s.recess, 0.0f)}};
    InnerBand(target, capsule, radius, scale, kRecessDepth, s.recess.a,
              GradientBrush(target, top, bottom, recessStops, 2));
    BottomLip(target, capsule, radius, scale, D2D1::ColorF(1.0f, 1.0f, 1.0f, s.rimBottom));
}
enum class Ink { Normal, Selected };

// Draws the rows' numbers, tags and text. The composition path draws every
// row twice, once per ink, and lets the moving capsule reveal the Selected
// layer; the HWND fallback draws only `onlyRow` in Selected ink.
inline void DrawRows(ID2D1RenderTarget* target, IDWriteFactory* factory, const TextFormats& formats,
              const Layout& layout, const Frame& frame, const Style& style, Ink ink,
              std::size_t onlyRow, std::size_t skipRow) {
    const bool selected = ink == Ink::Selected;
    const Palette& p = style.palette;
    const SelectionStyle& s = style.selection;
    auto textBrush = SolidBrush(target, selected ? s.text : p.text);
    auto mutedBrush = SolidBrush(target, selected ? s.mutedText : p.mutedText);
    auto tagBrush = SolidBrush(target, selected ? s.tagFill : p.tagFill);
    // The simple style marks the selection with its row and rail only.
    IDWriteTextFormat* textFormat =
        selected && !style.simple ? formats.textSelected.get() : formats.text.get();

    for (std::size_t i = 0; i < frame.rows.size(); ++i) {
        if ((onlyRow != kNoRow && i != onlyRow) || i == skipRow) continue;
        const Row& row = frame.rows[i];
        const D2D1_RECT_F rect = layout.RowRect(i);
        if (row.groupStart && !selected && (layout.columnRows == 0 || i % layout.columnRows != 0)) {
            // Between two kinds of candidates, in the gap above the row.
            const float y = std::round(rect.top - (layout.rowPitch - layout.rowHeight) / 2.0f) + 0.5f;
            auto line = SolidBrush(target, WithAlpha(p.mutedText, p.mutedText.a * 0.35f));
            target->DrawLine(D2D1::Point2F(rect.left + layout.rowPaddingLeft, y),
                             D2D1::Point2F(rect.right - layout.rowPaddingRight, y), line.get(), 1.0f);
        }

        const float numberLeft = rect.left + layout.rowPaddingLeft;
        const D2D1_RECT_F numberRect =
            D2D1::RectF(numberLeft, rect.top, numberLeft + layout.numberWidth, rect.bottom);
        if (formats.number) {
            const D2D1_RECT_F onBaseline = D2D1::RectF(numberRect.left, numberRect.top + formats.numberShift,
                                                        numberRect.right, numberRect.bottom + formats.numberShift);
            target->DrawText(row.number.c_str(), static_cast<UINT32>(row.number.size()),
                             formats.number.get(), onBaseline, mutedBrush.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        D2D1_RECT_F textRect = D2D1::RectF(numberRect.right + layout.numberGap, rect.top,
                                           rect.right - layout.rowPaddingRight, rect.bottom);
        if (!row.label.empty()) {
            if (auto tag = TagLayout(factory, formats.tag.get(), row.label, layout.tagHeight,
                                     layout.tagTracking)) {
                const float tagWidth = LayoutWidth(tag.get()) + 2.0f * layout.tagPaddingX;
                const float inset = std::round((layout.rowHeight - layout.tagHeight) / 2.0f);
                const float tagTop = rect.top + inset;
                const float tagRight = rect.right - inset;
                const D2D1_RECT_F tagRect = D2D1::RectF(tagRight - tagWidth, tagTop, tagRight, tagTop + layout.tagHeight);
                target->FillRoundedRectangle(Rounded(tagRect, layout.tagHeight / 2.0f), tagBrush.get());
                target->DrawTextLayout(D2D1::Point2F(tagRect.left + layout.tagPaddingX, tagRect.top),
                                       tag.get(), mutedBrush.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                textRect.right = tagRect.left - layout.tagGap;
            }
        }
        if (row.meaning && formats.icon) {
            const D2D1_RECT_F iconRect =
                D2D1::RectF(textRect.right - layout.iconWidth, rect.top, textRect.right, rect.bottom);
            target->DrawText(kDictionaryGlyph, 1, formats.icon.get(), iconRect, mutedBrush.get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
            textRect.right = iconRect.left - layout.iconGap;
        }
        if (textFormat) {
            target->DrawText(row.text.c_str(), static_cast<UINT32>(row.text.size()), textFormat,
                             textRect, textBrush.get(),
                             D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

// The page shown of how many, small and muted under the rows at their end,
// past a gap with a faint line.
inline void DrawPage(ID2D1RenderTarget* target, const TextFormats& formats, const Layout& layout, const Frame& frame,
              const Style& style) {
    if (layout.footerHeight <= 0.0f || frame.pages < 2 || !formats.page) return;
    const std::wstring text = std::to_wstring(frame.page + 1) + L" / " + std::to_wstring(frame.pages);
    auto muted = SolidBrush(target, style.palette.mutedText);
    const float bottom = layout.listHeight - layout.padding;
    const float gap = kFooterGap * layout.scale;
    const D2D1_RECT_F rect = D2D1::RectF(layout.padding + layout.rowPaddingLeft, bottom - layout.footerHeight + gap,
                                         layout.rowRight - layout.rowPaddingRight, bottom);
    // A faint line across, in the gap, sets it apart from the rows.
    auto line = SolidBrush(target, WithAlpha(style.palette.mutedText, style.palette.mutedText.a * 0.25f));
    const float y = std::round(bottom - layout.footerHeight + gap / 2.0f) + 0.5f;
    target->DrawLine(D2D1::Point2F(layout.padding + layout.rowPaddingLeft, y),
                     D2D1::Point2F(layout.rowRight - layout.rowPaddingRight, y), line.get(), std::max(1.0f, std::round(layout.scale)));
    target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), formats.page.get(), rect, muted.get(),
                     D2D1_DRAW_TEXT_OPTIONS_NONE);
}

constexpr int kBadgeAuto = 1;
constexpr int kBadgeDirect = 2;
constexpr int kBadgeJapanese = 3;
constexpr int kBadgeMixed = 4;

// The badge's mode icon (the taskbar's, dark versions on dark), centered.
inline void DrawBadge(ID2D1RenderTarget* target, const Layout& layout, int badge, HINSTANCE instance) {
    const bool dark = IsDarkThemeActive() && !IsHighContrast();
    const int resource = badge == kBadgeMixed      ? IDI_MIXED
                         : badge == kBadgeJapanese ? (dark ? IDI_JAPANESE_DARK : IDI_JAPANESE)
                         : badge == kBadgeDirect ? (dark ? IDI_DIRECT_DARK : IDI_DIRECT)
                                                 : IDI_AUTO;
    const int side = static_cast<int>(std::lround(kBadgeIcon * layout.scale));
    HICON icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON, side, side, LR_DEFAULTCOLOR));
    if (!icon) return;
    ICONINFO info{};
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(side) * side, 0);
    bool read = false;
    if (GetIconInfo(icon, &info)) {
        if (info.hbmColor) {
            BITMAPINFO bitmap{};
            bitmap.bmiHeader = {sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB};
            HDC screen = GetDC(nullptr);
            read = GetDIBits(screen, info.hbmColor, 0, static_cast<UINT>(side), pixels.data(), &bitmap,
                             DIB_RGB_COLORS) == side;
            ReleaseDC(nullptr, screen);
            DeleteObject(info.hbmColor);
        }
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
    DestroyIcon(icon);
    if (!read) return;
    // Straight alpha to premultiplied.
    for (auto& pixel : pixels) {
        const std::uint32_t alpha = pixel >> 24;
        const auto channel = [&](int shift) { return ((pixel >> shift) & 0xFF) * alpha / 255; };
        pixel = (alpha << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
    }
    winrt::com_ptr<ID2D1Bitmap> image;
    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(target->CreateBitmap(D2D1::SizeU(side, side), pixels.data(), static_cast<UINT32>(side * 4),
                                    &properties, image.put()))) {
        return;
    }
    const float x = (static_cast<float>(layout.width) - side) / 2.0f;
    const float y = (static_cast<float>(layout.height) - side) / 2.0f;
    target->DrawBitmap(image.get(), D2D1::RectF(x, y, x + side, y + side));
}

// The keys typed, small and muted, on their own line above the rows.
inline void DrawCaption(ID2D1RenderTarget* target, const TextFormats& formats, const Layout& layout, const Frame& frame,
                 const Style& style) {
    if (frame.caption.empty() || layout.captionHeight <= 0.0f || !formats.caption) return;
    auto muted = SolidBrush(target, style.palette.mutedText);
    const D2D1_RECT_F rect = D2D1::RectF(layout.padding + layout.rowPaddingLeft, layout.padding,
                                         layout.rowRight - layout.rowPaddingRight, layout.padding + layout.captionHeight);
    target->DrawText(frame.caption.c_str(), static_cast<UINT32>(frame.caption.size()), formats.caption.get(), rect,
                     muted.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// The meaning, in its own window.
inline void DrawNote(ID2D1RenderTarget* target, const Layout& layout, const Style& style) {
    if (!layout.detailText) return;
    auto text = SolidBrush(target, style.palette.text);
    target->DrawTextLayout(D2D1::Point2F(layout.detail.left, layout.detail.top), layout.detailText.get(),
                           text.get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Signed distance from (x, y) to a rounded rectangle; negative inside.
inline float RoundedRectDistance(float x, float y, float centerX, float centerY, float halfWidth,
                          float halfHeight, float radius) {
    const float qx = std::fabs(x - centerX) - (halfWidth - radius);
    const float qy = std::fabs(y - centerY) - (halfHeight - radius);
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

struct ShadowLayer {
    float sigma;
    float offset;
    float alpha;
};

// Rasterizes the panel's drop shadow as premultiplied black, with the panel
// itself cut out (antialiased) so the shadow window can sit above the glass
// without darkening it. A Gaussian-blurred edge is approximated by erfc of
// the signed distance, which is exact for straight edges and close enough
// around corners whose radius exceeds the blur.
inline void RasterizeShadow(std::uint32_t* pixels, int width, int height, float panelX, float panelY,
                     float panelWidth, float panelHeight, float radius, ShadowLayer key,
                     ShadowLayer ambient) {
    const float halfWidth = panelWidth / 2.0f;
    const float halfHeight = panelHeight / 2.0f;
    const float centerX = panelX + halfWidth;
    const float centerY = panelY + halfHeight;
    constexpr float kInvSqrt2 = 0.70710678f;
    const auto blurred = [&](float x, float y, const ShadowLayer& layer) {
        if (layer.alpha <= 0.0f || layer.sigma <= 0.0f) return 0.0f;
        const float distance =
            RoundedRectDistance(x, y - layer.offset, centerX, centerY, halfWidth, halfHeight, radius);
        return layer.alpha * 0.5f * std::erfc(distance * kInvSqrt2 / layer.sigma);
    };
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const float x = static_cast<float>(column) + 0.5f;
            const float y = static_cast<float>(row) + 0.5f;
            const float inside =
                std::clamp(0.5f - RoundedRectDistance(x, y, centerX, centerY, halfWidth, halfHeight, radius),
                           0.0f, 1.0f);
            float alpha = 0.0f;
            if (inside < 1.0f) {
                const float a1 = blurred(x, y, key);
                const float a2 = blurred(x, y, ambient);
                alpha = (1.0f - (1.0f - a1) * (1.0f - a2)) * (1.0f - inside);
            }
            pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(column)] =
                static_cast<std::uint32_t>(std::lround(alpha * 255.0f)) << 24;
        }
    }
}

// ---------------------------------------------------------------------------
// UI thread side.

struct Surface {
    wuc::CompositionDrawingSurface surface{nullptr};
    SIZE size{0, 0};
};

inline bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == D2DERR_RECREATE_TARGET;
}

// CLSID_D2D1Saturation, spelled out so the DLL needs no dxguid.lib.
constexpr GUID kSaturationEffectId{0x5cb2d9cf, 0x327d, 0x459f, {0xa0, 0xce, 0x40, 0xc0, 0xb2, 0x08, 0x6b, 0xf7}};

// Windows.Graphics.Effects description of the Direct2D saturation effect,
// the minimum the compositor needs to build an effect brush (what Win2D
// would otherwise provide).
struct SaturationEffect
    : winrt::implements<SaturationEffect, winrt::Windows::Graphics::Effects::IGraphicsEffect,
                        winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
                        ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop> {
    SaturationEffect(winrt::Windows::Graphics::Effects::IGraphicsEffectSource source, float saturation)
        : source_(std::move(source)), saturation_(saturation) {}

    winrt::hstring Name() const { return name_; }
    void Name(const winrt::hstring& name) { name_ = name; }

    HRESULT __stdcall GetEffectId(GUID* id) noexcept override {
        *id = kSaturationEffectId;
        return S_OK;
    }

    HRESULT __stdcall GetNamedPropertyMapping(
        LPCWSTR name, UINT* index,
        ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!name || wcscmp(name, L"Saturation") != 0) return E_INVALIDARG;
        *index = D2D1_SATURATION_PROP_SATURATION;
        *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
        return S_OK;
    }

    HRESULT __stdcall GetPropertyCount(UINT* count) noexcept override {
        *count = 1;
        return S_OK;
    }

    HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (index != D2D1_SATURATION_PROP_SATURATION) return E_INVALIDARG;
        try {
            *value = winrt::Windows::Foundation::PropertyValue::CreateSingle(saturation_)
                         .as<ABI::Windows::Foundation::IPropertyValue>()
                         .detach();
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT __stdcall GetSource(UINT index,
                                ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) noexcept override {
        if (index != 0) return E_INVALIDARG;
        try {
            *source = source_.as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>().detach();
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT __stdcall GetSourceCount(UINT* count) noexcept override {
        *count = 1;
        return S_OK;
    }

private:
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource source_;
    float saturation_;
    winrt::hstring name_;
};

}  // namespace tekito::tsf::candidate_look
