#include "Tsf/TextService.h"
#include "Tsf/TextServiceShared.h"

// Keys: which keys the text service takes, and each key carried out in an
// edit session.

namespace tekito::tsf {

// Keys that end the word and still reach the application: anything that is
// not part of a word, and Enter unless it picks a candidate from the list.
bool TextService::LetsKeyThrough(const KeyInput& input) const noexcept {
    // In Japanese, Enter only commits the text, as in Microsoft IME.
    if (TypesJapanese(mode_)) {
        return input.type == KeyInput::Type::EndComposition || input.type == KeyInput::Type::MixedEnterPeriod;
    }
    return input.type == KeyInput::Type::EndComposition ||
           (input.type == KeyInput::Type::Enter && !state_.IsCandidateNavigationActive());
}

bool TextService::ProcessPassesThrough() const noexcept {
    return processIsElevated_ || processPrefersPassThrough_ || processExcludedByUser_;
}

// Commits the word being typed as it stands, from outside a key press: the
// mode changed, focus moved, or the caret left the word.
void TextService::FinishCompositionLater() {
    candidateWindow_.Hide();
    if (!compositionContext_) {
        state_.Reset();
        rawText_.clear();
        japanese_.Clear();
        return;
    }
    KeyInput input{};
    input.type = japanese_.IsComposing() ? KeyInput::Type::JapaneseCommit
                                         : KeyInput::Type::EndComposition;
    auto* session = new (std::nothrow) KeyEditSession(this, compositionContext_, input);
    HRESULT sessionResult = E_FAIL;
    const HRESULT hr = session ? compositionContext_->RequestEditSession(
                                     clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                     &sessionResult)
                               : E_OUTOFMEMORY;
    if (session) session->Release();
    if (FAILED(hr)) {
        TraceHr(L"FinishCompositionLater RequestEditSession failed", hr);
        state_.Reset();
        rawText_.clear();
        japanese_.Clear();
    }
}

HRESULT TextService::OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    SyncRuntimeState();
    if (IsReadOnly(context)) {
        *eaten = FALSE;
        return S_OK;
    }
    ModeKey modeKey;
    if (TranslateModeKey(wParam, modeKey)) {
        *eaten = TRUE;
        return S_OK;
    }
    KeyInput input{};
    if (!TranslateKey(wParam, lParam, input) || !ResolveSpecialKey(context, input)) {
        *eaten = FALSE;
        return S_OK;
    }
    if (LetsKeyThrough(input)) {
        // Finish the word now; the key itself goes on to the application.
        // An application that asks about the key again, or calls OnKeyDown
        // for it anyway, does not get the word written twice.
        if (keyFinishedInTest_ && keyFinishedInTest_->first == wParam && keyFinishedInTest_->second == lParam) {
            *eaten = FALSE;
            return S_OK;
        }
        // Only a word finished here is passed over in OnKeyDown; when the
        // edit session failed, OnKeyDown finishes it.
        if (context && RequestKeyEditSession(context, input) == S_OK) {
            keyFinishedInTest_ = std::pair{wParam, lParam};
        } else {
            keyFinishedInTest_.reset();
        }
        *eaten = FALSE;
        return S_OK;
    }
    keyFinishedInTest_.reset();
    *eaten = TRUE;
    if (*eaten) Trace(L"OnTestKeyDown eaten");
    return S_OK;
}

HRESULT TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    ScopedTraceDuration duration(L"Perf OnKeyDown");
    if (TraceEnabled()) {
        wchar_t line[48]{};
        swprintf_s(line, L"OnKeyDown vk=0x%02X", static_cast<unsigned>(wParam));
        Trace(line);
    }
    if (!context || !eaten) return E_INVALIDARG;
    SyncRuntimeState();
    activationSettling_ = false;
    if (IsReadOnly(context)) {
        *eaten = FALSE;
        return S_OK;
    }
    // Cat: each key pressed meows, a key held mews once (not a modifier alone).
    if (userSettings_.catSounds && wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU &&
        wParam != VK_LWIN && wParam != VK_RWIN && wParam != VK_CAPITAL) {
        catSounds_.OnKey(wParam, (lParam & (1 << 30)) != 0);
    }
    ModeKey modeKey;
    if (TranslateModeKey(wParam, modeKey)) {
        ApplyModeKey(context, modeKey);
        *eaten = TRUE;
        return S_OK;
    }
    KeyInput input{};
    if (!TranslateKey(wParam, lParam, input) || !ResolveSpecialKey(context, input)) {
        NotePassedKey(wParam);
        *eaten = FALSE;
        return S_OK;
    }

    const bool passThrough = LetsKeyThrough(input);
    if (passThrough && keyFinishedInTest_ && keyFinishedInTest_->first == wParam &&
        keyFinishedInTest_->second == lParam) {
        keyFinishedInTest_.reset();
        NotePassedKey(wParam);
        *eaten = FALSE;
        return S_OK;
    }
    keyFinishedInTest_.reset();
    const HRESULT hr = RequestKeyEditSession(context, input);
    *eaten = hr == S_OK && !passThrough ? TRUE : FALSE;
    // The first key typed after focus moved: the mode by the caret, so typing
    // in the wrong one is seen at once.
    if (*eaten && std::exchange(badgeOnNextKey_, false) && userSettings_.modeIndicatorEnabled &&
        userSettings_.modeBadgeOnFocus) {
        ShowModeIndicator(context);
    }
    if (!*eaten) NotePassedKey(wParam);
    if (*eaten) Trace(L"OnKeyDown edit session requested");
    return S_OK;
}

