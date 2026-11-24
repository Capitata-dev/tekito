#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// Mix: Japanese and English typed together, chunk by chunk, judged and
// joined.

namespace tekito::japanese {

void JapaneseComposer::AddTypedEnglish() {
    if (!loanwords_ || phrases_.empty() || HasAscii()) return;
    const bool leftover = std::any_of(units_.begin(), units_.end(), [](const Unit& unit) {
        return LeftoverLetters(ToFullWidthAscii(unit.kana)) > 0;
    });
    const std::wstring keys = Keys(false);
    if (!leftover || keys.size() < 3 ||
        !std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; })) {
        return;
    }
    const auto spellings = loanwords_->ForEnglish(keys, 1);
    if (spellings.empty()) return;
    std::wstring written = loanwords_->WrittenAs(keys).value_or(keys);
    std::vector<PhraseCandidate> words;
    PhraseCandidate english{std::move(written), 0, PhraseCandidate::Kind::English, false};
    english.reading = Reading();
    words.push_back(std::move(english));
    PhraseCandidate katakana{spellings.front().katakana, 0, PhraseCandidate::Kind::Katakana, false};
    katakana.reading = Reading();
    words.push_back(std::move(katakana));
    // Keys romaji leaves letters of (the g of meeting) are the English
    // word's: it comes first.
    auto& candidates = phrases_.front().candidates;
    std::size_t at = 0;
    for (auto& word : words) {
        const bool seen = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const PhraseCandidate& c) { return c.text == word.text; });
        if (!seen) candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(at++), std::move(word));
    }
}

std::optional<std::wstring> JapaneseComposer::RunTogetherEnglish(bool* foreign) const {
    const ChunkJudge* splitter = chunkJudge_ ? chunkJudge_ : splitter_;
    if (!splitter || !converter_ || !table_) return std::nullopt;
    if (foreign) *foreign = false;
    const std::wstring keys = Keys(false);
    const auto kanaOf = [&](std::wstring_view part) -> std::optional<std::wstring> {
        std::wstring kana;
        for (const auto& token : ParseRomaji(*table_, part)) {
            if (token.leftover) return std::nullopt;
            kana += token.kana;
        }
        return ApplyTypingStyle(kana);
    };
    // Each run of keys between punctuation on its own (a post's sentences
    // and clauses: kyounolunchha..., ...). The pieces' choices, and as the
    // sentence model reads them, for the sentence check.
    constexpr std::size_t kChoices = 6;
    const bool check = reranker_ && sentenceCheck_;
    std::vector<std::vector<std::wstring>> choices;
    std::vector<std::vector<std::wstring>> read;
    bool split = false;
    for (std::size_t start = 0; start < keys.size();) {
        std::size_t end = keys.find_first_of(L".,!?", start);
        if (end == std::wstring::npos) end = keys.size();
        const std::uint16_t context = start == 0 ? context_ : 0;
        if (end > start) {
            const std::wstring_view part = std::wstring_view(keys).substr(start, end - start);
            const auto pieces = splitter->Split(part);
            if (pieces) {
                split = true;
            }
            std::vector<ChunkJudge::Piece> whole;
            if (!pieces) {
                const auto kana = kanaOf(part);
                if (!kana) return std::nullopt;
                whole.push_back({false, std::wstring(part), *kana});
            }
            const auto& parts = pieces ? *pieces : whole;
            const bool allEnglish = std::all_of(parts.begin(), parts.end(), [](const ChunkJudge::Piece& p) { return p.english; });
            for (std::size_t i = 0; i < parts.size(); ++i) {
                const auto& piece = parts[i];
                if (piece.english) {
                    if (foreign && ChunkJudge::ForeignLetters(piece.keys)) *foreign = true;
                    choices.push_back({start == 0 && i == 0 && allEnglish ? EnglishAsWritten(piece.text) : piece.text});
                    read.push_back({EnglishForModel(piece.text, HasCapital(piece.text))});
                    continue;
                }
                // A piece after another is inside the sentence, not at its start.
                const bool inside = i > 0 || start > 0;
                if (inside && converter_) converter_->SetAtSentenceStart(false);
                const auto phrases = ConvertReading(ApplyTypingStyle(piece.text), 0, i == 0 ? context : 0, WordsWithEnglish());
                if (inside) UpdateJoinedStart();
                if (std::any_of(phrases.begin(), phrases.end(), [](const Phrase& phrase) {
                        return phrase.candidates.empty();
                    })) {
                    return std::nullopt;
                }
                choices.push_back(TopConversions(phrases, check ? kChoices : 1));
                read.push_back(choices.back());
            }
        }
        if (end < keys.size()) {
            const auto mark = kanaOf(std::wstring_view(keys).substr(end, 1));
            if (!mark) return std::nullopt;
            choices.push_back({*mark});
            read.push_back({*mark});
        }
        start = end + 1;
    }
    if (!split) return std::nullopt;
    const auto picks = check ? ReadBest(read) : std::vector<std::size_t>(choices.size(), 0);
    std::wstring text;
    for (std::size_t i = 0; i < choices.size(); ++i) text += choices[i][picks[i]];
    return text;
}

std::vector<PhraseCandidate> JapaneseComposer::EnglishCandidates(const std::wstring& written,
                                                                 const std::wstring& reading) const {
    std::vector<PhraseCandidate> candidates;
    const auto add = [&](std::wstring text, PhraseCandidate::Kind kind) {
        if (text.empty()) return;
        const bool seen = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const PhraseCandidate& c) { return c.text == text; });
        if (!seen) candidates.push_back({std::move(text), 0, kind, false});
    };
    add(written, PhraseCandidate::Kind::English);
    // What English Auto offers for the word.
    std::optional<EnglishSpelling::Result> spelled;
    if (englishSpelling_) {
        std::wstring lower = Keys(false);
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        spelled = englishSpelling_->Spell(lower, englishText_);
    }
    // The word typed twice in an English sentence (the the), with nothing
    // after it: taking it out, offered right after it. Not where Japanese
    // is in the sentence.
    if (spelled && spelled->repeated && !japaneseInSentence_ && !written.empty() &&
        std::all_of(written.begin(), written.end(), [](wchar_t c) { return std::iswalpha(c) != 0 || c == L'\''; })) {
        PhraseCandidate removal{L"", 0, PhraseCandidate::Kind::English, false};
        removal.repeat = true;
        candidates.push_back(std::move(removal));
    }
    // Its emoji next, as English Auto offers it (heart: ❤️).
    if (spelled && emojiOnFirstPage_) {
        for (const auto& other : spelled->others) {
            const bool emoji = !other.empty() &&
                               std::none_of(other.begin(), other.end(), [](wchar_t c) { return std::iswalnum(c) != 0; }) &&
                               std::any_of(other.begin(), other.end(), [](wchar_t c) { return c >= 0x2190; });
            if (emoji) {
                add(other, PhraseCandidate::Kind::Special);
                break;
            }
        }
    }
    add(Keys(false), PhraseCandidate::Kind::English);
    // How a little-known name is written, after the keys as typed (meetin,
    // then MEETin).
    if (chunkJudge_) {
        if (auto name = chunkJudge_->OfferedName(Keys(false))) add(std::move(*name), PhraseCandidate::Kind::English);
    }
    // The word written with a hyphen it was typed without (usbc: USB-C).
    if (chunkJudge_) {
        if (const auto hyphened = chunkJudge_->Hyphenated(Keys(false))) add(*hyphened, PhraseCandidate::Kind::English);
    }
    // Slang offered for it and slang written out, as English Auto offers
    // them (sus: suspicious); never first.
    if (spelled) {
        for (const auto& offer : spelled->slang) {
            const std::size_t before = candidates.size();
            add(offer.text, PhraseCandidate::Kind::English);
            if (candidates.size() > before) candidates.back().slang = offer.slang;
        }
    }
    // How it is written in katakana (ギットハブ), then as Japanese.
    // As written when a slip was put right (review: レビュー).
    if (loanwords_) {
        std::wstring lower = written;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        for (const auto& word : {lower, Keys(false)}) {
            for (const auto& spelling : loanwords_->ForEnglish(word, 2)) {
                add(spelling.katakana, PhraseCandidate::Kind::Katakana);
            }
        }
    }
    // Symbols by its name, after its katakana (heart: the outlined and
    // filled hearts).
    if (special_ && specialOptions_.symbols) {
        std::wstring lower = Keys(false);
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        for (auto& symbol : special_->SymbolsNamed(lower)) add(std::move(symbol), PhraseCandidate::Kind::Special);
    }
    if (converter_ && LeftoverLetters(reading) == 0) {
        std::wstring japanese;
        for (const auto& phrase : ConvertReading(reading, 0, context_, WordsWithEnglish())) {
            if (phrase.candidates.empty()) {
                japanese.clear();
                break;
            }
            japanese += phrase.candidates.front().text;
        }
        add(std::move(japanese), PhraseCandidate::Kind::Dictionary);
        for (auto& kana : KanaCandidates(reading)) add(std::move(kana.text), kana.kind);
        // A reading of one phrase: its other words too (i: 位, 胃, 意).
        auto phrases = ConvertReading(reading, 0, context_, WordsWithEnglish());
        if (phrases.size() == 1) {
            for (auto& candidate : phrases.front().candidates) add(std::move(candidate.text), candidate.kind);
        }
    }
    // The word spelled otherwise than typed, its letters not only their case
    // or a hyphen (recieve: receive; not iphone: iPhone, usbc: USB-C): a
    // "maybe", and the keys as typed beside it.
    const auto letters = [](std::wstring_view text) {
        std::wstring out;
        for (const wchar_t c : text) {
            if (std::iswalnum(c)) out.push_back(static_cast<wchar_t>(std::towlower(c)));
        }
        return out;
    };
    const std::wstring typed = Keys(false);
    if (!candidates.empty() && candidates.front().kind == PhraseCandidate::Kind::English &&
        letters(candidates.front().text) != letters(typed)) {
        const auto asTyped = std::find_if(candidates.begin() + 1, candidates.end(),
                                          [&](const PhraseCandidate& c) { return c.text == typed; });
        if (asTyped != candidates.end()) {
            candidates.front().englishCorrection = true;
            asTyped->asTyped = true;
        }
    }
    return candidates;
}

