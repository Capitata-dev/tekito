#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// Conversion: the reading converted into phrases, and moving between phrases
// and their candidates.

namespace tekito::japanese {

std::vector<Phrase> JapaneseComposer::ConvertReading(const std::wstring& reading, std::size_t from,
                                                    std::uint16_t context,
                                                    const std::vector<std::wstring>& words) const {
    // Letters typed after a capital are a phrase of their own, kept as
    // typed; the kana around them are converted.
    std::vector<bool> ascii(reading.size(), false);
    for (std::size_t i = 0, at = 0; i < units_.size() && at < reading.size(); at += units_[i].kana.size(), ++i) {
        if (units_[i].ascii) ascii[at] = true;
    }
    std::vector<Phrase> out;
    for (std::size_t begin = from; begin < reading.size();) {
        std::size_t end = begin;
        while (end < reading.size() && ascii[end] == ascii[begin]) ++end;
        const std::wstring piece = reading.substr(begin, end - begin);
        if (ascii[begin]) {
            auto candidates = AsciiCandidates(piece);
            if (letterNames_) {
                std::wstring lower = piece;
                for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
                std::size_t at = 1;
                for (auto& name : letterNames_(lower)) {
                    if (std::any_of(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) { return c.text == name; })) continue;
                    candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(std::min(at++, candidates.size())),
                                      {std::move(name), 0, PhraseCandidate::Kind::Special, false});
                }
            }
            out.push_back({begin, piece.size(), std::move(candidates)});
        } else {
            SyncEdgeContext();
            const std::uint16_t left = begin == from ? context : begin > 0 && ascii[begin - 1] ? letterContext_ : 0;
            auto phrases = converter_->Convert(piece, {}, left, words, userDictionary_);
            // Every part of the reading has a phrase, or it would drop out of
            // the text: a piece nothing reads stays as typed.
            std::size_t covered = 0;
            for (const auto& phrase : phrases) covered += phrase.candidates.empty() ? 0 : phrase.length;
            if (covered != piece.size()) phrases = {{0, piece.size(), KanaCandidates(piece)}};
            for (auto& phrase : phrases) {
                phrase.begin += begin;
                StyleKanaNumbers(phrase.candidates);
                out.push_back(std::move(phrase));
            }
        }
        begin = end;
    }
    return out;
}

bool JapaneseComposer::HasAscii() const noexcept {
    return std::any_of(units_.begin(), units_.end(), [](const Unit& unit) { return unit.ascii; });
}

std::vector<PhraseCandidate> JapaneseComposer::AsciiCandidates(std::wstring_view text) {
    // As typed first, then the other cases and the full-width form.
    std::vector<PhraseCandidate> out;
    const auto add = [&](std::wstring candidate) {
        if (std::none_of(out.begin(), out.end(), [&](const auto& c) { return c.text == candidate; })) {
            out.push_back({std::move(candidate), 0, PhraseCandidate::Kind::Special, false});
        }
    };
    const std::wstring typed(text);
    add(typed);
    std::wstring lower = typed, upper = typed, title = typed;
    for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
    for (auto& ch : upper) ch = static_cast<wchar_t>(std::towupper(ch));
    for (std::size_t i = 0; i < title.size(); ++i) {
        title[i] = static_cast<wchar_t>(i == 0 ? std::towupper(title[i]) : std::towlower(title[i]));
    }
    add(title);
    add(lower);
    add(upper);
    add(ToFullWidthAscii(typed));
    return out;
}

void JapaneseComposer::AddOtherWidth(std::vector<PhraseCandidate>& candidates) const {
    if (candidates.empty()) return;
    const auto& first = candidates.front();
    std::wstring other = first.text;
    bool changed = false;
    // Marks alone (！, ？？, ・) get their other width too, and Japanese
    // punctuation the ASCII key it was typed with (・: /).
    const auto japaneseMark = [](wchar_t ch) -> wchar_t {
        switch (ch) {
        case L'\x3001': return L',';
        case L'\x3002': return L'.';
        case L'\x30FB': return L'/';
        case L'\x300C': return L'[';
        case L'\x300D': return L']';
        case L'\x30FC': return L'-';
        case L'\x301C': return L'~';
        default: return L'\0';
        }
    };
    const auto isAlphanumeric = [](wchar_t ch) {
        return (ch >= L'\xFF21' && ch <= L'\xFF3A') || (ch >= L'\xFF41' && ch <= L'\xFF5A') ||
               (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
               // Digits have number forms of their own.
               (ch >= L'\xFF10' && ch <= L'\xFF19') || (ch >= L'0' && ch <= L'9');
    };
    const bool marksOnly = std::all_of(other.begin(), other.end(), [&](wchar_t ch) {
        return !isAlphanumeric(ch) && ((ch >= L'\xFF01' && ch <= L'\xFF5E') || (ch >= L'!' && ch <= L'~') || japaneseMark(ch));
    });
    if (marksOnly) {
        for (auto& ch : other) {
            if (ch >= L'\xFF01' && ch <= L'\xFF5E') {
                ch = static_cast<wchar_t>(ch - 0xFEE0);
            } else if (const wchar_t ascii = japaneseMark(ch)) {
                ch = ascii;
            } else {
                ch = static_cast<wchar_t>(ch + 0xFEE0);
            }
        }
        changed = other != first.text;
    }
    for (auto& ch : other) {
        if (marksOnly) break;
        if ((ch >= L'\xFF21' && ch <= L'\xFF3A') || (ch >= L'\xFF41' && ch <= L'\xFF5A')) {
            ch = static_cast<wchar_t>(ch - 0xFEE0);
            changed = true;
        } else if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) {
            ch = static_cast<wchar_t>(ch + 0xFEE0);
            changed = true;
        }
    }
    if (!changed) return;
    const bool seen = std::any_of(candidates.begin(), candidates.end(),
                                  [&](const PhraseCandidate& c) { return c.text == other; });
    if (seen) return;
    PhraseCandidate candidate{std::move(other), first.cost, first.kind, false};
    candidate.reading = first.reading;
    candidates.insert(candidates.begin() + 1, std::move(candidate));
}

