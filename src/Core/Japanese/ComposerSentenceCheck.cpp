#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// The sentence check: the sentence model (and the AI) reading the sentence to
// choose each phrase.

namespace tekito::japanese {

// A text as the sentence model reads it: its numbers in kanji, as its text
// mostly writes them; digits in either width, with or without commas between
// their groups (4,980: 四千九百八十, not 四,九百八十, which reads worse than
// the kanji the number style does not write).
std::wstring JapaneseComposer::ForSentenceModel(std::wstring text) {
    for (auto& ch : text) {
        if (ch >= 0xFF10 && ch <= 0xFF19) ch = static_cast<wchar_t>(ch - 0xFEE0);
    }
    const auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    for (std::size_t i = 1; i + 1 < text.size();) {
        if ((text[i] == L',' || text[i] == L'ÿ0C') && digit(text[i - 1]) && digit(text[i + 1])) {
            text.erase(i, 1);
        } else {
            ++i;
        }
    }
    return KanaNumberText(text, NumberStyle::Kanji);
}

// English as the sentence model reads it in a Japanese sentence (it reads
// English poorly): the katakana word Japanese writes for it when that is a
// common loanword (hotel: ホテル, so that ホテルに泊まった reads better than
// ホテルに止まった), else それ, which fits anywhere. A name stays それ (Zoom,
// not ズーム), and so does a less common loanword: every loanword in its
// place did worse. The marks after it stay.
std::wstring JapaneseComposer::EnglishForModel(std::wstring_view english, bool named) const {
    // Loanwords pack scores (Loanwords::Spelling): a common loanword is 6.
    constexpr double kCommonLoanword = 6.0;
    std::wstring word;
    std::wstring marks;
    for (const wchar_t c : english) {
        if (std::wstring_view(L".,!?。、！？").find(c) != std::wstring_view::npos) {
            marks.push_back(c);
        } else if (c != L' ') {
            word.push_back(c);
        }
    }
    std::wstring text = L"それ";
    if (loanwords_ && !word.empty() && !named) {
        const auto spellings = loanwords_->ForEnglish(word, 1);
        if (!spellings.empty() && !spellings.front().katakana.empty() && spellings.front().score >= kCommonLoanword) {
            text = spellings.front().katakana;
        }
    }
    return text + marks;
}

bool JapaneseComposer::WaitsForAi() {
    if (!aiWaiting_) return false;
    if (aiShown_.empty()) {
        aiShown_ = Preedit();
        aiKeys_ = Keys(false);
        aiFocus_ = focus_;
    }
    return true;
}

bool JapaneseComposer::AiAnswerCame() const {
    return aiWaiting_ && (!aiJudge_ || !aiJudge_->Asking());
}

bool JapaneseComposer::TakeAiAnswer() {
    if (!aiWaiting_ || (aiJudge_ && aiJudge_->Asking())) return false;
    aiWaiting_ = false;
    const bool same = !aiShown_.empty() && Preedit() == aiShown_ && Keys(false) == aiKeys_ && focus_ == aiFocus_ &&
                      !listOpen_ && pending_.empty();
    aiShown_.clear();
    if (!same) return false;
    const std::wstring before = Preedit();
    // Converted again as before, the AI's answer now there for the asking.
    if (preview_) {
        if (!RebuildLive(true)) return false;
    } else if (IsConverted()) {
        BuildPhrases(true);
    } else {
        return false;
    }
    return Preedit() != before;
}

void JapaneseComposer::CheckSentence(bool typingOn) {
    lastChecked_.clear();
    lastCheckedSentences_ = {};
    if (phrases_.empty() || (!phrases_.front().candidates.empty() && !phrases_.front().candidates.front().reading.empty())) {
        return;  // the whole input read another way (English run in) stays first
    }
    // An organization as it is written, with its ・ (ドン・キホーテで, not ドンキホーテで),
    // over what differs from it only by the dots.
    for (auto& phrase : phrases_) {
        if (phrase.learned || phrase.selected >= phrase.candidates.size() || phrase.candidates[phrase.selected].organization) {
            continue;
        }
        const auto undotted = [](std::wstring text) {
            std::erase(text, L'・');
            return text;
        };
        const std::wstring shown = undotted(phrase.candidates[phrase.selected].text);
        for (std::size_t c = 0; c < phrase.candidates.size(); ++c) {
            const auto& candidate = phrase.candidates[c];
            if (candidate.organization && candidate.text != phrase.candidates[phrase.selected].text &&
                undotted(candidate.text) == shown) {
                phrase.selected = c;
                break;
            }
        }
    }
    // Each phrase's candidates for part of the input only, each text once.
    // Slips undone are left out: finding them for every phrase would take
    // longer than the conversion itself.
    constexpr std::size_t kListed = 20;
    std::vector<std::vector<std::size_t>> lists;
    for (const auto& phrase : phrases_) {
        if (phrase.candidates.empty()) return;
        // Marks typed as marks stay as typed (リック・ルービン: not /).
        const auto reading = std::wstring_view(conversionReading_).substr(
            std::min(phrase.begin, conversionReading_.size()), phrase.length);
        // Between digits (1/2) the check still reads them.
        const std::size_t end = phrase.begin + phrase.length;
        const bool betweenDigits = phrase.begin > 0 && end < conversionReading_.size() &&
                                   IsDigit(conversionReading_[phrase.begin - 1]) && IsDigit(conversionReading_[end]);
        // Digits typed stay in the number style (10/21: not 十).
        // The sentence's own punctuation (、。！？) is read as before.
        const bool marks = (!reading.empty() && !betweenDigits && std::none_of(reading.begin(), reading.end(), [](wchar_t c) {
                               return (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC ||
                                      std::iswalnum(c) != 0 || (c >= 0xFF10 && c <= 0xFF5A) || c == 0x3001 || c == 0x3002 ||
                                      c == 0xFF01 || c == 0xFF1F;
                           })) ||
                           std::any_of(reading.begin(), reading.end(), IsDigit) ||
                           // A dash between digits stays ー (090ー1234ー5678, not ―).
                           (betweenDigits && reading == L"ー");
        // A number read in kana stays in the number style (3時: not 三時);
        // another word may still read better (ご縁, not 5円).
        const auto& shown = phrase.candidates[phrase.selected < phrase.candidates.size() ? phrase.selected : 0];
        const auto shape = [](const std::wstring& text) {
            std::wstring out;
            for (const wchar_t c : text) {
                const bool numeral = IsDigit(c) || std::wstring_view(L"〇一二三四五六七八九十百千万億").find(c) != std::wstring_view::npos;
                if (!numeral) out += c;
                else if (out.empty() || out.back() != L'#') out += L'#';
            }
            return out;
        };
        const std::wstring numberShape = shown.kanaNumber ? shape(shown.text) : std::wstring{};
        std::vector<std::size_t> list;
        for (std::size_t c = 0; c < phrase.candidates.size() && list.size() < kListed; ++c) {
            const auto& candidate = phrase.candidates[c];
            if (!candidate.reading.empty()) continue;
            // Dates, symbols and emoji by their readings are offered as
            // they are, not read as the sentence's words: they would take
            // the place of the words the check reads.
            if (candidate.kind == PhraseCandidate::Kind::Special && c != phrase.selected) continue;
            if (!numberShape.empty() && c != phrase.selected) {
                // The number another way: in kanji (三時), or left as read
                // (く月, に月: the kana the reading starts with, then the rest).
                std::size_t kana = 0;
                while (kana < candidate.text.size() && kana < reading.size() && candidate.text[kana] == reading[kana]) ++kana;
                const bool asRead = kana > 0 && L"#" + shape(candidate.text.substr(kana)) == numberShape;
                // Or the counter kept with something else for the number
                // (苦月, 区月 for 9月); a word of its own (惨事, ご縁) stays.
                const std::wstring counter = numberShape.substr(numberShape.find(L'#') + 1);
                constexpr std::size_t kNumberRead = 3;
                const bool counterKept = numberShape.starts_with(L'#') && !counter.empty() && candidate.text.ends_with(counter) &&
                                         candidate.text.size() <= counter.size() + kNumberRead;
                if (shape(candidate.text) == numberShape || asRead || counterKept) continue;
            }
            const bool seen = std::any_of(list.begin(), list.end(), [&](std::size_t other) {
                return phrase.candidates[other].text == candidate.text;
            });
            if (!seen) list.push_back(c);
            // What learning put first stays, and a set phrase (画竜点睛:
            // the check reads its rare kanji as unlikely).
            if (phrase.learned || marks || shown.setPhrase || shown.name) break;
        }
        if (list.empty() || list.front() != phrase.selected) return;
        lists.push_back(std::move(list));
    }
    std::vector<std::vector<std::wstring>> parts;
    std::vector<std::vector<std::int64_t>> deltas;
    for (std::size_t i = 0; i < lists.size(); ++i) {
        auto& texts = parts.emplace_back();
        for (const std::size_t c : lists[i]) texts.push_back(phrases_[i].candidates[c].text);
        if (sentencesByCost_) deltas.push_back(CostDeltas(phrases_[i].candidates, lists[i]));
    }
    std::vector<std::vector<std::array<float, 5>>> signals;
    if (keepFeatures_ || sentenceWeights_.on) {
        for (std::size_t i = 0; i < lists.size(); ++i) {
            auto& each = signals.emplace_back();
            for (const std::size_t c : lists[i]) each.push_back(phrases_[i].candidates[c].signals);
        }
    }
    // Whole texts that break the reading elsewhere (作成 or 作 成) are
    // read alongside: what was meant is often only among them.
    // Not when learning put a phrase's choice first: that stays.
    std::vector<SentenceGuess> guesses;
    const bool learned = std::any_of(phrases_.begin(), phrases_.end(), [](const PhraseState& p) { return p.learned; });
    if (converter_ && wholeSentences_ > 0 && !learned && !typingOn) {
        SyncEdgeContext();
        guesses = converter_->BestSentences(conversionReading_, wholeSentences_, context_, userDictionary_);
    }
    // A name or set phrase that stays in its phrase stays in them too
    // (コカ・コーラ, not コカコーラ).
    std::vector<std::wstring> kept;
    for (const auto& phrase : phrases_) {
        if (phrase.selected >= phrase.candidates.size()) continue;
        const auto& shown = phrase.candidates[phrase.selected];
        if (shown.name || shown.setPhrase) kept.push_back(shown.text);
    }
    std::vector<std::wstring> wholes;
    for (const auto& guess : guesses) {
        const bool keeps = std::all_of(kept.begin(), kept.end(), [&](const std::wstring& text) {
            return guess.text.find(text) != std::wstring::npos;
        });
        if (keeps && InKanjiRange(guess.text)) wholes.push_back(guess.text);
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        auto& checked = lastChecked_.emplace_back();
        checked.reading = conversionReading_.substr(std::min(phrases_[i].begin, conversionReading_.size()), phrases_[i].length);
        checked.read = parts[i];
        for (const auto& candidate : phrases_[i].candidates) checked.all.push_back(candidate.text);
    }
    std::wstring written;
    const auto picks = ReadBest(parts, sentencesByCost_ ? &deltas : nullptr, !typingOn && !chunkJudge_, &written, wholes, signals);
    if (!written.empty()) {
        // One of those: its phrases, where they break.
        const auto guess = std::find_if(guesses.begin(), guesses.end(),
                                        [&](const SentenceGuess& g) { return g.text == written; });
        TakeWritten(written, guess != guesses.end() ? std::span<const std::size_t>(guess->breaks)
                                                    : std::span<const std::size_t>());
        SelectStyledNumbers();
        return;
    }
    for (std::size_t i = 0; i < lists.size(); ++i) phrases_[i].selected = lists[i][picks[i]];
    SelectStyledNumbers();
}

void JapaneseComposer::TakeWritten(const std::wstring& text, std::span<const std::size_t> breaks) {
    if (SelectWritten(text)) return;
    // Else the reading split where the text's phrases break, when that
    // makes it.
    if (!breaks.empty() && converter_) {
        SyncEdgeContext();
        auto rebuilt = converter_->Convert(conversionReading_, {}, context_, WordsWithEnglish(), userDictionary_, breaks);
        // Numbers as the number style writes them, as the first conversion's.
        for (auto& phrase : rebuilt) StyleKanaNumbers(phrase.candidates);
        if (!rebuilt.empty()) {
            auto before = std::move(phrases_);
            phrases_.clear();
            AddPhrases(std::move(rebuilt), conversionReading_);
            for (auto& phrase : phrases_) KeepKanjiInRange(phrase);
            if (SelectWritten(text)) {
                focus_ = std::min(focus_, phrases_.size() - 1);
                return;
            }
            phrases_ = std::move(before);
        }
    }
    // Otherwise the whole text, as one choice for all of the input; and
    // its words in the phrases they fall in, first among their candidates,
    // so moving to a phrase to change it keeps the others as written.
    PhraseCandidate whole{text, 0, PhraseCandidate::Kind::Dictionary, false};
    whole.reading = Reading();
    auto& first = phrases_.front();
    first.candidates.insert(first.candidates.begin(), std::move(whole));
    first.selected = 0;
    const auto places = converter_ ? converter_->Align(conversionReading_, text) : std::nullopt;
    if (!places) return;
    // Numbers read in kana are written as the number style says (二十二 or
    // 22), as the converter's are, wherever the writing has one on its own.
    const auto styled = [&](std::size_t from, std::size_t to) {
        std::wstring out;
        std::size_t k = from;
        while (k < to) {
            // The longest run of places from k that writes a number read in kana.
            std::size_t run = k;
            for (std::size_t next = k + 1; next <= to; ++next) {
                const auto [readFrom, textFrom] = (*places)[k];
                const auto [readTo, textTo] = (*places)[next];
                const auto read = std::wstring_view(conversionReading_).substr(readFrom, readTo - readFrom);
                if (std::any_of(read.begin(), read.end(), IsDigit) ||
                    !NumeralDigits(std::wstring_view(text).substr(textFrom, textTo - textFrom))) {
                    break;
                }
                run = next;
            }
            if (run > k) {
                const auto written = std::wstring_view(text).substr((*places)[k].second, (*places)[run].second - (*places)[k].second);
                out += KanaNumberText(*NumeralDigits(written), numberStyle_);
                k = run;
            } else {
                const auto [readFrom, textFrom] = (*places)[k];
                const auto [readTo, textTo] = (*places)[k + 1];
                // A letter, digit or mark written in the other width (Ａ for
                // A, ，for ,) is as the reading has it: the width and
                // punctuation settings made it so; the model knows nothing
                // of them.
                const bool otherWidth = readTo - readFrom == 1 && textTo - textFrom == 1 &&
                                        text[textFrom] != conversionReading_[readFrom] &&
                                        ToHalfWidthAscii(std::wstring(1, text[textFrom])) ==
                                            ToHalfWidthAscii(std::wstring(1, conversionReading_[readFrom]));
                const auto read = std::wstring_view(conversionReading_).substr(readFrom, readTo - readFrom);
                const auto piece = std::wstring_view(text).substr(textFrom, textTo - textFrom);
                // A word that starts with digits for a number read in kana
                // (22人, 100円): the number as the style writes it, the rest as
                // written. Kanji that start like a number (十分, 一緒) stay.
                const auto number = std::none_of(read.begin(), read.end(), IsDigit) &&
                                             std::any_of(piece.begin(), piece.end(), IsDigit)
                                         ? LeadingNumber(piece)
                                         : std::nullopt;
                if (otherWidth) {
                    out += read;
                } else if (number) {
                    out += KanaNumberText(number->first, numberStyle_) + number->second;
                } else {
                    out += piece;
                }
                ++k;
            }
        }
        return out;
    };
    phrases_.front().candidates.front().text = styled(0, places->size() - 1);
    const auto textAt = [&](std::size_t reading) -> std::optional<std::size_t> {
        const auto found = std::find_if(places->begin(), places->end(),
                                        [&](const auto& place) { return place.first == reading; });
        return found == places->end() ? std::nullopt : std::optional<std::size_t>(found->second);
    };
    for (std::size_t i = 0; i < phrases_.size(); ++i) {
        auto& phrase = phrases_[i];
        const auto start = textAt(phrase.begin);
        const auto end = textAt(phrase.begin + phrase.length);
        if (!start || !end || *end <= *start) continue;
        const auto placeOf = [&](std::size_t reading) {
            return static_cast<std::size_t>(std::find_if(places->begin(), places->end(),
                                                         [&](const auto& place) { return place.first == reading; }) -
                                            places->begin());
        };
        std::wstring piece = styled(placeOf(phrase.begin), placeOf(phrase.begin + phrase.length));
        // After the whole text in the first phrase: dropping it shows this.
        const std::size_t at = i == 0 ? 1 : 0;
        auto& candidates = phrase.candidates;
        const auto listed = std::find_if(candidates.begin() + static_cast<std::ptrdiff_t>(at), candidates.end(),
                                         [&](const PhraseCandidate& c) { return c.text == piece && c.reading.empty(); });
        PhraseCandidate word = listed != candidates.end() ? *listed
                                                          : PhraseCandidate{piece, 0, PhraseCandidate::Kind::Dictionary, false};
        if (listed != candidates.end()) candidates.erase(listed);
        candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(at), std::move(word));
        if (i > 0) phrase.selected = 0;
    }
}

bool JapaneseComposer::SelectWritten(const std::wstring& text) {
    // The phrases' own candidates, when they make the text: each phrase
    // keeps its list and can be changed on its own.
    std::vector<std::size_t> choice(phrases_.size(), 0);
    const std::function<bool(std::size_t, std::size_t)> fit = [&](std::size_t phrase, std::size_t at) {
        if (phrase == phrases_.size()) return at == text.size();
        const auto& candidates = phrases_[phrase].candidates;
        for (std::size_t c = 0; c < candidates.size(); ++c) {
            const auto& written = candidates[c].text;
            if (written.empty() || !candidates[c].reading.empty() || text.compare(at, written.size(), written) != 0) {
                continue;
            }
            choice[phrase] = c;
            if (fit(phrase + 1, at + written.size())) return true;
        }
        return false;
    };
    if (!fit(0, 0)) return false;
    for (std::size_t i = 0; i < phrases_.size(); ++i) phrases_[i].selected = choice[i];
    return true;
}

std::vector<std::int64_t> JapaneseComposer::CostDeltas(const std::vector<PhraseCandidate>& candidates,
                                                       const std::vector<std::size_t>& list) {
    // Kana, typed forms, English and the like have no cost of their own:
    // kUncostedDelta over the first. The list's order stays (each at least
    // the one before), as learning and the user's words set it.
    constexpr std::int64_t kUncostedDelta = 3000;
    std::vector<std::int64_t> deltas;
    std::int64_t first = 0;
    for (const std::size_t c : list) {
        const auto& candidate = candidates[c];
        const bool costed = candidate.kind == PhraseCandidate::Kind::Dictionary && candidate.cost > 0 &&
                            candidate.cost < (std::int64_t{1} << 30);
        if (deltas.empty()) {
            first = costed ? candidate.cost : 0;
            deltas.push_back(0);
            continue;
        }
        const std::int64_t delta = costed ? candidate.cost - first : kUncostedDelta;
        deltas.push_back(std::max(delta, deltas.back()));
    }
    return deltas;
}

std::vector<std::size_t> JapaneseComposer::ReadBest(const std::vector<std::vector<std::wstring>>& parts,
                                                    const std::vector<std::vector<std::int64_t>>* deltas, bool ai,
                                                    std::wstring* written, std::span<const std::wstring> wholes,
                                                    std::span<const std::vector<std::array<float, 5>>> signals) const {
    std::vector<std::size_t> keep(parts.size(), 0);
    std::vector<std::size_t> sizes;
    for (const auto& part : parts) {
        if (part.empty()) return keep;
        sizes.push_back(part.size());
    }
    const AiJudge* judge = ai ? aiJudge_ : nullptr;
    // The sentences by the sum of their choices' ranks, each text once.
    std::vector<std::wstring> sentences;
    std::vector<std::vector<std::size_t>> picked;
    ForPicksByRank(sizes, [&](const std::vector<std::size_t>& picks) {
        std::wstring text;
        for (std::size_t i = 0; i < parts.size(); ++i) text += parts[i][picks[i]];
        if (std::find(sentences.begin(), sentences.end(), text) == sentences.end()) {
            sentences.push_back(std::move(text));
            picked.push_back(picks);
        }
        return sentences.size() < std::max(reranker_->Top(), judge ? judge->Top() : 0);
    }, deltas);
    // With the costs and an AI model: the first by rank too, those the
    // costs left out after them, for the AI alone (it more often finds what
    // was meant among both).
    const std::size_t byCost = sentences.size();
    const auto among = [](const auto& texts, std::size_t from, std::size_t to, const std::wstring& text) {
        return std::find(texts.begin() + static_cast<std::ptrdiff_t>(from),
                         texts.begin() + static_cast<std::ptrdiff_t>(to), text) != texts.begin() + static_cast<std::ptrdiff_t>(to);
    };
    if (deltas && judge) {
        // Not one of the whole texts: those stay where the sentence check reads them.
        std::vector<std::wstring> ranked;
        ForPicksByRank(sizes, [&](const std::vector<std::size_t>& picks) {
            std::wstring text;
            for (std::size_t i = 0; i < parts.size(); ++i) text += parts[i][picks[i]];
            if (among(ranked, 0, ranked.size(), text)) return true;
            ranked.push_back(text);
            const bool whole = written && std::find(wholes.begin(), wholes.end(), text) != wholes.end();
            if (!whole && !among(sentences, 0, sentences.size(), text)) {
                sentences.push_back(std::move(text));
                picked.push_back(picks);
            }
            return ranked.size() < judge->Top();
        });
    }
    // Then the whole texts read with other phrase breaks, taken as written.
    std::size_t combined = sentences.size();
    for (const auto& whole : wholes) {
        if (written && !among(sentences, 0, byCost, whole) && !among(sentences, combined, sentences.size(), whole)) {
            sentences.push_back(whole);
        }
    }
    // Numbers are read by the sentence model in kanji, as its text mostly
    // writes them (2時間 read as 二時間, not beaten by 似時間).
    auto read = sentences;
    for (auto& text : read) text = ForSentenceModel(std::move(text));
    // The sentence check reads those by cost and the whole ones.
    std::vector<std::wstring> checked(read.begin(), read.begin() + static_cast<std::ptrdiff_t>(byCost));
    checked.insert(checked.end(), read.begin() + static_cast<std::ptrdiff_t>(combined), read.end());
    // A conversion's sentences (with what its phrases' candidates say) by
    // the learned weights; others by the sentence model and the prior.
    const bool weighed = sentenceWeights_.on && !signals.empty();
    std::vector<std::pair<double, double>> scored;
    std::size_t chosen = reranker_->Choose(checked, byCost, wholePenalty_, precedingText_,
                                           keepFeatures_ || weighed ? &scored : nullptr, &rerankerMemory_);
    if (!weighed && chosen != 0 && chosen < checked.size()) {
        const auto& pick = chosen < byCost ? sentences[chosen] : sentences[combined + chosen - byCost];
        if (KeepsTopic(sentences[0], pick)) chosen = 0;
    }
    lastCheckedSentences_.shown.assign(sentences.begin(), sentences.begin() + static_cast<std::ptrdiff_t>(byCost));
    lastCheckedSentences_.shown.insert(lastCheckedSentences_.shown.end(), sentences.begin() + static_cast<std::ptrdiff_t>(combined),
                                       sentences.end());
    lastCheckedSentences_.reading = conversionReading_;
    lastCheckedSentences_.read = checked;
    lastCheckedSentences_.scores.clear();
    if (scored.size() == checked.size()) {
        for (const auto& [model, prior] : scored) lastCheckedSentences_.scores.push_back(static_cast<float>(model));
    }
    lastCheckedSentences_.ranked = byCost;
    lastCheckedSentences_.chosen = chosen;
    lastCheckedSentences_.serial = ++checks_;
    lastCheckedSentences_.features.clear();
    if ((keepFeatures_ || weighed) && scored.size() == checked.size()) {
        auto& features = lastCheckedSentences_.features;
        features.assign(checked.size(), {});
        for (std::size_t k = 0; k < checked.size(); ++k) {
            auto& f = features[k];
            if (k < scored.size()) {
                f[kFeatureModel] = static_cast<float>(scored[k].first);
                f[kFeaturePrior] = static_cast<float>(scored[k].second);
            }
            const bool whole = k >= byCost;
            const auto& text = whole ? sentences[combined + k - byCost] : sentences[k];
            f[kFeatureWhole] = whole ? 1.0f : 0.0f;
            f[kFeatureRank] = whole ? -1.0f : static_cast<float>(k);
            f[kFeatureLength] = static_cast<float>(sentences[0].size());
            if (k > 0 && KeepsTopic(sentences[0], text)) f[kFeatureKeepsTopic] = 1.0f;
            if (whole) continue;
            double cost = 0.0;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                const std::size_t pick = picked[k][i];
                if (pick == 0) continue;
                f[kFeatureChanged] += 1.0f;
                if (deltas) cost += static_cast<double>((*deltas)[i][pick]);
                if (i < signals.size() && pick < signals[i].size()) {
                    for (std::size_t s = 0; s < 5; ++s) f[kFeatureSignals + s] += signals[i][pick][s] - signals[i][0][s];
                }
            }
            f[kFeatureCost] = static_cast<float>(cost / 1000.0);
        }
        if (weighed) {
            const auto& w = sentenceWeights_;
            const auto sum = [&](const std::array<float, kSentenceFeatures>& f) {
                double total = w.model * f[kFeatureModel] + w.prior * f[kFeaturePrior] + w.whole * f[kFeatureWhole] +
                               w.cost * std::clamp(f[kFeatureCost], 0.0f, 10.0f) + w.changed * f[kFeatureChanged] +
                               w.keepsTopic * f[kFeatureKeepsTopic];
                for (std::size_t s = 0; s < 5; ++s) total += w.signals[s] * f[kFeatureSignals + s];
                return total;
            };
            const double first = sum(features[0]);
            double best = first + w.margin;
            chosen = 0;
            for (std::size_t k = 1; k < features.size(); ++k) {
                if (const double total = sum(features[k]); total > best) {
                    best = total;
                    chosen = k;
                }
            }
            lastCheckedSentences_.chosen = chosen;
            // The scores the chunk candidates are measured by: what the
            // check chose by.
            auto& scores = lastCheckedSentences_.scores;
            if (scores.size() == features.size()) {
                for (std::size_t k = 0; k < features.size(); ++k) scores[k] = static_cast<float>(sum(features[k]));
            }
        }
        if (!keepFeatures_) features.clear();
    }
    if (chosen >= byCost) chosen += combined - byCost;
    if (beamWidth_ > 0 && deltas && reranker_->IsOpen()) {
        // Phrase by phrase: each start read once, the likeliest kept.
        SentenceReranker::Reader reader(*reranker_, precedingText_);
        const auto asRead = [](std::wstring text) { return ForSentenceModel(std::move(text)); };
        struct Start {
            std::vector<std::size_t> picks;
            std::wstring text;
            std::int64_t cost{0};
            double score{0};
        };
        std::vector<Start> beam(1);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const bool last = i + 1 == parts.size();
            std::vector<Start> next;
            for (const auto& start : beam) {
                for (std::size_t k = 0; k < parts[i].size(); ++k) {
                    Start longer{start.picks, start.text + parts[i][k], start.cost + (*deltas)[i][k], 0};
                    longer.picks.push_back(k);
                    longer.score = reader.LogProbability(asRead(longer.text), last) -
                                   static_cast<double>(longer.cost) / beamCostPerNat_;
                    const auto same = std::find_if(next.begin(), next.end(),
                                                   [&](const Start& other) { return other.text == longer.text; });
                    if (same == next.end()) next.push_back(std::move(longer));
                    else if (longer.score > same->score) *same = std::move(longer);
                }
            }
            std::stable_sort(next.begin(), next.end(), [](const Start& a, const Start& b) { return a.score > b.score; });
            if (next.size() > beamWidth_) next.resize(beamWidth_);
            beam = std::move(next);
            reader.Forget();
        }
        std::wstring first;
        for (const auto& part : parts) first += part.front();
        const double firstScore = reader.LogProbability(asRead(first), true);
        double bestScore = firstScore + beamMargin_;
        std::optional<std::wstring> best;
        std::vector<std::size_t> bestPicks;
        if (!beam.empty() && beam.front().score > bestScore) {
            bestScore = beam.front().score;
            best = beam.front().text;
            bestPicks = beam.front().picks;
        }
        if (written) {
            for (std::size_t i = combined; i < sentences.size(); ++i) {
                const double score = reader.LogProbability(read[i], true) - beamWholeCost_ / beamCostPerNat_;
                if (score > bestScore) {
                    bestScore = score;
                    best = sentences[i];
                    bestPicks.clear();
                }
            }
        }
        chosen = 0;
        if (best && *best != first) {
            const auto at = std::find(sentences.begin(), sentences.end(), *best);
            if (at != sentences.end() && (static_cast<std::size_t>(at - sentences.begin()) >= combined || !bestPicks.empty())) {
                chosen = static_cast<std::size_t>(at - sentences.begin());
                if (chosen < combined && !bestPicks.empty()) picked[chosen] = bestPicks;
            } else if (!bestPicks.empty()) {
                sentences.insert(sentences.begin() + static_cast<std::ptrdiff_t>(combined), *best);
                picked.push_back(bestPicks);
                chosen = combined++;
            }
        }
    }
    if (judge && !sentences.empty()) {
        // The AI reads the first few of the phrases' sentences, those by
        // rank, and all the whole ones.
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < sentences.size(); ++i) {
            if (i < std::min(byCost, judge->Top()) || i >= byCost) order.push_back(i);
        }
        if (order.size() > aihelper::kMaxSentences) order.resize(aihelper::kMaxSentences);
        // Numbers read in kana in half-width digits, as the model's text
        // writes them (the number style is applied to the choice
        // afterwards); digits typed stay as typed, as the reading has them.
        const bool typedWide = std::any_of(conversionReading_.begin(), conversionReading_.end(),
                                           [](wchar_t c) { return c >= 0xFF10 && c <= 0xFF19; });
        std::vector<std::wstring> asked;
        for (const std::size_t i : order) {
            std::wstring text = sentences[i];
            for (auto& ch : text) {
                if (!typedWide && ch >= 0xFF10 && ch <= 0xFF19) ch = static_cast<wchar_t>(ch - 0xFEE0);
            }
            asked.push_back(std::move(text));
        }
        const auto answer = aiWaits_ ? judge->Convert(conversionReading_, asked, recentText_)
                                     : judge->Ask(conversionReading_, asked, recentText_);
        aiWaiting_ = !answer && !aiWaits_ && judge->Asking();
        aiShown_.clear();
        const auto at = std::find(order.begin(), order.end(), chosen);
        if (answer && at != order.end()) {
            const auto& scores = answer->scores;
            const std::size_t mine = static_cast<std::size_t>(at - order.begin());
            std::size_t best = mine;
            std::optional<std::wstring> own;
            double bestScore = scores[mine];
            // What the model makes of the choice: its score, or that of the
            // choice written otherwise when it reads that better (三時 for
            // 3時): the words are what it rates, the notation stays.
            double mineScore = scores[mine];
            if (keepNotation_ && judge->Writes()) {
                for (std::size_t i = 0; i < order.size(); ++i) {
                    if (OnlyNotationDiffers(sentences[chosen], asked[i])) mineScore = std::max(mineScore, scores[i]);
                }
                for (const auto& [text, score] : answer->written) {
                    if (OnlyNotationDiffers(sentences[chosen], text)) mineScore = std::max(mineScore, score);
                }
                bestScore = mineScore;
            }
            if (judge->Writes()) {
                // A model trained for conversion: the likeliest of the
                // sentences and its own writings that write the reading;
                // not one that only writes the sentence check's choice
                // otherwise (お代わり for おかわり, 1日 for 一日): the
                // dictionary's notation, or the user's, stays.
                for (std::size_t i = 0; i < order.size(); ++i) {
                    if (scores[i] > bestScore && !(keepNotation_ && OnlyNotationDiffers(sentences[chosen], asked[i]))) {
                        bestScore = scores[i];
                        best = i;
                    }
                }
                for (const auto& [text, score] : answer->written) {
                    const bool listed = std::find(asked.begin(), asked.end(), text) != asked.end();
                    if (score > bestScore && !listed && written && converter_ && InKanjiRange(text) &&
                        !(keepNotation_ && OnlyNotationDiffers(sentences[chosen], text)) &&
                        converter_->CanWrite(conversionReading_, text)) {
                        bestScore = score;
                        own = text;
                    }
                }
            } else {
                // A model that reads sentences alone: not one with fewer
                // kanji, as it reads a sentence that starts in kana
                // (あしたは) as likelier than in kanji (明日は).
                const auto kanji = [](const std::wstring& text) {
                    return std::count_if(text.begin(), text.end(),
                                         [](wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005; });
                };
                const auto least = kanji(sentences[chosen]);
                for (std::size_t i = 0; i < order.size(); ++i) {
                    if (scores[i] > bestScore && kanji(asked[i]) >= least) {
                        bestScore = scores[i];
                        best = i;
                    }
                }
            }
            // Only a clearly better reading replaces the sentence check's.
            if (bestScore - mineScore > judge->Margin()) {
                if (own) {
                    *written = std::move(*own);
                    return keep;
                }
                chosen = order[best];
            }
        }
    }
    // What was chosen at last (the beam or the AI may have read another),
    // as the sentences kept count it, so the chunk candidates measure by it.
    if (chosen < sentences.size()) {
        auto& last = lastCheckedSentences_;
        const auto at = std::find(last.shown.begin(), last.shown.end(), sentences[chosen]);
        if (at != last.shown.end()) last.chosen = static_cast<std::size_t>(at - last.shown.begin());
    }
    if (chosen >= combined) {
        *written = sentences[chosen];
        return keep;
    }
    return chosen == 0 ? keep : picked[chosen];
}