std::wstring JapaneseComposer::EnglishAsWritten(std::wstring written) const {
    const bool lower = std::all_of(written.begin(), written.end(), [](wchar_t c) { return !(c >= L'A' && c <= L'Z'); });
    if (sentenceStart_ && lower && !written.empty() && written.front() >= L'a' && written.front() <= L'z') {
        written.front() = static_cast<wchar_t>(written.front() - L'a' + L'A');
    }
    return written;
}

std::wstring JapaneseComposer::MarksAfterEnglish(std::wstring_view keys) const {
    std::wstring marks(keys);
    if (!japaneseInSentence_) return marks;
    for (auto& c : marks) {
        c = c == L'.' ? L'。' : c == L',' ? L'、' : c == L'!' ? L'！' : L'？';
    }
    return ApplyTypingStyle(std::move(marks));
}

bool JapaneseComposer::ChoseEnglish() const noexcept {
    if (!IsConverted() || preview_) return false;
    if (const auto* whole = WholeChoice()) return whole->kind == PhraseCandidate::Kind::English;
    return std::all_of(phrases_.begin(), phrases_.end(), [](const PhraseState& phrase) {
        return !phrase.form && phrase.selected < phrase.candidates.size() &&
               phrase.candidates[phrase.selected].kind == PhraseCandidate::Kind::English;
    });
}

void JapaneseComposer::SettleChunk() {
    if (!Typing()) return;
    SettledChunk chunk;
    chunk.typed = typedCaseKept_ && !typedCase_.empty() ? typedCase_ : Keys(false);
    const std::wstring keys = Keys(false);
    std::size_t word = keys.size();
    while (word > 0 && std::wstring_view(L".,!?").find(keys[word - 1]) != std::wstring_view::npos) --word;
    chunk.keys = keys.substr(0, word);
    chunk.marksOnly = word == 0;
    chunk.picked = PredictionChosen() || std::any_of(phrases_.begin(), phrases_.end(), [](const PhraseState& p) {
                       return p.selected != 0 || p.chosenByCheck;
                   });
    chunk.locked = chunk.picked || TypedWithCapitals(chunk.keys).has_value();
    chunk.sentenceStart = sentenceStart_;
    // Both ways, for when the sentence judges the chunk again.
    const std::wstring marks = MarksAfterEnglish(std::wstring_view(keys).substr(word));
    if (word > 0) {
        const auto judged = JudgeChunk(chunk.keys);
        // Taken as Japanese, as English writes it all the same, for when the
        // sentence takes it as English after all (jira: Jira).
        const std::wstring written = TypedWithCapitals(chunk.keys).value_or(
            judged.english || !chunkJudge_ ? judged.written : chunkJudge_->EnglishWritten(chunk.keys));
        chunk.named = HasCapital(written);
        const std::wstring asWritten = EnglishAsWritten(written);
        chunk.capitalized = asWritten != written;
        chunk.english = asWritten + marks;
        chunk.either = judged.either;
    } else {
        chunk.english = marks;
    }
    if (converter_ && word > 0) {
        // The reading as converted: with a slip undone (hirzite: ひらいて),
        // so that Enter reads the chunk again as it was put right.
        const std::wstring typedReading = ApplyTypingStyle(Reading());
        const bool slipUndone = IsConverted() && !conversionReading_.empty() && conversionReading_ != typedReading &&
                                conversionReading_.size() + 2 >= typedReading.size();
        chunk.reading = slipUndone ? conversionReading_ : typedReading;
        chunk.context = context_;
        auto phrases = ConvertReading(chunk.reading, 0, context_, WordsWithEnglish());
        // Only kanji in the range set, as the chunk showed (れんらく, not 連らく).
        for (auto& phrase : phrases) {
            PhraseState state{phrase.begin, phrase.length, std::move(phrase.candidates)};
            KeepKanjiInRange(state, chunk.reading);
            phrase.candidates = std::move(state.candidates);
        }
        for (const auto& phrase : phrases) {
            if (!phrase.candidates.empty()) chunk.japanese += phrase.candidates.front().text;
        }
        // Its next conversions, for the sentence check on Enter.
        constexpr std::size_t kChoices = 6;
        if (reranker_ && sentenceCheck_) {
            chunk.japaneseChoices = TopConversions(phrases, kChoices);
            // And whole texts that break the reading elsewhere (何|時ですか
            // for 何|児ですか), read apart as Japanese mode's check reads
            // them; not those with a number, which the number style writes.
            if (converter_ && wholeSentences_ > 0) {
                SyncEdgeContext();
                for (const auto& guess : converter_->BestSentences(chunk.reading, wholeSentences_, context_, userDictionary_)) {
                    const bool number = std::any_of(guess.text.begin(), guess.text.end(), [](wchar_t c) {
                        return IsDigit(c) || std::wstring_view(L"〇一二三四五六七八九十百千万億兆").find(c) != std::wstring_view::npos;
                    });
                    if (number || !InKanjiRange(guess.text)) continue;
                    if (std::find(chunk.japaneseChoices.begin(), chunk.japaneseChoices.end(), guess.text) == chunk.japaneseChoices.end()) {
                        chunk.japaneseWholes.push_back(guess.text);
                    }
                }
            }
        }
    }
    if (chunk.japanese.empty()) chunk.japanese = ApplyTypingStyle(Reading());
    if (settled_.empty()) {
        spaceBeforeSentence_ = spaceAfterEnglish_;
        // What was written before the sentence: its chunks are added to the
        // recent text as they settle.
        mixContext_ = recentText_;
    }
    const bool asTyped = !IsConverted();
    auto text = CommitChunk();
    while (!text.empty() && text.back() == L' ') text.pop_back();
    chunk.isEnglish = lastChunkEnglish_;
    lastChunkEnglish_ = false;
    // Japanese settled as typed (Enter) stays as typed, and an address.
    if (asTyped && !chunk.isEnglish) chunk.locked = chunk.picked = true;
    if (IsAddressTyping(chunk.keys)) chunk.locked = chunk.picked = true;
    // A slip put right in its kana: Enter, which reads the chunk's reading
    // again, would take it back.
    if (!chunk.isEnglish && !asTyped && slipUndone_) chunk.locked = true;
    // English run into the Japanese (meetingde: meetingで) was split off
    // as it is: the sentence check and the run converted again on Enter,
    // which read only kana, leave it.
    if (!chunk.isEnglish && std::any_of(text.begin(), text.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
        })) {
        chunk.locked = true;
    }
    // As it was taken (a candidate picked stays).
    (chunk.isEnglish ? chunk.english : chunk.japanese) = text;
    // What was taken comes first.
    const auto taken = std::find(chunk.japaneseChoices.begin(), chunk.japaneseChoices.end(), chunk.japanese);
    if (taken != chunk.japaneseChoices.end()) chunk.japaneseChoices.erase(taken);
    chunk.japaneseChoices.insert(chunk.japaneseChoices.begin(), chunk.japanese);
    chunk.ownEnglish = chunk.english;
    lastCommit_.reset();
    settled_.push_back(std::move(chunk));
    LowerSentenceStarts();
}