std::vector<PhraseCandidate> JapaneseComposer::KanaCandidates(std::wstring_view reading) const {
    PhraseCandidate hiragana{std::wstring(reading), 0, PhraseCandidate::Kind::Hiragana, false};
    PhraseCandidate katakana{ToKatakana(reading), 0, PhraseCandidate::Kind::Katakana, false};
    // Space gives the other kana than the one being typed.
    if (TypingForm() == KanaForm::Katakana) return {hiragana, katakana};
    return {katakana, hiragana};
}

void JapaneseComposer::BuildPhrases(bool convert) {
    BuildPhrasesFor(convert);
    if (!convert) return;
    for (auto& phrase : phrases_) KeepKanjiInRange(phrase);
}

bool JapaneseComposer::InKanjiRange(std::wstring_view text) const {
    return !kanjiSets_ || kanjiRange_ == KanjiRange::All || kanjiSets_->Allows(text, kanjiRange_, kanjiGrade_, kanjiNames_);
}

void JapaneseComposer::KeepKanjiInRange(PhraseState& phrase, std::wstring_view reading) const {
    if (!kanjiSets_ || kanjiRange_ == KanjiRange::All || phrase.candidates.empty()) return;
    auto& candidates = phrase.candidates;
    const std::wstring chosen = phrase.selected < candidates.size() ? candidates[phrase.selected].text : std::wstring{};
    const bool firstOut = !kanjiSets_->Allows(candidates.front().text, kanjiRange_, kanjiGrade_, kanjiNames_);
    std::erase_if(candidates, [&](const PhraseCandidate& c) { return !kanjiSets_->Allows(c.text, kanjiRange_, kanjiGrade_, kanjiNames_); });
    // A word with kanji outside the range is written in kana first, not as
    // another word that happens to fit (差して is さして, not さし手).
    if ((firstOut || candidates.empty()) && phrase.begin + phrase.length <= reading.size()) {
        PhraseCandidate hiragana{std::wstring(reading.substr(phrase.begin, phrase.length)), 0, PhraseCandidate::Kind::Hiragana, false};
        std::erase_if(candidates, [&](const PhraseCandidate& c) { return c.text == hiragana.text; });
        candidates.insert(candidates.begin(), std::move(hiragana));
    }
    const auto kept = std::find_if(candidates.begin(), candidates.end(),
                                   [&](const PhraseCandidate& c) { return c.text == chosen; });
    phrase.selected = kept == candidates.end() ? 0 : static_cast<std::size_t>(kept - candidates.begin());
}

