// Standalone CandidateWindow renderer for visual review without registering
// the TSF. Shows sample candidates over a backdrop window and saves a
// screenshot of the composited screen, so the capture includes the backdrop
// blur, the shadow window and the rounded corners exactly as a user would
// see them.
//
//   tekito_candidate_window_preview.exe <out.bmp> [stripes|doc|paper|dusk] [short|long] [glide] [click]
//                                       [meaning|kanji|kanji-list|chunks|page]
//                                       [badge-auto|badge-direct|badge-japanese]
//
// `stripes` puts saturated color bands behind the glass to show the blur;
// `doc` is a plain page of text, the common case; `paper` is a soft light
// background for product shots; `dusk` is a dark gradient
// wallpaper. `long` shows more
// candidates than fit on a page, which brings up the page indicator.
// `glide` moves the selection two rows down and captures mid-animation.
// `click` clicks row 4 and reports the index the host callback received.
// `meaning` shows the meaning pane; `kanji` shows Japanese candidates with
// it, `kanji-list` without it.
// `badge-*` shows the mode badge instead of candidates, then checks that it
// fades out by itself (exit code 4 if it stays).
//
// TEKITO_PREVIEW_STYLE=simple renders the simple candidate window style;
// TEKITO_PREVIEW_LANGUAGE=ja shows the Japanese tags, and the English
// word's meaning in Japanese.
// Environment overrides (preview builds only): TEKITO_PREVIEW_THEME=dark|light,
// TEKITO_PREVIEW_NO_COMPOSITION=1 (HWND fallback renderer),
// TEKITO_PREVIEW_HIGH_CONTRAST=1 (high-contrast palette from system colors).
#include "Tsf/CandidateWindow.h"

#include <windows.h>

#include <fstream>
#include <string>
#include <dwmapi.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr int kCaptureMargin = 40;
bool g_documentBackdrop = false;
bool g_duskBackdrop = false;
bool g_paperBackdrop = false;

// Dark slate-to-teal gradient, a calm dark wallpaper.
void PaintDusk(HDC dc, const RECT& client) {
    const int height = client.bottom - client.top;
    for (int y = 0; y < height; ++y) {
        const double f = static_cast<double>(y) / static_cast<double>(height > 1 ? height - 1 : 1);
        const auto mix = [f](int a, int b) { return static_cast<BYTE>(a + (b - a) * f); };
        HBRUSH brush = CreateSolidBrush(RGB(mix(22, 92), mix(34, 118), mix(42, 126)));
        RECT line{client.left, y, client.right, y + 1};
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }
}

// Soft light gray-blue, top to bottom.
void PaintPaper(HDC dc, const RECT& client) {
    const int height = client.bottom - client.top;
    for (int y = 0; y < height; ++y) {
        const double f = static_cast<double>(y) / static_cast<double>(height > 1 ? height - 1 : 1);
        const auto mix = [f](int a, int b) { return static_cast<BYTE>(a + (b - a) * f); };
        HBRUSH brush = CreateSolidBrush(RGB(mix(244, 226), mix(246, 232), mix(250, 241)));
        RECT line{client.left, y, client.right, y + 1};
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }
}