void JapaneseComposer::LowerSentenceStarts() {
    for (std::size_t first = 0; first < settled_.size();) {
        std::size_t end = first + 1;
        while (end < settled_.size() && !settled_[end].sentenceStart) ++end;
        const bool japanese = std::any_of(settled_.begin() + static_cast<std::ptrdiff_t>(first),
                                          settled_.begin() + static_cast<std::ptrdiff_t>(end),
                                          [](const SettledChunk& c) { return !c.isEnglish && !c.marksOnly; });
        for (std::size_t i = first; japanese && i < end; ++i) {
            auto& chunk = settled_[i];
            if (!chunk.capitalized || !chunk.isEnglish || chunk.english.empty()) continue;
            const auto lower = [](std::wstring& text) {
                if (!text.empty() && text.front() >= L'A' && text.front() <= L'Z') {
                    text.front() = static_cast<wchar_t>(text.front() - L'A' + L'a');
                }
            };
            lower(chunk.english);
            lower(chunk.ownEnglish);
            chunk.capitalized = false;
        }
        first = end;
    }
}

std::optional<std::wstring> JapaneseComposer::TypedLetters() const {
    // Only with the caret at the end, where the letters and kana agree.
    if (!chunkJudge_ || !Typing() || IsConverted() || caret_ != units_.size() || HasAscii()) return std::nullopt;
    const bool englishSoFar = !settled_.empty() && std::none_of(settled_.begin(), settled_.end(), [](const SettledChunk& c) {
                                  return !c.marksOnly && !c.isEnglish;
                              });
    // Letters no kana took, in either width.
    const bool leftover = std::any_of(units_.begin(), units_.end(), [](const Unit& unit) {
        return !unit.ascii && LeftoverLetters(ToFullWidthAscii(unit.kana)) > 0;
    });
    // Letters romaji keeps for small kana in what reads as English (lol,
    // not ぉl).
    const auto foreignWord = [&] {
        std::wstring keys = Keys(true);
        for (auto& c : keys) c = static_cast<wchar_t>(std::towlower(c));
        if (keys.empty() || !ChunkJudge::ForeignLetters(keys, false) ||
            !std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; })) {
            return false;
        }
        return chunkJudge_->MayStartEnglishWord(keys) || JudgeChunk(keys).english;
    };
    const std::wstring keys = Keys(true);
    // A capital typed in it makes it English as typed (Kikai, GitHub): its
    // letters from the start, not kana that Space turns into them.
    const bool asTyped = typedCaseKept_ && typedCase_.size() == keys.size();
    const bool capital = asTyped && std::any_of(typedCase_.begin(), typedCase_.end(),
                                                [](wchar_t c) { return c >= L'A' && c <= L'Z'; });
    // After English alone, letters while they may be English too (see you
    // on); keys no English word starts with that read clearly as Japanese
    // (SNS, then nitoukoushita) show as Japanese, and Enter commits them so.
    // The keys still pending are left out (nit is に and t); once shown so,
    // the chunk stays Japanese while it is typed on. A particle right after
    // one English chunk too (NHKの, SNS に), not in an English sentence (see
    // you on mo...).
    const auto clearlyJapanese = [&] {
        if (japaneseAfterEnglish_) return true;
        std::wstring lower = keys;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        if (pending_.size() < lower.size()) lower.resize(lower.size() - pending_.size());
        if (lower.size() < 2 ||
            !std::all_of(lower.begin(), lower.end(), [](wchar_t c) { return (c >= L'a' && c <= L'z') || c == L'-'; })) {
            return false;
        }
        if (lower.size() <= 2) {
            constexpr std::wstring_view kParticles[] = {L"no", L"ni", L"ga", L"wo", L"de", L"ha", L"wa", L"mo", L"to", L"he", L"ya"};
            const auto english = std::count_if(settled_.begin(), settled_.end(), [](const SettledChunk& c) { return c.isEnglish; });
            return english == 1 && std::find(std::begin(kParticles), std::end(kParticles), lower) != std::end(kParticles) &&
                   !JudgeChunk(lower).english;
        }
        if (chunkJudge_->MayStartEnglishWord(lower)) return false;
        const auto judged = JudgeChunk(lower);
        japaneseAfterEnglish_ = !judged.english && !judged.either;
        return japaneseAfterEnglish_;
    };
    if (!capital && !(englishSoFar && !clearlyJapanese()) && !leftover && !foreignWord()) return std::nullopt;
    if (asTyped) return typedCase_;
    return keys;
}

std::optional<std::wstring> JapaneseComposer::MixLiveSplit() const { return MixSplitShown(true); }

std::optional<std::wstring> JapaneseComposer::AddressShown() const {
    if (!Typing() || IsConverted() || caret_ != units_.size() || HasAscii()) return std::nullopt;
    std::wstring keys = Keys(true);
    if (!IsAddressTyping(keys)) return std::nullopt;
    if (typedCaseKept_ && typedCase_.size() == keys.size()) return typedCase_;
    return keys;
}

std::optional<std::wstring> JapaneseComposer::LettersShown() const {
    const auto letters = TypedLetters();
    if (!letters) return std::nullopt;
    if (const auto split = MixSplitShown(false)) return split;
    return letters;
}