void JapaneseComposer::BuildPhrasesFor(bool convert) {
    // Mixed typing: shown as letters while typed (3abc), the keys still
    // pending as they showed.
    const auto shownLetters = convert ? TypedLetters() : std::nullopt;
    FlushAll();
    predictions_.clear();
    chosenPrediction_.reset();
    // Back from a conversion, typing goes on at the end.
    caret_ = units_.size();
    const std::wstring reading = Reading();
    conversionReading_ = reading;
    readingKeys_.assign(reading.size() + 1, std::wstring::npos);
    for (std::size_t offset = 0, key = 0, i = 0;; ++i) {
        if (offset < readingKeys_.size()) readingKeys_[offset] = key;
        if (i == units_.size()) break;
        offset += units_[i].kana.size();
        key += units_[i].keys.size();
    }
    phrases_.clear();
    focus_ = 0;
    listOpen_ = false;
    if (reading.empty()) return;
    // A URL, address, path or mention stays as typed.
    if (convert && !HasAscii()) {
        const std::wstring keys = Keys(false);
        if (IsAddressTyping(keys)) {
            // Mixed typing: with the capitals typed (C:\Users).
            const std::wstring typed = typedCaseKept_ && typedCase_.size() == keys.size() ? typedCase_ : keys;
            std::vector<PhraseCandidate> candidates{{typed, 0, PhraseCandidate::Kind::English, false}};
            candidates.push_back({ToFullWidthAscii(keys), 0, PhraseCandidate::Kind::Hiragana, false});
            candidates.push_back({reading, 0, PhraseCandidate::Kind::Hiragana, false});
            phrases_.push_back({0, reading.size(), std::move(candidates)});
            return;
        }
    }
    // Mixed typing: committed as it showed in letters (CommitChunk).
    if (const auto shown = std::exchange(committedAsShown_, std::nullopt); shown && convert) {
        const bool japanese = std::exchange(committedAsShownJapanese_, false);
        phrases_.push_back({0, reading.size(),
                            {{*shown, 0, japanese ? PhraseCandidate::Kind::Dictionary : PhraseCandidate::Kind::English, false}}});
        return;
    }
    // Mixed typing: the chunk may be English, with punctuation typed after
    // it (you., hello,), or be only punctuation after English.
    std::optional<ChunkJudge::Judgement> chunk;
    if (convert && chunkJudge_ && !HasAscii()) {
        std::wstring keys = Keys(false);
        std::size_t word = keys.size();
        while (word > 0 && std::wstring_view(L".,!?").find(keys[word - 1]) != std::wstring_view::npos) --word;
        const auto marks = MarksAfterEnglish(std::wstring_view(keys).substr(word));
        if (word == 0 && spaceAfterEnglish_) {
            phrases_.push_back({0, reading.size(), {{marks, 0, PhraseCandidate::Kind::English, false}}});
            return;
        }
        if (word > 0) {
            chunk = JudgeChunk(std::wstring_view(keys).substr(0, word));
            JudgeWholeSentence(chunk, std::wstring_view(keys).substr(0, word));
            // A capital typed in it: English, as typed (I, Have), or as the
            // name is written when only its first letter was (Github: GitHub).
            if (const auto typed = TypedWithCapitals(std::wstring_view(keys).substr(0, word))) {
                const bool firstOnly = std::none_of(typed->begin() + 1, typed->end(),
                                                    [](wchar_t c) { return c >= L'A' && c <= L'Z'; });
                const bool named = chunk->english && std::any_of(chunk->written.begin() + 1, chunk->written.end(),
                                                                 [](wchar_t c) { return c >= L'A' && c <= L'Z'; });
                if (!(firstOnly && named)) chunk->written = *typed;
                chunk->english = true;
                chunk->word = true;
                chunk->meant.clear();
            }
            // A slip of a particle (dr): the particle, then the keys as typed.
            if (!chunk->english && !chunk->meant.empty() && table_ && converter_) {
                std::wstring kana;
                for (const auto& token : ParseRomaji(*table_, chunk->meant)) kana += token.kana;
                std::vector<PhraseCandidate> candidates;
                for (const auto& phrase : ConvertReading(ApplyTypingStyle(kana) + marks, 0, context_, WordsWithEnglish())) {
                    if (!phrase.candidates.empty()) {
                        candidates.push_back({phrase.candidates.front().text, 0, PhraseCandidate::Kind::Dictionary, false});
                    }
                }
                if (candidates.size() == 1) {
                    candidates.push_back({keys, 0, PhraseCandidate::Kind::English, false});
                    phrases_.push_back({0, reading.size(), std::move(candidates)});
                    return;
                }
            }
            // Keys that are no word may be Japanese run into one (korehagithub),
            // with punctuation after it too (bugwofixshita.).
            if (chunk->english && !chunk->word) {
                if (const auto split = RunTogetherEnglish()) {
                    std::vector<PhraseCandidate> candidates{{*split, 0, PhraseCandidate::Kind::Dictionary, false}};
                    candidates.push_back({keys, 0, PhraseCandidate::Kind::English, false});
                    candidates.push_back({reading, 0, PhraseCandidate::Kind::Hiragana, false});
                    phrases_.push_back({0, reading.size(), std::move(candidates)});
                    return;
                }
            }
            if (chunk->english) {
                // Marks as the sentence now reads (judged again just above).
                const auto after = MarksAfterEnglish(std::wstring_view(keys).substr(word));
                phrases_.push_back({0, reading.size(),
                                    EnglishCandidates(EnglishAsWritten(chunk->written) + after, reading)});
                return;
            }
        }
    }
    if (convert) {
        if (auto sum = SumCandidates(reading); !sum.empty()) {
            phrases_.push_back({0, reading.size(), std::move(sum)});
            return;
        }
        if (auto postal = PostalCandidates(reading); !postal.empty()) {
            phrases_.push_back({0, reading.size(), std::move(postal)});
            return;
        }
    }
    // A slip in the keys was undone: English typed into it comes second.
    bool slipUndone = false;
    slipUndone_ = false;
    // Keys that end in letters romaji left as they were (おk, っk) are meant
    // so: no slip is undone to drop or change them.
    const bool endsInLetters = !units_.empty() && !units_.back().ascii &&
                               LeftoverLetters(ToFullWidthAscii(units_.back().kana)) > 0;
    if (convert && converter_) {
        // A slip in the keys: converted from the keys as meant when the
        // user allows auto-apply. Esc still goes back to what was typed.
        if (auto conversion = slipAutoApply_ && keyConverter_ && !HasAscii() && !KanaTyped() && !endsInLetters
                                  ? keyConverter_->Convert(Keys(false), context_, WordsWithEnglish(), userDictionary_)
                                            : std::nullopt) {
            slipUndone = true;
            conversionReading_ = ApplyTypingStyle(std::move(conversion->reading));
            readingKeys_ = std::move(conversion->keyAt);
            for (auto& phrase : conversion->phrases) StyleKanaNumbers(phrase.candidates);
            AddPhrases(std::move(conversion->phrases), conversionReading_);
        } else {
            AddPhrases(ConvertReading(reading, 0, context_, WordsWithEnglish()), reading);
        }
        // In Mix, a chunk taken as Japanese that reads as no English (not
        // either, no word) is Japanese with a slip as in Japanese mode
        // (ktouno: 今日の); English-looking keys are left to the English.
        const bool japaneseChunk = !chunkJudge_ || (chunk && !chunk->english && !chunk->either && !chunk->word &&
                                                    mixSlips_);
        if (slipAutoApply_ && slips_ && table_ && japaneseChunk && !HasAscii() && !KanaTyped() && !reconverted_ &&
            !endsInLetters && !phrases_.empty() && FixSlips()) {
            slipUndone = true;
            // Put right in the kana, not in its reading: Enter keeps it.
            slipUndone_ = true;
        }
        // A slip undone: the keys as typed stay a choice, after what was
        // meant (konpyu-ra-: コンピューラー after コンピューター), for a short input
        // that reads as kana.
        constexpr std::size_t kTypedChoiceLength = 16;
        if (slipUndone && !phrases_.empty() && reading.size() <= kTypedChoiceLength &&
            LeftoverLetters(ToFullWidthAscii(reading)) == 0) {
            std::wstring typed;
            for (const auto& phrase : ConvertReading(reading, 0, context_, WordsWithEnglish())) {
                if (phrase.candidates.empty()) {
                    typed.clear();
                    break;
                }
                typed += phrase.candidates.front().text;
            }
            std::wstring shown;
            for (const auto& phrase : phrases_) shown += PhraseText(phrase);
            auto& candidates = phrases_.front().candidates;
            if (!typed.empty() && typed != shown &&
                std::none_of(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) { return c.text == typed; })) {
                PhraseCandidate asTyped{typed, candidates.empty() ? 0 : candidates.front().cost,
                                        PhraseCandidate::Kind::Dictionary, false};
                asTyped.reading = std::wstring(reading);
                candidates.insert(candidates.begin() + std::min<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(candidates.size())),
                                  std::move(asTyped));
            }
        }
    }
    if (phrases_.empty()) {
        phrases_.push_back({0, reading.size(),
                            convert ? KanaCandidates(reading) : std::vector<PhraseCandidate>{}});
        if (convert) {
            AddOtherWidth(phrases_.back().candidates);
            AddSpecial(reading, phrases_.back().candidates);
        }
    }
    if (convert && !chunkJudge_) AddTypedEnglish();
    // Japanese mode: English typed into it without spaces, first when the
    // keys make no kana.
    if (convert && !chunkJudge_ && splitter_ && !HasAscii() && !phrases_.empty()) {
        if (auto split = RunTogetherEnglish()) {
            const bool leftover = std::any_of(units_.begin(), units_.end(), [](const Unit& unit) {
                return !unit.ascii && LeftoverLetters(ToFullWidthAscii(unit.kana)) > 0;
            });
            PhraseCandidate whole{std::move(*split), 0, PhraseCandidate::Kind::Dictionary, false};
            whole.reading = Reading();
            auto& candidates = phrases_.front().candidates;
            // First when romaji leaves letters of the keys, even with a slip
            // undone to read them as kana (meetingsimasyou: めえちんしましょう).
            candidates.insert(candidates.begin() + (leftover || candidates.empty() ? 0 : 1), std::move(whole));
        }
    }
    // Japanese mode: an English word typed with letters romaji keeps for
    // small kana or cannot read (lol, love: not お or ぉで, with the letters taken for slips).
    if (convert && !chunkJudge_ && splitter_ && !HasAscii() && !phrases_.empty()) {
        std::wstring keys = Keys(false);
        const bool letters = keys.size() >= 3 &&
                             std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; });
        const bool leftover = std::any_of(units_.begin(), units_.end(), [](const Unit& unit) {
            return !unit.ascii && LeftoverLetters(ToFullWidthAscii(unit.kana)) > 0;
        });
        if (letters && (leftover || ChunkJudge::ForeignLetters(keys, false))) {
            const auto judged = splitter_->Judge(keys, 0, true, nullptr, {}, spacelessEnglish_);
            auto& candidates = phrases_.front().candidates;
            if (judged.english && judged.word && !judged.written.empty()) {
                std::erase_if(candidates, [&](const PhraseCandidate& c) { return c.text == judged.written; });
                PhraseCandidate english{judged.written, 0, PhraseCandidate::Kind::English, false};
                english.reading = Reading();
                candidates.insert(candidates.begin(), std::move(english));
            }
        }
    }
    if (chunk && !chunk->english && !phrases_.empty()) {
        bool foreign = false;
        if (auto split = RunTogetherEnglish(&foreign)) {
            // Letters romaji cannot read (the l of mailwo), or hardly uses
            // (the g of bugwo, ぶぐぉ), are the English word's own: the slip
            // undone to read them as kana comes second.
            const bool leftover = foreign || std::any_of(units_.begin(), units_.end(), [](const Unit& unit) {
                return !unit.ascii && LeftoverLetters(ToFullWidthAscii(unit.kana)) > 0;
            });
            PhraseCandidate whole{std::move(*split), 0, PhraseCandidate::Kind::Dictionary, false};
            whole.reading = Reading();
            auto& candidates = phrases_.front().candidates;
            candidates.insert(candidates.begin() + (slipUndone && !leftover && !candidates.empty() ? 1 : 0),
                              std::move(whole));
        }
    }
    // A slip undone read the keys another way (nyuusu: にゅうしゅ, 入手); the
    // keys as typed may be a katakana word with a vowel for ー (ニュース).
    if (slipUndone && phrases_.size() == 1 && Reading() != conversionReading_) {
        AddLongVowelWords(phrases_.front().candidates, Reading(), true);
    }
    // Japanese, but it reads as English too: the English for the whole
    // chunk is the next choice.
    if (chunk && chunk->either && !phrases_.front().candidates.empty()) {
        auto& candidates = phrases_.front().candidates;
        PhraseCandidate english{EnglishAsWritten(chunk->written), candidates.front().cost,
                                PhraseCandidate::Kind::English, false};
        english.reading = conversionReading_;
        english.otherLanguage = true;
        candidates.insert(candidates.begin() + 1, std::move(english));
    }
    // Mixed typing: letters shown as typed that convert only to kana or kanji
    // with letters left in them (3abc: 3亜bc) stay letters first, as Enter
    // commits them.
    if (shownLetters && chunk && !chunk->english && !phrases_.empty()) {
        std::wstring text;
        for (const auto& phrase : phrases_) text += PhraseText(phrase);
        const bool latin = std::any_of(text.begin(), text.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
        });
        const bool japanese = std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= 0x3041; });
        // English run into Japanese as one reading (meetingで) is meant so.
        const bool wholeRead = std::any_of(phrases_.begin(), phrases_.end(), [](const PhraseState& phrase) {
            return !phrase.form && phrase.selected < phrase.candidates.size() &&
                   !phrase.candidates[phrase.selected].reading.empty();
        });
        if (latin && japanese && !wholeRead) {
            conversionReading_ = reading;
            phrases_.clear();
            phrases_.push_back({0, reading.size(), EnglishCandidates(*shownLetters, reading)});
            return;
        }
    }
    if (convert) JoinEllipsis();
    // An AI model chosen reads the sentence check's choices, with it on or off.
    // Mix's Japanese chunks too, by the sentence model alone (its AI reads
    // the whole sentence on Enter), so that what Space shows is checked as
    // Japanese mode's is (何時ですか, not 何児ですか).
    if (convert && reranker_ && (chunkJudge_ ? sentenceCheck_ : sentenceCheck_ || aiJudge_) && !reconverted_) CheckSentence();
    if (convert) ApplyNumberStyle();
    // What the sentence check, a whole text or the AI chose shows first in
    // its list, the others as they came: Space goes on from it.
    if (convert) PutShownFirst();
    if (convert) AddVariantForms();
    if (convert) AddTypedForms();
    if (convert) {
        for (auto& phrase : phrases_) TidyFirstPage(phrase);
    }
}

