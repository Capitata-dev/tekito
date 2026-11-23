#pragma once

// What every file of the Settings app shares: Windows and WebView2, the
// window's messages and names, the theme, and JSON text.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Core/DataPackPath.h"
#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/AiJudge.h"
#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/RomajiTable.h"
#include "Dictionary/ExternalDictionaryProvider.h"
#include "apps/tekito-settings/Companion.h"
#include "UserData/AiModelInstall.h"
#include "UserData/DataPackValidation.h"
#include "UserData/KeyboardLayout.h"
#include "UserData/ImeDictionaryImport.h"
#include "UserData/PostalCodeImport.h"
#include "UserData/RuntimeModeState.h"
#include "UserData/UiLanguage.h"
#include "UserData/UserDataEncryption.h"
#include "UserData/UserDataRepository.h"
#include "UserData/UserDictionaryFile.h"
#include "assets/tekito_resource.h"

#include <windows.h>
#include <commdlg.h>
#include <initguid.h>  // DXCore's GUIDs, defined here
#include <dxcore.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <urlmon.h>
#include <wrl.h>
#include "WebView2.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace tekito::settings {

constexpr wchar_t kClassName[] = L"TekitoSettingsWindow";

// From project(VERSION) in CMakeLists.txt.
#define TEKITO_WIDEN_(text) L##text
#define TEKITO_WIDEN(text) TEKITO_WIDEN_(text)
constexpr wchar_t kVersion[] = TEKITO_WIDEN(TEKITO_VERSION_STRING);
constexpr int kRetryWebView = 200;
// How often the host checks the shared runtime state for changes made
// outside this window (mode switched from the Language Bar, the IME saving
// learning data, ...). Reading four counters is essentially free; the page
// is only sent a new state when one of them moved.
constexpr UINT_PTR kStateWatchTimer = 1;
constexpr UINT kStateWatchIntervalMs = 500;
// A postal code download finished (lParam: PostalDownload*).
constexpr UINT kPostalDownloaded = WM_APP + 1;

// The copy on TEKITO's release comes with its SHA-256 beside it (the
// postal-codes workflow writes utf_ken_all.zip.sha256): a copy that does not
// match it is not taken. Japan Post's own has none; a release from before the
// checksum was published neither, and is taken as it is.
inline bool PostalZipChecked(const wchar_t* url, const std::filesystem::path& zip) {
    if (!std::wstring_view(url).starts_with(L"https://github.com/")) return true;
    const auto sumFile = std::filesystem::path(zip.wstring() + L".sha256");
    const std::wstring sumUrl = std::wstring(url) + L".sha256";
    if (FAILED(URLDownloadToFileW(nullptr, sumUrl.c_str(), sumFile.c_str(), 0, nullptr))) return true;
    std::string expected;
    {
        std::ifstream in(sumFile);
        in >> expected;
    }
    std::error_code ignored;
    std::filesystem::remove(sumFile, ignored);
    for (auto& c : expected) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return !expected.empty() && tekito::userdata::FileSha256(zip) == expected;
}

struct PostalDownload {
    std::wstring requestId;
    std::filesystem::path file;
    bool ok{false};
};
// An AI model download and install finished (lParam: AiModelDownload*).
constexpr UINT kAiModelInstalled = WM_APP + 2;

struct AiModelDownload {
    int model{0};
    tekito::userdata::AiModelInstallError error{tekito::userdata::AiModelInstallError::None};
};
// Making the NPU ready finished (wParam: 1 when it is).
constexpr UINT kAiNpuPrepared = WM_APP + 3;
// Windows ML's runtime installed, or not (wParam: AiModelInstallError,
// lParam: the device it was for).
constexpr UINT kAiWindowsMlInstalled = WM_APP + 4;
// The Japanese data downloaded and installed (wParam: AiModelInstallError).
constexpr UINT kJapaneseDataInstalled = WM_APP + 5;
// TEKITO's two keyboards in Windows' language list, as setup adds them
// (installer/install.ps1): English, and Japanese, which also types English.
constexpr wchar_t kEnglishTip[] = L"0409:{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}{8D99240A-5C9C-4ED8-8DE4-85F21DF23763}";
constexpr wchar_t kJapaneseTip[] = L"0411:{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}{2876747C-FA52-4CCC-B308-F406190F8CDE}";

// Windows accent color as "#rrggbb" (the DWM value is stored as 0xAABBGGRR).
inline std::wstring AccentColorHex() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return L"#0078d4";
    }
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"#%02x%02x%02x", value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff);
    return buffer;
}

inline bool AppsUseDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                        &size) == ERROR_SUCCESS &&
           value == 0;
}

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

inline std::wstring JsonEscape(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 8);
    for (const wchar_t character : value) {
        switch (character) {
        case L'"': result += L"\\\""; break;
        case L'\\': result += L"\\\\"; break;
        case L'\r': result += L"\\r"; break;
        case L'\n': result += L"\\n"; break;
        case L'\t': result += L"\\t"; break;
        default:
            if (character < 0x20) {
                constexpr wchar_t digits[] = L"0123456789abcdef";
                result += L"\\u000";
                result += digits[character & 0x0f];
            } else {
                result += character;
            }
            break;
        }
    }
    return result;
}

}  // namespace tekito::settings