void PaintStripes(HDC dc, const RECT& client) {
    const COLORREF stripes[] = {RGB(255, 94, 98), RGB(255, 170, 64), RGB(76, 201, 140),
                                RGB(64, 156, 255), RGB(160, 100, 255)};
    const int stripeWidth = (client.right + 4) / 5;
    for (int i = 0; i < 5; ++i) {
        RECT stripe{i * stripeWidth, 0, (i + 1) * stripeWidth, client.bottom};
        HBRUSH brush = CreateSolidBrush(stripes[i]);
        FillRect(dc, &stripe, brush);
        DeleteObject(brush);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 20, 30));
    HFONT font = CreateFontW(-56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT textRect = client;
    DrawTextW(dc, L"TEKITO glass", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

void PaintDocument(HDC dc, const RECT& client) {
    HBRUSH page = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &client, page);
    DeleteObject(page);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(30, 30, 30));
    HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    const wchar_t* lines[] = {
        L"Dear team,",
        L"",
        L"Hello wrold",
        L"",
        L"Thanks for the quick turnaround on the release notes. I went",
        L"through the draft this morning and only have a couple of small",
        L"comments. The section about the new keyboard shortcuts reads",
        L"well, but the example under \"Getting started\" still mentions",
        L"the old settings page. Could we also add a short note about how",
        L"corrections work when you keep typing? People asked about it",
        L"twice in the beta forum. Otherwise this looks ready to ship.",
        L"",
        L"Best,",
        L"Sam",
    };
    int y = 24;
    for (const auto* line : lines) {
        TextOutW(dc, 28, y, line, static_cast<int>(wcslen(line)));
        y += 28;
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        if (g_duskBackdrop) {
            PaintDusk(dc, client);
        } else if (g_paperBackdrop) {
            PaintPaper(dc, client);
        } else if (g_documentBackdrop) {
            PaintDocument(dc, client);
        } else {
            PaintStripes(dc, client);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateBackdropWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = &BackdropProc;
    wc.lpszClassName = L"TekitoPreviewBackdrop";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                L"TekitoPreviewBackdrop", L"", WS_POPUP, 160, 200, 1000, 720,
                                nullptr, nullptr, instance, nullptr);
    if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

bool SaveScreenRectToBmp(RECT rect, const wchar_t* path) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);
    BitBlt(memDc, 0, 0, width, height, screenDc, rect.left, rect.top, SRCCOPY | CAPTUREBLT);

    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    BITMAPINFO bmi{};
    bmi.bmiHeader = infoHeader;
    GetDIBits(memDc, bitmap, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());

    FILE* file = nullptr;
    _wfopen_s(&file, path, L"wb");
    bool ok = false;
    if (file) {
        fwrite(&fileHeader, sizeof(fileHeader), 1, file);
        fwrite(&infoHeader, sizeof(infoHeader), 1, file);
        fwrite(pixels.data(), pixels.size(), 1, file);
        fclose(file);
        ok = true;
    }

    SelectObject(memDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return ok;
}

tekito::Candidate MakeCandidate(const wchar_t* text, tekito::SemanticLabel label, std::uint32_t id,
                                bool original = false) {
    tekito::Candidate candidate;
    candidate.text = text;
    candidate.label = label;
    candidate.isOriginal = original;
    candidate.id = id;
    return candidate;
}

std::vector<tekito::Candidate> SampleCandidates(bool longList) {
    using tekito::SemanticLabel;
    std::vector<tekito::Candidate> candidates{
        MakeCandidate(L"wrold", SemanticLabel::None, 1, true),
        MakeCandidate(L"world", SemanticLabel::None, 2),
        MakeCandidate(L"would", SemanticLabel::None, 3),
        MakeCandidate(L"\U0001F30D", SemanticLabel::Emoji, 4),
        MakeCandidate(L"worldwide", SemanticLabel::None, 5),
    };
    if (longList) {
        const wchar_t* more[] = {L"worldly", L"word", L"worlds", L"wold", L"wield", L"weld", L"wroth"};
        std::uint32_t id = 6;
        for (const auto* text : more) {
            candidates.push_back(MakeCandidate(text, SemanticLabel::None, id++));
        }
    }
    return candidates;
}

std::vector<tekito::Candidate> SampleKanjiCandidates() {
    using tekito::SemanticLabel;
    return {
        MakeCandidate(L"\u6a5f\u68b0", SemanticLabel::None, 1),       // kikai: machine
        MakeCandidate(L"\u6a5f\u4f1a", SemanticLabel::None, 2),       // opportunity
        MakeCandidate(L"\u5668\u68b0", SemanticLabel::None, 3),       // instrument
        MakeCandidate(L"\u304d\u304b\u3044", SemanticLabel::None, 4),  // hiragana
        MakeCandidate(L"\u30ad\u30ab\u30a4", SemanticLabel::None, 5),  // katakana
    };
}

// A list from a UTF-8 file (`list <file>`): a line for each candidate,
// its text, then after tabs a tag (suggestion, emoji, original) and m when
// it has a meaning; "#selected N", "#page N" (the first row shown), "#rows N"
// and "#detail headword|sense|sense" set the rest.
struct FileList {
    std::vector<tekito::Candidate> candidates;
    std::size_t selected{0}, pageStart{0}, rows{12};
    tekito::tsf::CandidateDetail detail;
};

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

FileList ReadList(const wchar_t* path) {
    FileList list;
    std::ifstream in(path, std::ios::binary);
    std::uint32_t id = 1;
    for (std::string raw; std::getline(in, raw);) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::wstring line = FromUtf8(raw);
        if (line.empty()) continue;
        if (line.rfind(L"#selected ", 0) == 0) list.selected = std::stoul(line.substr(10));
        else if (line.rfind(L"#page ", 0) == 0) list.pageStart = std::stoul(line.substr(6));
        else if (line.rfind(L"#rows ", 0) == 0) list.rows = std::stoul(line.substr(6));
        else if (line.rfind(L"#detail ", 0) == 0) {
            std::wstring rest = line.substr(8);
            std::vector<std::wstring> parts;
            for (std::size_t at = 0;;) {
                const std::size_t bar = rest.find(L'|', at);
                parts.push_back(rest.substr(at, bar == std::wstring::npos ? std::wstring::npos : bar - at));
                if (bar == std::wstring::npos) break;
                at = bar + 1;
            }
            list.detail.headword = parts.front();
            list.detail.senses.assign(parts.begin() + 1, parts.end());
        } else {
            std::vector<std::wstring> fields;
            for (std::size_t at = 0;;) {
                const std::size_t tab = line.find(L'\t', at);
                fields.push_back(line.substr(at, tab == std::wstring::npos ? std::wstring::npos : tab - at));
                if (tab == std::wstring::npos) break;
                at = tab + 1;
            }
            const std::wstring tag = fields.size() > 1 ? fields[1] : L"";
            const auto label = tag == L"suggestion" ? tekito::SemanticLabel::Suggestion
                               : tag == L"emoji"    ? tekito::SemanticLabel::Emoji
                                                    : tekito::SemanticLabel::None;
            auto candidate = MakeCandidate(fields[0].c_str(), label, 0, tag == L"original");
            candidate.id = id;
            id = id % static_cast<std::uint32_t>(std::max<std::size_t>(list.rows, 1)) + 1;
            candidate.hasMeaning = fields.size() > 2 && fields[2] == L"m";
            list.candidates.push_back(std::move(candidate));
        }
    }
    return list;
}