void JapaneseComposer::AddPhrases(std::vector<Phrase> phrases, const std::wstring& reading) {
    for (auto& phrase : phrases) {
        if (phrase.candidates.empty()) continue;
        const auto phraseReading = std::wstring_view(reading).substr(phrase.begin, phrase.length);
        if (std::none_of(phraseReading.begin(), phraseReading.end(), IsDigit)) PutDigitsAfterWords(phrase.candidates);
        const std::wstring first = phrase.candidates.front().text;
        if (learning_) learning_->Reorder(LearningReading(reading, phrase.begin, phrase.length), phrase.candidates);
        const bool learned = phrase.candidates.front().text != first;
        if (!learned) PreferWordsWritten(phrase.candidates);
        AddLoanwords(phrase.candidates, phraseReading);
        AddLongVowelWords(phrase.candidates, phraseReading);
        AddOtherWidth(phrase.candidates);
        AddSpecial(std::wstring_view(reading).substr(phrase.begin, phrase.length), phrase.candidates);
        phrases_.push_back({phrase.begin, phrase.length, std::move(phrase.candidates)});
        phrases_.back().learned = learned;
    }
}

void JapaneseComposer::Convert() {
    if (!Typing()) return;
    if (Live() && preview_) {
        // Live: the text converted as Space converts it without live
        // conversion (it may have been shown as typed so far), then the
        // next candidate: the whole input read another way (English run in)
        // when there is one, else the last phrase's. The next Space opens
        // the list.
        // Keys still pending (100en: 100絵n) are first read as Space reads
        // them (100円), which is shown before any other candidate.
        const std::wstring typedShown = Preedit();
        const bool flushed = !pending_.empty();
        if (flushed) {
            FlushAll();
            UpdateLive();
        }
        const std::wstring shown = Preedit();
        const bool rebuilt = RebuildLive();
        StartPicking();
        if (!IsConverted()) return;
        // The last word's, not the punctuation after it (今日は雨。: 雨).
        const auto marksOnly = [](const std::wstring& text) {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) {
                return std::wstring_view(L"。、．，.,！？!?…‥・").find(c) != std::wstring_view::npos;
            });
        };
        while (focus_ > 0 && marksOnly(PhraseText(phrases_[focus_]))) --focus_;
        // An address stays as typed; the next Space goes on to its other forms.
        if (phrases_[focus_].address) return;
        bool codeBefore = false;
        // So do the digits of a code (090-1234-5678): another width or
        // kanji numerals for its last group are no other reading.
        {
            std::wstring before;
            for (std::size_t i = 0; i < focus_; ++i) before += PhraseText(phrases_[i]);
            const std::wstring text = PhraseText(phrases_[focus_]);
            constexpr std::wstring_view kJoins = L"-\xFF0D\x30FC:\xFF1A/\xFF0F\x30FB";
            const bool codeMarks = std::all_of(text.begin(), text.end(), [&](wchar_t c) {
                return IsDigit(c) || kJoins.find(c) != std::wstring_view::npos || std::wstring_view(L".\xFF0E").find(c) != std::wstring_view::npos;
            });
            const bool joined = text.find_first_of(kJoins) != std::wstring::npos ||
                                (before.size() >= 2 && kJoins.find(before.back()) != std::wstring_view::npos &&
                                 IsDigit(before[before.size() - 2]));
            if (!text.empty() && IsDigit(text.back()) && codeMarks && joined) return;
            // A number, or the word read after one (3時30分, 1,000円, 1・2年生):
            // what the number makes of it stays; the next Space opens the list.
            if (!text.empty() && (IsDigit(text.front()) || (!before.empty() && IsDigit(before.back())))) return;
            codeBefore = before.size() >= 2 && kJoins.find(before.back()) != std::wstring_view::npos &&
                         IsDigit(before[before.size() - 2]);
            codeBefore = codeBefore || (!text.empty() && IsDigit(text.front()) && before.size() >= 2 &&
                                        IsDigit(before.back()) && text.find_first_of(kJoins) != std::wstring::npos);
        }
        if (flushed && Preedit() != typedShown) return;
        // What was shown changed (English run in, a slip undone): that comes
        // first, as Space shows it without live conversion.
        // A whole-input reading shown: Space and its list are that phrase's.
        bool showingWhole = false;
        for (std::size_t i = 0; i < phrases_.size(); ++i) {
            const auto& phrase = phrases_[i];
            if (!phrase.form && phrase.selected < phrase.candidates.size() &&
                !phrase.candidates[phrase.selected].reading.empty()) {
                focus_ = i;
                showingWhole = true;
                break;
            }
        }
        if (rebuilt && Preedit() != shown) return;
        std::optional<std::wstring> whole;
        for (std::size_t i = 0; i < phrases_.size() && !whole && !showingWhole; ++i) {
            const auto& candidates = phrases_[i].candidates;
            if (phrases_[i].selected == 0 && candidates.size() > 1 && !candidates[1].reading.empty() &&
                !candidates[1].slip) {
                focus_ = i;
                whole = candidates[1].text;
            }
        }
        AddChunkCandidates(focus_);
        AddLongerWords(focus_);
        auto& phrase = phrases_[focus_];
        AddSlipCandidates(phrase);
        // Its first page for words again, with what joined it.
        TidyFirstPage(phrase);
        phrase.form.reset();
        if (phrase.candidates.empty()) return;
        // The slips just added may come before the whole input's reading.
        const auto at = whole ? std::find_if(phrase.candidates.begin(), phrase.candidates.end(),
                                             [&](const PhraseCandidate& c) { return c.text == *whole; })
                              : phrase.candidates.end();
        const std::size_t was = phrase.selected;
        phrase.selected = at != phrase.candidates.end() ? static_cast<std::size_t>(at - phrase.candidates.begin())
                                                        : (phrase.selected + 1) % phrase.candidates.size();
        // After the digits of a code (090-1234-5678です), its digits stay:
        // the same with other widths or commas, or with the digits in kanji,
        // is no other reading. The next one that keeps them, if any.
        if (codeBefore && at == phrase.candidates.end()) {
            const auto narrow = [](std::wstring text) {
                for (auto& c : text) {
                    if (c >= L'\xFF01' && c <= L'\xFF5E') c = static_cast<wchar_t>(c - 0xFEE0);
                }
                std::erase(text, L',');
                return text;
            };
            const auto digits = [](const std::wstring& text) {
                std::wstring out;
                for (const wchar_t c : text) {
                    if (c >= L'0' && c <= L'9') out += c;
                }
                return out;
            };
            const std::wstring current = narrow(phrase.candidates[was].text);
            const auto same = [&](std::size_t index) {
                const std::wstring text = narrow(phrase.candidates[index].text);
                return text == current || digits(text) != digits(current);
            };
            while (phrase.selected != was && same(phrase.selected)) {
                phrase.selected = (phrase.selected + 1) % phrase.candidates.size();
            }
        }
        return;
    }
    if (!IsConverted()) {
        BuildPhrases(true);
        // From the end: the last word, not the punctuation after it.
        if (convertFromEnd_ && !phrases_.empty() && !WholeChoice()) {
            focus_ = phrases_.size() - 1;
            const auto marksOnly = [](const std::wstring& text) {
                return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) {
                    return std::wstring_view(L"。、．，.,！？!?…‥・").find(c) != std::wstring_view::npos;
                });
            };
            while (focus_ > 0 && marksOnly(PhraseText(phrases_[focus_]))) --focus_;
        }
        return;
    }
    NextCandidate();
}

