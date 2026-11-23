#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// Slips: keys read with a slip undone, offered in the list or put right.

namespace tekito::japanese {

bool JapaneseComposer::FixSlips() {
    // Readings with slips undone must beat the keys as typed by this much
    // (nats, kana LSTM less the edits) to be converted at all; then the
    // conversions are weighed with the sentence model, the kana score
    // counting half. Chosen on dev sets only: lower gates fix more slips
    // that still read as kana but change sentences typed right.
    // A Mix chunk is shorter than a sentence and has less to go on: 8.
    // (Chosen on eval/ja/mix_long_keys.tsv with two slips a sentence: more
    // slips put right, no more text typed right changed.)
    const double kSlipGate = chunkJudge_ ? 8.0 : 12.0;
    // ...and the corrected conversion must then win by this much. More
    // than one corrected reading tried loses: the sentence model likes the
    // shorter ones.
    constexpr double kSlipMargin = 3.0;
    constexpr std::size_t kSlipTries = 1;
    constexpr double kKanaWeight = 0.5;
    const std::wstring keys = Keys(false);
    const auto found = slips_->Find(keys, *table_, kSlipTries);
    if (found.size() < 2 || found[1].score - found[0].score <= kSlipGate) return false;

    struct Converted {
        std::vector<PhraseState> phrases;
        std::wstring reading;
        std::vector<std::size_t> keyAt;
        double score{0};
    };
    // Corrected keys converted as typed keys are, their key positions taken
    // back to the typed keys (none inside what was put in).
    const auto convert = [&](const KanaSlips::Reading& fixed) {
        Converted out;
        out.score = fixed.score;
        std::vector<Phrase> converted;
        if (auto conversion = keyConverter_ ? keyConverter_->Convert(fixed.keys, context_, WordsWithEnglish(), userDictionary_)
                                            : std::nullopt) {
            out.reading = ApplyTypingStyle(std::move(conversion->reading));
            out.keyAt = std::move(conversion->keyAt);
            converted = std::move(conversion->phrases);
            for (auto& phrase : converted) StyleKanaNumbers(phrase.candidates);
        } else {
            std::wstring kana;
            std::vector<std::pair<std::size_t, std::size_t>> units;  // kana offset, key offset
            std::size_t key = 0;
            for (const auto& token : ParseRomaji(*table_, fixed.keys)) {
                units.emplace_back(kana.size(), key);
                kana += ToFullWidthAscii(token.kana);
                key += token.keys;
            }
            out.reading = ApplyTypingStyle(std::move(kana));
            out.keyAt.assign(out.reading.size() + 1, std::wstring::npos);
            for (const auto& [offset, at] : units) out.keyAt[offset] = at;
            out.keyAt[out.reading.size()] = key;
            converted = ConvertReading(out.reading, 0, context_, WordsWithEnglish());
        }
        for (auto& at : out.keyAt) {
            if (at == std::wstring::npos) continue;
            std::ptrdiff_t shift = 0;
            bool inside = false;
            for (const auto& edit : fixed.edits) {
                const std::size_t begin = edit.begin + shift;
                if (at <= begin) break;
                if (at < begin + edit.with.size()) {
                    inside = true;
                    break;
                }
                shift += static_cast<std::ptrdiff_t>(edit.with.size()) - static_cast<std::ptrdiff_t>(edit.end - edit.begin);
            }
            at = inside ? std::wstring::npos : static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) - shift);
        }
        std::swap(phrases_, out.phrases);
        phrases_.clear();
        AddPhrases(std::move(converted), out.reading);
        std::swap(phrases_, out.phrases);
        return out;
    };
    std::vector<Converted> tried;
    for (std::size_t i = 1; i < found.size() && i <= kSlipTries; ++i) {
        if (found[i].score - found[0].score <= kSlipGate) break;
        auto converted = convert(found[i]);
        if (!converted.phrases.empty()) tried.push_back(std::move(converted));
    }
    if (tried.empty()) return false;
    std::size_t chosen = 0;
    if (reranker_) {
        const auto sentenceOf = [](const std::vector<PhraseState>& phrases) {
            std::wstring text;
            for (const auto& phrase : phrases) {
                if (!phrase.candidates.empty()) text += phrase.candidates[std::min(phrase.selected, phrase.candidates.size() - 1)].text;
            }
            return text;
        };
        std::vector<std::wstring> sentences{sentenceOf(phrases_)};
        for (const auto& t : tried) sentences.push_back(sentenceOf(t.phrases));
        const auto logProbabilities = reranker_->LogProbabilities(sentences, precedingText_, &rerankerMemory_);
        double best = kKanaWeight * found[0].score + logProbabilities[0] + kSlipMargin;
        chosen = tried.size();
        for (std::size_t i = 0; i < tried.size(); ++i) {
            const double score = kKanaWeight * tried[i].score + logProbabilities[i + 1];
            if (score > best) {
                best = score;
                chosen = i;
            }
        }
        if (chosen == tried.size()) return false;
    }
    phrases_ = std::move(tried[chosen].phrases);
    conversionReading_ = std::move(tried[chosen].reading);
    readingKeys_ = std::move(tried[chosen].keyAt);
    return true;
}