// A Japanese list longer than a page (こうえん), shown a page of twelve.
std::vector<tekito::Candidate> SamplePageCandidates() {
    using tekito::SemanticLabel;
    std::vector<tekito::Candidate> candidates;
    std::uint32_t id = 1;
    for (const wchar_t* text : {L"公園", L"公演", L"講演", L"🏞️", L"後援", L"好演", L"コーエン", L"口演", L"公苑",
                                L"口縁", L"後縁", L"香園", L"⛲️", L"高遠"}) {
        candidates.push_back(MakeCandidate(text, SemanticLabel::None, id++));
    }
    return candidates;
}

// A phrase's list with its chunk candidates first (にじがでる: 虹が, then
// 虹がでる and 二次が出る over the phrases after it), a line before its own.
std::vector<tekito::Candidate> SampleChunkCandidates() {
    using tekito::SemanticLabel;
    std::vector<tekito::Candidate> candidates{
        MakeCandidate(L"虹が", SemanticLabel::None, 1),
        MakeCandidate(L"虹がでる", SemanticLabel::None, 2),
        MakeCandidate(L"二次が出る", SemanticLabel::None, 3),
        MakeCandidate(L"二次が", SemanticLabel::None, 4),
        MakeCandidate(L"2時が", SemanticLabel::None, 5),
    };
    candidates[3].groupStart = true;
    return candidates;
}