std::optional<std::wstring> JapaneseComposer::MixSplitShown(bool convert) const {
    if (!chunkJudge_) return std::nullopt;
    // Typed with a capital, or an English word still being typed (meetin):
    // the letters.
    std::wstring keys = Keys(true);
    if (TypedWithCapitals(keys)) return std::nullopt;
    for (auto& c : keys) c = static_cast<wchar_t>(std::towlower(c));
    if (keys.empty() || chunkJudge_->MayStartEnglishWord(keys)) return std::nullopt;
    // The letters of a syllable still being typed stay as typed: the keys
    // still pending, and letters no kana took at the end (full-width in the
    // reading, ｋ).
    const std::wstring reading = Reading();
    std::size_t pending = pending_.size();
    const std::size_t pendingKeys = pending;
    std::size_t at = reading.size();
    for (; at > 0 && pending < keys.size(); --at) {
        const wchar_t c = reading[at - 1];
        const bool letter = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= 0xFF21 && c <= 0xFF3A) ||
                            (c >= 0xFF41 && c <= 0xFF5A);
        if (!letter) break;
        ++pending;
    }
    // Letters no kana took are in what is still being typed (github's b).
    const bool leftoverAtEnd = pending > pendingKeys;
    // A small tsu at the end waits for its kana too (app: あっp).
    if (at > 0 && reading[at - 1] == L'\u3063' && pending == pendingKeys && pending < keys.size()) ++pending;
    std::wstring core = keys.substr(0, keys.size() - pending);
    std::wstring marks;
    while (!core.empty() && std::wstring_view(L".,!?").find(core.back()) != std::wstring_view::npos) {
        marks.insert(marks.begin(), core.back());
        core.pop_back();
    }
    if (core.empty()) return std::nullopt;
    std::size_t& tailTaken = liveSplitTailTaken_;
    if (core != liveSplitKeys_ || convert != liveSplitConverted_ || pending != liveSplitPending_) {
        liveSplitPending_ = pending;
        liveSplitKeys_ = core;
        liveSplitConverted_ = convert;
        liveSplitText_.reset();
        // An n at the end of what is shown is ん when a key came after it
        // (kakunin then s: かくにん), as the reading has it.
        const bool nTaken = pending > 0 || !marks.empty();
        const auto kanaOf = [&](std::wstring_view part, bool atEnd = false) -> std::optional<std::wstring> {
            std::wstring kana;
            const auto tokens = ParseRomaji(*table_, part);
            for (std::size_t t = 0; t < tokens.size(); ++t) {
                const auto& token = tokens[t];
                if (token.leftover) {
                    if (atEnd && nTaken && t + 1 == tokens.size() && token.kana == L"n") {
                        kana += L'\u3093';
                        continue;
                    }
                    return std::nullopt;
                }
                kana += token.kana;
            }
            return ApplyTypingStyle(kana);
        };
        std::wstring text;
        bool english = false, japanese = false, ok = true;
        tailTaken = 0;
        // English run into Japanese, as Space splits it (meetinghananiji:
        // meetingは何時), each run between punctuation on its own: first how
        // each run splits, then, when English and Japanese are both in them,
        // the Japanese converted (Japanese alone shows otherwise).
        struct Run {
            std::size_t start, end;
            std::vector<ChunkJudge::Piece> parts;
        };
        std::vector<Run> runs;
        for (std::size_t start = 0; start < core.size();) {
            std::size_t end = core.find_first_of(L".,!?", start);
            if (end == std::wstring::npos) end = core.size();
            runs.push_back({start, end, {}});
            if (end > start) {
                const std::wstring_view run = std::wstring_view(core).substr(start, end - start);
                constexpr std::size_t kTyping = 16;
                constexpr std::size_t kShortest = 4;
                const bool last = end == core.size();
                // An English word being typed at the end (...github,
                // ...slac): it as typed, after what splits or reads as kana;
                // the longest such word.
                const auto englishAtEnd = [&]() -> std::vector<ChunkJudge::Piece> {
                    std::vector<ChunkJudge::Piece> found;
                    const std::size_t from = std::max(kShortest, run.size() > kTyping ? run.size() - kTyping : 0);
                    for (std::size_t cut = from; found.empty() && cut + 2 <= run.size(); ++cut) {
                        const std::wstring rest(run.substr(cut));
                        // A common English word's start (sla: slack).
                        const std::wstring tail = last && marks.empty() ? keys.substr(keys.size() - pending) : std::wstring{};
                        if (!chunkJudge_->MayStartEnglishWord(rest + tail)) continue;
                        const std::wstring_view head = run.substr(0, cut);
                        if (auto pieces = chunkJudge_->Split(head)) {
                            found = std::move(*pieces);
                        } else if (const auto kana = kanaOf(head)) {
                            found.push_back({false, std::wstring(head), *kana});
                        } else {
                            continue;
                        }
                        found.push_back({true, rest, rest});
                    }
                    // Else the fewest letters at the end no kana takes
                    // (slac: not wo too), after what splits or reads as kana.
                    for (std::size_t cut = run.size() - 2; found.empty() && cut >= from && cut < run.size(); --cut) {
                        const std::wstring rest(run.substr(cut));
                        if (kanaOf(rest)) continue;
                        const std::wstring_view head = run.substr(0, cut);
                        if (auto headPieces = chunkJudge_->Split(head)) {
                            found = std::move(*headPieces);
                        } else if (const auto kana = kanaOf(head)) {
                            found.push_back({false, std::wstring(head), *kana});
                        } else {
                            continue;
                        }
                        found.push_back({true, rest, rest});
                    }
                    return found;
                };
                std::vector<ChunkJudge::Piece> parts;
                // An English word that ends in keys still pending (github's
                // b, then n): split with them, as Space will once they are
                // taken.
                if (last && marks.empty() && leftoverAtEnd) {
                    const std::wstring tail = keys.substr(keys.size() - pending);
                    for (std::size_t k = tail.size(); k > 0 && parts.empty(); --k) {
                        auto pieces = chunkJudge_->Split(std::wstring(run) + tail.substr(0, k));
                        if (!pieces || pieces->empty() || !pieces->back().english) continue;
                        parts = std::move(*pieces);
                        tailTaken = k;
                    }
                }
                if (!parts.empty()) {
                } else if (auto pieces = chunkJudge_->Split(run)) {
                    parts = std::move(*pieces);
                } else if (last && leftoverAtEnd && !(parts = englishAtEnd()).empty()) {
                } else if (const auto kana = kanaOf(run, last)) {
                    parts.push_back({false, std::wstring(run), *kana});
                } else {
                    // Still being typed (urlwokakunins): split as far as it
                    // splits, the rest as kana; else an English word at the
                    // end; else this run as letters, the runs before it as
                    // they split.
                    for (std::size_t cut = run.size() - 1; cut >= kShortest && cut + kTyping >= run.size(); --cut) {
                        const auto rest = kanaOf(run.substr(cut), last);
                        if (!rest) continue;
                        if (auto cutPieces = chunkJudge_->Split(run.substr(0, cut))) {
                            parts = std::move(*cutPieces);
                            parts.push_back({false, std::wstring(run.substr(cut)), *rest});
                            break;
                        }
                    }
                    if (parts.empty()) parts = englishAtEnd();
                    // Else letters no kana takes at the start with a
                    // particle after them (pcwoka: pc, then をか), the fewest
                    // of them; not an English word still being typed (docke).
                    constexpr std::wstring_view kParticles[] = {L"wo", L"ga", L"ni", L"de", L"ha", L"wa", L"no", L"mo", L"to"};
                    const std::wstring typedTail = last && marks.empty() ? keys.substr(keys.size() - pending) : std::wstring{};
                    const bool englishGoingOn = chunkJudge_->MayStartEnglishWord(std::wstring(run) + typedTail);
                    for (std::size_t cut = 2; !englishGoingOn && parts.empty() && cut < run.size() && cut <= kTyping; ++cut) {
                        if (kanaOf(run.substr(0, cut))) continue;
                        const std::wstring_view after = run.substr(cut);
                        if (std::none_of(std::begin(kParticles), std::end(kParticles),
                                         [&](std::wstring_view particle) { return after.starts_with(particle); })) {
                            continue;
                        }
                        const auto rest = kanaOf(after, last);
                        if (!rest) continue;
                        parts.push_back({true, std::wstring(run.substr(0, cut)), std::wstring(run.substr(0, cut))});
                        parts.push_back({false, std::wstring(run.substr(cut)), *rest});
                    }
                    if (parts.empty()) parts.push_back({true, std::wstring(run), std::wstring(run)});
                }
                runs.back().parts = std::move(parts);
            }
            start = end + 1;
        }
        const auto hasPiece = [&](bool inEnglish) {
            return std::any_of(runs.begin(), runs.end(), [&](const Run& r) {
                return std::any_of(r.parts.begin(), r.parts.end(), [&](const ChunkJudge::Piece& p) { return p.english == inEnglish; });
            });
        };
        if (!hasPiece(true) || !hasPiece(false)) runs.clear();
        for (const auto& [start, end, parts] : runs) {
            if (!ok) break;
            if (end > start) {
                // English with Japanese stays in small letters at the start
                // of a sentence too (LowerSentenceStarts).
                const bool allEnglish = std::all_of(parts.begin(), parts.end(), [](const ChunkJudge::Piece& p) { return p.english; });
                for (std::size_t i = 0; i < parts.size() && ok; ++i) {
                    const auto& piece = parts[i];
                    if (piece.english) {
                        english = true;
                        text += !convert ? piece.keys : start == 0 && i == 0 && allEnglish ? EnglishAsWritten(piece.text) : piece.text;
                        continue;
                    }
                    japanese = true;
                    if (!convert) {
                        text += ApplyTypingStyle(piece.text);
                        continue;
                    }
                    // A piece after another is inside the sentence, not at its start.
                    const bool inside = i > 0 || start > 0;
                    if (inside && converter_) converter_->SetAtSentenceStart(false);
                    const auto piecePhrases = ConvertReading(ApplyTypingStyle(piece.text), 0, inside ? 0 : context_, WordsWithEnglish());
                    if (inside) UpdateJoinedStart();
                    for (const auto& phrase : piecePhrases) {
                        if (phrase.candidates.empty()) {
                            ok = false;
                            break;
                        }
                        text += phrase.candidates.front().text;
                    }
                }
            }
            if (end < core.size()) {
                const auto mark = kanaOf(std::wstring_view(core).substr(end, 1));
                text += mark ? *mark : std::wstring(1, core[end]);
            }
        }
        if (ok && english && japanese) liveSplitText_ = std::move(text);
    }
    if (!liveSplitText_) return std::nullopt;
    return *liveSplitText_ + MarksAfterEnglish(marks) + keys.substr(keys.size() - pending + tailTaken);
}

