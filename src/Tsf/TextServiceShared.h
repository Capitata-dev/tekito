#pragma once

// Helpers the text service's files share (TextService.cpp and the
// TextService*.cpp files); private to them.

#include "Tsf/TextService.h"
#include "Core/Japanese/JapaneseData.h"
#include "Tsf/ComPtr.h"
#include "Tsf/Compartments.h"
#include "Tsf/DisplayAttributes.h"
#include "Tsf/Diagnostics.h"
#include "Tsf/EnglishText.h"
#include "Tsf/Globals.h"
#include "Tsf/TekitoGuids.h"
#include "UserData/KeyboardLayout.h"
#include "UserData/UiLanguage.h"
#include "assets/tekito_resource.h"
#include <inputscope.h>
#include <algorithm>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <shellapi.h>
#include <filesystem>
#include <iterator>
#include <new>
#include <utility>
#include <oleauto.h>
#include <vector>

namespace tekito::tsf {

constexpr GUID kGuidPropInputScope = {0x1713dd5a,
                                      0x68e7,
                                      0x4a5b,
                                      {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

inline bool HasBlockedModifier() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (GetKeyState(VK_MENU) & 0x8000) != 0 ||
           (GetKeyState(VK_LWIN) & 0x8000) != 0 ||
           (GetKeyState(VK_RWIN) & 0x8000) != 0;
}

// Fields where correcting words would be wrong or unsafe: secrets, and
// fields that hold addresses, numbers, paths or code rather than prose.
inline bool IsPassThroughInputScope(InputScope scope) {
    switch (scope) {
    case IS_PASSWORD:
    case IS_NUMERIC_PASSWORD:
    case IS_NUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN_SET:
    case IS_URL:
    case IS_EMAIL_USERNAME:
    case IS_EMAIL_SMTPEMAILADDRESS:
    case IS_LOGINNAME:
    case IS_DIGITS:
    case IS_NUMBER:
    case IS_NUMBER_FULLWIDTH:
    case IS_TELEPHONE_FULLTELEPHONENUMBER:
    case IS_TELEPHONE_COUNTRYCODE:
    case IS_TELEPHONE_AREACODE:
    case IS_TELEPHONE_LOCALNUMBER:
    case IS_CURRENCY_AMOUNTANDSYMBOL:
    case IS_CURRENCY_AMOUNT:
    case IS_DATE_FULLDATE:
    case IS_DATE_MONTH:
    case IS_DATE_DAY:
    case IS_DATE_YEAR:
    case IS_TIME_FULLTIME:
    case IS_TIME_HOUR:
    case IS_TIME_MINORSEC:
    case IS_FILE_FULLFILEPATH:
    case IS_FILE_FILENAME:
    case IS_REGULAREXPRESSION:
    case IS_SRGS:
    case IS_XML:
    case IS_ALPHANUMERIC_HALFWIDTH:
        return true;
    default:
        return false;
    }
}

inline std::wstring ReadRangeText(ITfRange* range, TfEditCookie editCookie) {
    if (!range) return {};
    wchar_t buffer[256]{};
    ULONG length = 0;
    if (FAILED(range->GetText(editCookie, 0, buffer, 256, &length))) return {};
    std::wstring text(buffer, std::min<ULONG>(length, 255));
    // Browsers keep a space typed last as a no-break space; it is a space here.
    std::replace(text.begin(), text.end(), L'\x00A0', L' ');
    return text;
}

// Returns whether the text before the composition was read: nothing read
// is the start of the document only when the range could not move back.
inline bool ReadCompositionContext(ITfComposition* composition, TfEditCookie editCookie,
                            std::wstring& preceding, std::wstring& following) {
    if (!composition) return false;
    ComPtr<ITfRange> compositionRange;
    if (FAILED(composition->GetRange(compositionRange.Put()))) return false;

    bool read = false;
    ComPtr<ITfRange> before;
    if (SUCCEEDED(compositionRange->Clone(before.Put())) &&
        SUCCEEDED(before->Collapse(editCookie, TF_ANCHOR_START))) {
        LONG moved = 0;
        if (SUCCEEDED(before->ShiftStart(editCookie, -128, &moved, nullptr))) {
            preceding = ReadRangeText(before.Get(), editCookie);
            read = moved == 0 || !preceding.empty();
        }
    }

    ComPtr<ITfRange> after;
    if (SUCCEEDED(compositionRange->Clone(after.Put())) &&
        SUCCEEDED(after->Collapse(editCookie, TF_ANCHOR_END))) {
        LONG moved = 0;
        if (SUCCEEDED(after->ShiftEnd(editCookie, 128, &moved, nullptr))) {
            following = ReadRangeText(after.Get(), editCookie);
        }
    }
    return read;
}

inline std::wstring ReadSelectionContext(ITfContext* context, TfEditCookie editCookie) {
    if (!context) return {};
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection,
                                     &fetched)) || fetched != 1 || !selection.range) {
        return {};
    }
    ComPtr<ITfRange> range;
    range.Attach(selection.range);
    if (FAILED(range->Collapse(editCookie, TF_ANCHOR_START))) return {};
    LONG moved = 0;
    if (FAILED(range->ShiftStart(editCookie, -256, &moved, nullptr))) return {};
    return ReadRangeText(range.Get(), editCookie);
}

