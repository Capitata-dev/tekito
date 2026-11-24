#pragma once

// Windows itself: TEKITO's keyboards in the language list, the default
// keyboard, Ctrl+Shift, the registry, the clipboard and folders.

#include "apps/tekito-settings/SettingsShared.h"

namespace tekito::settings {

inline std::wstring RegisteredTsfDll() {
    constexpr wchar_t keyPath[] =
        L"CLSID\\{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}\\InprocServer32";
    wchar_t value[1024]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CLASSES_ROOT, keyPath, nullptr, RRF_RT_REG_SZ, nullptr,
                        value, &size) == ERROR_SUCCESS
               ? std::wstring(value)
               : std::wstring{};
}

inline std::filesystem::path LogFolder() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, path);
    return length == 0 || length >= MAX_PATH ? std::filesystem::path{}
                                             : std::filesystem::path(path);
}

inline std::wstring PolicyName(std::uint32_t flags) {
    if (flags & tekito::CandidatePolicyProtect) return L"Protect original";
    if (flags & tekito::CandidatePolicyCorrect) return L"Correct spelling";
    if (flags & tekito::CandidatePolicyNormalize) return L"Normalize";
    if (flags & tekito::CandidatePolicyExpand) return L"Expand abbreviation";
    if (flags & tekito::CandidatePolicyContext) return L"Context only";
    return L"Suggest alternative";
}

inline bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool copied = false;
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            CopyMemory(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            if (!copied) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return copied;
}

inline std::filesystem::path ExecutableDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return length == 0 || length == MAX_PATH
               ? std::filesystem::path{}
               : std::filesystem::path(path, path + length).parent_path();
}

// Adds a keyboard to the user's language list, or (`remove`) takes it out,
// as Windows' language settings do (input.dll's InstallLayoutOrTip).
inline bool ChangeKeyboard(const wchar_t* tip, bool remove) {
    using InstallLayoutOrTipFunction = BOOL(WINAPI*)(LPCWSTR, DWORD);
    constexpr DWORD kUninstall = 0x00000001;
    const HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!input) return false;
    const auto install = reinterpret_cast<InstallLayoutOrTipFunction>(GetProcAddress(input, "InstallLayoutOrTip"));
    const bool changed = install && install(tip, remove ? kUninstall : 0) != FALSE;
    FreeLibrary(input);
    return changed;
}

inline std::filesystem::path WebViewUserDataDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(localAppData) / L"TEKITO" / L"settings-webview";
}

// Windows' own input settings Settings can change for the user, as Windows'
// language settings do, with what they were before kept to put back.
constexpr wchar_t kTekitoWindowsKey[] = L"Software\\TEKITO\\Windows";
constexpr wchar_t kTekitoClsid[] = L"{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}";

inline std::optional<std::wstring> ReadUserString(const wchar_t* key, const wchar_t* name) {
    wchar_t value[512]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return std::wstring(value);
}

inline bool WriteUserString(const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

inline void DeleteUserValue(const wchar_t* key, const wchar_t* name) { RegDeleteKeyValueW(HKEY_CURRENT_USER, key, name); }

// input.dll's view of a keyboard in the user's list (LAYOUTORTIPPROFILE).
struct LayoutOrTip {
    DWORD profileType;
    LANGID language;
    CLSID clsid;
    GUID profile;
    GUID category;
    DWORD substituteLayout;
    DWORD flags;
    WCHAR id[MAX_PATH];
};
constexpr DWORD kLayoutOrTipDefault = 0x00000001;

// The keyboards in the user's list (the default one flagged).
inline std::vector<LayoutOrTip> EnabledKeyboards() {
    using EnumFunction = UINT(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, LayoutOrTip*, UINT);
    std::vector<LayoutOrTip> keyboards;
    const HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!input) return keyboards;
    if (const auto enumerate = reinterpret_cast<EnumFunction>(GetProcAddress(input, "EnumEnabledLayoutOrTip"))) {
        const UINT count = enumerate(nullptr, nullptr, nullptr, nullptr, 0);
        keyboards.resize(count);
        if (count > 0) keyboards.resize(enumerate(nullptr, nullptr, nullptr, keyboards.data(), count));
    }
    FreeLibrary(input);
    return keyboards;
}