void JapaneseComposer::ShowCandidates() {
    if (!IsConverted()) Convert();
    if (!IsConverted()) return;
    StartPicking();
    AddChunkCandidates(focus_);
    AddLongerWords(focus_);
    auto& phrase = phrases_[focus_];
    AddSlipCandidates(phrase);
    if (phrase.candidates.empty()) {
        phrase.candidates = KanaCandidates(conversionReading_.substr(phrase.begin, phrase.length));
    }
    listOpen_ = true;
}

void JapaneseComposer::NextCandidate() {
    if (!IsConverted()) {
        Convert();
        return;
    }
    StartPicking();
    AddChunkCandidates(focus_);
    AddLongerWords(focus_);
    auto& phrase = phrases_[focus_];
    AddSlipCandidates(phrase);
    if (phrase.candidates.empty()) {
        phrase.candidates = KanaCandidates(conversionReading_.substr(phrase.begin, phrase.length));
    }
    if (!phrase.form) phrase.selected = (phrase.selected + 1) % phrase.candidates.size();
    phrase.form.reset();
    listOpen_ = true;
    TakeLongerWord();
}

void JapaneseComposer::PreviousCandidate() {
    if (!IsConverted()) {
        Convert();
        return;
    }
    StartPicking();
    AddChunkCandidates(focus_);
    AddLongerWords(focus_);
    auto& phrase = phrases_[focus_];
    AddSlipCandidates(phrase);
    if (phrase.candidates.empty()) {
        phrase.candidates = KanaCandidates(conversionReading_.substr(phrase.begin, phrase.length));
    }
    const std::size_t count = phrase.candidates.size();
    if (!phrase.form) phrase.selected = (phrase.selected + count - 1) % count;
    phrase.form.reset();
    listOpen_ = true;
    TakeLongerWord();
}