inline HRESULT ReplaceTrailingWhitespaceAtSelection(ITfContext* context, TfEditCookie editCookie,
                                             std::size_t whitespaceCount,
                                             const std::wstring& text) {
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection,
                                     &fetched)) || fetched != 1 || !selection.range) {
        return E_FAIL;
    }
    ComPtr<ITfRange> range;
    range.Attach(selection.range);
    HRESULT hr = range->Collapse(editCookie, TF_ANCHOR_START);
    if (FAILED(hr)) return hr;
    LONG moved = 0;
    hr = range->ShiftStart(editCookie, -static_cast<LONG>(whitespaceCount), &moved, nullptr);
    if (FAILED(hr) || moved != -static_cast<LONG>(whitespaceCount)) return E_FAIL;
    hr = range->SetText(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()));
    if (FAILED(hr)) return hr;
    hr = range->Collapse(editCookie, TF_ANCHOR_END);
    if (FAILED(hr)) return hr;
    TF_SELECTION next{};
    next.range = range.Get();
    next.style.ase = TF_AE_END;
    next.style.fInterimChar = FALSE;
    return context->SetSelection(editCookie, 1, &next);
}

// The input scopes the application gives the field at the selection.
inline std::vector<InputScope> ContextInputScopes(ITfContext* context, TfEditCookie editCookie) {
    if (!context) return {};

    ComPtr<ITfReadOnlyProperty> property;
    if (FAILED(context->GetAppProperty(kGuidPropInputScope, property.Put()))) return {};

    TF_SELECTION selection{};
    ULONG fetched = 0;
    ComPtr<ITfRange> range;
    if (SUCCEEDED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) &&
        fetched == 1 && selection.range) {
        range.Attach(selection.range);
    }

    VARIANT value{};
    VariantInit(&value);
    HRESULT hr = property->GetValue(editCookie, range.Get(), &value);
    if (FAILED(hr) || value.vt != VT_UNKNOWN || !value.punkVal) {
        VariantClear(&value);
        return {};
    }

    ComPtr<ITfInputScope> inputScope;
    hr = value.punkVal->QueryInterface(IID_PPV_ARGS(inputScope.Put()));
    VariantClear(&value);
    if (FAILED(hr)) return {};

    InputScope* scopes = nullptr;
    UINT count = 0;
    hr = inputScope->GetInputScopes(&scopes, &count);
    if (FAILED(hr)) return {};
    std::vector<InputScope> out(scopes, scopes + count);
    CoTaskMemFree(scopes);
    return out;
}

