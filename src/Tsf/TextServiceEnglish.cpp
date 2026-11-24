#include "Tsf/TextService.h"
#include "Tsf/TextServiceShared.h"

// English input: keys through the English state machine, special endings,
// smart punctuation, and words looked at again before the caret.

namespace tekito::tsf {

bool TextService::TranslateEnglishKey(WPARAM wParam, KeyInput& input) {
    if (!EnsureEngine()) return false;  // language data still loading

    if (HasBlockedModifier()) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }

    if (wParam == VK_SPACE) {
        if (!state_.IsActive()) return false;
        input.type = (GetKeyState(VK_SHIFT) & 0x8000) ? KeyInput::Type::ShiftSpace
                                                     : KeyInput::Type::Space;
        return true;
    }
    if (wParam == VK_TAB) {
        if (!state_.IsActive()) return false;
        input.type = (GetKeyState(VK_SHIFT) & 0x8000) ? KeyInput::Type::ShiftTab
                                                     : KeyInput::Type::Tab;
        return true;
    }
    if (wParam == VK_BACK) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::Backspace;
        return true;
    }
    if (wParam == VK_ESCAPE) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::Cancel;
        return true;
    }
    if (wParam == VK_RETURN) {
        if (!state_.IsActive() && !userSettings_.periodOnEnter) return false;
        input.type = KeyInput::Type::Enter;
        return true;
    }
    if ((state_.IsCandidateNavigationActive() || state_.State() == CompositionState::Cycling) &&
        (wParam == VK_UP || wParam == VK_DOWN)) {
        input.type = wParam == VK_UP ? KeyInput::Type::UpArrow : KeyInput::Type::DownArrow;
        return true;
    }
    if (IsNavigationKey(wParam)) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }

    BYTE keyboardState[256]{};
    if (!GetKeyboardState(keyboardState)) return false;
    wchar_t buffer[8]{};
    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(wParam), MAPVK_VK_TO_VSC);
    const int count = ToUnicodeEx(static_cast<UINT>(wParam), scanCode, keyboardState,
                                  buffer, static_cast<int>(std::size(buffer)), 4,
                                  GetKeyboardLayout(0));
    if (count == 1 && buffer[0] >= 0x20 && buffer[0] != 0x7f) {
        // "--" becomes a dash; a hyphen inside a word still ends it as typed.
        if (userSettings_.smartPunctuation && buffer[0] == L'-' &&
            !(state_.IsActive() && state_.State() == CompositionState::Composing)) {
            input.type = KeyInput::Type::SmartPunctuation;
            input.character = buffer[0];
            return true;
        }
        // Not in an English word typed inside Japanese, which ends on its own keys.
        if (!TypesJapanese(mode_) && SpecialConversions::Installed().MayEndEnglish(buffer[0], SpecialOptions())) {
            input.type = KeyInput::Type::SpecialEnd;
            input.character = buffer[0];
            return true;
        }
        return TranslateEnglishCharacter(buffer[0], input);
    }

    return false;
}

bool TextService::TranslateEnglishCharacter(wchar_t character, KeyInput& input) const {
    if (userSettings_.bracketPairs && std::wstring_view(L"([{").find(character) != std::wstring_view::npos) {
        input.type = KeyInput::Type::BracketPair;
        input.character = character;
        return true;
    }
    if (userSettings_.smartPunctuation && (character == L'"' || character == L'\'')) {
        const bool inWord = state_.IsActive() && state_.State() == CompositionState::Composing;
        if (inWord && character == L'"') {
            input.type = KeyInput::Type::Punctuation;
            input.character = L'\u201D';
            input.punctuationRole = PunctuationRole::ClauseSeparator;
            return true;
        }
        if (!inWord) {
            input.type = KeyInput::Type::SmartPunctuation;
            input.character = character;
            return true;
        }
    }
    // Right after a word a mark put right: a letter or digit (or @ _ / %)
    // goes on with it, as typed (Printable undoes the fix).
    if (!state_.IsActive() && !markFixShown_.empty() &&
        (std::iswalnum(character) || std::wstring_view(L"@_/%").find(character) != std::wstring_view::npos)) {
        input.type = KeyInput::Type::Printable;
        input.character = character;
        return true;
    }
    PunctuationRole punctuationRole{};
    if (TryClassifyPunctuation(character, punctuationRole)) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::Punctuation;
        input.character = character;
        input.punctuationRole = punctuationRole;
        return true;
    }
    if (!IsWordCharacter(character)) {
        // Digits and symbols are not part of a word: finish the word
        // being typed as it stands and let the character through.
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }
    if (!state_.IsActive() && !std::iswalpha(character)) return false;  // no word starts with '
    input.type = KeyInput::Type::Printable;
    input.character = character;
    return true;
}