void JapaneseComposer::SelectCandidate(std::size_t index) {
    if (!IsConverted()) return;
    auto& phrase = phrases_[focus_];
    if (index >= phrase.candidates.size()) return;
    phrase.selected = index;
    phrase.form.reset();
    listOpen_ = false;
    TakeLongerWord();
}

void JapaneseComposer::MoveFocus(int delta) {
    if (!IsConverted()) return;
    listOpen_ = false;
    // From the live conversion the caret is after the last phrase (picking
    // may change the phrases: the pending keys become kana first).
    const bool fromLive = preview_;
    StartPicking();
    if (phrases_.empty()) return;
    // The whole input read at once is one piece: moving shows the phrases.
    if (phrases_.size() > 1) DropWholeChoice();
    const auto count = static_cast<int>(phrases_.size());
    const int from = fromLive ? count : static_cast<int>(focus_);
    focus_ = static_cast<std::size_t>(std::clamp(from + delta, 0, count - 1));
}

void JapaneseComposer::ResizeFocus(int delta) {
    if (!IsConverted() || !converter_) return;
    StartPicking();
    DropWholeChoice();
    const std::wstring reading = conversionReading_;
    const auto& focused = phrases_[focus_];
    const auto length = static_cast<long long>(focused.length) + delta;
    if (length < 1 || focused.begin + static_cast<std::size_t>(length) > reading.size()) return;

    // Phrases before the focus keep their length and choice; the focused one
    // takes its new length; the rest is split and converted again.
    std::vector<std::size_t> fixed;
    for (std::size_t i = 0; i < focus_; ++i) fixed.push_back(phrases_[i].length);
    fixed.push_back(static_cast<std::size_t>(length));
    std::vector<PhraseState> kept(phrases_.begin(), phrases_.begin() + static_cast<long long>(focus_));

    phrases_.clear();
    SyncEdgeContext();
    AddPhrases(converter_->Convert(reading, fixed, context_, WordsWithEnglish(), userDictionary_), reading);
    if (phrases_.size() <= focus_) {
        BuildPhrases(true);
        return;
    }
    for (std::size_t i = 0; i < kept.size(); ++i) phrases_[i] = std::move(kept[i]);
    listOpen_ = false;
}