// A private window (a browser's InPrivate or incognito one marks its fields
// IS_PRIVATE): typed in as anywhere, but nothing is learned.
inline bool ContextIsPrivate(ITfContext* context, TfEditCookie editCookie) {
    const auto scopes = ContextInputScopes(context, editCookie);
    return std::find(scopes.begin(), scopes.end(), IS_PRIVATE) != scopes.end();
}

inline bool ContextHasPassThroughInputScope(ITfContext* context, TfEditCookie editCookie) {
    const auto scopes = ContextInputScopes(context, editCookie);
    return std::any_of(scopes.begin(), scopes.end(), IsPassThroughInputScope);
}

// Japanese is typed in more fields than English is corrected in: a
// browser's address bar (a URL field) searches in Japanese, and addresses,
// user names and file names may be Japanese too.
inline bool ContextHasJapanesePassThroughInputScope(ITfContext* context, TfEditCookie editCookie) {
    const auto scopes = ContextInputScopes(context, editCookie);
    return std::any_of(scopes.begin(), scopes.end(), [](InputScope scope) {
        switch (scope) {
        case IS_URL:
        case IS_EMAIL_USERNAME:
        case IS_EMAIL_SMTPEMAILADDRESS:
        case IS_LOGINNAME:
        case IS_FILE_FULLFILEPATH:
        case IS_FILE_FILENAME:
            return false;
        default:
            return IsPassThroughInputScope(scope);
        }
    });
}

// A field for katakana (a furigana field) types katakana.
inline std::optional<japanese::KanaForm> FieldKanaForm(ITfContext* context, TfEditCookie editCookie) {
    for (const auto scope : ContextInputScopes(context, editCookie)) {
        if (scope == IS_KATAKANA_FULLWIDTH || scope == IS_KATAKANA_HALFWIDTH) return japanese::KanaForm::Katakana;
        if (scope == IS_HIRAGANA) return japanese::KanaForm::Hiragana;
    }
    return std::nullopt;
}

// No document, or one the application marks read-only: keys are not ours.
inline bool IsReadOnly(ITfContext* context) {
    if (!context) return true;
    TF_STATUS status{};
    return SUCCEEDED(context->GetStatus(&status)) && (status.dwDynamicFlags & TF_SD_READONLY) != 0;
}

inline bool FocusIsClassicPasswordEdit() {
    HWND focus = GetFocus();
    if (!focus) return false;

    wchar_t className[32]{};
    if (GetClassNameW(focus, className, static_cast<int>(std::size(className))) <= 0) {
        return false;
    }
    if (std::wcscmp(className, L"Edit") != 0) return false;

    const LONG_PTR style = GetWindowLongPtrW(focus, GWL_STYLE);
    if ((style & ES_PASSWORD) != 0) return true;

    return SendMessageW(focus, EM_GETPASSWORDCHAR, 0, 0) != 0;
}

inline bool CurrentProcessIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

inline std::wstring CurrentProcessName() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    return std::filesystem::path(path, path + length).filename().wstring();
}

inline bool IsBuiltInPassThroughProcess(const std::wstring& name) {
    const auto& apps = userdata::kBuiltInExcludedApps;
    return std::any_of(std::begin(apps), std::end(apps), [&](const wchar_t* app) {
        return _wcsicmp(name.c_str(), app) == 0;
    });
}

inline bool ToggleKeyFor(int key, TF_PRESERVEDKEY& preserved) {
    switch (key) {
    case 1:
        // A US keyboard's stand-in for Hankaku/Zenkaku.
        preserved = {VK_OEM_3, TF_MOD_ALT};
        return true;
    case 2:
        preserved = {VK_SPACE, TF_MOD_CONTROL};
        return true;
    case 3:
        preserved = {VK_SPACE, TF_MOD_CONTROL | TF_MOD_SHIFT};
        return true;
    default:
        return false;
    }
}