std::optional<std::wstring> JapaneseComposer::MixLiveText() const {
    if (!mixLive_ || !chunkJudge_ || !converter_ || !Typing() || IsConverted() || caret_ != units_.size() ||
        mixKanaShown_ || HasAscii() || KanaTyped() || AddressShown()) {
        return std::nullopt;
    }
    // English run into Japanese shows as Space will split it.
    if (const auto split = MixLiveSplit()) return split;
    if (TypedLetters()) return std::nullopt;
    // The letters of a syllable still being typed (kyouhas) stay as typed.
    const std::wstring reading = Reading();
    std::size_t end = reading.size();
    const auto letter = [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'\xFF21' && c <= L'\xFF3A') ||
               (c >= L'\xFF41' && c <= L'\xFF5A');
    };
    while (end > 0 && letter(reading[end - 1])) --end;
    if (end == 0 || LeftoverLetters(std::wstring_view(reading).substr(0, end)) > 0) return std::nullopt;
    // Clearly Japanese: the chunk judge takes the keys for Japanese and not
    // as English too (me, to may be either: they stay as typed).
    // The keys still being typed count (push is not ぷ).
    std::wstring keys = Keys(true);
    for (auto& c : keys) c = static_cast<wchar_t>(std::towlower(c));
    while (!keys.empty() && std::wstring_view(L".,!?").find(keys.back()) != std::wstring_view::npos) keys.pop_back();
    if (keys.empty()) return std::nullopt;
    // Long keys are judged without the syllable still being typed (...k):
    // with it they read as no kana, and undoing a slip in all of them at
    // each key costs much; the keys before it are judged once.
    constexpr std::size_t kLongKeys = 20;
    std::wstring_view judgedKeys = keys;
    if (keys.size() >= kLongKeys && pending_.size() < keys.size()) judgedKeys.remove_suffix(pending_.size());
    const auto judged = JudgeChunk(judgedKeys);
    if (judged.english || judged.either || TypedWithCapitals(keys)) return std::nullopt;
    // Keys an English word may still be typed from (meetin) stay as typed,
    // so the display does not turn Japanese, then English.
    if (chunkJudge_->MayStartEnglishWord(keys)) return std::nullopt;
    const std::wstring kana = reading.substr(0, end);
    if (kana != mixLiveReading_ || mixLiveContext_ != context_) {
        // Typed on from what showed: the phrases before the last few stay as
        // they showed and the rest is converted after them, as Japanese
        // mode's live conversion does, so a key costs the same however long
        // the sentence; typed otherwise, all of it.
        std::vector<Phrase> phrases;
        std::size_t from = 0;
        std::uint16_t context = context_;
        std::vector<std::wstring> words = WordsWithEnglish();
        if (mixLiveContext_ == context_ && kana.size() > mixLiveReading_.size() && kana.starts_with(mixLiveReading_) &&
            mixLivePhrases_.size() > liveOpenPhrases_) {
            phrases.assign(mixLivePhrases_.begin(), mixLivePhrases_.end() - static_cast<std::ptrdiff_t>(liveOpenPhrases_));
            from = phrases.back().begin + phrases.back().length;
            std::wstring settled;
            for (const auto& phrase : phrases) settled += phrase.candidates.front().text;
            context = ContextAfter(settled, phrases.back().candidates.front().rightId);
            AddContentWords(settled, words);
        }
        for (auto& phrase : ConvertReading(kana, from, context, words)) {
            if (phrase.candidates.empty()) return std::nullopt;
            phrases.push_back(std::move(phrase));
        }
        std::wstring text;
        for (const auto& phrase : phrases) text += phrase.candidates.front().text;
        mixLiveReading_ = kana;
        mixLiveContext_ = context_;
        mixLiveText_ = std::move(text);
        mixLivePhrases_ = std::move(phrases);
    }
    // The keys still pending follow, as typed (3時 then k), as in Japanese
    // mode's live conversion.
    return mixLiveText_ + reading.substr(end) + PendingText();
}

std::wstring JapaneseComposer::SettledText() const {
    return JoinChunks(settled_);
}

std::wstring JapaneseComposer::JoinChunks(const std::vector<SettledChunk>& chunks) {
    std::wstring text;
    bool englishBefore = false;
    for (const auto& chunk : chunks) {
        const bool english = chunk.marksOnly ? englishBefore : chunk.isEnglish;
        // A word taken out (the the): nothing, and no space for it.
        if (english && !chunk.marksOnly && chunk.english.empty()) continue;
        // English words apart; punctuation and Japanese against what is before.
        if (!text.empty() && englishBefore && english && !chunk.marksOnly) text.push_back(L' ');
        text += english ? chunk.english : chunk.japanese;
        englishBefore = english;
    }
    return text;
}

std::vector<std::wstring> JapaneseComposer::WordsWithEnglish(std::size_t before) const {
    // The words before, and the English typed right before what is
    // converted (error を直す): conversion reads that through English. Only
    // that one: English further off is no object of the verb (AI に…を直す).
    auto words = contextWords_;
    if (before > settled_.size()) before = settled_.size();
    if (before > 0 && settled_[before - 1].isEnglish && settled_[before - 1].english.size() >= 2) {
        words.push_back(settled_[before - 1].english);
    }
    return words;
}