void JapaneseComposer::CycleKana() {
    if (!Typing()) return;
    // From the live conversion: the whole text, as before conversion.
    if (preview_) {
        phrases_.clear();
        ResetLive();
    }
    if (!IsConverted()) BuildPhrases(false);
    auto& phrase = phrases_[focus_];
    if (!phrase.form || IsAlphanumeric(*phrase.form)) {
        // First press: the other kana than the one being typed.
        phrase.form = TypingForm() == KanaForm::Katakana ? KanaForm::Hiragana : KanaForm::Katakana;
    } else if (*phrase.form == KanaForm::Hiragana) {
        phrase.form = KanaForm::Katakana;
    } else if (*phrase.form == KanaForm::Katakana) {
        phrase.form = KanaForm::HalfWidthKatakana;
    } else {
        phrase.form = KanaForm::Hiragana;
    }
    listOpen_ = false;
}

void JapaneseComposer::Transliterate(KanaForm form) {
    if (!Typing()) return;
    // From the live conversion: the whole text, as before conversion.
    if (preview_) {
        phrases_.clear();
        ResetLive();
    }
    if (!IsConverted()) BuildPhrases(false);
    auto& phrase = phrases_[focus_];
    phrase.letterCase = IsAlphanumeric(form) && phrase.form == form ? (phrase.letterCase + 1) % 4 : 0;
    phrase.form = form;
    listOpen_ = false;
}