inline ComPtr<ITfCompartment> OpenCloseCompartment(ITfThreadMgr* threadManager) {
    return ThreadCompartment(threadManager, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
}

inline ComPtr<ITfCompartment> ConversionCompartment(ITfThreadMgr* threadManager) {
    return ThreadCompartment(threadManager, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
}

inline HRESULT AdviseSink(IUnknown* source, REFIID riid, IUnknown* sink, DWORD& cookie) {
    ComPtr<ITfSource> tfSource;
    HRESULT hr = source->QueryInterface(IID_PPV_ARGS(tfSource.Put()));
    if (SUCCEEDED(hr)) hr = tfSource->AdviseSink(riid, sink, &cookie);
    if (FAILED(hr)) cookie = TF_INVALID_COOKIE;
    return hr;
}

inline void UnadviseSink(IUnknown* source, DWORD& cookie) {
    if (cookie == TF_INVALID_COOKIE) return;
    ComPtr<ITfSource> tfSource;
    if (source && SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(tfSource.Put())))) {
        tfSource->UnadviseSink(cookie);
    }
    cookie = TF_INVALID_COOKIE;
}

inline DWORD WINAPI PreloadEngineDataThread(void* parameter) {
    const auto module = static_cast<HMODULE>(parameter);
    try {
        PreloadDefaultEngineData();
    } catch (...) {
        Trace(L"Engine data preload failed");
    }
    // The thread holds its own reference on this DLL so it can finish even
    // if the text service is released meanwhile.
    FreeLibraryAndExitThread(module, 0);
}

// Loads the language data once per process, on a worker thread.
inline void StartEngineDataPreload() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&PreloadEngineDataThread), &module)) {
        started = false;
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, &PreloadEngineDataThread, module, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        FreeLibrary(module);
        started = false;
    }
}

inline bool IsNavigationKey(WPARAM key) {
    switch (key) {
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_DELETE:
    case VK_INSERT:
        return true;
    default:
        return false;
    }
}

class DisplayAttributeInfoEnum final : public IEnumTfDisplayAttributeInfo {
public:
    // Takes a reference to each info.
    explicit DisplayAttributeInfoEnum(std::vector<ITfDisplayAttributeInfo*> infos, std::size_t next = 0)
        : infos_(std::move(infos)), next_(next) {
        for (auto* info : infos_) {
            if (info) info->AddRef();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IEnumTfDisplayAttributeInfo) {
            *object = static_cast<IEnumTfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --refCount_;
        if (value == 0) delete this;
        return value;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumTfDisplayAttributeInfo** clone) override {
        if (!clone) return E_INVALIDARG;
        *clone = new (std::nothrow) DisplayAttributeInfoEnum(infos_, next_);
        return *clone ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfDisplayAttributeInfo** info, ULONG* fetched) override {
        if (!info || (count != 1 && !fetched)) return E_INVALIDARG;
        ULONG taken = 0;
        while (taken < count && next_ < infos_.size()) {
            info[taken] = infos_[next_++];
            info[taken]->AddRef();
            ++taken;
        }
        if (fetched) *fetched = taken;
        return taken == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        next_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const std::size_t left = infos_.size() - next_;
        next_ += std::min<std::size_t>(count, left);
        return count <= left ? S_OK : S_FALSE;
    }

private:
    ~DisplayAttributeInfoEnum() {
        for (auto* info : infos_) {
            if (info) info->Release();
        }
    }

    std::atomic<ULONG> refCount_{1};
    std::vector<ITfDisplayAttributeInfo*> infos_;
    std::size_t next_{0};
};



// Alt+カタカナひらがな: the ローマ字 key (VK 0xF5) on a JIS keyboard, or
// Alt with the ひらがな key (VK 0xF2) as some drivers send it.
constexpr TF_PRESERVEDKEY kKanaInputKeys[] = {{0xF5, 0}, {0xF5, TF_MOD_ALT}, {0xF2, TF_MOD_ALT}};


}  // namespace tekito::tsf