// The meaning pane, as the TSF fills it from the meaning packs.
tekito::tsf::CandidateDetail SampleDetail(bool kanji, bool japanese) {
    if (kanji) {
        return {L"\u6a5f\u4f1a",
                {L"\u7269\u4e8b\u3092\u884c\u3046\u306e\u306b\u3001\u6700\u3082\u9069\u3057\u305f"
                 L"\u6642\u671f\u3002\u3061\u3087\u3046\u3069\u3088\u3044\u6642\u3002\u304a\u308a\u3002",
                 L"\u3061\u3087\u3046\u3069\u3088\u3044\u30c1\u30e3\u30f3\u30b9\u3002"}};
    }
    // With the Japanese tags, as Settings in Japanese shows it: the meaning
    // in Japanese (english-meanings-ja's row for world).
    if (japanese) {
        return {L"world",
                {L"存在、宇宙、六合：存在するすべてのもの",
                 L"界、世界、社会：一般の人々",
                 L"グローバル、世界的、ワールドワイド：地球全体を含むさま",
                 L"世界、現実：自分にとって物事がどう見えるかを決定する自分のすべての経験"}};
    }
    return {L"world",
            {L"everything that exists anywhere",
             L"people in general; especially a distinctive group of people with some shared interest",
             L"all of your experiences that determine how things appear to you"}};
}

}  // namespace

