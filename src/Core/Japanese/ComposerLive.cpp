#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// Live conversion: the text converted as it is typed, kept steady, and
// converted again after a pause.

namespace tekito::japanese {

void JapaneseComposer::SetLiveConversion(bool on) {
    if (on == live_) return;
    live_ = on;
    if (preview_) phrases_.clear();
    ResetLive();
}

void JapaneseComposer::ResetLive() noexcept {
    preview_ = false;
    kanaView_ = false;
    settledPhrases_ = 0;
    settledLength_ = 0;
    liveBreaks_.clear();
    liveShown_.clear();
    liveShownBefore_.clear();
    liveShownReading_.clear();
    liveShownUnits_ = 0;
}

void JapaneseComposer::RememberLiveShown() {
    // Shown again for the same keys: what was shown before stays.
    if (units_.size() != liveShownUnits_ || conversionReading_ != liveShownReading_) liveShownBefore_ = std::move(liveShown_);
    liveShown_ = phrases_;
    liveShownReading_ = conversionReading_;
    liveShownUnits_ = units_.size();
}

void JapaneseComposer::KeepLiveSteady(const std::wstring& reading) {
    // Units typed since the conversion shown, at most, before the new one
    // is shown anyway.
    constexpr std::size_t kHeldUnits = 3;
    if (liveShown_.empty() || !reading.starts_with(liveShownReading_) || units_.size() <= liveShownUnits_) {
        RememberLiveShown();
        return;
    }
    const std::size_t added = units_.size() - liveShownUnits_;
    // What must stay: the first phrases shown that are whole, a word and
    // its particle (今日は, 明日の, 食べてから), or were shown the same before too; the
    // last one only while one unit has come after it, and only if whole. A
    // word growing as it is typed (おつか: お疲れ, おく: 送る) changes
    // freely, and so does one that just came (置く|リマ, before 送ります).
    // Kana not converted never stays.
    const auto whole = [&](const PhraseState& phrase) {
        const std::wstring text = PhraseText(phrase);
        const auto kana = [](wchar_t c) { return c >= 0x3041 && c <= 0x3096; };
        return text.size() >= 2 && std::wstring_view(L"はがをにでとのもへやらど").find(text.back()) != std::wstring_view::npos &&
               !std::all_of(text.begin(), text.end(), kana);
    };
    const auto typed = [](const PhraseState& phrase) {
        return !phrase.form && phrase.selected < phrase.candidates.size() &&
               phrase.candidates[phrase.selected].kind == PhraseCandidate::Kind::Hiragana;
    };
    const auto shownBefore = [&](const PhraseState& phrase) {
        return std::any_of(liveShownBefore_.begin(), liveShownBefore_.end(), [&](const PhraseState& other) {
            return other.begin == phrase.begin && other.length == phrase.length && PhraseText(other) == PhraseText(phrase);
        });
    };
    std::size_t keep = 0;
    while (keep < liveShown_.size()) {
        const auto& phrase = liveShown_[keep];
        const bool last = keep + 1 == liveShown_.size();
        const bool stays = !typed(phrase) && (last ? added <= 1 && whole(phrase) : whole(phrase) || shownBefore(phrase));
        if (!stays) break;
        ++keep;
    }
    std::wstring kept;
    for (std::size_t k = 0; k < keep; ++k) kept += PhraseText(liveShown_[k]);
    std::wstring now;
    for (const auto& phrase : phrases_) now += PhraseText(phrase);
    if (keep == 0 || now.starts_with(kept) || added > kHeldUnits) {
        RememberLiveShown();
        return;
    }
    // The kept phrases, then the rest of the kana as typed.
    const std::size_t covered = liveShown_[keep - 1].begin + liveShown_[keep - 1].length;
    if (covered > reading.size()) {
        RememberLiveShown();
        return;
    }
    phrases_.assign(liveShown_.begin(), liveShown_.begin() + static_cast<std::ptrdiff_t>(keep));
    if (covered < reading.size()) {
        const std::wstring rest = reading.substr(covered);
        PhraseCandidate hiragana{rest, 0, PhraseCandidate::Kind::Hiragana, false};
        PhraseCandidate katakana{ToKatakana(rest), 0, PhraseCandidate::Kind::Katakana, false};
        phrases_.push_back({covered, rest.size(), {hiragana, katakana}});
    }
    settledPhrases_ = std::min(settledPhrases_, keep);
    settledLength_ = settledPhrases_ ? phrases_[settledPhrases_ - 1].begin + phrases_[settledPhrases_ - 1].length : 0;
}

void JapaneseComposer::SettleAll() {
    // A reading corrected for a slip, or the whole input read again, is not
    // what was typed: start over from the typed kana.
    if (conversionReading_ != Reading() || WholeChoice()) {
        phrases_.clear();
        ResetLive();
        return;
    }
    settledPhrases_ = phrases_.size();
    settledLength_ = conversionReading_.size();
    liveBreaks_.push_back(settledLength_);
    preview_ = true;
    listOpen_ = false;
}

void JapaneseComposer::StartPicking() {
    if (!preview_) return;
    // The keys still pending become kana first, as Space would.
    if (!pending_.empty()) {
        FlushAll();
        UpdateLive();
    }
    preview_ = false;
    predictions_.clear();
    chosenPrediction_.reset();
    focus_ = phrases_.empty() ? 0 : phrases_.size() - 1;
    AddVariantForms();
    AddTypedForms();
}

void JapaneseComposer::UpdateLive() {
    refinedKeys_.clear();
    const std::wstring reading = Reading();  // the keys still pending stay as typed
    if (!Live() || kanaView_ || !converter_ || reading.empty()) {
        phrases_.clear();
        const bool kana = kanaView_;
        ResetLive();
        kanaView_ = kana;
        conversionReading_.clear();
        readingKeys_.clear();
        return;
    }
    // Phrases the reading no longer covers (Backspace) are no longer settled.
    while (settledPhrases_ > 0 &&
           phrases_[settledPhrases_ - 1].begin + phrases_[settledPhrases_ - 1].length > reading.size()) {
        --settledPhrases_;
    }
    settledLength_ = settledPhrases_ ? phrases_[settledPhrases_ - 1].begin + phrases_[settledPhrases_ - 1].length : 0;
    std::vector<PhraseState> settled(phrases_.begin(), phrases_.begin() + static_cast<std::ptrdiff_t>(settledPhrases_));

    conversionReading_ = reading;
    readingKeys_.assign(reading.size() + 1, std::wstring::npos);
    for (std::size_t offset = 0, key = 0, i = 0;; ++i) {
        if (offset < readingKeys_.size()) readingKeys_[offset] = key;
        if (i == units_.size()) break;
        offset += units_[i].kana.size();
        key += units_[i].keys.size();
    }

    // The rest goes on from the settled phrases, as from committed text.
    std::uint16_t context = context_;
    std::vector<std::wstring> words = contextWords_;
    if (!settled.empty()) {
        std::wstring text;
        for (const auto& phrase : settled) text += PhraseText(phrase);
        const auto& last = settled.back();
        const bool chosen = !last.form && last.selected < last.candidates.size();
        context = ContextAfter(text, chosen ? last.candidates[last.selected].rightId : 0);
        AddContentWords(text, words);
    }
    phrases_ = settled;
    // An address or URL typed from the start, or after where typing went on
    // after picking, stays as typed (the keys still pending after it too).
    std::erase_if(liveBreaks_, [&](std::size_t at) { return at > reading.size(); });
    if (const std::size_t from = liveBreaks_.empty() ? 0 : liveBreaks_.back();
        from < reading.size() && readingKeys_[from] != std::wstring::npos && !HasAscii()) {
        std::wstring keys = Keys(false).substr(readingKeys_[from]);
        if (IsAddressTyping(keys + pending_)) {
            while (!phrases_.empty() && phrases_.back().begin + phrases_.back().length > from) phrases_.pop_back();
            const bool atBoundary = phrases_.empty() ? from == 0 : phrases_.back().begin + phrases_.back().length == from;
            if (atBoundary) {
                PhraseState address{from, reading.size() - from, {{std::move(keys), 0, PhraseCandidate::Kind::English, false}}};
                address.address = true;
                settledPhrases_ = phrases_.size();
                settledLength_ = from;
                phrases_.push_back(std::move(address));
                preview_ = true;
                focus_ = phrases_.size() - 1;
                listOpen_ = false;
                return;
            }
            phrases_.assign(settled.begin(), settled.end());
        }
    }
    const std::wstring rest = reading.substr(settledLength_);
    std::vector<Phrase> converted;
    if (settledLength_ == 0) {
        if (auto sum = SumCandidates(reading); !sum.empty()) {
            converted.push_back({0, reading.size(), std::move(sum)});
        } else if (auto postal = PostalCandidates(reading); !postal.empty()) {
            converted.push_back({0, reading.size(), std::move(postal)});
        }
    }
    if (converted.empty() && !rest.empty()) converted = ConvertReading(reading, settledLength_, context, words);
    const std::size_t before = phrases_.size();
    AddPhrases(std::move(converted), reading);
    if (phrases_.size() == before && !rest.empty()) {
        // Nothing reads it: shown as typed.
        PhraseCandidate hiragana{rest, 0, PhraseCandidate::Kind::Hiragana, false};
        PhraseCandidate katakana{ToKatakana(rest), 0, PhraseCandidate::Kind::Katakana, false};
        phrases_.push_back({settledLength_, rest.size(), {hiragana, katakana}});
    }
    // Digits typed stay digits as Space writes them (11がつ: 11月, not 十一月),
    // that one first: nothing is picked yet, and Enter converts it again.
    if (phrases_.size() > before) {
        ApplyNumberStyle(before);
        for (std::size_t i = before; i < phrases_.size(); ++i) {
            auto& phrase = phrases_[i];
            if (phrase.form || phrase.selected == 0 || phrase.selected >= phrase.candidates.size()) continue;
            std::rotate(phrase.candidates.begin(), phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected),
                        phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected) + 1);
            phrase.selected = 0;
        }
    }
    bool checked = false;
    if (phrases_.size() > settledPhrases_ + liveOpenPhrases_) {
        // Before phrases stay as they show, the sentence so far is checked
        // as a pause checks it, so they stay as it reads them (昨日NHKの
        // ニュースを見た, not 機能NHKの), Enter committing them as shown.
        checked = CheckBeforeSettling();
        if (phrases_.size() > liveOpenPhrases_) {
            settledPhrases_ = phrases_.size() - liveOpenPhrases_;
            settledLength_ = phrases_[settledPhrases_ - 1].begin + phrases_[settledPhrases_ - 1].length;
        }
    }
    for (auto& phrase : phrases_) KeepKanjiInRange(phrase);
    // What the check changed shows, not what showed before it.
    if (checked) {
        RememberLiveShown();
    } else {
        KeepLiveSteady(reading);
    }
    preview_ = true;
    focus_ = phrases_.size() - 1;
    listOpen_ = false;
}