std::optional<SpecialConversions::Ending> TextService::SpecialEnding(std::wstring_view preceding,
                                                                    wchar_t character) const {
    std::wstring text(preceding);
    text.push_back(character);
    auto ending = SpecialConversions::Installed().EnglishEnding(text, SpecialOptions());
    // What was typed before the key must be in the document: at least one
    // character of it.
    if (ending && ending->length < 2) ending.reset();
    return ending;
}

bool TextService::ResolveSpecialKey(ITfContext* context, KeyInput& input) {
    if (input.type != KeyInput::Type::SpecialEnd) return true;
    bool found = false;
    if (context) {
        auto* session = new (std::nothrow) ReadEditSession([&](TfEditCookie editCookie) {
            found = SpecialEnding(ReadSelectionContext(context, editCookie), input.character).has_value();
        });
        if (session) {
            HRESULT result = E_FAIL;
            [[maybe_unused]] const HRESULT hr =
                context->RequestEditSession(clientId_, session, TF_ES_SYNC | TF_ES_READ, &result);
            session->Release();
        }
    }
    if (found) return true;
    const wchar_t character = input.character;
    input = KeyInput{};
    return TranslateEnglishCharacter(character, input);
}

HRESULT TextService::HandleSpecialEnd(ITfContext* context, TfEditCookie editCookie, wchar_t character) {
    const std::wstring preceding = ReadSelectionContext(context, editCookie);
    const auto ending = SpecialEnding(preceding, character);
    // A word being typed ends as it stands.
    if (composition_) {
        HRESULT hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
    }
    candidateWindow_.Hide();
    state_.Reset();
    rawText_.clear();
    // The document changed since the key was looked at: type it as it is.
    if (!ending) return InsertAtSelection(context, editCookie, std::wstring(1, character));

    HRESULT hr = StartCompositionBefore(context, editCookie, ending->length - 1);
    if (FAILED(hr)) return InsertAtSelection(context, editCookie, std::wstring(1, character));
    rawText_ = preceding.substr(preceding.size() - (ending->length - 1));
    rawText_.push_back(character);
    hr = ReplaceComposition(context, editCookie, rawText_);
    if (FAILED(hr)) return hr;
    // What was typed stays first; the symbols or the sum are only offered.
    std::vector<Candidate> candidates;
    Candidate typed;
    typed.text = rawText_;
    typed.label = SemanticLabel::Original;
    typed.isOriginal = true;
    typed.isProtected = true;
    typed.policyFlags = CandidatePolicyProtect;
    candidates.push_back(std::move(typed));
    for (const auto& offer : ending->offers) {
        if (offer == rawText_) continue;
        Candidate candidate;
        candidate.text = offer;
        candidate.isProtected = true;
        candidate.policyFlags = CandidatePolicySuggestOnly;
        candidates.push_back(std::move(candidate));
    }
    symbolComposition_ = true;
    state_.BeginOrUpdate(rawText_, std::move(candidates));
    ShowCandidates(context, editCookie);
    return S_OK;
}

HRESULT TextService::HandleSmartPunctuation(ITfContext* context, TfEditCookie editCookie, wchar_t character) {
    // A word ended by Space stays as it is shown.
    if (composition_) {
        HRESULT hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
    }
    candidateWindow_.Hide();
    state_.Reset();
    rawText_.clear();

    const auto preceding = ReadSelectionContext(context, editCookie);
    if (character == L'-') {
        if (MakesEmDash(preceding)) return ReplaceTrailingWhitespaceAtSelection(context, editCookie, 1, L"\u2014");
        if (SpecialEnding(preceding, character)) return HandleSpecialEnd(context, editCookie, character);
        return InsertAtSelection(context, editCookie, L"-");
    }
    return InsertAtSelection(context, editCookie, std::wstring(1, SmartQuote(character, preceding)));
}

