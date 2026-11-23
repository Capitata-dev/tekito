#include "Tsf/TextService.h"
#include "Tsf/TextServiceShared.h"

// The candidate list and the mode badge: what they show and where, and a
// click on a candidate.

namespace tekito::tsf {

void TextService::OnCandidateSelected(std::size_t index) {
    if (!compositionContext_ || (!state_.IsActive() && !japanese_.IsConverted())) return;
    KeyInput input{};
    input.type = KeyInput::Type::CandidateSelection;
    input.candidateIndex = index;
    [[maybe_unused]] const auto ignored = RequestKeyEditSession(compositionContext_, input);
}

void TextService::RefreshCandidates(ITfContext* context, TfEditCookie editCookie) {
    ScopedTraceDuration totalDuration(L"Perf RefreshCandidates");
    if (!candidateEngine_) {
        state_.Reset();
        candidateWindow_.Hide();
        return;
    }
    Context conversionContext;
    ReadCompositionContext(composition_, editCookie, conversionContext.precedingText,
                            conversionContext.followingText);
    ConversionRequest request;
    request.rawText = rawText_;
    request.context = std::move(conversionContext);
    request.context.sentenceStart = sentenceStart_;
    request.capitalization.origin = capitalizationOrigin_;
    request.options.correctionEnabled = userSettings_.correctionEnabled;
    request.options.capitalizedCorrection = userSettings_.capitalizedCorrection;
    request.options.commonMisspellingsEnabled = userSettings_.commonMisspellingsEnabled;
    request.options.contextSuggestionsEnabled = userSettings_.contextSuggestionsEnabled;
    request.options.completionEnabled = userSettings_.completionEnabled;
    request.options.japanesePhoneticSuggestionsEnabled =
        userSettings_.japanesePhoneticSuggestionsEnabled;
    request.options.socialExpressionRange = userSettings_.socialExpressionRange;
    request.options.nextWordPrediction = userSettings_.nextWordPrediction;
    request.options.doubledWords = userSettings_.doubledWordCheck;
    request.options.spacelessEnglish = userSettings_.spacelessEnglish;
    request.options.spacelessEnglishAlways = userSettings_.spacelessEnglishAlways;
    request.options.spelling = static_cast<EnglishVariety>(std::clamp(userSettings_.englishSpelling, 0, 2));
    request.options.special = SpecialOptions();
    {
        ScopedTraceDuration duration(L"Perf CandidateEngine");
        auto result = candidateEngine_->Convert(request);
        state_.BeginOrUpdate(rawText_, std::move(result.candidates));
    }
    ScopedTraceDuration duration(L"Perf CandidateWindow");
    ShowCandidates(context, editCookie);
}

void TextService::ShowCandidates(ITfContext* context, TfEditCookie editCookie) {
    if (!userSettings_.candidateWindowEnabled) {
        candidateWindow_.Hide();
        return;
    }
    const RECT anchor = GetCandidateAnchor(context, editCookie);
    candidateAnchor_ = anchor;
    candidateWindow_.SetStyle(userSettings_.candidateWindowStyle);
    candidateWindow_.SetZoom(userSettings_.candidateWindowScale);
    candidateWindow_.SetRainbow(userSettings_.rainbowAccent);
    candidateWindow_.SetJapanese(userdata::UseJapaneseUi(userSettings_.uiLanguage));
    candidateWindow_.Show(anchor, EnglishRows(), state_.SelectedIndex(), state_.PageStart(), state_.VisibleCount(),
                          SelectedEnglishMeaning(), {}, 0, state_.PageIndex(), state_.PageCount());
    // Not where nothing is learned (secret mode, a private window, learning off).
    const bool secret = runtimeMode_ && runtimeMode_->Secret();
    const bool keepsNothing = !userSettings_.learningEnabled || secret ||
                              (userSettings_.noLearningInPrivate && privateField_);
    if (userSettings_.socialPersonalization > 0 && !keepsNothing) {
        const auto trigger = rawText_.starts_with(L':') ? std::wstring_view(rawText_).substr(1)
                                                        : std::wstring_view(rawText_);
        for (const auto& candidate : state_.Candidates()) {
            if (candidate.socialRange != SocialRangeUnspecified) {
                socialLearning_.RecordExposure(trigger, candidate.text);
            }
        }
    }
}

RECT TextService::CaretAnchor(ITfContext* context, TfEditCookie editCookie) const {
    if (context && !composition_) {
        TF_SELECTION selection{};
        ULONG fetched = 0;
        if (SUCCEEDED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) &&
            fetched == 1 && selection.range) {
            ComPtr<ITfContextView> view;
            RECT rect{};
            BOOL clipped = FALSE;
            const bool found = SUCCEEDED(context->GetActiveView(view.Put())) &&
                               SUCCEEDED(view->GetTextExt(editCookie, selection.range, &rect, &clipped)) &&
                               rect.bottom > rect.top;
            selection.range->Release();
            if (found) return rect;
        }
    }
    return GetCandidateAnchor(composition_ ? context : nullptr, editCookie);
}