const PhraseCandidate* JapaneseComposer::WholeChoice() const noexcept {
    // Live: only once the text was converted again after a pause (the
    // phrases typed on hold no reading of the whole input).
    if (!IsConverted() || (preview_ && pending_.size() > 0)) return nullptr;
    // A phrase's list may hold readings of the whole input: chosen in any
    // phrase, that reading is the text (the focused phrase first).
    const auto chosen = [](const PhraseState& phrase) -> const PhraseCandidate* {
        if (phrase.form || phrase.selected >= phrase.candidates.size()) return nullptr;
        const auto& candidate = phrase.candidates[phrase.selected];
        return candidate.reading.empty() ? nullptr : &candidate;
    };
    if (const auto* whole = chosen(phrases_[focus_])) return whole;
    for (const auto& phrase : phrases_) {
        if (const auto* whole = chosen(phrase)) return whole;
    }
    return nullptr;
}

void JapaneseComposer::DropWholeChoice() noexcept {
    if (preview_) return;
    for (auto& phrase : phrases_) {
        if (phrase.form || phrase.selected >= phrase.candidates.size() ||
            phrase.candidates[phrase.selected].reading.empty()) {
            continue;
        }
        const auto own = std::find_if(phrase.candidates.begin(), phrase.candidates.end(),
                                      [](const PhraseCandidate& c) { return c.reading.empty(); });
        phrase.selected = own == phrase.candidates.end() ? 0 : static_cast<std::size_t>(own - phrase.candidates.begin());
    }
}

}  // namespace tekito::japanese