HRESULT TextService::StartCompositionBefore(ITfContext* context, TfEditCookie editCookie, std::size_t count) {
    TF_SELECTION selection{};
    ULONG fetched = 0;
    HRESULT hr = context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(hr) || fetched != 1 || !selection.range) return FAILED(hr) ? hr : E_FAIL;
    ComPtr<ITfRange> range;
    range.Attach(selection.range);
    hr = range->Collapse(editCookie, TF_ANCHOR_START);
    if (FAILED(hr)) return hr;
    LONG moved = 0;
    hr = range->ShiftStart(editCookie, -static_cast<LONG>(count), &moved, nullptr);
    if (FAILED(hr)) return hr;
    if (moved != -static_cast<LONG>(count)) return E_FAIL;

    ComPtr<ITfContextComposition> compositionContext;
    hr = context->QueryInterface(IID_PPV_ARGS(compositionContext.Put()));
    if (FAILED(hr)) return hr;
    hr = compositionContext->StartComposition(editCookie, range.Get(), this, &composition_);
    if (FAILED(hr) || !composition_) {
        TraceHr(L"StartCompositionBefore StartComposition failed", hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    compositionContext_ = context;
    compositionContext_->AddRef();
    AdviseContextSinks(context);
    return S_OK;
}

HRESULT TextService::HandleKeyInEditSessionCore(ITfContext* context, TfEditCookie editCookie,
                                                const KeyInput& input) {
    Trace(L"HandleKeyInEditSession");
    if (!context) return E_INVALIDARG;
    // Nothing is learned from what is typed in a private window, nor
    // anywhere in secret mode (which also keeps nothing, as a private
    // window does whatever the settings say).
    const bool secret = runtimeMode_ && runtimeMode_->Secret();
    const bool privateWindow = ContextIsPrivate(context, editCookie);
    privateField_ = privateWindow || secret;
    // The mode icon shows a private window where nothing is learned.
    const bool shown = privateWindow && userSettings_.noLearningInPrivate && userSettings_.learningEnabled;
    if (shown != privateWindowShown_) {
        privateWindowShown_ = shown;
        if (modeLangBarItem_) modeLangBarItem_->NotifyUpdate();
    }
    const bool privateField = (userSettings_.noLearningInPrivate && privateField_) || secret;
    japanese_.SetLearning(userSettings_.learningEnabled && !privateField ? &japaneseLearning_ : nullptr);
    // Secret mode uses nothing learned before either: no word typed earlier
    // shows among the English candidates while others can see the screen.
    userLearningProvider_.SetEnabled(userSettings_.learningEnabled && !secret);
    socialLearningProvider_.SetProfile(userSettings_.socialExpressionRange, secret ? 0 : userSettings_.socialPersonalization);
    if (input.type == KeyInput::Type::ShowModeIndicator) {
        ShowModeBadge(CaretAnchor(context, editCookie));
        return S_OK;
    }
    if (input.type == KeyInput::Type::Reposition) {
        if (!composition_ || !candidateWindow_.IsShown()) return S_OK;
        // Japanese conversion candidates and predictions follow the text too.
        if (japanese_.IsComposing()) {
            ShowJapaneseCandidates(context, editCookie);
            return S_OK;
        }
        const RECT anchor = GetCandidateAnchor(context, editCookie);
        if (!EqualRect(&anchor, &candidateAnchor_)) {
            candidateAnchor_ = anchor;
            candidateWindow_.Show(anchor, EnglishRows(), state_.SelectedIndex(), state_.PageStart(),
                                  state_.VisibleCount(), SelectedEnglishMeaning(), {}, 0, state_.PageIndex(),
                                  state_.PageCount());
        }
        return S_OK;
    }
    if (input.type == KeyInput::Type::EnglishSegmentEnd ||
        input.type == KeyInput::Type::EnglishSegmentCommit) {
        return HandleEnglishSegmentEnd(context, editCookie, input);
    }
    if (input.englishSegment && !englishSegment_ && TypesJapanese(mode_)) {
        englishSegment_ = true;
        state_.SetInputMode(InputMode::Convert);
    }
    if (input.type == KeyInput::Type::JapaneseRefine) {
        if (context != compositionContext_ || !composition_ || !japanese_.RefineLive()) return S_OK;
        return ShowJapanesePreedit(context, editCookie);
    }
    if (input.type == KeyInput::Type::JapaneseRedisplay) {
        if (context != compositionContext_ || !composition_ || !japanese_.IsComposing()) return S_OK;
        return ShowJapanesePreedit(context, editCookie);
    }
    if (input.type == KeyInput::Type::JapaneseAiAnswer) {
        if (context != compositionContext_ || !composition_ || !japanese_.TakeAiAnswer()) return S_OK;
        return ShowJapanesePreedit(context, editCookie);
    }
    // Japanese typing, and committing it after the mode or focus changed.
    if ((TypesJapanese(mode_) && !englishSegment_) ||
        input.type == KeyInput::Type::JapaneseCommit || japanese_.IsComposing()) {
        if (input.type != KeyInput::Type::JapaneseCommit &&
            (ContextHasJapanesePassThroughInputScope(context, editCookie) || FocusIsClassicPasswordEdit() ||
             ProcessPassesThrough())) {
            const HRESULT hr = CommitJapanese(context, editCookie);
            return SUCCEEDED(hr) ? S_FALSE : hr;
        }
        if (!japanese_.IsComposing()) {
            japanese_.SetFieldForm(FieldKanaForm(context, editCookie));
            // The AI model reads the text before the caret as what the
            // sentence follows (it does not change while typing): kept in
            // memory only, and not read in a private window.
            if (japanese_.UsesAi() && !privateField_) {
                const std::wstring before = ReadSelectionContext(context, editCookie);
                if (!before.empty()) japanese_.SetTextBefore(before);
            }
        }
        return HandleJapaneseKey(context, editCookie, input);
    }
    if (state_.Mode() == InputMode::Direct) {
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        const HRESULT hr = EndComposition(editCookie);
        return SUCCEEDED(hr) ? S_FALSE : hr;
    }
    if (ContextHasPassThroughInputScope(context, editCookie) || FocusIsClassicPasswordEdit() ||
        ProcessPassesThrough()) {
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        HRESULT hr = EndComposition(editCookie);
        return SUCCEEDED(hr) ? S_FALSE : hr;
    }

    const std::wstring markFixShown = std::exchange(markFixShown_, {});
    const std::wstring markFixTyped = std::exchange(markFixTyped_, {});
    if (input.type == KeyInput::Type::SpecialEnd) return HandleSpecialEnd(context, editCookie, input.character);
    if (input.type == KeyInput::Type::SmartPunctuation) {
        return HandleSmartPunctuation(context, editCookie, input.character);
    }
    if (input.type == KeyInput::Type::BracketPair) {
        // The word being typed ends as it is shown.
        if (composition_) {
            const HRESULT hr = EndComposition(editCookie);
            if (FAILED(hr)) return hr;
        }
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        constexpr std::wstring_view kOpen = L"([{";
        constexpr std::wstring_view kClose = L")]}";
        const auto at = kOpen.find(input.character);
        return InsertPairAtSelection(context, editCookie, std::wstring(1, input.character),
                                     std::wstring(1, kClose[at]));
    }
    // A spelling or sum: typing on keeps it as typed; Backspace takes its
    // last character and keeps the rest.
    if (symbolComposition_ && state_.State() == CompositionState::Composing) {
        if (input.type == KeyInput::Type::Backspace) {
            if (!rawText_.empty()) rawText_.pop_back();
            HRESULT hr = ReplaceComposition(context, editCookie, rawText_);
            if (FAILED(hr)) return hr;
            candidateWindow_.Hide();
            state_.Reset();
            rawText_.clear();
            return EndComposition(editCookie);
        }
        if (input.type == KeyInput::Type::Printable) {
            HRESULT hr = EndComposition(editCookie);
            if (FAILED(hr)) return hr;
            candidateWindow_.Hide();
            state_.Reset();
            rawText_.clear();
        }
    }
    const bool learning = userSettings_.learningEnabled && !symbolComposition_ && !privateField;

    // Social learning keeps nothing either where nothing is learned (secret
    // mode, a private window, learning off).
    const auto recordSocialSelection = [&](const Candidate& candidate) {
        if (!learning || userSettings_.socialPersonalization <= 0 ||
            candidate.socialRange == SocialRangeUnspecified) return;
        const auto trigger = rawText_.starts_with(L':') ? std::wstring_view(rawText_).substr(1)
                                                        : std::wstring_view(rawText_);
        socialLearning_.RecordSelection(trigger, candidate.text);
        for (const auto& other : state_.Candidates()) {
            if (&other != &candidate &&
                other.socialRange != SocialRangeUnspecified) {
                socialLearning_.RecordNonSelection(trigger, other.text);
            }
        }
    };
    const auto recordLearningDecision = [&](std::wstring_view raw,
                                            const std::vector<Candidate>& candidates,
                                            std::size_t selectedIndex,
                                            bool explicitSelection) {
        if (!learning || raw.empty() || selectedIndex >= candidates.size()) {
            return;
        }
        const auto& selected = candidates[selectedIndex];
        if (selected.text.empty()) return;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (candidate.text.empty()) continue;
            userLearning_.RecordExposure(raw, candidate.text);
            if (index != selectedIndex) userLearning_.RecordNonSelection(raw, candidate.text);
        }
        if (explicitSelection) {
            userLearning_.RecordSelection(raw, selected.text);
        } else {
            userLearning_.RecordAutomaticAcceptance(raw, selected.text);
        }
    };

    switch (input.type) {
    case KeyInput::Type::Printable: {
        if (!composition_ && !state_.IsActive() &&
            !CanStartWordAfter(ReadSelectionContext(context, editCookie))) {
            // Inside a URL, address, path or identifier: type it as is, and
            // the word a mark just put right (haruka.sato) as typed too.
            if (!markFixShown.empty()) ReplaceBeforeCaret(context, editCookie, markFixShown, markFixTyped);
            return InsertAtSelection(context, editCookie, std::wstring(1, input.character));
        }
        const auto action = state_.OnPrintable();
        if (action.kind == ActionKind::CommitAndStartNext) {
            HRESULT hr = EndComposition(editCookie);
            if (FAILED(hr)) return hr;
            candidateWindow_.Hide();
            rawText_.clear();
        }

        // The text before the word, read from the caret before the new
        // composition starts: right after one ends, some applications show
        // the new composition's range nothing before it.
        const std::wstring atCaret = rawText_.empty() ? ReadSelectionContext(context, editCookie) : std::wstring{};

        HRESULT hr = EnsureComposition(context, editCookie);
        if (FAILED(hr)) return hr;

        auto character = input.character;
        if (rawText_.empty()) {
            Context conversionContext;
            const bool read = ReadCompositionContext(composition_, editCookie,
                                                     conversionContext.precedingText,
                                                     conversionContext.followingText);
            // Nothing read before the word: what was typed here says what is
            // before it, when anything was.
            const std::wstring& before = !conversionContext.precedingText.empty() ? conversionContext.precedingText
                                         : !atCaret.empty()                        ? atCaret
                                                                                   : typedTail_;
            // Text that could not be read is no sentence start: a capital
            // put in mid-sentence is worse than one left out.
            sentenceStart_ = (read || !before.empty()) && IsSentenceStart(before);
            capitalizationOrigin_ = CapitalizationOrigin::UserTyped;
            if (sentenceStart_ && std::iswlower(character)) {
                character = static_cast<wchar_t>(std::towupper(character));
                capitalizationOrigin_ = CapitalizationOrigin::EngineApplied;
            }
        } else if (capitalizationOrigin_ == CapitalizationOrigin::EngineApplied &&
                   rawText_.size() == 1 && std::iswupper(character)) {
            rawText_.front() = static_cast<wchar_t>(std::towlower(rawText_.front()));
            capitalizationOrigin_ = CapitalizationOrigin::UserTyped;
        }
        rawText_.push_back(character);
        hr = ReplaceComposition(context, editCookie, rawText_);
        if (FAILED(hr)) return hr;
        RefreshCandidates(context, editCookie);
        Trace(L"Printable handled");
        return S_OK;
    }

    case KeyInput::Type::Space: {
        const auto rawBefore = rawText_;
        // The word before this one, when Space left it as typed; only on
        // this word's first Space.
        const bool firstSpace = state_.State() == CompositionState::Composing;
        const auto keptBefore = firstSpace ? std::exchange(keptWord_, std::wstring{}) : std::wstring{};
        const auto typedBefore = firstSpace ? std::exchange(keptTyped_, std::wstring{}) : std::wstring{};
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const bool explicitSelection = state_.IsCandidateNavigationActive();
        if (explicitSelection && selectedBefore < state_.Candidates().size()) {
            recordLearningDecision(rawBefore, state_.Candidates(), selectedBefore, true);
            recordSocialSelection(state_.Candidates()[selectedBefore]);
        }
        const auto action = state_.OnSpace();
        if (action.kind == ActionKind::CommitAndStartNext) {
            if (learning) {
                if (explicitSelection && !candidateBefore.empty() && candidateBefore != rawBefore) {
                    userLearning_.RecordCandidateSelection(candidateBefore);
                } else {
                    userLearning_.RecordRawKeep(rawBefore);
                }
            }
            HRESULT hr = ReplaceComposition(context, editCookie, action.text);
            if (FAILED(hr)) return hr;
            hr = EndComposition(editCookie);
            if (FAILED(hr)) return hr;
            candidateWindow_.Hide();
            rawText_.clear();
            return S_OK;
        }
        if (action.kind != ActionKind::ReplaceComposition) return S_FALSE;
        const auto selectedAfter = state_.SelectedIndex();
        if (selectedAfter < state_.Candidates().size()) {
            if (explicitSelection) recordSocialSelection(state_.Candidates()[selectedAfter]);
        }
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (SUCCEEDED(hr)) {
            if (action.showCandidates) {
                ShowCandidates(context, editCookie);
            } else {
                candidateWindow_.Hide();
            }
            if (firstSpace) {
                std::wstring left = action.text;
                while (!left.empty() && left.back() == L' ') left.pop_back();
                keptWord_ = left;
                keptTyped_ = rawBefore;
                if (userSettings_.recheckPreviousWord && !keptBefore.empty()) {
                    RecheckPreviousWord(context, editCookie, keptBefore, typedBefore, left);
                }
            }
        }
        return hr;
    }

    case KeyInput::Type::ShiftSpace:
    case KeyInput::Type::Tab:
    case KeyInput::Type::ShiftTab:
    case KeyInput::Type::UpArrow:
    case KeyInput::Type::DownArrow:
    case KeyInput::Type::CandidateSelection: {
        InputAction action;
        if (input.type == KeyInput::Type::CandidateSelection) {
            if (input.candidateIndex < state_.Candidates().size()) {
                recordSocialSelection(state_.Candidates()[input.candidateIndex]);
            }
            action = state_.OnCandidateSelected(input.candidateIndex);
        } else if (input.type == KeyInput::Type::ShiftSpace ||
                   input.type == KeyInput::Type::ShiftTab ||
                   input.type == KeyInput::Type::UpArrow) {
            action = state_.OnPreviousCandidate();
        } else {
            action = state_.OnNextCandidate();
        }
        if (action.kind != ActionKind::ReplaceComposition) return S_FALSE;
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (SUCCEEDED(hr)) ShowCandidates(context, editCookie);
        return hr;
    }

    case KeyInput::Type::Backspace: {
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const bool socialCandidateBefore = selectedBefore < state_.Candidates().size() &&
            state_.Candidates()[selectedBefore].socialRange != SocialRangeUnspecified;
        const auto action = state_.OnBackspace();
        if (action.kind == ActionKind::RestoreOriginal) {
            if (learning && !candidateBefore.empty()) userLearning_.RecordUndo(candidateBefore);
            if (learning && !candidateBefore.empty()) {
                userLearning_.RecordExposure(rawText_, candidateBefore);
                userLearning_.RecordUndo(rawText_, candidateBefore);
            }
            if (learning && socialCandidateBefore && userSettings_.socialPersonalization > 0) {
                const auto trigger = rawText_.starts_with(L':') ? std::wstring_view(rawText_).substr(1)
                                                                : std::wstring_view(rawText_);
                socialLearning_.RecordUndo(trigger, candidateBefore);
            }
            rawText_ = action.text;
            HRESULT hr = ReplaceComposition(context, editCookie, rawText_);
            if (SUCCEEDED(hr)) ShowCandidates(context, editCookie);
            return hr;
        }

        if (!rawText_.empty()) rawText_.pop_back();
        if (rawText_.empty()) {
            // The underline off while it still has text to come off: once
            // the text is gone, some browsers keep drawing it.
            ClearCompositionUnderline(editCookie);
            HRESULT hr = ReplaceComposition(context, editCookie, rawText_);
            if (FAILED(hr)) return hr;
            state_.Reset();
            candidateWindow_.Hide();
            return EndComposition(editCookie);
        }
        HRESULT hr = ReplaceComposition(context, editCookie, rawText_);
        if (SUCCEEDED(hr)) RefreshCandidates(context, editCookie);
        return hr;
    }

    case KeyInput::Type::Punctuation: {
        const auto rawBefore = rawText_;
        const bool firstMark = state_.State() == CompositionState::Composing;
        const auto keptBefore = firstMark ? std::exchange(keptWord_, std::wstring{}) : std::wstring{};
        const auto typedBefore = firstMark ? std::exchange(keptTyped_, std::wstring{}) : std::wstring{};
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const bool explicitSelection = state_.IsCandidateNavigationActive();
        if (selectedBefore < state_.Candidates().size()) {
            recordLearningDecision(rawBefore, state_.Candidates(), selectedBefore, explicitSelection);
        }
        const auto action = state_.OnPunctuation(input.character, input.punctuationRole);
        if (action.kind != ActionKind::ReplaceComposition) return S_FALSE;
        if (!candidateBefore.empty() && candidateBefore != rawBefore) {
            if (learning) userLearning_.RecordCandidateSelection(candidateBefore);
        } else {
            if (learning) userLearning_.RecordRawKeep(rawBefore);
        }
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (FAILED(hr)) return hr;
        std::wstring shown = action.text;
        if (firstMark && userSettings_.recheckPreviousWord && candidateEngine_ && !action.text.empty()) {
            const std::wstring left = action.text.substr(0, action.text.size() - 1);
            const std::wstring mark = action.text.substr(action.text.size() - 1);
            // The word before this one, as Space does, now this one is done.
            if (!keptBefore.empty()) RecheckPreviousWord(context, editCookie, keptBefore, typedBefore, left);
            // This word, kept as typed, when the mark ends the sentence: it
            // has only the words before it to go by.
            if (left == rawBefore && (mark == L"." || mark == L"!" || mark == L"?")) {
                if (const auto meant = candidateEngine_->Recheck(left, CommitContext(context, editCookie), mark)) {
                    shown = *meant + mark;
                    hr = ReplaceComposition(context, editCookie, shown);
                    if (FAILED(hr)) return hr;
                }
            }
        }
        hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
        candidateWindow_.Hide();
        rawText_.clear();
        // A word a period or colon put right, until the next key.
        if ((input.character == L'.' || input.character == L':') && shown != rawBefore + input.character) {
            markFixShown_ = shown;
            markFixTyped_ = rawBefore + input.character;
        }
        return S_OK;
    }

    case KeyInput::Type::Cancel: {
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const auto action = state_.OnCancel();
        if (action.kind != ActionKind::ReplaceComposition &&
            action.kind != ActionKind::RestoreOriginal) return S_FALSE;
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (FAILED(hr)) return hr;
        if (action.kind == ActionKind::RestoreOriginal) {
            if (learning && !candidateBefore.empty()) {
                userLearning_.RecordExposure(rawText_, candidateBefore);
                userLearning_.RecordUndo(rawText_, candidateBefore);
            }
            rawText_ = action.text;
            ShowCandidates(context, editCookie);
            return S_OK;
        }
        hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
        candidateWindow_.Hide();
        rawText_.clear();
        return S_OK;
    }

    case KeyInput::Type::Enter: {
        if (!state_.IsActive()) {
            if (!userSettings_.periodOnEnter) return S_FALSE;
            const auto preceding = ReadSelectionContext(context, editCookie);
            if (!NeedsTerminalPeriod(preceding)) return S_FALSE;
            std::size_t trailingWhitespace = 0;
            while (trailingWhitespace < preceding.size()) {
                const auto ch = preceding[preceding.size() - trailingWhitespace - 1];
                if (ch != L' ' && ch != L'\t') break;
                ++trailingWhitespace;
            }
            return trailingWhitespace == 0
                       ? InsertAtSelection(context, editCookie, L".")
                       : ReplaceTrailingWhitespaceAtSelection(context, editCookie,
                                                              trailingWhitespace, L".");
        }
        const auto rawBefore = rawText_;
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const bool explicitSelection = state_.IsCandidateNavigationActive();
        if (selectedBefore < state_.Candidates().size()) {
            recordLearningDecision(rawBefore, state_.Candidates(), selectedBefore, explicitSelection);
        }
        const auto action = state_.OnEnter(userSettings_.periodOnEnter, userSettings_.enterCommitsAsShown);
        if (action.kind != ActionKind::CommitBeforeNewline &&
            action.kind != ActionKind::EndComposition) return S_FALSE;
        if (!candidateBefore.empty() && candidateBefore != rawBefore) {
            if (learning) userLearning_.RecordCandidateSelection(candidateBefore);
        } else {
            if (learning) userLearning_.RecordRawKeep(rawBefore);
        }
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (FAILED(hr)) return hr;
        hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
        candidateWindow_.Hide();
        rawText_.clear();
        return S_OK;
    }

    case KeyInput::Type::EndComposition: {
        HRESULT hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        return S_OK;
    }
    }

    return E_UNEXPECTED;
}