std::vector<std::pair<PhraseCandidate, std::wstring>> JapaneseComposer::SlipReadings(
    const std::wstring& keys, std::set<std::wstring> skip, std::int64_t typedCost) const {
    // Every slip: a neighboring key, a key dropped (any letter), an extra
    // key, two keys swapped.
    std::vector<std::wstring> variants;
    for (std::size_t at = 0; at <= keys.size(); ++at) {
        for (const wchar_t missing : kSlipDroppedKeys) {
            std::wstring variant = keys;
            variant.insert(at, 1, missing);
            variants.push_back(std::move(variant));
        }
        if (at == keys.size()) break;
        for (wchar_t other = L'a'; other <= L'z'; ++other) {
            if (!AreQwertyNeighbors(keys[at], other)) continue;
            std::wstring variant = keys;
            variant[at] = other;
            variants.push_back(std::move(variant));
        }
        std::wstring removed = keys;
        removed.erase(at, 1);
        variants.push_back(std::move(removed));
        if (at + 1 < keys.size() && keys[at] != keys[at + 1]) {
            std::wstring swapped = keys;
            std::swap(swapped[at], swapped[at + 1]);
            variants.push_back(std::move(swapped));
        }
    }
    std::vector<std::pair<PhraseCandidate, std::wstring>> found;  // with its kana
    for (const auto& variant : variants) {
        std::wstring kana;
        bool readable = !variant.empty();
        for (const auto& token : ParseRomaji(*table_, variant)) {
            readable = readable && !token.leftover;
            kana += token.kana;
        }
        if (!readable) continue;
        kana = ApplyTypingStyle(std::move(kana));
        if (!skip.insert(kana).second) continue;
        auto candidate = converter_->Best(kana, 0, userDictionary_);
        if (candidate && !candidate->spellingCorrection && candidate->cost <= typedCost + kSlipMargin) {
            found.emplace_back(std::move(*candidate), std::move(kana));
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first.cost < b.first.cost; });
    return found;
}

void JapaneseComposer::AddSlipCandidates(PhraseState& phrase) const {
    if (phrase.slipsAdded) return;
    AddSlipCandidatesFor(phrase);
    KeepKanjiInRange(phrase);
}

