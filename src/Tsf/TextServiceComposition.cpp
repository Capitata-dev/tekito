#include "Tsf/TextService.h"
#include "Tsf/TextServiceShared.h"

// The composition in the document: starting, replacing and ending it, its
// underline, and inserting text.

namespace tekito::tsf {

HRESULT TextService::EnsureComposition(ITfContext* context, TfEditCookie editCookie) {
    if (composition_) return S_OK;

    ComPtr<ITfInsertAtSelection> insertAtSelection;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(insertAtSelection.Put()));
    if (FAILED(hr)) {
        TraceHr(L"EnsureComposition QueryInterface ITfInsertAtSelection failed", hr);
        return hr;
    }

    ComPtr<ITfRange> range;
    hr = insertAtSelection->InsertTextAtSelection(editCookie, TF_IAS_QUERYONLY,
                                                  nullptr, 0, range.Put());
    if (FAILED(hr)) {
        TraceHr(L"EnsureComposition InsertTextAtSelection query failed", hr);
        return hr;
    }

    ComPtr<ITfContextComposition> compositionContext;
    hr = context->QueryInterface(IID_PPV_ARGS(compositionContext.Put()));
    if (FAILED(hr)) {
        TraceHr(L"EnsureComposition QueryInterface ITfContextComposition failed", hr);
        return hr;
    }

    hr = compositionContext->StartComposition(editCookie, range.Get(), this, &composition_);
    if (FAILED(hr)) {
        TraceHr(L"EnsureComposition StartComposition failed", hr);
        return hr;
    }

    compositionContext_ = context;
    compositionContext_->AddRef();
    AdviseContextSinks(context);
    return S_OK;
}

HRESULT TextService::ReplaceComposition(ITfContext* context, TfEditCookie editCookie,
                                        const std::wstring& written) {
    if (!composition_) return E_UNEXPECTED;
    std::wstring text = written;
    if (userSettings_.smartPunctuation && (!TypesJapanese(mode_) || englishSegment_)) {
        std::replace(text.begin(), text.end(), L'\'', L'\u2019');
    }
    ComPtr<ITfRange> range;
    HRESULT hr = composition_->GetRange(range.Put());
    if (FAILED(hr)) {
        TraceHr(L"ReplaceComposition GetRange failed", hr);
        return hr;
    }
    hr = range->SetText(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()));
    if (FAILED(hr)) {
        TraceHr(L"ReplaceComposition SetText failed", hr);
        return hr;
    }
    compositionText_ = text;

    if (!text.empty()) {
        hr = ApplyDisplayAttribute(context, editCookie, range.Get(),
                                   mode_ == InputMode::Convert && userSettings_.modeUnderlineColors
                                       ? JapaneseUnderlineGuid(JapaneseUnderline::AutoInput)
                                       : GUID_TekitoDisplayAttribute);
        if (FAILED(hr)) {
            TraceHr(L"ReplaceComposition ApplyDisplayAttribute failed", hr);
            return hr;
        }
    }

    hr = range->Collapse(editCookie, TF_ANCHOR_END);
    if (FAILED(hr)) {
        TraceHr(L"ReplaceComposition Collapse failed", hr);
        return hr;
    }

    TF_SELECTION selection{};
    selection.range = range.Get();
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    return context->SetSelection(editCookie, 1, &selection);
}

HRESULT TextService::ApplyDisplayAttribute(ITfContext* context, TfEditCookie editCookie,
                                           ITfRange* range, REFGUID attribute) {
    std::size_t slot = 0;
    for (int i = 0; i < kJapaneseUnderlineCount; ++i) {
        if (attribute == JapaneseUnderlineGuid(static_cast<JapaneseUnderline>(i))) slot = i + 1;
    }
    TfGuidAtom& atom = displayAttributeAtoms_[slot];
    if (atom == TF_INVALID_GUIDATOM) {
        ComPtr<ITfCategoryMgr> categoryManager;
        HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(categoryManager.Put()));
        if (FAILED(hr)) {
            TraceHr(L"ApplyDisplayAttribute CoCreateInstance category manager failed", hr);
            return hr;
        }
        hr = categoryManager->RegisterGUID(attribute, &atom);
        if (FAILED(hr)) {
            TraceHr(L"ApplyDisplayAttribute RegisterGUID failed", hr);
            return hr;
        }
    }

    ComPtr<ITfProperty> property;
    HRESULT hr = context->GetProperty(GUID_PROP_ATTRIBUTE, property.Put());
    if (FAILED(hr)) {
        TraceHr(L"ApplyDisplayAttribute GetProperty attribute failed", hr);
        return hr;
    }

    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = static_cast<LONG>(atom);
    return property->SetValue(editCookie, range, &value);
}