HRESULT TextService::PutEnglishPeriod(ITfContext* context, TfEditCookie editCookie) {
    const auto preceding = ReadSelectionContext(context, editCookie);
    japanese_.ForgetContext();
    japanese_.SetSentenceStart(true);
    if (!NeedsTerminalPeriod(preceding)) return S_OK;
    // Mix leaves no space after the sentence; a space typed after it goes.
    if (!preceding.ends_with(L' ')) return InsertAtSelection(context, editCookie, L".");
    return ReplaceTrailingWhitespaceAtSelection(context, editCookie, 1, L".");
}

std::wstring TextService::CommitContext(ITfContext* context, TfEditCookie editCookie) const {
    if (!composition_) return ReadSelectionContext(context, editCookie);
    std::wstring preceding;
    std::wstring following;
    ReadCompositionContext(composition_, editCookie, preceding, following);
    return preceding;
}

void TextService::RecheckPreviousWord(ITfContext* context, TfEditCookie editCookie, const std::wstring& word,
                                      const std::wstring& typed, const std::wstring& following) {
    if (!candidateEngine_ || !composition_) return;
    // The text before the composition ends with the word and its space.
    const std::wstring before = CommitContext(context, editCookie);
    const std::wstring ending = word + L" ";
    if (!before.ends_with(ending)) return;
    const std::wstring preceding = before.substr(0, before.size() - ending.size());
    const auto meant = typed.empty() || typed == word ? candidateEngine_->Recheck(word, preceding, following)
                                                      : candidateEngine_->Revise(typed, word, preceding, following);
    if (!meant) return;
    ComPtr<ITfRange> range;
    ComPtr<ITfRange> previous;
    if (FAILED(composition_->GetRange(range.Put())) || FAILED(range->Clone(previous.Put()))) return;
    LONG moved = 0;
    const LONG length = static_cast<LONG>(ending.size());
    if (FAILED(previous->Collapse(editCookie, TF_ANCHOR_START)) ||
        FAILED(previous->ShiftStart(editCookie, -length, &moved, nullptr)) || moved != -length ||
        ReadRangeText(previous.Get(), editCookie) != ending) {
        return;
    }
    const std::wstring text = *meant + L" ";
    (void)previous->SetText(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()));
}