void JapaneseComposer::ConvertJapaneseRuns() {
    if (!converter_) return;
    const auto convertible = [](const SettledChunk& chunk) {
        return !chunk.isEnglish && !chunk.marksOnly && !chunk.locked && !chunk.reading.empty() &&
               std::none_of(chunk.reading.begin(), chunk.reading.end(), IsDigit);
    };
    for (std::size_t first = 0; first < settled_.size();) {
        if (!convertible(settled_[first])) {
            ++first;
            continue;
        }
        std::size_t end = first;
        std::wstring reading;
        std::vector<std::size_t> breaks;
        while (end < settled_.size() && convertible(settled_[end])) {
            if (end > first) breaks.push_back(reading.size());
            reading += settled_[end].reading;
            ++end;
        }
        if (end - first < 2) {
            first = end;
            continue;
        }
        SyncEdgeContext();
        const auto phrases =
            converter_->Convert(reading, {}, settled_[first].context, WordsWithEnglish(first), userDictionary_, breaks);
        // Each chunk's phrases, as the breaks keep them.
        std::size_t p = 0, at = 0;
        bool whole = true;
        std::vector<std::vector<Phrase>> perChunk(end - first);
        for (std::size_t c = first; c < end && whole; ++c) {
            const std::size_t chunkEnd = at + settled_[c].reading.size();
            while (p < phrases.size() && phrases[p].begin + phrases[p].length <= chunkEnd) {
                PhraseState state{phrases[p].begin, phrases[p].length, phrases[p].candidates};
                // As a chunk's own conversion has them (AddPhrases): what the
                // user chose before, loanwords, katakana typed with a vowel
                // for ー (first: nyuusu was shown ニュース).
                const auto phraseReading = std::wstring_view(reading).substr(state.begin, state.length);
                if (learning_) learning_->Reorder(LearningReading(reading, state.begin, state.length), state.candidates);
                AddLoanwords(state.candidates, phraseReading);
                AddLongVowelWords(state.candidates, phraseReading, true);
                StyleKanaNumbers(state.candidates);
                KeepKanjiInRange(state, reading);
                perChunk[c - first].push_back({state.begin, state.length, std::move(state.candidates)});
                ++p;
            }
            whole = !perChunk[c - first].empty() && perChunk[c - first].back().begin + perChunk[c - first].back().length == chunkEnd;
            at = chunkEnd;
        }
        if (!whole) {
            first = end;
            continue;
        }
        constexpr std::size_t kChoices = 6;
        for (std::size_t c = first; c < end; ++c) {
            auto& chunk = settled_[c];
            std::wstring text;
            for (const auto& phrase : perChunk[c - first]) {
                if (phrase.candidates.empty()) {
                    text.clear();
                    break;
                }
                text += phrase.candidates.front().text;
            }
            if (text.empty() || text == chunk.japanese) continue;
            // The chunk as the whole run reads it first; what it was before
            // stays a choice for the sentence check.
            auto choices = TopConversions(perChunk[c - first], kChoices);
            for (const auto& before : chunk.japaneseChoices) {
                if (std::find(choices.begin(), choices.end(), before) == choices.end()) choices.push_back(before);
            }
            chunk.japanese = std::move(text);
            chunk.japaneseChoices = std::move(choices);
        }
        first = end;
    }
}

void JapaneseComposer::CheckMixedSentence() {
    // Each chunk as taken first, then its other conversions when it was
    // taken as Japanese. Picked or typed with capitals stays as it is. The
    // model knows little English: an English chunk is read as a common
    // loanword or それ (EnglishForModel).
    std::vector<std::vector<std::wstring>> parts;
    for (const auto& chunk : settled_) {
        if (chunk.isEnglish && !chunk.marksOnly) {
            parts.push_back({EnglishForModel(chunk.english, chunk.named)});
        } else if (chunk.marksOnly || chunk.locked || chunk.japaneseChoices.empty()) {
            parts.push_back({chunk.japanese});
        } else {
            parts.push_back(chunk.japaneseChoices);
        }
    }
    // And the sentence with one chunk's whole reading broken elsewhere,
    // the others as taken first.
    std::vector<std::wstring> wholes;
    std::vector<std::pair<std::size_t, std::wstring>> wholeOf;
    for (std::size_t i = 0; i < settled_.size(); ++i) {
        const auto& chunk = settled_[i];
        if (chunk.isEnglish || chunk.marksOnly || chunk.locked) continue;
        for (const auto& guess : chunk.japaneseWholes) {
            std::wstring text;
            for (std::size_t k = 0; k < parts.size(); ++k) text += k == i ? guess : parts[k].front();
            if (std::find(wholes.begin(), wholes.end(), text) != wholes.end()) continue;
            wholes.push_back(std::move(text));
            wholeOf.emplace_back(i, guess);
        }
    }
    std::wstring written;
    const auto picks = ReadBest(parts, nullptr, false, wholes.empty() ? nullptr : &written, wholes);
    if (const auto whole = std::find(wholes.begin(), wholes.end(), written); !written.empty() && whole != wholes.end()) {
        const auto& [i, guess] = wholeOf[static_cast<std::size_t>(whole - wholes.begin())];
        settled_[i].japanese = guess;
        parts[i] = {guess};
    }
    for (std::size_t i = 0; i < settled_.size(); ++i) {
        if (picks[i] > 0) settled_[i].japanese = parts[i][picks[i]];
    }
    // A chunk taken as Japanese that reads as English too (data: 立たを
    // 更新する) becomes English when the sentence reads clearly better with
    // the word in it, as Japanese writes it in katakana (データを更新する).
    // A word without a katakana form stays (mou: もう); それ would fit any.
    if (!loanwords_) return;
    constexpr double kLanguageMargin = 5.0;
    // Loanwords pack scores (Loanwords::Spelling): a common loanword is 6.
    constexpr double kCommonLoanword = 6.0;
    constexpr double kPerKnown = 8.0;
    std::vector<std::wstring> read(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) read[i] = parts[i][picks[i]];
    const auto join = [](const std::vector<std::wstring>& texts) {
        std::wstring text;
        for (const auto& part : texts) text += part;
        return text;
    };
    for (std::size_t i = 0; i < settled_.size(); ++i) {
        auto& chunk = settled_[i];
        if (chunk.isEnglish || !chunk.either || chunk.locked || chunk.marksOnly) continue;
        const auto katakana = loanwords_->ForEnglish(chunk.keys, 1);
        if (katakana.empty() || katakana.front().katakana.empty()) continue;
        // With the word in, the other chunks may read better another way
        // too (去年のデータをもとに, not データを下に): the sentence is
        // checked again with it.
        auto withWord = parts;
        for (std::size_t k = 0; k < withWord.size(); ++k) {
            if (k != i && settled_[k].isEnglish && !settled_[k].marksOnly) withWord[k] = {read[k]};
        }
        // The marks after the chunk stay with the word (kite, : カイト、,
        // not カイト), or the sentence reads worse for the comma lost.
        const auto marksAt = [](std::wstring_view text) {
            std::size_t end = text.size();
            while (end > 0 && std::wstring_view(L"、。，．！？!?,.").find(text[end - 1]) != std::wstring_view::npos) --end;
            return end;
        };
        const std::wstring word = katakana.front().katakana + read[i].substr(marksAt(read[i]));
        withWord[i] = {word};
        const auto again = ReadBest(withWord);
        std::vector<std::wstring> english(parts.size());
        for (std::size_t k = 0; k < parts.size(); ++k) english[k] = withWord[k][again[k]];
        const std::wstring both[] = {join(read), join(english)};
        const auto scores = reranker_->LogProbabilities(both, precedingText_, &rerankerMemory_);
        // The model reads katakana easily (果てに vs ヘイトに, 当て vs
        // アーテー), so a loanword less well known than a common one
        // (score 6: データ, ゲーム) takes more: ヘイト at 3 needs 29.
        const double margin = kLanguageMargin + kPerKnown * std::max(0.0, kCommonLoanword - katakana.front().score);
        if (scores.size() == 2 && scores[1] - scores[0] > margin) {
            // Its reading spells the word itself (manga: マンガ): Japanese,
            // in katakana.
            const auto reading = ToKatakana(chunk.reading);
            const bool japanese = std::wstring_view(reading).substr(0, marksAt(reading)) == katakana.front().katakana;
            if (japanese) {
                chunk.japanese = word;
            } else {
                chunk.isEnglish = true;
            }
            for (std::size_t k = 0; k < parts.size(); ++k) {
                if (k != i && !settled_[k].isEnglish && !settled_[k].marksOnly) settled_[k].japanese = english[k];
                read[k] = english[k];
            }
            if (!japanese) read[i] = EnglishForModel(chunk.english, chunk.named);
        }
    }
}