inline std::wstring DefaultKeyboard() {
    for (const auto& keyboard : EnabledKeyboards()) {
        if (keyboard.flags & kLayoutOrTipDefault) return keyboard.id;
    }
    return {};
}

inline bool IsTekito(const std::wstring& keyboard) {
    std::wstring upper = keyboard;
    for (auto& c : upper) c = static_cast<wchar_t>(std::towupper(c));
    return upper.find(kTekitoClsid) != std::wstring::npos;
}

inline bool SetDefaultKeyboard(const std::wstring& keyboard) {
    using SetDefaultFunction = BOOL(WINAPI*)(LPCWSTR, DWORD);
    const HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!input) return false;
    const auto setDefault = reinterpret_cast<SetDefaultFunction>(GetProcAddress(input, "SetDefaultLayoutOrTip"));
    const bool set = setDefault && setDefault(keyboard.c_str(), 0) != FALSE;
    FreeLibrary(input);
    return set;
}

// TEKITO as Windows' default input method: the Japanese one when it is in
// the list, else the English one. The default before is kept.
inline bool MakeTekitoDefault() {
    const auto before = DefaultKeyboard();
    std::wstring tekito = kEnglishTip;
    for (const auto& keyboard : EnabledKeyboards()) {
        if (_wcsicmp(keyboard.id, kJapaneseTip) == 0) tekito = kJapaneseTip;
    }
    if (!before.empty() && !IsTekito(before)) WriteUserString(kTekitoWindowsKey, L"PreviousDefault", before);
    return SetDefaultKeyboard(tekito);
}

inline bool PutBackDefault() {
    const auto before = ReadUserString(kTekitoWindowsKey, L"PreviousDefault");
    if (!before || !SetDefaultKeyboard(*before)) return false;
    DeleteUserValue(kTekitoWindowsKey, L"PreviousDefault");
    return true;
}

// Ctrl+Shift switching keyboards (Windows' "Switch keyboard layout" hotkey,
// and the language one when it is Ctrl+Shift): "2" is Ctrl+Shift, "3" none.
constexpr wchar_t kToggleKey[] = L"Keyboard Layout\\Toggle";

inline bool CtrlShiftSwitches() {
    const auto layout = ReadUserString(kToggleKey, L"Layout Hotkey").value_or(L"2");
    const auto language = ReadUserString(kToggleKey, L"Language Hotkey").value_or(L"1");
    return layout == L"2" || language == L"2";
}

inline bool SetCtrlShiftSwitches(bool on) {
    bool done = true;
    for (const wchar_t* name : {L"Layout Hotkey", L"Language Hotkey"}) {
        const std::wstring kept = std::wstring(L"Previous ") + name;
        if (!on) {
            const auto now = ReadUserString(kToggleKey, name).value_or(std::wstring(name) == L"Layout Hotkey" ? L"2" : L"1");
            if (now != L"2") continue;
            WriteUserString(kTekitoWindowsKey, kept.c_str(), now);
            done = WriteUserString(kToggleKey, name, L"3") && done;
        } else if (const auto before = ReadUserString(kTekitoWindowsKey, kept.c_str())) {
            done = WriteUserString(kToggleKey, name, *before) && done;
            DeleteUserValue(kTekitoWindowsKey, kept.c_str());
        } else if (std::wstring(name) == L"Layout Hotkey") {
            done = WriteUserString(kToggleKey, name, L"2") && done;
        }
    }
    if (ReadUserString(kToggleKey, L"Hotkey")) {
        // The older name of the language hotkey, kept in step.
        WriteUserString(kToggleKey, L"Hotkey", ReadUserString(kToggleKey, L"Language Hotkey").value_or(L"1"));
    }
    SystemParametersInfoW(SPI_SETLANGTOGGLE, 0, nullptr, SPIF_SENDCHANGE);
    return done;
}

}  // namespace tekito::settings