bool JapaneseComposer::CheckBeforeSettling() {
    if (!liveRefine_ || !reranker_ || !sentenceCheck_ || chunkJudge_ || reconverted_ || phrases_.empty()) return false;
    const std::wstring reading = conversionReading_;
    // The check's choices, each its phrase's first: nothing was picked.
    auto kept = phrases_;
    CheckSentence(true);
    // A reading of the whole input another way: as it was. Other breaks
    // of the same reading stay (昨日|NHKの, where it read 機能|NHK|の).
    const bool covers = !phrases_.empty() && phrases_.back().begin + phrases_.back().length == reading.size();
    if (conversionReading_ != reading || !covers) {
        phrases_ = std::move(kept);
        conversionReading_ = reading;
        return false;
    }
    for (auto& phrase : phrases_) {
        if (phrase.form || phrase.selected == 0 || phrase.selected >= phrase.candidates.size()) continue;
        std::rotate(phrase.candidates.begin(), phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected),
                    phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected) + 1);
        phrase.selected = 0;
    }
    // Only the phrases about to stay take what the check read; the ones
    // still being typed on show as converted (the words after them are
    // not there yet: 図書館, not 図書感), where they break the same.
    const std::size_t staying = phrases_.size() > liveOpenPhrases_ ? phrases_.size() - liveOpenPhrases_ : 0;
    for (std::size_t i = staying; i < phrases_.size(); ++i) {
        const auto same = std::find_if(kept.begin(), kept.end(), [&](const PhraseState& k) {
            return k.begin == phrases_[i].begin && k.length == phrases_[i].length;
        });
        if (same != kept.end()) phrases_[i] = *same;
    }
    // Whether it reads the text otherwise than shown.
    if (phrases_.size() != kept.size()) return true;
    for (std::size_t i = 0; i < phrases_.size(); ++i) {
        if (phrases_[i].begin != kept[i].begin || PhraseText(phrases_[i]) != PhraseText(kept[i])) return true;
    }
    return false;
}