// This process's candidate window: TEKITO installed on the PC has hidden
// ones of the same class in other applications.
HWND OwnCandidateWindow() {
    struct Search {
        DWORD process;
        HWND found;
    } search{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(lParam);
            wchar_t name[64]{};
            DWORD process = 0;
            GetWindowThreadProcessId(hwnd, &process);
            if (process != s.process || !GetClassNameW(hwnd, name, 64) || wcscmp(name, L"TekitoCandidateWindow") != 0) {
                return TRUE;
            }
            s.found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* outputPath = argc > 1 ? argv[1] : L"candidate_window_preview.bmp";
    bool longList = false;
    bool glide = false;
    bool click = false;
    bool meaning = false;
    bool form = false;
    bool kanji = false;
    bool page = false;
    const wchar_t* listFile = nullptr;
    bool chunks = false;
    int badge = 0;
    // The keys typed shown above the list, or alone.
    const wchar_t* caption = L"";
    bool captionOnly = false;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"caption") == 0) caption = L"kyouhaamedesu";
        if (wcscmp(argv[i], L"caption-only") == 0) caption = L"kyouhaame", captionOnly = true;
        if (wcscmp(argv[i], L"badge-auto") == 0) badge = 1;
        if (wcscmp(argv[i], L"badge-direct") == 0) badge = 2;
        if (wcscmp(argv[i], L"badge-japanese") == 0) badge = 3;
        if (wcscmp(argv[i], L"meaning") == 0) meaning = true;
        if (wcscmp(argv[i], L"form") == 0) meaning = form = true;
        if (wcscmp(argv[i], L"kanji") == 0) kanji = meaning = true;
        if (wcscmp(argv[i], L"kanji-list") == 0) kanji = true;
        // A page of twelve Japanese candidates, as Japanese mode shows.
        if (wcscmp(argv[i], L"page") == 0) page = true;
        if (wcscmp(argv[i], L"list") == 0 && i + 1 < argc) listFile = argv[++i];
        if (wcscmp(argv[i], L"chunks") == 0) chunks = true;
        if (wcscmp(argv[i], L"doc") == 0) g_documentBackdrop = true;
        if (wcscmp(argv[i], L"dusk") == 0) g_duskBackdrop = true;
        if (wcscmp(argv[i], L"paper") == 0) g_paperBackdrop = true;
        if (wcscmp(argv[i], L"long") == 0) longList = true;
        if (wcscmp(argv[i], L"glide") == 0) glide = true;
        if (wcscmp(argv[i], L"click") == 0) click = true;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND backdrop = CreateBackdropWindow(instance);
    // The backdrop painted and on screen before the list shows, so that its
    // glass takes it in rather than what is behind it.
    if (backdrop) {
        UpdateWindow(backdrop);
        for (const auto start = GetTickCount64(); GetTickCount64() - start < 200;) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(1);
        }
        DwmFlush();
    }

    tekito::tsf::CandidateWindow window;
    const DWORD mainThread = GetCurrentThreadId();
    if (!window.Initialize(instance, [mainThread](std::size_t index) {
            wprintf(L"Selected index %zu on %ls thread\n", index,
                    GetCurrentThreadId() == mainThread ? L"the host" : L"another");
        })) {
        fwprintf(stderr, L"Initialize failed\n");
        return 1;
    }

    wchar_t style[16]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_STYLE", style, 16) > 0 && wcscmp(style, L"simple") == 0) {
        window.SetStyle(1);
    }
    // TEKITO_PREVIEW_ZOOM=145: the list's size in percent.
    wchar_t zoom[8]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_ZOOM", zoom, 8) > 0) window.SetZoom(_wtoi(zoom));
    wchar_t language[8]{};
    const bool japaneseUi = GetEnvironmentVariableW(L"TEKITO_PREVIEW_LANGUAGE", language, 8) > 0 && wcscmp(language, L"ja") == 0;
    if (japaneseUi) window.SetJapanese(true);
    auto candidates = page ? SamplePageCandidates() : chunks ? SampleChunkCandidates() : kanji ? SampleKanjiCandidates() : SampleCandidates(longList);
    if (meaning) {
        // Rows with a meaning show the dictionary sign.
        for (std::size_t i = 0; i < candidates.size(); ++i) candidates[i].hasMeaning = i != 0 && i != 3;
    }
    auto detail = meaning ? SampleDetail(kanji, japaneseUi) : tekito::tsf::CandidateDetail{};
    if (form) {
        // Another form of a word: went, go's past tense, with go's meaning.
        detail = {L"went",
                  {L"参る、行く、動く：場所を変える",
                   L"行く、去る：去っていく"},
                  L"go の過去形"};
    }
    // Caret under "wrold" on the document's third line.
    RECT caret{232, 280, 234, 302};
    LARGE_INTEGER frequency{};
    LARGE_INTEGER shown{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&shown);
    if (badge) {
        window.ShowModeBadge(caret, badge == 3 ? tekito::InputMode::Japanese
                                    : badge == 2 ? tekito::InputMode::Direct
                                                 : tekito::InputMode::Convert);
    } else if (captionOnly) {
        window.Show(caret, {}, tekito::tsf::CandidateWindow::kNoSelection, 0, 0, {}, caption);
    } else if (listFile) {
        auto list = ReadList(listFile);
        // The ids number each page from 1.
        const std::size_t rows = std::min(list.rows, list.candidates.size() - list.pageStart);
        window.Show(caret, list.candidates, list.selected, list.pageStart, rows, list.detail, caption);
    } else if (page) {
        window.Show(caret, candidates, 0, 0, 12, detail, caption);
    } else {
        window.Show(caret, candidates, 1, 0, 5, detail, caption);
    }
    // Time until the UI thread has built its windows and presented the first
    // frame (composition init, device creation and effect compilation).
    for (int spin = 0; spin < 5000; ++spin) {
        HWND panel = OwnCandidateWindow();
        if (panel && IsWindowVisible(panel)) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            wprintf(L"First frame after %.1f ms\n",
                    1000.0 * static_cast<double>(now.QuadPart - shown.QuadPart) /
                        static_cast<double>(frequency.QuadPart));
            break;
        }
        Sleep(1);
    }
    const auto pump = [](ULONGLONG milliseconds) {
        MSG msg;
        const auto start = GetTickCount64();
        while (GetTickCount64() - start < milliseconds) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(1);
        }
    };
    pump(badge ? 300 : 1500);

    HWND target = OwnCandidateWindow();
    if (!target) {
        fwprintf(stderr, L"Candidate window not found\n");
        return 1;
    }
    if (click) {
        // Row 4 (index 3): padding 6 + 3 rows of (36 + 2) + half a row, in DIPs.
        const UINT dpi = GetDpiForWindow(target);
        const int y = MulDiv(6 + 3 * 38 + 18, static_cast<int>(dpi), 96);
        const int x = MulDiv(60, static_cast<int>(dpi), 96);
        PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        pump(300);
    }
    if (glide) {
        window.Show(caret, candidates, 3, 0, 5);
        pump(35);
    }
    RECT rect{};
    GetWindowRect(target, &rect);
    InflateRect(&rect, kCaptureMargin, kCaptureMargin);
    if (!SaveScreenRectToBmp(rect, outputPath)) {
        fwprintf(stderr, L"Screenshot failed\n");
        return 1;
    }
    wprintf(L"Saved %ls\n", outputPath);
    if (badge) {
        pump(1500);
        if (IsWindowVisible(target)) {
            fwprintf(stderr, L"The badge did not fade\n");
            return 4;
        }
    }
    window.Hide();
    if (backdrop) DestroyWindow(backdrop);
    return 0;
}