void TextService::ClearCompositionUnderline(TfEditCookie editCookie) {
    if (!composition_) return;
    ComPtr<ITfRange> range;
    ComPtr<ITfContext> context;
    ComPtr<ITfProperty> attribute;
    if (SUCCEEDED(composition_->GetRange(range.Put())) && SUCCEEDED(range->GetContext(context.Put())) &&
        SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, attribute.Put()))) {
        (void)attribute->Clear(editCookie, range.Get());
    }
}

HRESULT TextService::EndComposition(TfEditCookie editCookie) {
    symbolComposition_ = false;
    if (!composition_) return S_OK;
    NoteTyped(compositionText_);
    compositionText_.clear();
    ITfComposition* composition = composition_;
    composition->AddRef();
    // The underline goes with the composition: some apps (WordPad, Word)
    // keep a display attribute left on committed text.
    ClearCompositionUnderline(editCookie);
    HRESULT hr = composition->EndComposition(editCookie);
    if (composition_ == composition) {
        composition_->Release();
        composition_ = nullptr;
        if (compositionContext_) {
            compositionContext_->Release();
            compositionContext_ = nullptr;
        }
    }
    composition->Release();
    return hr;
}

HRESULT TextService::InsertPairAtSelection(ITfContext* context, TfEditCookie editCookie, const std::wstring& open,
                                           const std::wstring& close) {
    ComPtr<ITfInsertAtSelection> insertAtSelection;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(insertAtSelection.Put()));
    if (FAILED(hr)) return hr;
    const std::wstring text = open + close;
    ComPtr<ITfRange> inserted;
    hr = insertAtSelection->InsertTextAtSelection(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()),
                                                  inserted.Put());
    if (FAILED(hr) || !inserted) return hr;
    // The caret after the opening bracket.
    LONG moved = 0;
    if (FAILED(inserted->Collapse(editCookie, TF_ANCHOR_START)) ||
        FAILED(inserted->ShiftStart(editCookie, static_cast<LONG>(open.size()), &moved, nullptr)) ||
        FAILED(inserted->Collapse(editCookie, TF_ANCHOR_START))) {
        return S_OK;
    }
    TF_SELECTION selection{};
    selection.range = inserted.Get();
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    (void)context->SetSelection(editCookie, 1, &selection);
    // An application TSF only bridges to (CUAS: an older edit control) puts
    // the caret after the committed text whatever the selection: Left takes
    // it back between the brackets.
    TF_STATUS status{};
    if (SUCCEEDED(context->GetStatus(&status)) && (status.dwStaticFlags & TF_SS_TRANSITORY) != 0) {
        std::vector<INPUT> keys;
        for (std::size_t i = 0; i < close.size(); ++i) {
            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wVk = VK_LEFT;
            INPUT up = down;
            up.ki.dwFlags = KEYEVENTF_KEYUP;
            keys.push_back(down);
            keys.push_back(up);
        }
        // The only keys TEKITO sends (scripts/verify.ps1 allows them by this mark): verify-allow
        (void)SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
    }
    return S_OK;
}

HRESULT TextService::InsertAtSelection(ITfContext* context, TfEditCookie editCookie,
                                       const std::wstring& text) {
    ComPtr<ITfInsertAtSelection> insertAtSelection;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(insertAtSelection.Put()));
    if (FAILED(hr)) return hr;

    ComPtr<ITfRange> insertedRange;
    hr = insertAtSelection->InsertTextAtSelection(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()),
                                                  insertedRange.Put());
    if (FAILED(hr) || !insertedRange) return hr;
    // Some applications leave the caret before the inserted text; typing on
    // must go after it (a space, then the next word).
    if (FAILED(insertedRange->Collapse(editCookie, TF_ANCHOR_END))) return S_OK;
    TF_SELECTION selection{};
    selection.range = insertedRange.Get();
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    (void)context->SetSelection(editCookie, 1, &selection);
    return S_OK;
}

}  // namespace tekito::tsf