bool JapaneseComposer::KeepsTopic(const std::wstring& first, const std::wstring& pick) const {
    // A tie this strong keeps the word, against one under the other.
    constexpr double kKept = 2.0;
    constexpr double kNone = 1.0;
    if (!converter_ || first == pick) return false;
    // Where they part: from the first character that differs.
    std::size_t at = 0;
    while (at < first.size() && at < pick.size() && first[at] == pick[at]) ++at;
    if (at >= first.size() || at >= pick.size()) return false;
    // Only one word put in for another of its shape (売った, 打った): the
    // same length, and the rest of the sentence as it was; where the check
    // changed more, the rest may be what it put right.
    std::size_t end = 0;
    while (end < first.size() - at && end < pick.size() - at && first[first.size() - 1 - end] == pick[pick.size() - 1 - end]) ++end;
    if (first.size() != pick.size()) return false;
    const auto content = [](wchar_t c) {
        return (c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005 || (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC;
    };
    std::size_t run = at;
    while (run < first.size() && content(first[run]) && content(pick[run])) ++run;
    if (run == at || first.size() - end > run) return false;
    // Nor where the pick makes a longer word the model knows (差し支え, not
    // 指し支え): that is a word, the first choice's run was not.
    if (converter_->KnownLength(std::wstring_view(pick).substr(at)) >
        converter_->KnownLength(std::wstring_view(first).substr(at))) {
        return false;
    }
    // Nor where the pick goes with the word before it as one, clearly more
    // than the first choice's word does (映画界, not 映画会).
    constexpr double kBetterPair = 1.0;
    std::size_t left = at;
    while (left > 0 && content(first[left - 1])) --left;
    if (left < at) {
        const auto before = std::wstring_view(first).substr(left, at - left);
        const auto mine = converter_->PairFit(before, std::wstring_view(first).substr(at, run - at));
        const auto theirs = converter_->PairFit(before, std::wstring_view(pick).substr(at, run - at));
        if (theirs && *theirs > mine.value_or(0.0) + kBetterPair) return false;
    }
    std::vector<std::wstring> others = contextWords_;
    AddContentWords(first, others);
    const auto kept = converter_->TopicTie(std::wstring_view(first).substr(at), others);
    if (!kept || *kept < kKept) return false;
    // A word the model does not know may well belong: only one it knows
    // and finds no tie for is put back.
    std::vector<std::wstring> theirs = contextWords_;
    AddContentWords(pick, theirs);
    const auto put = converter_->TopicTie(std::wstring_view(pick).substr(at), theirs);
    return put && *put < kNone;
}

}  // namespace tekito::japanese