void TextService::ReplaceBeforeCaret(ITfContext* context, TfEditCookie editCookie, const std::wstring& shown,
                                     const std::wstring& typed) {
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) || fetched != 1 ||
        !selection.range) {
        return;
    }
    ComPtr<ITfRange> range;
    range.Attach(selection.range);
    LONG moved = 0;
    const LONG length = static_cast<LONG>(shown.size());
    if (FAILED(range->Collapse(editCookie, TF_ANCHOR_START)) ||
        FAILED(range->ShiftStart(editCookie, -length, &moved, nullptr)) || moved != -length ||
        ReadRangeText(range.Get(), editCookie) != shown) {
        return;
    }
    (void)range->SetText(editCookie, 0, typed.c_str(), static_cast<LONG>(typed.size()));
}

void TextService::TakeBackSpaceBefore(ITfContext* context, TfEditCookie editCookie) {
    ComPtr<ITfRange> space;
    if (composition_) {
        ComPtr<ITfRange> range;
        if (FAILED(composition_->GetRange(range.Put())) || FAILED(range->Clone(space.Put()))) return;
    } else {
        TF_SELECTION selection{};
        ULONG fetched = 0;
        if (FAILED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) ||
            fetched != 1 || !selection.range) {
            return;
        }
        space.Attach(selection.range);
    }
    LONG moved = 0;
    if (FAILED(space->Collapse(editCookie, TF_ANCHOR_START)) ||
        FAILED(space->ShiftStart(editCookie, -1, &moved, nullptr)) || moved != -1 ||
        ReadRangeText(space.Get(), editCookie) != L" ") {
        return;
    }
    (void)space->SetText(editCookie, 0, L"", 0);
}

}  // namespace tekito::tsf