void JapaneseComposer::AddSlipCandidatesFor(PhraseState& phrase) const {
    phrase.slipsAdded = true;
    if (!converter_ || !table_ || HasAscii() || reconverted_) return;
    const auto lettersOnly = [](const std::wstring& keys) {
        return std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; });
    };
    const auto typedKanaOf = [&](const std::wstring& keys) {
        std::wstring kana;
        for (const auto& token : ParseRomaji(*table_, keys)) kana += token.kana;
        return ApplyTypingStyle(ToFullWidthAscii(kana));
    };
    const auto costOf = [&](const std::optional<PhraseCandidate>& candidate) {
        return candidate ? candidate->cost : std::numeric_limits<std::int64_t>::max() / 4;
    };

    std::vector<PhraseCandidate> added;
    bool likelier = false;
    const auto offer = [&](PhraseCandidate candidate) {
        const auto same = [&](const PhraseCandidate& c) { return c.text == candidate.text; };
        if (std::any_of(phrase.candidates.begin(), phrase.candidates.end(), same) ||
            std::any_of(added.begin(), added.end(), same)) {
            return false;
        }
        candidate.slip = true;
        added.push_back(std::move(candidate));
        return true;
    };

    // A short input split into phrases: the whole input with a slip undone,
    // when the slip is what made the phrases (はじめまして typed hahimemashite
    // is は|秘めまして). A slip inside one phrase is left to that phrase's
    // own list below. These go after the phrase's own candidates.
    const std::wstring allKeys = Keys(false);
    std::vector<PhraseCandidate> wholeInput;
    // A whole-input slip reads better than what is shown: the sentences the
    // check read for the chunk candidates were of the slip.
    bool wholeSlipBetter = false;
    // So much better that it is likely what was meant (ありがとうございます
    // for ありがようございます): right after the first choice, the next
    // Space; not when what was typed reads nearly as well (介護, 会議).
    bool wholeSlipLikely = false;
    if (phrases_.size() > 1 && allKeys.size() <= kWholeSlipKeys && lettersOnly(allKeys)) {
        const std::wstring typedAll = typedKanaOf(allKeys);
        const std::int64_t typedCost = costOf(converter_->Best(typedAll, 0, userDictionary_));
        std::vector<std::size_t> starts;
        for (const auto& p : phrases_) starts.push_back(p.begin);
        std::size_t examined = 0;
        for (auto& [candidate, kana] : SlipReadings(allKeys, {typedAll, conversionReading_}, typedCost)) {
            if (wholeInput.size() >= kWholeSlipCandidates || examined++ >= kWholeSlipExamined) break;
            if (SamePhrasing(conversionReading_, starts, kana)) continue;
            // Not with the start of the input taken away: what was typed first
            // was meant (igaitai is not が痛い).
            if (kana.size() < typedAll.size() && typedAll.ends_with(kana)) continue;
            candidate.reading = kana;
            candidate.slip = true;
            wholeInput.push_back(std::move(candidate));
        }
        if (!wholeInput.empty()) {
            if (reranker_ && sentenceCheck_) {
                std::wstring shown;
                for (const auto& p : phrases_) shown += PhraseText(p);
                std::vector<std::wstring> sentences{shown};
                for (const auto& c : wholeInput) sentences.push_back(c.text);
                const auto scores = reranker_->LogProbabilities(sentences, precedingText_, &rerankerMemory_);
                // ln p: ありがとうございます over ありがようございます by 10 and
                // more; 会議 over 介護 by a few.
                constexpr double kWholeSlipLikely = 6.0;
                for (std::size_t i = 1; i < scores.size(); ++i) {
                    wholeSlipBetter = wholeSlipBetter || scores[i] > scores[0];
                    if (i == 1) wholeSlipLikely = scores[i] > scores[0] + kWholeSlipLikely;
                }
            } else {
                for (const auto& c : wholeInput) wholeSlipBetter = wholeSlipBetter || c.cost + kSlipLikelier < typedCost;
                wholeSlipLikely = wholeInput.front().cost + 2 * kSlipLikelier < typedCost;
            }
        }
        // The whole input split another way inside its first phrase, when
        // that reads nearly as likely: いがい|たい is 以外帯, い|がいたい is
        // 胃が痛い. Not a slip: what was typed is kept.
        const auto& first = phrases_.front();
        const std::wstring& reading = conversionReading_;
        const std::int64_t shownCost = costOf(converter_->Best(reading, 0, userDictionary_));
        std::size_t resplits = 0;
        std::wstring shownText;
        for (const auto& p : phrases_) shownText += PhraseText(p);
        for (std::size_t split = 1; split < first.length && split < reading.size(); ++split) {
            if (resplits >= kWholeSlipCandidates) break;
            const auto left = converter_->Best(reading.substr(0, split), 0, userDictionary_);
            if (!left) continue;
            const auto right = converter_->Best(reading.substr(split), left->rightId, userDictionary_);
            if (!right || left->cost + right->cost > shownCost + kSlipMargin) continue;
            // Converted as a sentence that breaks there; the first phrase
            // the one the sentence model reads best (い: 胃, not 位).
            const std::size_t breaks[] = {split};
            SyncEdgeContext();
            const auto parts = converter_->Convert(reading, {}, context_, WordsWithEnglish(), userDictionary_, breaks);
            if (parts.empty() || parts.front().length != split || parts.front().candidates.empty()) continue;
            std::wstring rest;
            for (std::size_t i = 1; i < parts.size(); ++i) {
                if (!parts[i].candidates.empty()) rest += parts[i].candidates.front().text;
            }
            const auto& heads = parts.front().candidates;
            std::size_t head = 0;
            if (reranker_ && sentenceCheck_) {
                // Offered only when it reads better than what is shown
                // (はしをわたる split as は|しをわたる is no sentence).
                constexpr std::size_t kHeads = 6;
                constexpr double kResplitNats = 0.0;
                std::vector<std::wstring> sentences{shownText};
                for (std::size_t i = 0; i < heads.size() && i < kHeads; ++i) sentences.push_back(heads[i].text + rest);
                const auto scores = reranker_->LogProbabilities(sentences, precedingText_, &rerankerMemory_);
                for (std::size_t i = 2; i < scores.size(); ++i) {
                    if (scores[i] > scores[head + 1]) head = i - 1;
                }
                if (scores[head + 1] < scores[0] - kResplitNats) continue;
            }
            if (heads[head].text + rest == shownText) continue;
            PhraseCandidate candidate = heads[head];
            candidate.text += rest;
            candidate.cost = left->cost + right->cost;
            candidate.reading = reading;
            candidate.slip = false;
            const bool shown = std::any_of(wholeInput.begin(), wholeInput.end(), [&](const PhraseCandidate& c) { return c.text == candidate.text; });
            // Before the slips: these keep what was typed.
            if (!shown) wholeInput.insert(wholeInput.begin() + static_cast<std::ptrdiff_t>(resplits++), std::move(candidate));
        }
    }

    // The keys the phrase was read from (when it starts and ends between
    // units or words).
    if (phrase.begin + phrase.length >= readingKeys_.size()) return;
    const std::size_t firstKey = readingKeys_[phrase.begin];
    const std::size_t endKey = readingKeys_[phrase.begin + phrase.length];
    std::optional<PhraseCandidate> asTyped;
    const std::wstring keys = firstKey == std::wstring::npos || endKey == std::wstring::npos || endKey <= firstKey
                                  ? std::wstring{}
                                  : allKeys.substr(firstKey, endKey - firstKey);
    if (keys.size() >= kShortestSlipKeys && keys.size() <= kSlipPhraseKeys && lettersOnly(keys)) {
        // The kana as typed; when a slip was corrected for the first choice,
        // they are not the phrase's reading, and what they convert to is
        // offered right after it.
        const std::wstring shown = conversionReading_.substr(phrase.begin, phrase.length);
        const std::wstring typedKana = typedKanaOf(keys);
        auto typed = converter_->Best(typedKana, 0, userDictionary_);
        if (typed && typed->spellingCorrection) typed.reset();
        const std::int64_t typedCost = costOf(typed);
        if (typedKana != shown) {
            asTyped = typed ? std::move(typed) : std::optional<PhraseCandidate>{KanaCandidates(typedKana).back()};
        }
        // The likeliest readings with a slip undone, each with its likeliest
        // spellings (きかいが: 機会が, 機械が), then the next readings.
        const auto found = SlipReadings(keys, {typedKana, shown}, typedCost);
        // Keys that are a word as typed (あした: 明日) are not taken for a
        // slip however cheap one reads (ました, a word that only follows
        // another): its readings wait their place further down.
        const bool typedWord = converter_->IsWord(typedKana);
        std::size_t offered = 0;
        for (std::size_t i = 0; i < found.size() && offered < kSlipCandidates; ++i) {
            likelier = likelier || (i == 0 && !typedWord && found[i].first.cost + kSlipLikelier < typedCost);
            if (i >= kSlipSpelledReadings) {
                if (offer(found[i].first)) ++offered;
                continue;
            }
            const std::size_t whole[] = {found[i].second.size()};
            SyncEdgeContext();
            const auto phrases = converter_->Convert(found[i].second, whole, 0, {}, userDictionary_);
            std::size_t spellings = 0;
            for (std::size_t c = 0; phrases.size() == 1 && c < phrases.front().candidates.size() &&
                                    spellings < kSlipSpellings && offered < kSlipCandidates;
                 ++c) {
                const auto& candidate = phrases.front().candidates[c];
                if (candidate.kind != PhraseCandidate::Kind::Dictionary || candidate.spellingCorrection) continue;
                if (offer(candidate)) ++offered;
                ++spellings;
            }
        }
    }
    // Right after the first choice when a slip reads much more likely (or
    // the first choice is already a correction), otherwise after the first
    // few candidates.
    if (asTyped && std::none_of(phrase.candidates.begin(), phrase.candidates.end(),
                                [&](const PhraseCandidate& c) { return c.text == asTyped->text; })) {
        added.insert(added.begin(), std::move(*asTyped));
        likelier = true;
    }
    // Whole-input readings never come right after the first choice (the
    // next Space would show them): after the phrase's own when those do,
    // otherwise first at kSlipPosition, as the likelier sign of a slip.
    std::erase_if(wholeInput, [&](const PhraseCandidate& candidate) {
        const auto same = [&](const PhraseCandidate& c) { return c.text == candidate.text; };
        return std::any_of(phrase.candidates.begin(), phrase.candidates.end(), same) ||
               std::any_of(added.begin(), added.end(), same);
    });
    if (wholeSlipBetter) {
        for (std::size_t k = phrase.candidates.size(); k-- > 0;) {
            if (!phrase.candidates[k].chunk) continue;
            phrase.candidates.erase(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(k));
            if (phrase.selected > k) --phrase.selected;
        }
    }
    if (wholeSlipLikely && !wholeInput.empty()) {
        added.insert(added.begin(), std::make_move_iterator(wholeInput.begin()), std::make_move_iterator(wholeInput.end()));
        likelier = true;
    } else if (likelier) {
        added.insert(added.end(), std::make_move_iterator(wholeInput.begin()), std::make_move_iterator(wholeInput.end()));
    } else {
        added.insert(added.begin(), std::make_move_iterator(wholeInput.begin()), std::make_move_iterator(wholeInput.end()));
    }
    if (added.empty()) return;
    // Only the likeliest on the first page, with the words read the same
    // and the emoji; the rest after the first page, not filling it.
    const std::size_t firstPage = pageSize_;
    const std::size_t at = SlipPosition(phrase.candidates, likelier);
    phrase.candidates.insert(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(at), std::move(added.front()));
    if (phrase.selected >= at) ++phrase.selected;
    const std::size_t rest = std::min(phrase.candidates.size(), std::max(at + 1, firstPage));
    phrase.candidates.insert(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(rest),
                             std::make_move_iterator(added.begin() + 1), std::make_move_iterator(added.end()));
    if (phrase.selected >= rest) phrase.selected += added.size() - 1;
}