void TextService::ShowModeIndicator(ITfContext* context) {
    if (!userSettings_.modeIndicatorEnabled || ProcessPassesThrough()) return;
    ComPtr<ITfContext> focused;
    if (!context && threadManager_) {
        ComPtr<ITfDocumentMgr> document;
        if (SUCCEEDED(threadManager_->GetFocus(document.Put())) && document) {
            (void)document->GetTop(focused.Put());
        }
        context = focused.Get();
    }
    if (context) {
        KeyInput input{};
        input.type = KeyInput::Type::ShowModeIndicator;
        if (auto* session = new (std::nothrow) KeyEditSession(this, context, input)) {
            HRESULT sessionResult = E_FAIL;
            const HRESULT hr = context->RequestEditSession(clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READ,
                                                           &sessionResult);
            session->Release();
            if (SUCCEEDED(hr)) return;
        }
    }
    // No document to ask: the system caret, or the pointer.
    ShowModeBadge(GetCandidateAnchor(nullptr, 0));
}

void TextService::ShowModeBadge(const RECT& caret) {
    candidateWindow_.SetStyle(userSettings_.candidateWindowStyle);
    candidateWindow_.SetZoom(userSettings_.candidateWindowScale);
    candidateWindow_.SetRainbow(userSettings_.rainbowAccent);
    candidateWindow_.ShowModeBadge(caret, mode_);
}

RECT TextService::GetCandidateAnchor(ITfContext* context, TfEditCookie editCookie) const {
    RECT rect{};
    BOOL clipped = FALSE;
    if (context && composition_) {
        ComPtr<ITfContextView> view;
        ComPtr<ITfRange> range;
        if (SUCCEEDED(context->GetActiveView(view.Put())) &&
            SUCCEEDED(composition_->GetRange(range.Put())) &&
            SUCCEEDED(view->GetTextExt(editCookie, range.Get(), &rect, &clipped)) &&
            (rect.right > rect.left || rect.bottom > rect.top)) {
            return rect;
        }
    }

    // Some hosts fail GetTextExt or report an empty rect. The system caret is
    // a far better anchor than the mouse pointer, which can be anywhere.
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(GetForegroundWindow(), nullptr), &info) &&
        info.hwndCaret &&
        (info.rcCaret.right > info.rcCaret.left || info.rcCaret.bottom > info.rcCaret.top)) {
        POINT topLeft{info.rcCaret.left, info.rcCaret.top};
        POINT bottomRight{info.rcCaret.right, info.rcCaret.bottom};
        ClientToScreen(info.hwndCaret, &topLeft);
        ClientToScreen(info.hwndCaret, &bottomRight);
        return {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    }

    POINT point{};
    GetCursorPos(&point);
    rect.left = rect.right = point.x;
    rect.top = rect.bottom = point.y;
    return rect;
}

}  // namespace tekito::tsf