std::vector<std::vector<JapaneseComposer::MixedOption>> JapaneseComposer::MixedOptions() const {
    std::vector<std::vector<MixedOption>> options;
    for (const auto& chunk : settled_) {
        std::vector<MixedOption> own{{chunk.isEnglish, chunk.isEnglish ? chunk.english : chunk.japanese}};
        const auto add = [&](bool english, const std::wstring& text) {
            if (text.empty()) return;
            const bool listed = std::any_of(own.begin(), own.end(),
                                            [&](const MixedOption& o) { return o.english == english && o.text == text; });
            if (!listed) own.push_back({english, text});
        };
        if (!chunk.marksOnly && !chunk.locked) {
            if (chunk.either && !chunk.isEnglish) add(true, chunk.english);
            for (const auto& choice : chunk.japaneseChoices) add(false, choice);
            if (chunk.either && chunk.isEnglish) add(false, chunk.japanese);
        }
        options.push_back(std::move(own));
    }
    return options;
}

std::vector<std::wstring> JapaneseComposer::MixedSentences(std::size_t count,
                                                           std::vector<std::vector<std::size_t>>* picks) const {
    std::vector<std::wstring> texts;
    if (settled_.empty() || count == 0) return texts;
    const auto options = MixedOptions();
    std::vector<std::size_t> sizes;
    for (const auto& own : options) sizes.push_back(own.size());
    ForPicksByRank(sizes, [&](const std::vector<std::size_t>& chosen) {
        auto chunks = settled_;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const auto& option = options[i][chosen[i]];
            chunks[i].isEnglish = option.english && !chunks[i].marksOnly;
            (option.english ? chunks[i].english : chunks[i].japanese) = option.text;
        }
        auto text = JoinChunks(chunks);
        if (std::find(texts.begin(), texts.end(), text) == texts.end()) {
            texts.push_back(std::move(text));
            if (picks) picks->push_back(chosen);
        }
        return texts.size() < count;
    });
    return texts;
}

void JapaneseComposer::CheckMixedByAi() {
    if (!aiJudge_ || !mixAi_ || !aiJudge_->Writes() || settled_.empty()) return;
    std::vector<std::vector<std::size_t>> picks;
    const auto texts = MixedSentences(std::min(mixAiCount_, aihelper::kMaxSentences), &picks);
    if (texts.size() < 2) return;
    const std::wstring keys = SettledKeys();
    if (keys.size() > aihelper::kMaxCharacters) return;
    // As the model writes the text: no space after the last English word.
    std::vector<std::wstring> asked;
    for (auto text : texts) {
        while (!text.empty() && text.back() == L' ') text.pop_back();
        asked.push_back(std::move(text));
    }
    // Only the scores: the model's own writings are not offered here. Enter
    // waits for them, as an answer after the text is committed is too late,
    // but only briefly: the application waits with it, and a late answer
    // leaves the sentence as the sentence check chose it. (The evaluation,
    // which waits for every answer, waits for this one too.)
    constexpr std::chrono::milliseconds kEnterWait{500};
    const auto answer = aiWaits_ ? aiJudge_->Convert(keys, asked, mixContext_, false)
                                 : aiJudge_->ConvertWithin(keys, asked, mixContext_, false, kEnterWait);
    if (!answer || answer->scores.size() != asked.size()) return;
    std::size_t best = 0;
    for (std::size_t i = 1; i < asked.size(); ++i) {
        if (mixAiKeepNotation_ && OnlyNotationDiffers(asked[0], asked[i]) && !LanguageDiffers(asked[0], asked[i])) continue;
        if (answer->scores[i] > answer->scores[best]) best = i;
    }
    if (best == 0 || answer->scores[best] - answer->scores[0] <= mixAiMargin_) return;
    const auto options = MixedOptions();
    for (std::size_t i = 0; i < settled_.size(); ++i) {
        auto& chunk = settled_[i];
        if (chunk.marksOnly) continue;
        const auto& option = options[i][picks[best][i]];
        chunk.isEnglish = option.english;
        (option.english ? chunk.english : chunk.japanese) = option.text;
    }
}

std::wstring JapaneseComposer::SettledKeys() const {
    std::wstring keys;
    for (const auto& chunk : settled_) {
        if (!keys.empty() && !chunk.marksOnly) keys.push_back(L' ');
        keys += chunk.typed;
    }
    return keys;
}

std::wstring JapaneseComposer::CommitSentence() {
    // As the chunks were shown when each settled, before Enter judges them again.
    lastShown_ = SettledText();
    // As shown (enterAsShown_): the chunks as they were shown, not judged or
    // converted again.
    if (!enterAsShown_) {
        ConvertJapaneseRuns();
        if (reranker_ && sentenceCheck_) CheckMixedSentence();
    }
    LowerSentenceStarts();
    if (keepMixed_ > 0) {
        lastMixed_ = MixedSentences(keepMixed_);
        lastMixedKeys_ = SettledKeys();
    }
    if (!enterAsShown_) CheckMixedByAi();
    std::wstring text = SettledText();
    const bool firstJapanese = !settled_.front().marksOnly && !settled_.front().isEnglish;
    bool lastEnglish = false;
    for (const auto& chunk : settled_) {
        if (!chunk.marksOnly) lastEnglish = chunk.isEnglish;
    }
    const bool japaneseMark = !text.empty() && text.back() >= 0x3000;
    // As a chunk of its own would be: a space after English, taken back
    // when Japanese follows; the space before the sentence taken back when
    // it starts with Japanese.
    if (lastEnglish && !japaneseMark) text.push_back(L' ');
    dropSpaceBefore_ = spaceBeforeSentence_ && firstJapanese;
    spaceAfterEnglish_ = lastEnglish && !japaneseMark;
    settled_.clear();
    spaceBeforeSentence_ = false;
    SetSentenceStart(EndsSentence(text));
    lastCommit_.reset();
    return text;
}

void JapaneseComposer::RecountSentence() {
    englishRun_ = 0;
    englishText_.clear();
    japaneseInSentence_ = false;
    for (const auto& chunk : settled_) {
        if (chunk.marksOnly) continue;
        if (chunk.isEnglish) {
            ++englishRun_;
            englishText_ += chunk.english + L" ";
        } else {
            englishRun_ = 0;
            englishText_.clear();
            japaneseInSentence_ = true;
        }
    }
    LowerSentenceStarts();
}