std::size_t JapaneseComposer::SlipPosition(const std::vector<PhraseCandidate>& candidates, bool likelier) noexcept {
    // A chunk Mix took as English is not a slip of Japanese: Japanese read
    // from its keys (online: 鬼ね) waits further down.
    if (!candidates.empty() && candidates.front().kind == PhraseCandidate::Kind::English) likelier = false;
    // Nor are keys that spell a word in both languages (sake: 避け, Sake,
    // then 酒, not だけ): their own words come before a slip undone.
    if (std::any_of(candidates.begin(), candidates.end(), [](const PhraseCandidate& c) { return c.otherLanguage; })) {
        likelier = false;
    }
    std::size_t at = std::min(candidates.size(), likelier ? std::size_t{1} : kSlipPosition);
    // The chunk in the other language keeps its place right after the
    // first (data: 立た, data, then だった).
    while (at < candidates.size() && at > 0 && candidates[at].otherLanguage) ++at;
    return at;
}

bool JapaneseComposer::SamePhrasing(const std::wstring& reading, const std::vector<std::size_t>& starts,
                                    const std::wstring& corrected) const {
    // Where the two readings differ, and the phrases the corrected one makes.
    std::size_t prefix = 0;
    while (prefix < reading.size() && prefix < corrected.size() && reading[prefix] == corrected[prefix]) ++prefix;
    std::size_t suffix = 0;
    while (suffix < reading.size() - prefix && suffix < corrected.size() - prefix &&
           reading[reading.size() - 1 - suffix] == corrected[corrected.size() - 1 - suffix]) {
        ++suffix;
    }
    std::vector<std::size_t> correctedStarts;
    SyncEdgeContext();
    for (const auto& phrase : converter_->Convert(corrected, {}, 0, {}, userDictionary_)) correctedStarts.push_back(phrase.begin);
    // Phrase starts outside the difference, in the reading's positions; one
    // inside it is phrased differently.
    const auto mapped = [&](const std::vector<std::size_t>& list, std::size_t size) {
        std::vector<std::size_t> out;
        for (const std::size_t start : list) {
            if (start > prefix && start < size - suffix) return std::optional<std::vector<std::size_t>>{};
            out.push_back(start <= prefix ? start : start - size + reading.size());
        }
        return std::optional<std::vector<std::size_t>>{out};
    };
    const auto before = mapped(starts, reading.size());
    const auto after = mapped(correctedStarts, corrected.size());
    return before && after && *before == *after;
}

}  // namespace tekito::japanese