bool JapaneseComposer::RefineLive() {
    if (!liveRefine_ || !pending_.empty()) return false;
    const std::wstring before = Preedit();
    // Mix's live conversion: all of it converted again, as one, once
    // phrases stayed while typing on (MixLiveText).
    if (chunkJudge_ && mixLive_ && !IsConverted()) {
        if (mixLivePhrases_.size() <= liveOpenPhrases_) return false;
        mixLiveReading_.clear();
        mixLivePhrases_.clear();
        return Preedit() != before;
    }
    return RebuildLive() && Preedit() != before;
}

bool JapaneseComposer::RebuildLive(bool again) {
    if (!Live() || !preview_ || kanaView_ || !pending_.empty() || units_.empty() || (LiveRefined() && !again)) return false;
    // A candidate picked while typing stays, as on Enter, and an address
    // typed after other text.
    if (!again && std::any_of(phrases_.begin(), phrases_.end(), [](const PhraseState& phrase) {
            return phrase.selected != 0 || phrase.form || phrase.address;
        })) {
        return false;
    }
    // A prediction picked with Tab stays as it is shown.
    if (chosenPrediction_) return false;
    // The predictions stay offered: Tab picks one after the pause too.
    auto predictions = std::move(predictions_);
    ResetLive();
    BuildPhrases(true);
    predictions_ = std::move(predictions);
    preview_ = true;
    focus_ = phrases_.empty() ? 0 : phrases_.size() - 1;
    listOpen_ = false;
    refinedKeys_ = Keys(false);
    RememberLiveShown();
    return true;
}

bool JapaneseComposer::LiveRefined() const {
    return preview_ && !refinedKeys_.empty() && pending_.empty() && refinedKeys_ == Keys(false);
}

void JapaneseComposer::LeaveLivePreview() {
    if (!IsLivePreview()) return;
    phrases_.clear();
    listOpen_ = false;
    ResetLive();
}

void JapaneseComposer::ShowLiveAtEnd() {
    if (!Live()) return;
    // The kana while the caret is inside the text; converted again once it
    // is back at the end.
    kanaView_ = caret_ != units_.size();
    UpdateLive();
}

}  // namespace tekito::japanese