void JapaneseComposer::JudgeWholeSentence(std::optional<ChunkJudge::Judgement>& current, std::wstring_view currentKeys) {
    if (!chunkJudge_ || settled_.empty()) return;
    std::vector<std::wstring> keys;
    std::vector<std::size_t> at;  // the settled chunk each key is from
    for (std::size_t i = 0; i < settled_.size(); ++i) {
        if (settled_[i].marksOnly) continue;
        keys.push_back(settled_[i].keys);
        at.push_back(i);
    }
    keys.emplace_back(currentKeys);
    auto whole = chunkJudge_->JudgeSentence(keys, englishCorrection_ ? englishSpelling_ : nullptr, spacelessEnglish_);
    for (std::size_t k = 0; k < whole.size(); ++k) ApplyLearnedLanguage(whole[k], keys[k]);
    // A vowel typed alone stays kana (SetSingleVowelKana) unless an English
    // word other than such a vowel is beside it: i ga itai is い, i think so
    // is I, a i u e o stays kana.
    const auto vowel = [](std::wstring_view typed) {
        return typed.size() == 1 && std::wstring_view(L"aiueo").find(typed[0]) != std::wstring_view::npos;
    };
    const auto englishWord = [&](std::size_t k) { return k < whole.size() && whole[k].english && !vowel(keys[k]); };
    // An English sentence (every chunk English): the chunks taken as
    // Japanese are written as English, and its punctuation stays as typed
    // (to, home, morning.; not と, 褒め, morning。).
    const bool englishSentence =
        std::all_of(whole.begin(), whole.end(), [](const ChunkJudge::Judgement& j) { return j.english; });
    // No English sentence (any more): each chunk's English as it was taken.
    if (!englishSentence) {
        for (auto& chunk : settled_) {
            if (!chunk.ownEnglish.empty()) chunk.english = chunk.ownEnglish;
        }
    }
    const auto typedMarks = [](const std::wstring& typed) {
        std::size_t end = typed.size();
        while (end > 0 && std::wstring_view(L".,!?").find(typed[end - 1]) != std::wstring_view::npos) --end;
        return typed.substr(end);
    };
    const auto withoutMarks = [](std::wstring text) {
        while (!text.empty() && std::wstring_view(L".,!?。、！？．，").find(text.back()) != std::wstring_view::npos) text.pop_back();
        return text;
    };
    for (std::size_t k = 0; k < at.size(); ++k) {
        auto& chunk = settled_[at[k]];
        if (chunk.locked) {
            if (englishSentence && chunk.isEnglish) {
                // Typed with a capital (Monda, GtiHub), not picked: a slip
                // is put right in its capitals.
                std::wstring lower = whole[k].written;
                for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
                std::wstring text = withoutMarks(chunk.english);
                if (!chunk.picked && lower != chunk.keys && !whole[k].written.empty()) {
                    text = whole[k].written;
                    if (chunk.typed.size() > 0 && std::iswupper(chunk.typed[0]) && text[0] >= L'a' && text[0] <= L'z') {
                        text[0] = static_cast<wchar_t>(text[0] - L'a' + L'A');
                    }
                }
                chunk.english = text + typedMarks(chunk.typed);
            }
            continue;
        }
        bool english = whole[k].english;
        if (english && singleVowelKana_ && vowel(chunk.keys)) english = (k > 0 && englishWord(k - 1)) || englishWord(k + 1);
        if (englishSentence && english) {
            // As English Auto writes it in the sentence (slips put right;
            // i is I wherever it is).
            std::wstring written = withoutMarks(whole[k].written);
            if (written == L"i") written = L"I";
            if (chunk.sentenceStart && !written.empty() && written.front() >= L'a' && written.front() <= L'z') {
                written.front() = static_cast<wchar_t>(written.front() - L'a' + L'A');
            }
            chunk.english = written + typedMarks(chunk.typed);
        }
        chunk.isEnglish = english;
    }
    if (englishSentence) {
        for (auto& chunk : settled_) {
            if (chunk.marksOnly) chunk.english = typedMarks(chunk.typed);
        }
    }
    if (current && !TypedWithCapitals(currentKeys) && whole.back().english != current->english &&
        !(SingleVowelKana(currentKeys) && whole.back().english)) {
        current = whole.back();
    }
    RecountSentence();
}

bool JapaneseComposer::SingleVowelKana(std::wstring_view keys) const noexcept {
    return singleVowelKana_ && englishRun_ == 0 && keys.size() == 1 && std::wstring_view(L"aiueo").find(keys[0]) != std::wstring_view::npos;
}

ChunkJudge::Judgement JapaneseComposer::JudgeChunk(std::wstring_view keys) const {
    const EnglishSpelling* spelling = englishCorrection_ ? englishSpelling_ : nullptr;
    const auto same = [&](const JudgedKeys& j) {
        return j.judge == chunkJudge_ && j.keys == keys && j.englishRun == englishRun_ &&
               j.japaneseInSentence == japaneseInSentence_ && j.spelling == spelling && j.englishText == englishText_ &&
               j.spaceless == spacelessEnglish_;
    };
    ChunkJudge::Judgement judged;
    if (const auto found = std::find_if(judgedKeys_.begin(), judgedKeys_.end(), same); found != judgedKeys_.end()) {
        judged = found->judged;
    } else {
        judged = chunkJudge_->Judge(keys, englishRun_, japaneseInSentence_, spelling, englishText_, spacelessEnglish_);
        constexpr std::size_t kRemembered = 8;
        if (judgedKeys_.size() >= kRemembered) judgedKeys_.erase(judgedKeys_.begin());
        judgedKeys_.push_back({std::wstring(keys), englishRun_, japaneseInSentence_, spelling, englishText_, spacelessEnglish_,
                               chunkJudge_, judged});
    }
    if (judged.english && SingleVowelKana(keys)) {
        // The letter is the next choice (either).
        judged.english = false;
        judged.either = true;
        judged.word = false;
        judged.written = std::wstring(keys);
        judged.written[0] = static_cast<wchar_t>(std::towupper(judged.written[0]));
    }
    ApplyLearnedLanguage(judged, keys);
    return judged;
}

void JapaneseComposer::ApplyLearnedLanguage(ChunkJudge::Judgement& judged, std::wstring_view keys) const {
    if (!learning_ || !judged.either || !table_ || keys.empty()) return;
    std::wstring kana;
    for (const auto& token : ParseRomaji(*table_, keys)) {
        if (token.leftover) return;
        kana += token.kana;
    }
    const std::wstring reading = ApplyTypingStyle(std::move(kana));
    // The English as it was committed: as written, as typed, or with a
    // capital at the start of a sentence.
    std::wstring lower(keys);
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    std::wstring capital = lower;
    capital[0] = static_cast<wchar_t>(std::towupper(capital[0]));
    double preference = 0.0;
    for (const auto& text : {judged.written, lower, capital}) {
        const double p = learning_->Preference(reading, text);
        if (std::abs(p) > std::abs(preference)) preference = p;
    }
    if (preference > 0.0 && !judged.english) {
        judged.english = true;
        judged.written = lower;
    } else if (preference < 0.0 && judged.english) {
        judged.english = false;
        judged.written = std::wstring(keys);
    }
}

void JapaneseComposer::UpdateJoinedStart() const noexcept {
    if (!converter_) return;
    bool unlikely = sentenceStart_;
    if (!unlikely && !precedingText_.empty()) {
        const wchar_t last = precedingText_.back();
        unlikely = last >= 0x3041 && last <= 0x309F;  // hiragana
    }
    converter_->SetAtSentenceStart(unlikely);
}

std::wstring JapaneseComposer::SpacedEnglish(std::wstring_view before, std::wstring_view text) {
    // The text with the character before it, to see the first boundary too.
    std::wstring all;
    if (!before.empty()) all.push_back(before.back());
    const std::size_t offset = all.size();
    all += text;
    // Whether the run of letters and digits around `i` has a letter.
    const auto wordHasLetter = [&](std::size_t i) {
        std::size_t begin = i, end = i;
        while (begin > 0 && IsAsciiWordChar(all[begin - 1])) --begin;
        while (end < all.size() && IsAsciiWordChar(all[end])) ++end;
        for (std::size_t k = begin; k < end; ++k) {
            if ((all[k] >= L'a' && all[k] <= L'z') || (all[k] >= L'A' && all[k] <= L'Z')) return true;
        }
        return false;
    };
    std::wstring out;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i > 0 && i >= offset) {
            const wchar_t left = all[i - 1];
            const wchar_t right = all[i];
            const bool japaneseThenEnglish = IsJapaneseLetter(left) && IsAsciiWordChar(right) && wordHasLetter(i);
            const bool englishThenJapanese = IsAsciiWordChar(left) && IsJapaneseLetter(right) && wordHasLetter(i - 1);
            if (japaneseThenEnglish || englishThenJapanese) out.push_back(L' ');
        }
        if (i >= offset) out.push_back(all[i]);
    }
    return out;
}

}  // namespace tekito::japanese
