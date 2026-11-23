#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// The composer itself: settings, committing, the context after what was
// committed, and what the composition shows. The rest is in the Composer*.cpp
// files by module.

namespace tekito::japanese {

void JapaneseComposer::SetTable(const RomajiTable* table) noexcept {
    table_ = table;
    ClearChunk();
}

void JapaneseComposer::SetInputForm(KanaForm form) noexcept {
    inputForm_ = form == KanaForm::Katakana ? KanaForm::Katakana : KanaForm::Hiragana;
}

std::wstring JapaneseComposer::FormText(KanaForm form, std::size_t begin, std::size_t length,
                                        int letterCase) const {
    std::wstring reading = conversionReading_.substr(begin, length);
    if (table_ && HasAscii() && form != KanaForm::FullWidthAlphanumeric &&
        form != KanaForm::HalfWidthAlphanumeric) {
        // Letters typed after a capital: F6-F8 read the keys as romaji.
        std::wstring keys = KeysFor(begin, length);
        for (auto& ch : keys) ch = static_cast<wchar_t>(std::towlower(ch));
        std::wstring kana;
        for (const auto& token : ParseRomaji(*table_, keys)) kana += token.kana;
        reading = ApplyTypingStyle(ToFullWidthAscii(kana));
    }
    switch (form) {
    case KanaForm::Hiragana: return ToHiragana(reading);
    case KanaForm::Katakana: return ToKatakana(reading);
    case KanaForm::HalfWidthKatakana: return ToHalfWidthKatakana(reading);
    case KanaForm::FullWidthAlphanumeric:
        return ToFullWidthAscii(ChangeCase(KeysFor(begin, length), letterCase));
    case KanaForm::HalfWidthAlphanumeric:
        return ChangeCase(KeysFor(begin, length), letterCase);
    }
    return reading;
}

std::wstring JapaneseComposer::PhraseText(const PhraseState& phrase) const {
    if (phrase.form) return FormText(*phrase.form, phrase.begin, phrase.length, phrase.letterCase);
    if (phrase.selected < phrase.candidates.size()) return phrase.candidates[phrase.selected].text;
    return conversionReading_.substr(phrase.begin, phrase.length);
}

std::wstring JapaneseComposer::Preedit() const {
    if (!IsConverted()) return AddressShown().value_or(RenderTyping(true));
    if (const auto* whole = WholeChoice()) return whole->text;
    std::wstring text;
    for (const auto& phrase : phrases_) text += PhraseText(phrase);
    if (preview_) text += PendingText();
    return text;
}

std::vector<PreeditSegment> JapaneseComposer::Segments() const {
    std::vector<PreeditSegment> segments;
    if (!settled_.empty()) {
        // One segment for each run of English or Japanese chunks, joined as
        // SettledText joins them (English words apart).
        bool englishBefore = false;
        for (const auto& chunk : settled_) {
            const bool english = chunk.marksOnly ? englishBefore : chunk.isEnglish;
            // A word taken out (the the) shows nothing.
            if (english && !chunk.marksOnly && chunk.english.empty()) continue;
            std::wstring text;
            if (!segments.empty() && englishBefore && english && !chunk.marksOnly) text.push_back(L' ');
            text += english ? chunk.english : chunk.japanese;
            if (!segments.empty() && segments.back().english == english) {
                segments.back().text += text;
            } else {
                segments.push_back({std::move(text), true, false, true, english});
            }
            englishBefore = english;
        }
        // English settled last keeps its space before what is typed next,
        // unless that is Japanese.
        bool englishLast = false;
        for (const auto& chunk : settled_) {
            if (!chunk.marksOnly) englishLast = chunk.isEnglish;
        }
        // Japanese next: converted, or typed and shown as kana (このfileを as
        // it will be committed; while only a key or letters show, the space
        // stays, so that they do not read as part of the word).
        const bool kanaTyped = !IsConverted() && !units_.empty() && !TypedLetters();
        const bool japaneseNext = (IsConverted() && !ChoseEnglish()) || kanaTyped;
        auto& last = segments.back().text;
        if (englishLast && Typing() && !japaneseNext && !last.empty() && last.back() < 0x3000) last.push_back(L' ');
    }
    if (!IsConverted()) {
        if (const auto address = AddressShown()) {
            segments.push_back({*address, false, false});
        } else if (const auto live = MixLiveText()) {
            if (ConversionWaits()) {
                // The conversion waits: the kana, marked when it will change.
                const std::wstring kana = DisplayTyping();
                segments.push_back({kana, false, false, false, false, pendingMarker_ && *live != RenderTyping(true)});
            } else {
                segments.push_back({*live, true, false});
            }
        } else if (Typing()) {
            // Not taken as anything yet, even shown as letters.
            segments.push_back({LettersShown().value_or(DisplayTyping()), false, false});
        }
        return segments;
    }
    // Live conversion waiting to show: each phrase as its kana, marked when
    // it will change, then the keys still pending.
    if (preview_ && !WholeChoice() && ConversionWaits()) {
        const bool katakana = TypingForm() == KanaForm::Katakana;
        for (const auto& phrase : phrases_) {
            if (phrase.begin + phrase.length > conversionReading_.size()) continue;
            std::wstring kana = conversionReading_.substr(phrase.begin, phrase.length);
            if (katakana) kana = ToKatakana(kana);
            const bool changes = PhraseText(phrase) != kana;
            segments.push_back({std::move(kana), false, false, false, false, pendingMarker_ && changes});
        }
        if (!pending_.empty()) segments.push_back({PendingText(), false, false});
        // The units still shown as their keys, at the end.
        if (const std::size_t fresh = FreshUnits(); fresh > 0) {
            std::size_t kana = 0;
            std::wstring keys;
            for (std::size_t i = units_.size() - fresh; i < units_.size(); ++i) {
                kana += units_[i].kana.size();
                keys += units_[i].keys;
            }
            const bool pendingLast = !pending_.empty();
            std::wstring tailPending = pendingLast ? segments.back().text : std::wstring{};
            if (pendingLast) segments.pop_back();
            while (kana > 0 && !segments.empty()) {
                auto& last = segments.back().text;
                const std::size_t take = std::min(kana, last.size());
                last.resize(last.size() - take);
                kana -= take;
                if (last.empty()) segments.pop_back();
            }
            segments.push_back({keys + tailPending, false, false});
        }
        return segments;
    }
    const auto english = [](const PhraseCandidate& candidate) { return candidate.kind == PhraseCandidate::Kind::English; };
    if (const auto* whole = WholeChoice()) {
        segments.push_back({whole->text, true, true, false, chunkJudge_ && english(*whole)});
    } else {
        for (std::size_t i = 0; i < phrases_.size(); ++i) {
            const auto& phrase = phrases_[i];
            const bool isEnglish = chunkJudge_ && !phrase.form && phrase.selected < phrase.candidates.size() &&
                                   english(phrase.candidates[phrase.selected]);
            segments.push_back({PhraseText(phrase), true, !preview_ && i == focus_, false, isEnglish});
        }
    }
    // Live: the keys still pending follow, as typed.
    if (preview_ && !pending_.empty()) segments.push_back({PendingText(), false, false});
    // Mix: English converted with Space shows the space it will be followed
    // by, so what is typed next is seen as a word of its own.
    if (chunkJudge_ && ChoseEnglish()) {
        const auto& last = segments.back().text;
        if (!last.empty() && last.back() < 0x3000 && last.back() != L' ') segments.push_back({L" ", false, false});
    }
    return segments;
}

const std::vector<PhraseCandidate>* JapaneseComposer::FocusedCandidates() const noexcept {
    if (!IsConverted()) return nullptr;
    return &phrases_[focus_].candidates;
}

std::size_t JapaneseComposer::FocusedSelection() const noexcept {
    return IsConverted() ? phrases_[focus_].selected : 0;
}

std::wstring JapaneseComposer::FocusedReading() const {
    if (!IsConverted()) return {};
    const auto& phrase = phrases_[focus_];
    if (phrase.begin >= conversionReading_.size()) return {};
    return conversionReading_.substr(phrase.begin, phrase.length);
}

std::vector<std::wstring> JapaneseComposer::PhraseKeys() const {
    std::vector<std::wstring> keys;
    const std::wstring typed = Keys(false);
    for (const auto& phrase : phrases_) {
        const std::size_t end = phrase.begin + phrase.length;
        if (end >= readingKeys_.size() || readingKeys_[phrase.begin] == std::wstring::npos ||
            readingKeys_[end] == std::wstring::npos || readingKeys_[end] < readingKeys_[phrase.begin]) {
            keys.emplace_back();
            continue;
        }
        keys.push_back(typed.substr(readingKeys_[phrase.begin], readingKeys_[end] - readingKeys_[phrase.begin]));
    }
    return keys;
}

std::wstring JapaneseComposer::Commit() {
    markLeft_ = false;
    if (!chunkJudge_) return CommitChunk();
    // Enter commits what is shown, and converts nothing: Japanese typed
    // without Space stays kana, unless live conversion shows it converted;
    // English is taken as Space takes it (CommitChunk); a prediction picked
    // with Tab as it is.
    if (Typing() && !IsConverted() && !PredictionChosen()) {
        if (const auto live = MixLiveText()) {
            // As shown (enterAsShown_): the live conversion as it shows.
            if (enterAsShown_) {
                committedAsShown_ = *live;
                committedAsShownJapanese_ = true;
                BuildPhrases(true);
            } else {
                Convert();
            }
        }
    }
    std::wstring text;
    if (settled_.empty()) {
        text = CommitChunk();
    } else {
        // Then the sentence.
        if (Typing()) SettleChunk();
        text = CommitSentence();
    }
    // No space is left after the text (Hello, nice to meet you.); English
    // committed next takes one before it, Japanese none.
    const bool owed = spaceOwed_;
    spaceOwed_ = false;
    while (!text.empty() && text.back() == L' ') {
        text.pop_back();
        spaceOwed_ = true;
    }
    const wchar_t first = text.empty() ? L'\0' : text.front();
    const bool english = (first >= L'a' && first <= L'z') || (first >= L'A' && first <= L'Z') ||
                         (first >= L'0' && first <= L'9');
    if (owed && english) text.insert(0, 1, L' ');
    dropSpaceBefore_ = false;
    return text;
}

std::wstring JapaneseComposer::CommitChunk() {
    FlushAll();
    // Live: Enter converts the whole text again as Space does (slips undone,
    // the whole input read at once), unless a candidate was picked while
    // typing; then only the keys that were pending are converted too.
    if (Live() && preview_ && !LiveRefined()) {
        const bool picked = std::any_of(phrases_.begin(), phrases_.end(), [](const PhraseState& phrase) {
            return phrase.selected != 0 || phrase.form || phrase.address;
        });
        // As shown (enterAsShown_): only the keys still pending are read too.
        if (picked || enterAsShown_) {
            UpdateLive();
        } else {
            ResetLive();
            BuildPhrases(true);
        }
    }
    // A URL, address, path or mention is committed as typed on Enter too.
    if (Typing() && !IsConverted() && !HasAscii() && !chosenPrediction_ && IsAddressTyping(Keys(false))) {
        BuildPhrases(true);
    }
    // Mixed typing: Enter before Space takes the chunk as Space would.
    if (chunkJudge_ && Typing() && !IsConverted() && !HasAscii() && !chosenPrediction_) {
        const std::wstring keys = Keys(false);
        std::size_t word = keys.size();
        while (word > 0 && std::wstring_view(L".,!?").find(keys[word - 1]) != std::wstring_view::npos) --word;
        const auto chunk = JudgeChunk(std::wstring_view(keys).substr(0, word));
        const bool capitals = TypedWithCapitals(std::wstring_view(keys).substr(0, word)).has_value();
        // As shown (enterAsShown_): letters as they are shown, not put right.
        if (enterAsShown_) {
            if (const auto letters = LettersShown()) {
                committedAsShown_ = *letters;
                BuildPhrases(true);
            }
        } else if ((chunk.english || capitals) && word > 0) {
            BuildPhrases(true);
        }
        // Not English, yet shown as letters (3abc: the b and c no kana
        // took): committed as it shows.
        if (!IsConverted() && !Live()) {
            if (const auto letters = LettersShown()) {
                committedAsShown_ = *letters;
                BuildPhrases(true);
            }
        }
    }
    if (chunkJudge_) {
        const bool english = ChoseEnglish();
        lastChunkEnglish_ = english;
        englishRun_ = english ? englishRun_ + 1 : 0;
        if (!english) englishText_.clear();
        if (english) {
            auto text = Preedit();
            englishText_ += text;
            englishText_.push_back(L' ');
            if (learning_ && IsConverted()) {
                const auto& phrase = phrases_[focus_];
                if (phrase.selected < phrase.candidates.size()) {
                    learning_->RecordChoice(conversionReading_, phrase.candidates[phrase.selected].text,
                                            phrase.candidates.front().text);
                }
            }
            // A Japanese mark after English (you。) takes no space; English
            // punctuation (you.) is spaced like a word.
            const bool japaneseMark = !text.empty() && text.back() >= 0x3000;
            if (!japaneseMark) text.push_back(L' ');
            context_ = englishContext_;
            // Only punctuation after English (a "." of its own) goes in
            // place of the space after it; ./src does not.
            const auto marks = std::wstring_view(text).substr(0, text.find_last_not_of(L' ') + 1);
            dropSpaceBefore_ = spaceAfterEnglish_ && !marks.empty() && marks.find_first_not_of(L".,!?。、．，！？") == std::wstring_view::npos;
            spaceAfterEnglish_ = !japaneseMark;
            SetSentenceStart(EndsSentence(text));
            ClearChunk();
            lastCommit_.reset();
            return text;
        }
        // Japanese after English takes its space back.
        dropSpaceBefore_ = spaceAfterEnglish_;
        spaceAfterEnglish_ = false;
        japaneseInSentence_ = true;
    }
    CommitRecord record{{}, units_, phrases_, conversionReading_, readingKeys_, context_, contextWords_, focus_,
                        preview_, asciiMode_, capitals_, settledPhrases_, settledLength_};
    const auto keep = [&](const std::wstring& text) {
        lastCommit_.reset();
        if (text.empty()) return;
        record.text = text;
        lastCommit_ = std::move(record);
    };
    if (chosenPrediction_ && *chosenPrediction_ < predictions_.size() && IsTyping()) {
        const auto prediction = predictions_[*chosenPrediction_];
        if (learning_) learning_->RecordChoice(prediction.reading, prediction.text, prediction.text);
        // A prediction's part of speech is not known here.
        context_ = 0;
        RememberWords(prediction.text);
        ClearChunk();
        keep(prediction.text);
        return prediction.text;
    }
    if (const auto* whole = WholeChoice()) {
        if (learning_) learning_->RecordChoice(whole->reading, whole->text, whole->text);
        auto text = whole->text;
        context_ = ContextAfter(text, whole->rightId);
        SetSentenceStart(EndsSentence(text));
        RememberWords(text);
        ClearChunk();
        keep(text);
        return text;
    }
    if (learning_ && IsConverted()) {
        const std::wstring& reading = conversionReading_;
        for (const auto& phrase : phrases_) {
            // Dates and sums change; they are not learned.
            if (!phrase.form && phrase.selected < phrase.candidates.size() &&
                phrase.candidates[phrase.selected].kind == PhraseCandidate::Kind::Special) {
                continue;
            }
            const std::wstring chosen = PhraseText(phrase);
            const std::wstring first = phrase.candidates.empty() ? chosen : phrase.candidates.front().text;
            learning_->RecordChoice(LearningReading(reading, phrase.begin, phrase.length), chosen, first);
        }
    }
    std::uint16_t last = 0;
    if (IsConverted()) {
        const auto& phrase = phrases_.back();
        if (!phrase.form && phrase.selected < phrase.candidates.size()) last = phrase.candidates[phrase.selected].rightId;
    } else if (converter_ && !units_.empty() && pending_.empty() && !HasAscii()) {
        // Kana committed as typed (こんにちは) goes on as the words it reads
        // as, not as a sentence start (こんにちは, then にじがでる: 虹が出る,
        // not に地が出る).
        if (const auto best = converter_->Best(Reading(), context_)) last = best->rightId;
    }
    auto text = IsConverted() ? Preedit() : RenderTyping(false);
    context_ = ContextAfter(text, last);
    if (!text.empty()) SetSentenceStart(EndsSentence(text));
    RememberWords(text);
    ClearChunk();
    keep(text);
    return text;
}

std::wstring JapaneseComposer::CommitBeforeMark(wchar_t key) {
    if (!IsConverted()) Convert();
    std::wstring text = Commit();
    if (text.empty() || IsComposing()) return text;
    const wchar_t mark = text.back();
    if (std::wstring_view(L"\x3001\x3002\xFF0C\xFF0E\xFF01\xFF1F").find(mark) == std::wstring_view::npos) return text;
    // The mark typed again on its own, converted: it stays when it shows as
    // it was committed.
    Insert(key);
    if (!IsConverted()) Convert();
    if (!IsConverted() || phrases_.size() != 1 || PhraseText(phrases_.front()) != std::wstring(1, mark)) {
        ClearChunk();
        return text;
    }
    text.pop_back();
    markLeft_ = true;
    return text;
}

void JapaneseComposer::DropMark() noexcept {
    if (!MarkLeft()) return;
    ClearChunk();
    markLeft_ = false;
}

bool JapaneseComposer::EndsAtPunctuation() const {
    if (!commitAtPunctuation_ || !Typing() || !pending_.empty() || caret_ != units_.size() || units_.empty()) return false;
    if (units_.back().ascii || IsAddressTyping(Keys(false))) return false;
    const std::wstring reading = Reading();
    if (reading.empty() || std::wstring_view(L"\x3001\x3002\xFF0C\xFF0E\xFF01\xFF1F").find(reading.back()) == std::wstring_view::npos) {
        return false;
    }
    // After a digit or a letter the mark may be a decimal point or part of
    // a name (1.5, v1.0, tetr.io): it waits.
    if (reading.size() >= 2) {
        const wchar_t before = reading[reading.size() - 2];
        const bool letter = (before >= L'a' && before <= L'z') || (before >= L'A' && before <= L'Z') ||
                            (before >= L'\xFF21' && before <= L'\xFF3A') || (before >= L'\xFF41' && before <= L'\xFF5A');
        if (IsDigit(before) || letter) return false;
    }
    // Mix: English (hello,) goes on as a sentence; only Japanese ends here.
    if (chunkJudge_) {
        std::wstring keys = Keys(false);
        while (!keys.empty() && std::wstring_view(L".,!?").find(keys.back()) != std::wstring_view::npos) keys.pop_back();
        if (!keys.empty()) {
            const auto judged = JudgeChunk(keys);
            if (judged.english || judged.either || TypedWithCapitals(keys)) return false;
        }
    }
    return true;
}

bool JapaneseComposer::CanCommitToFocus() const noexcept {
    if (!IsConverted() || preview_ || !pending_.empty() || !settled_.empty() || WholeChoice()) return false;
    if (chunkJudge_ && ChoseEnglish()) return false;
    if (focus_ + 1 >= phrases_.size()) return false;
    const std::size_t end = phrases_[focus_].begin + phrases_[focus_].length;
    return end < readingKeys_.size() && readingKeys_[end] != std::wstring::npos;
}

std::wstring JapaneseComposer::CommitToFocus() {
    if (!CanCommitToFocus()) return Commit();
    const std::size_t end = phrases_[focus_].begin + phrases_[focus_].length;
    const std::size_t keyEnd = readingKeys_[end];
    // The units the committed keys were typed as; a unit split in two
    // (a slip undone across it) commits everything.
    std::size_t units = 0;
    for (std::size_t keys = 0; keys < keyEnd && units < units_.size(); ++units) keys += units_[units].keys.size();
    std::size_t typed = 0;
    for (std::size_t i = 0; i < units; ++i) typed += units_[i].keys.size();
    if (typed != keyEnd || units == 0 || units >= units_.size()) return Commit();

    std::wstring text;
    for (std::size_t i = 0; i <= focus_; ++i) {
        const auto& phrase = phrases_[i];
        const std::wstring chosen = PhraseText(phrase);
        text += chosen;
        if (!learning_) continue;
        // Dates and sums change; they are not learned.
        if (!phrase.form && phrase.selected < phrase.candidates.size() &&
            phrase.candidates[phrase.selected].kind == PhraseCandidate::Kind::Special) {
            continue;
        }
        const std::wstring first = phrase.candidates.empty() ? chosen : phrase.candidates.front().text;
        learning_->RecordChoice(LearningReading(conversionReading_, phrase.begin, phrase.length), chosen, first);
    }
    const auto& last = phrases_[focus_];
    const std::uint16_t rightId =
        !last.form && last.selected < last.candidates.size() ? last.candidates[last.selected].rightId : 0;

    // The rest, as it was converted.
    std::vector<PhraseState> rest(phrases_.begin() + static_cast<std::ptrdiff_t>(focus_) + 1, phrases_.end());
    for (auto& phrase : rest) phrase.begin -= end;
    std::vector<std::size_t> restKeys;
    for (std::size_t i = end; i < readingKeys_.size(); ++i) {
        restKeys.push_back(readingKeys_[i] == std::wstring::npos ? std::wstring::npos : readingKeys_[i] - keyEnd);
    }
    const std::wstring restReading = conversionReading_.substr(end);
    std::vector<Unit> restUnits(units_.begin() + static_cast<std::ptrdiff_t>(units), units_.end());
    const bool caseKept = typedCase_.size() >= keyEnd;
    const std::wstring restCase = caseKept ? typedCase_.substr(keyEnd) : std::wstring{};

    if (chunkJudge_) {
        // Japanese after English takes its space back, as Enter does.
        dropSpaceBefore_ = spaceAfterEnglish_;
        spaceAfterEnglish_ = false;
        japaneseInSentence_ = true;
        lastChunkEnglish_ = false;
    }
    context_ = ContextAfter(text, rightId);
    SetSentenceStart(EndsSentence(text));
    RememberWords(text);
    const bool ascii = asciiMode_;
    const int capitals = capitals_;
    ClearChunk();
    lastCommit_.reset();
    units_ = std::move(restUnits);
    caret_ = units_.size();
    asciiMode_ = ascii;
    capitals_ = capitals;
    if (caseKept) typedCase_ = restCase;
    conversionReading_ = restReading;
    readingKeys_ = std::move(restKeys);
    phrases_ = std::move(rest);
    focus_ = 0;
    return text;
}

bool JapaneseComposer::Reconvert(std::wstring_view text) {
    if (!converter_ || text.empty() || IsComposing()) return false;
    const auto guess = converter_->ReadingOf(text);
    if (!guess || guess->reading.empty()) return false;
    ClearChunk();
    lastCommit_.reset();
    // Units as if typed: two kana together where the table has them (きょ).
    const std::wstring& reading = guess->reading;
    for (std::size_t at = 0; at < reading.size();) {
        const wchar_t c = reading[at];
        Unit unit{std::wstring(1, c), std::wstring(1, c), c < 0x80};
        if (table_ && !unit.ascii) {
            for (std::size_t size = std::min<std::size_t>(2, reading.size() - at); size > 0; --size) {
                auto keys = table_->KeysFor(std::wstring_view(reading).substr(at, size));
                if (keys.empty()) continue;
                unit.kana = reading.substr(at, size);
                unit.keys = std::move(keys);
                break;
            }
        }
        at += unit.kana.size();
        units_.push_back(std::move(unit));
    }
    caret_ = units_.size();
    reconverted_ = true;
    const auto* slips = keyConverter_;
    keyConverter_ = nullptr;
    BuildPhrases(true);
    keyConverter_ = slips;
    if (phrases_.empty()) {
        ClearChunk();
        return false;
    }
    // Where each word ends, in the reading and in the text: a phrase that
    // starts and ends there shows the text as it was written.
    std::map<std::size_t, std::size_t> ends{{0, 0}};
    for (std::size_t inText = 0, inReading = 0, i = 0; i < guess->pieces.size(); ++i) {
        inText += guess->pieces[i].first;
        inReading += guess->pieces[i].second;
        ends[inReading] = inText;
    }
    for (auto& phrase : phrases_) {
        const auto begin = ends.find(phrase.begin);
        const auto end = ends.find(phrase.begin + phrase.length);
        if (begin == ends.end() || end == ends.end()) continue;
        const std::wstring written(text.substr(begin->second, end->second - begin->second));
        const auto found = std::find_if(phrase.candidates.begin(), phrase.candidates.end(),
                                        [&](const PhraseCandidate& c) { return c.text == written; });
        if (found == phrase.candidates.end()) {
            phrase.candidates.insert(phrase.candidates.begin(), {written, 0, PhraseCandidate::Kind::Dictionary, false});
            phrase.selected = 0;
        } else {
            phrase.selected = static_cast<std::size_t>(found - phrase.candidates.begin());
        }
    }
    if (Preedit() != text) {
        // The phrases do not line up with the words: the text as one phrase.
        auto candidates = KanaCandidates(conversionReading_);
        candidates.insert(candidates.begin(), {std::wstring(text), 0, PhraseCandidate::Kind::Dictionary, false});
        phrases_.clear();
        phrases_.push_back({0, conversionReading_.size(), std::move(candidates)});
    }
    focus_ = 0;
    listOpen_ = false;
    return true;
}

bool JapaneseComposer::UndoCommit() {
    if (!lastCommit_ || IsComposing()) return false;
    auto record = std::move(*lastCommit_);
    lastCommit_.reset();
    units_ = std::move(record.units);
    caret_ = units_.size();
    pending_.clear();
    phrases_ = std::move(record.phrases);
    conversionReading_ = std::move(record.conversionReading);
    readingKeys_ = std::move(record.readingKeys);
    // The sentence goes on from before the text, as it did then.
    context_ = record.context;
    contextWords_ = std::move(record.contextWords);
    focus_ = phrases_.empty() ? 0 : std::min(record.focus, phrases_.size() - 1);
    listOpen_ = false;
    preview_ = record.preview;
    kanaView_ = false;
    asciiMode_ = record.asciiMode;
    capitals_ = record.capitals;
    settledPhrases_ = record.settledPhrases;
    settledLength_ = record.settledLength;
    UpdatePredictions();
    return true;
}

void JapaneseComposer::AddContentWords(std::wstring_view text, std::vector<std::wstring>& words) {
    const auto content = [](wchar_t c) {
        return (c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005 || (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC;
    };
    for (std::size_t i = 0; i < text.size();) {
        if (!content(text[i])) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && content(text[end])) ++end;
        if (end - i >= 2) words.emplace_back(text.substr(i, end - i));
        i = end;
    }
}

void JapaneseComposer::SetTextBefore(std::wstring_view text) {
    if (text.size() > kRecentCharacters) text.remove_prefix(text.size() - kRecentCharacters);
    recentText_.assign(text);
}

std::uint16_t JapaneseComposer::ContextOf(std::wstring_view text) const {
    if (!converter_ || text.empty() || EndsSentence(text)) return 0;
    // Only after Japanese: English and numbers have contexts of their own.
    const wchar_t last = text.back();
    const bool japanese = (last >= 0x3041 && last <= 0x30FF) || (last >= 0x3400 && last <= 0x9FFF) ||
                          (last >= 0xF900 && last <= 0xFAFF) || last == 0x3005 || last == L'、' || last == L'，';
    if (!japanese) return 0;
    // The end of the sentence it is in, as much as its last words need.
    constexpr std::size_t kTailChars = 12;
    std::size_t begin = text.size() > kTailChars ? text.size() - kTailChars : 0;
    for (std::size_t at = text.size(); at > begin; --at) {
        if (std::wstring_view(L"。．.！!？?\r\n \x3000\t「『（(").find(text[at - 1]) != std::wstring_view::npos) {
            begin = at;
            break;
        }
    }
    const auto tail = text.substr(begin);
    if (tail == contextOfText_) return contextOfRight_;
    const auto guess = converter_->ReadingOf(tail);
    contextOfText_ = std::wstring(tail);
    contextOfRight_ = guess ? guess->right : 0;
    return contextOfRight_;
}

void JapaneseComposer::SetPrecedingText(std::wstring_view text) {
    // The end of it: what the sentence check reads (its last pieces) and
    // the content words conversion weighs.
    constexpr std::size_t kPrecedingChars = 60;
    precedingText_ = std::wstring(text.size() > kPrecedingChars ? text.substr(text.size() - kPrecedingChars) : text);
    // Text TEKITO did not just commit (the caret moved, or it was typed
    // otherwise): it is as written, so its last word's part of speech is
    // where the conversion goes on from (住所を + へいき: 併記).
    // (Its own, from a commit, stays: that knows the words as converted.)
    if (context_ == 0 || context_ == textContext_) {
        context_ = ContextOf(precedingText_);
        textContext_ = context_;
    }
    UpdateJoinedStart();
    contextWords_.clear();
    AddContentWords(precedingText_, contextWords_);
    if (contextWords_.size() > kContextWords) {
        contextWords_.erase(contextWords_.begin(),
                            contextWords_.end() - static_cast<std::ptrdiff_t>(kContextWords));
    }
}

void JapaneseComposer::SyncEdgeContext() const {
    // The text before the caret the converter's edge scorer reads.
    if (converter_) converter_->SetEdgeContext(precedingText_.empty() ? recentText_ : precedingText_);
}

void JapaneseComposer::RememberWords(std::wstring_view text) {
    recentText_ += text;
    if (recentText_.size() > kRecentCharacters) recentText_.erase(0, recentText_.size() - kRecentCharacters);
    AddContentWords(text, contextWords_);
    if (contextWords_.size() > kContextWords) {
        contextWords_.erase(contextWords_.begin(),
                            contextWords_.end() - static_cast<std::ptrdiff_t>(kContextWords));
    }
}

bool JapaneseComposer::EndsSentence(std::wstring_view before) noexcept {
    std::size_t end = before.size();
    while (end > 0 && (before[end - 1] == L' ' || before[end - 1] == L'\x3000' || before[end - 1] == L'\t')) --end;
    return end == 0 || std::wstring_view(L"。．.！!？?\r\n").find(before[end - 1]) != std::wstring_view::npos;
}

std::uint16_t JapaneseComposer::ContextAfter(std::wstring_view text, std::uint16_t rightId) noexcept {
    // A sentence that ended starts the next one afresh.
    if (text.empty() || std::wstring_view(L"。．.！!？?\n").find(text.back()) != std::wstring_view::npos) return 0;
    return rightId;
}

void JapaneseComposer::ClearChunk() noexcept {
    predictions_.clear();
    chosenPrediction_.reset();
    units_.clear();
    conversionReading_.clear();
    readingKeys_.clear();
    caret_ = 0;
    pending_.clear();
    phrases_.clear();
    focus_ = 0;
    listOpen_ = false;
    asciiMode_ = false;
    capitals_ = 0;
    reconverted_ = false;
    keysShown_ = false;
    mixKanaShown_ = false;
    mixLiveReading_.clear();
    mixLivePhrases_.clear();
    japaneseAfterEnglish_ = false;
    ResetLive();
}

void JapaneseComposer::Clear() noexcept {
    ClearChunk();
    rerankerMemory_.Clear();
    // Mixed typing: the chunks held for Enter go too, so they do not come
    // back with what is typed next.
    if (!settled_.empty()) {
        settled_.clear();
        RecountSentence();
    }
    spaceBeforeSentence_ = false;
    lastChunkEnglish_ = false;
}

}  // namespace tekito::japanese