HRESULT TextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    return S_OK;
}

HRESULT TextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    return S_OK;
}

HRESULT TextService::OnPreservedKey(ITfContext* context, REFGUID guid, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    if (guid == GUID_TekitoKanaInputKey) {
        SyncRuntimeState();
        ToggleKanaInput(context);
        *eaten = TRUE;
        return S_OK;
    }
    if (guid != GUID_TekitoToggleKey) return S_OK;
    SyncRuntimeState();
    ModeKey key;
    key.mode = ToggledMode();
    ApplyModeKey(context, key);
    *eaten = TRUE;
    return S_OK;
}

bool TextService::TranslateKey(WPARAM wParam, LPARAM, KeyInput& input) {
    if (ProcessPassesThrough() || FocusIsClassicPasswordEdit()) return false;
    if (TypesJapanese(mode_)) {
        if (TranslateEnglishSegmentKey(wParam, input)) return true;
        return TranslateJapaneseKey(wParam, input);
    }
    if (state_.Mode() == InputMode::Direct) return false;
    return TranslateEnglishKey(wParam, input);
}

HRESULT TextService::RequestKeyEditSession(ITfContext* context, const KeyInput& input) {
    auto* session = new (std::nothrow) KeyEditSession(this, context, input);
    if (!session) return E_OUTOFMEMORY;

    HRESULT sessionResult = E_FAIL;
    HRESULT hr = context->RequestEditSession(clientId_, session,
                                             TF_ES_SYNC | TF_ES_READWRITE,
                                             &sessionResult);
    const bool guardedTarget = ProcessPassesThrough() || FocusIsClassicPasswordEdit();
    const bool syncRejected = hr == TF_E_SYNCHRONOUS || sessionResult == TF_E_SYNCHRONOUS ||
                              (hr == E_UNEXPECTED && sessionResult == E_FAIL);
    // Retried only when it did not run: a key handled and then failing is
    // not handled again (the text would be written twice).
    const bool retryAsync = !guardedTarget && syncRejected && !session->Ran();
    if (retryAsync) {
        Trace(L"RequestEditSession retry async");
        sessionResult = E_FAIL;
        hr = context->RequestEditSession(clientId_, session,
                                         TF_ES_ASYNC | TF_ES_READWRITE,
                                         &sessionResult);
        if (SUCCEEDED(hr)) {
            session->Release();
            return S_OK;
        }
    }
    session->Release();
    if (FAILED(hr) || FAILED(sessionResult)) {
        TraceHr(L"RequestEditSession failed", FAILED(hr) ? hr : sessionResult);
    }
    if (FAILED(hr)) return hr;
    if (FAILED(sessionResult)) return sessionResult;
    return sessionResult;
}

HRESULT TextService::HandleKeyInEditSession(ITfContext* context, TfEditCookie editCookie,
                                            const KeyInput& input) {
    const HRESULT hr = HandleKeyInEditSessionCore(context, editCookie, input);
    if (input.type != KeyInput::Type::JapaneseRefine && input.type != KeyInput::Type::JapaneseAiAnswer) {
        if (japanese_.RefinesOnPause()) {
            ScheduleJapaneseRefine();
        } else {
            CancelJapaneseRefine();
        }
    }
    if (japanese_.IsComposing() && japanese_.WaitsForAi()) {
        WatchAiAnswer();
    } else {
        StopWatchingAiAnswer();
    }
    // An English word typed with Shift in Japanese ends once it is committed.
    if (englishSegment_ && !state_.IsActive()) EndEnglishSegment();
    return hr;
}

void TextService::NoteTyped(std::wstring_view text) {
    constexpr std::size_t kKept = 64;
    typedTail_ += text;
    if (typedTail_.size() > kKept) typedTail_.erase(0, typedTail_.size() - kKept);
}

void TextService::NotePassedKey(WPARAM key) {
    switch (key) {
    case VK_RETURN: NoteTyped(L"\n"); break;
    case VK_SPACE: NoteTyped(L" "); break;
    case VK_TAB: NoteTyped(L"\t"); break;
    case VK_BACK:
        if (!typedTail_.empty()) typedTail_.pop_back();
        break;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_CAPITAL:
        break;
    default: {
        // A character typed as it is goes on the end; any other key may
        // have moved the caret (arrows, Home, a shortcut), and what was
        // typed no longer says what is before it.
        BYTE keyboardState[256]{};
        wchar_t buffer[4]{};
        const bool command = (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000);
        // Flag 4: the keyboard state (dead keys) is left as it is.
        const int count = !command && GetKeyboardState(keyboardState)
                              ? ToUnicodeEx(static_cast<UINT>(key), MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC),
                                            keyboardState, buffer, 4, 4, GetKeyboardLayout(0))
                              : 0;
        if (count == 1 && buffer[0] >= 0x20) {
            NoteTyped(std::wstring_view(buffer, 1));
        } else {
            typedTail_.clear();
        }
        break;
    }
    }
}

}  // namespace tekito::tsf
