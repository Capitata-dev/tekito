#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// Candidate lists: numbers in their style, other forms of what was typed,
// longer words, chunk candidates, loanwords, dates and symbols, and the order
// of a list's first page.

namespace tekito::japanese {

// A number read in kana, written in digits by the converter, in `style`.
std::wstring JapaneseComposer::KanaNumberText(const std::wstring& text, NumberStyle style) {
    std::wstring out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] < L'0' || text[i] > L'9') {
            out += text[i++];
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && text[end] >= L'0' && text[end] <= L'9') ++end;
        const std::wstring digits = text.substr(i, end - i);
        // Too long to be a number said (a phone or card number): as it is.
        constexpr std::size_t kLongestNumber = 16;
        if (digits.size() > kLongestNumber) {
            out += digits;
            i = end;
            continue;
        }
        switch (style) {
        case NumberStyle::Kanji: out += KanjiNumber(std::stoull(digits)); break;
        case NumberStyle::FullWidth:
            for (const wchar_t c : digits) out += static_cast<wchar_t>(c + 0xFEE0);
            break;
        case NumberStyle::Official:
            // A year is written without commas (2026年).
            out += end < text.size() && text[end] == L'年' ? digits : SpecialConversions::Official(digits);
            break;
        case NumberStyle::Mixed:
            if (digits.size() == 1) {
                out += static_cast<wchar_t>(digits[0] + 0xFEE0);
            } else {
                out += digits;
            }
            break;
        default: out += digits; break;
        }
        i = end;
    }
    return out;
}

// Numbers read in kana (written in digits by the converter) in the number
// style, and the other way of writing them next (2005年, 二千五年).
void JapaneseComposer::StyleKanaNumbers(std::vector<PhraseCandidate>& candidates) const {
    {
        const auto first = std::find_if(candidates.begin(), candidates.end(), [](const PhraseCandidate& c) { return c.kanaNumber; });
        if (first == candidates.end()) return;
        // The other way of the first: kanji after digits, digits after kanji.
        // The other way only with other forms on (数字の変換: ほかの表記).
        PhraseCandidate other = *first;
        other.text = KanaNumberText(first->text, numberStyle_ == NumberStyle::Kanji ? NumberStyle::HalfWidth : NumberStyle::Kanji);
        for (auto& candidate : candidates) {
            if (candidate.kanaNumber) candidate.text = KanaNumberText(candidate.text, numberStyle_);
        }
        // Written alike now (三百 in kanji and the dictionary's): once.
        for (std::size_t i = 1; i < candidates.size(); ++i) {
            const auto same = std::find_if(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(i),
                                           [&](const PhraseCandidate& c) { return c.text == candidates[i].text; });
            if (same != candidates.begin() + static_cast<std::ptrdiff_t>(i)) candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(i--));
        }
        // Next to the number, the dictionary's own (三 for さん) moved up
        // there; one above the number (五十 of 五十歩百歩) stays.
        if (specialOptions_.numbers) {
            const auto number = static_cast<std::size_t>(
                std::find_if(candidates.begin(), candidates.end(), [](const PhraseCandidate& c) { return c.kanaNumber; }) -
                candidates.begin());
            const auto same = static_cast<std::size_t>(
                std::find_if(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) { return c.text == other.text; }) -
                candidates.begin());
            if (same == candidates.size()) {
                candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(number + 1), std::move(other));
            } else if (same > number + 1) {
                std::rotate(candidates.begin() + static_cast<std::ptrdiff_t>(number + 1),
                            candidates.begin() + static_cast<std::ptrdiff_t>(same),
                            candidates.begin() + static_cast<std::ptrdiff_t>(same + 1));
            }
        }
    }
}

void JapaneseComposer::WidenDigits(PhraseState& phrase, std::wstring_view reading) const {
    if (phrase.form || phrase.selected >= phrase.candidates.size()) return;
    // Digits typed stay digits (5安打, not 五安打): the first candidate that
    // keeps them, when the one shown does not.
    const auto digitsOf = [](std::wstring_view text) {
        std::wstring digits;
        for (const wchar_t c : text) {
            if (c >= L'0' && c <= L'9') digits += c;
            else if (c >= L'\xFF10' && c <= L'\xFF19') digits += static_cast<wchar_t>(c - 0xFEE0);
        }
        return digits;
    };
    const std::wstring typed = digitsOf(reading);
    if (!typed.empty() && digitsOf(phrase.candidates[phrase.selected].text) != typed) {
        const auto keeps = std::find_if(phrase.candidates.begin(), phrase.candidates.end(), [&](const PhraseCandidate& c) {
            return c.reading.empty() && digitsOf(c.text) == typed;
        });
        if (keeps != phrase.candidates.end()) {
            phrase.selected = static_cast<std::size_t>(keeps - phrase.candidates.begin());
        }
    }
    const auto widened = [&](const std::wstring& text) {
        std::wstring out = text;
        for (std::size_t i = 0; i < out.size();) {
            const auto digit = [&](std::size_t at) {
                return at < out.size() && ((out[at] >= L'0' && out[at] <= L'9') || (out[at] >= L'\xFF10' && out[at] <= L'\xFF19'));
            };
            if (!digit(i)) {
                ++i;
                continue;
            }
            std::size_t end = i;
            while (digit(end)) ++end;
            bool half = true;
            switch (numberStyle_) {
            case NumberStyle::AsTyped: half = halfWidthDigits_ && !(singleDigitFull_ && end - i == 1); break;
            case NumberStyle::FullWidth: half = false; break;
            case NumberStyle::Mixed: half = end - i > 1; break;
            default: half = true; break;
            }
            for (std::size_t k = i; k < end; ++k) {
                const wchar_t c = out[k];
                if (half && c >= L'\xFF10') out[k] = static_cast<wchar_t>(c - 0xFEE0);
                if (!half && c <= L'9') out[k] = static_cast<wchar_t>(c + 0xFEE0);
            }
            i = end;
        }
        return MarksAsDigits(std::move(out));
    };
    auto& shown = phrase.candidates[phrase.selected];
    const std::wstring wanted = widened(shown.text);
    if (wanted == shown.text) return;
    // The form already listed is taken, first in the list as the style's
    // own (1.1 before １．１); otherwise the one shown is written so.
    const auto listed = std::find_if(phrase.candidates.begin(), phrase.candidates.end(),
                                     [&](const PhraseCandidate& c) { return c.text == wanted; });
    if (listed != phrase.candidates.end()) {
        if (phrase.selected == 0) {
            std::rotate(phrase.candidates.begin(), listed, listed + 1);
        } else {
            phrase.selected = static_cast<std::size_t>(listed - phrase.candidates.begin());
        }
    } else {
        shown.text = wanted;
    }
}

void JapaneseComposer::ApplyNumberStyle(std::size_t from) {
    // A rough number is part of a word and written in kanji: 数十人, 十数人,
    // 十余年, 二、三日.
    constexpr std::wstring_view kNumerals = L"〇一二三四五六七八九";
    constexpr std::wstring_view kRoughBefore = L"数何幾";
    constexpr std::wstring_view kRoughAfter = L"数余";
    const auto digitOf = [](wchar_t c) { return c >= L'\xFF10' ? c - L'\xFF10' : c - L'0'; };
    for (std::size_t index = from; index < phrases_.size(); ++index) {
        auto& phrase = phrases_[index];
        if (phrase.candidates.empty() || phrase.begin + phrase.length > conversionReading_.size()) continue;
        const std::wstring_view reading = std::wstring_view(conversionReading_).substr(phrase.begin, phrase.length);
        const std::wstring before = index > 0 ? PhraseText(phrases_[index - 1]) : std::wstring{};
        const std::wstring after = index + 1 < phrases_.size() ? PhraseText(phrases_[index + 1]) : std::wstring{};
        // 2、3 before a counter (日, 人): 二、三.
        if (roughKanji_ && reading.size() == 3 && IsDigit(reading[0]) && IsDigit(reading[2]) &&
            std::wstring_view(L",、，").find(reading[1]) != std::wstring_view::npos &&
            digitOf(reading[2]) == digitOf(reading[0]) + 1 && digitOf(reading[0]) >= 1 && !after.empty() &&
            after.front() >= 0x4E00 && after.front() <= 0x9FFF) {
            const std::wstring rough{kNumerals[digitOf(reading[0])], L'、', kNumerals[digitOf(reading[2])]};
            auto& candidates = phrase.candidates;
            std::erase_if(candidates, [&](const PhraseCandidate& c) { return c.text == rough; });
            candidates.insert(candidates.begin(), {rough, candidates.empty() ? 0 : candidates.front().cost,
                                                   PhraseCandidate::Kind::Dictionary, false});
            phrase.selected = 0;
            phrase.form.reset();
            continue;
        }
        // A year read in kana is written without commas too (にせんにじゅう
        // ろくねん: 2026年).
        if (numberStyle_ == NumberStyle::Official && !after.empty() && after.front() == L'年') {
            for (auto& candidate : phrase.candidates) {
                if (candidate.kanaNumber) std::erase(candidate.text, L',');
            }
        }
        if (!IsDigits(reading)) {
            // Digits in a phrase with other words (5安打): the width the
            // number style asks for, as a phrase of digits alone gets.
            if (std::any_of(reading.begin(), reading.end(), IsDigit)) WidenDigits(phrase, reading);
            continue;
        }
        // 数 as a word of its own before it (数十人), not the end of one
        // (打数5安打).
        const bool rough = roughKanji_ && ((before.size() == 1 && kRoughBefore.find(before.back()) != std::wstring_view::npos) ||
                                           (!after.empty() && kRoughAfter.find(after.front()) != std::wstring_view::npos));
        std::wstring half(reading);
        for (auto& c : half) {
            if (c >= L'\xFF10') c = static_cast<wchar_t>(c - 0xFEE0);
        }
        std::wstring full = half;
        for (auto& c : full) c = static_cast<wchar_t>(c + 0xFEE0);
        std::wstring wanted;
        NumberStyle style = rough ? NumberStyle::Kanji : numberStyle_;
        // Digits from 0 (006, 0120) or joined to others by a dash (090-1234)
        // are a code, not a number: kept as typed, none dropped.
        // The dash may be a phrase of its own (090|ー|1234).
        constexpr std::wstring_view kDashes = L"-ー－―‐";
        const auto digit = [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'\xFF10' && c <= L'\xFF19'); };
        const auto dash = [&](wchar_t c) { return kDashes.find(c) != std::wstring_view::npos; };
        std::wstring dashedBefore = before;
        if (before.size() == 1 && dash(before[0]) && index >= 2) dashedBefore = PhraseText(phrases_[index - 2]) + before;
        std::wstring dashedAfter = after;
        if (after.size() == 1 && dash(after[0]) && index + 2 < phrases_.size()) dashedAfter = after + PhraseText(phrases_[index + 2]);
        const bool dashed = (dashedBefore.size() >= 2 && dash(dashedBefore.back()) && digit(dashedBefore[dashedBefore.size() - 2])) ||
                            (dashedAfter.size() >= 2 && dash(dashedAfter.front()) && digit(dashedAfter[1]));
        if (((half.size() > 1 && half.front() == L'0') || dashed) &&
            (style == NumberStyle::Kanji || style == NumberStyle::Official)) {
            style = NumberStyle::AsTyped;
        }
        switch (style) {
        case NumberStyle::AsTyped:
            wanted = halfWidthDigits_ && !(singleDigitFull_ && half.size() == 1) ? half : full;
            break;
        case NumberStyle::HalfWidth: wanted = half; break;
        case NumberStyle::FullWidth: wanted = full; break;
        case NumberStyle::Mixed: wanted = half.size() == 1 ? full : half; break;
        case NumberStyle::Kanji: break;
        case NumberStyle::Official:
            // A year is written without commas (2026年).
            wanted = !after.empty() && after.front() == L'年' ? half : SpecialConversions::Official(half);
            break;
        }
        auto& candidates = phrase.candidates;
        auto found = candidates.end();
        if (style == NumberStyle::Kanji) {
            // The first written with kanji numerals as read (十五, not 一五 or 壱拾伍).
            constexpr std::wstring_view kNumerals = L"〇一二三四五六七八九十百千万億兆";
            found = std::find_if(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) {
                return !c.text.empty() && std::all_of(c.text.begin(), c.text.end(), [&](wchar_t ch) {
                    return kNumerals.find(ch) != std::wstring_view::npos;
                });
            });
        } else {
            found = std::find_if(candidates.begin(), candidates.end(),
                                 [&](const PhraseCandidate& c) { return c.text == wanted; });
            if (found == candidates.end()) {
                candidates.insert(candidates.begin(), {wanted, candidates.front().cost, PhraseCandidate::Kind::Hiragana, false});
                found = candidates.begin();
            }
        }
        if (found == candidates.end()) continue;
        std::rotate(candidates.begin(), found, found + 1);
        phrase.selected = 0;
        phrase.form.reset();
    }
    // Digits joined by marks, across phrases (090|ー|1234, 1|．|1), written
    // as one code (CodeMarks): 090-1234-5678 and 1.1.1, not 090ー1234ー5678,
    // or 1．1．1 when typing is full-width and the style half-width.
    std::wstring whole;
    std::vector<std::size_t> starts;
    for (const auto& phrase : phrases_) {
        starts.push_back(whole.size());
        whole += PhraseText(phrase);
    }
    const std::wstring coded = CodeMarks(whole);
    if (coded == whole) return;
    for (std::size_t index = from; index < phrases_.size(); ++index) {
        auto& phrase = phrases_[index];
        if (phrase.form || phrase.selected >= phrase.candidates.size()) continue;
        const std::wstring shown = PhraseText(phrase);
        const std::wstring wanted = coded.substr(starts[index], shown.size());
        if (wanted == shown) continue;
        auto& candidates = phrase.candidates;
        const auto listed = std::find_if(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) { return c.text == wanted; });
        if (listed != candidates.end()) {
            phrase.selected = static_cast<std::size_t>(listed - candidates.begin());
        } else {
            candidates[phrase.selected].text = wanted;
        }
    }
}

void JapaneseComposer::AddVariantForms() {
    // Variants of the first few words only, a few each: names are where
    // they are wanted (髙橋, 山﨑, 澤田), and the list stays short.
    constexpr std::size_t kWords = 3;
    constexpr std::size_t kForms = 6;
    if (!kanjiVariants_ || !kanjiSets_ || !kanjiSets_->HasVariants() || (chunkJudge_ && ChoseEnglish())) return;
    for (auto& phrase : phrases_) {
        auto& candidates = phrase.candidates;
        std::vector<PhraseCandidate> added;
        std::size_t words = 0;
        for (const auto& candidate : candidates) {
            if (words == kWords || added.size() == kForms) break;
            if (candidate.kind != PhraseCandidate::Kind::Dictionary || !candidate.reading.empty()) continue;
            ++words;
            for (auto& form : kanjiSets_->VariantForms(candidate.text, kForms - added.size())) {
                const auto same = [&](const PhraseCandidate& c) { return c.text == form; };
                if (std::any_of(candidates.begin(), candidates.end(), same) || std::any_of(added.begin(), added.end(), same)) {
                    continue;
                }
                PhraseCandidate variant = candidate;
                variant.text = std::move(form);
                variant.slip = false;
                added.push_back(std::move(variant));
            }
        }
        candidates.insert(candidates.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    }
}

void JapaneseComposer::AddTypedForms() {
    if (!typedForms_ || (chunkJudge_ && ChoseEnglish())) return;
    // The keys are known for each part of the reading when it is the reading
    // typed, or when readingKeys_ maps the phrase back to the typed keys
    // after a slip was undone.
    const bool typedReading = conversionReading_ == Reading();
    // Small kana (っ, ぃ), when every kana has one.
    const auto small = [](std::wstring text) {
        constexpr std::wstring_view kLarge = L"あいうえおつやゆよわ";
        constexpr std::wstring_view kSmall = L"ぁぃぅぇぉっゃゅょゎ";
        for (auto& c : text) {
            const auto at = kLarge.find(c);
            if (at == std::wstring_view::npos) return std::wstring{};
            c = kSmall[at];
        }
        return text;
    };
    const auto upper = [](std::wstring text) {
        for (auto& c : text) c = static_cast<wchar_t>(std::towupper(c));
        return text;
    };
    for (auto& phrase : phrases_) {
        if (phrase.candidates.empty() || !phrase.candidates.front().reading.empty()) continue;
        const std::wstring reading = conversionReading_.substr(phrase.begin, phrase.length);
        // Physical keys for this phrase (via readingKeys_ after a slip).
        std::wstring keys = KeysFor(phrase.begin, phrase.length);
        // Forms show what was typed: after a slip, kana of the keys as typed
        // rather than the corrected reading.
        std::wstring formReading = reading;
        if (!typedReading && table_ && !keys.empty()) {
            std::wstring kana;
            for (const auto& token : ParseRomaji(*table_, keys)) kana += ToFullWidthAscii(token.kana);
            if (!kana.empty()) formReading = ApplyTypingStyle(std::move(kana));
        }
        // Letters alone (q) already offer the other width next; do not move
        // those width pairs into the typed-forms tail.
        if (std::none_of(formReading.begin(), formReading.end(),
                         [](wchar_t c) { return c >= 0x3041 && c <= 0x30FA; })) {
            continue;
        }
        // In the order Up reaches them from the first candidate (the list
        // goes round to its end): hiragana, katakana, half-width katakana,
        // the keys in both widths and cases, small kana.
        std::vector<std::pair<std::wstring, PhraseCandidate::Kind>> forms{
            {ToHiragana(formReading), PhraseCandidate::Kind::Hiragana},
            {ToKatakana(formReading), PhraseCandidate::Kind::Katakana},
            {ToHalfWidthKatakana(formReading), PhraseCandidate::Kind::Katakana}};
        if (!keys.empty()) {
            forms.emplace_back(ToFullWidthAscii(keys), PhraseCandidate::Kind::English);
            forms.emplace_back(ToFullWidthAscii(upper(keys)), PhraseCandidate::Kind::English);
            forms.emplace_back(keys, PhraseCandidate::Kind::English);
            forms.emplace_back(upper(keys), PhraseCandidate::Kind::English);
        }
        forms.emplace_back(small(ToHiragana(formReading)), PhraseCandidate::Kind::Hiragana);
        auto& candidates = phrase.candidates;
        // What the sentence check chose stays chosen as the list moves.
        const std::wstring chosen = phrase.selected < candidates.size() ? candidates[phrase.selected].text : std::wstring{};
        // One or two kana (i, ki, ai): the long tail of kanji read so,
        // outside 常用 and 人名用 and hardly written (獺, 鷽), after the
        // others; words in such kanji that are written (嘘, 飴, 蜘蛛) stay
        // where they are, and so does the first.
        if (typedReading && phrases_.size() == 1 && reading.size() <= 2 && kanjiSets_ && candidates.size() > 1) {
            constexpr double kWrittenCount = 3.5;  // ln count: 蟻 4.0, 鯖 4.1; 獺 2.9, 蠅 2.3
            std::stable_partition(candidates.begin() + 1, candidates.end(), [&](const PhraseCandidate& c) {
                if (c.text == chosen || kanjiSets_->Common(c.text)) return true;
                return converter_ && converter_->WordCount(c.text).value_or(0.0) >= kWrittenCount;
            });
        }
        // The forms end the list, the first one last, all but the first
        // candidate moved there.
        const std::int64_t cost = candidates.back().cost;
        std::vector<PhraseCandidate> tail;
        for (auto& [text, kind] : forms) {
            if (text.empty()) continue;
            if (std::any_of(tail.begin(), tail.end(), [&](const PhraseCandidate& c) { return c.text == text; })) continue;
            const auto seen = std::find_if(candidates.begin(), candidates.end(),
                                           [&](const PhraseCandidate& c) { return c.text == text; });
            // The first stays, and so does the keys as typed after a slip
            // undone (its second place).
            if (seen == candidates.begin() || (seen != candidates.end() && !seen->reading.empty())) continue;
            if (seen != candidates.end()) {
                tail.push_back(std::move(*seen));
                candidates.erase(seen);
            } else {
                tail.push_back({std::move(text), cost, kind, false});
            }
        }
        if (tail.empty()) {
            const auto kept = std::find_if(candidates.begin(), candidates.end(),
                                           [&](const PhraseCandidate& c) { return c.text == chosen; });
            if (kept != candidates.end()) phrase.selected = static_cast<std::size_t>(kept - candidates.begin());
            continue;
        }
        for (auto& form : tail) form.group = PhraseCandidate::Group::Typed;
        candidates.insert(candidates.end(), std::make_move_iterator(tail.rbegin()), std::make_move_iterator(tail.rend()));
        const auto kept = std::find_if(candidates.begin(), candidates.end(),
                                       [&](const PhraseCandidate& c) { return c.text == chosen; });
        if (kept != candidates.end()) phrase.selected = static_cast<std::size_t>(kept - candidates.begin());
    }
}

// "..." typed (。。。) is an ellipsis: one phrase written … first (‥ for
// two), with the marks as typed next.
void JapaneseComposer::JoinEllipsis() {
    for (std::size_t i = 0; i < phrases_.size(); ++i) {
        std::size_t end = i;
        std::size_t length = 0;
        while (end < phrases_.size() && phrases_[end].begin + phrases_[end].length <= conversionReading_.size()) {
            const auto reading = std::wstring_view(conversionReading_).substr(phrases_[end].begin, phrases_[end].length);
            if (reading.empty() || std::any_of(reading.begin(), reading.end(), [](wchar_t c) { return c != 0x3002; })) break;
            length += phrases_[end].length;
            ++end;
        }
        if (length < 2 || length > 3) continue;
        PhraseState joined;
        joined.begin = phrases_[i].begin;
        joined.length = length;
        const std::wstring typed(length, static_cast<wchar_t>(0x3002));
        const std::wstring ellipsis(1, static_cast<wchar_t>(length == 3 ? 0x2026 : 0x2025));
        joined.candidates.push_back({ellipsis, 0, PhraseCandidate::Kind::Special, false});
        joined.candidates.push_back({typed, 0, PhraseCandidate::Kind::Hiragana, false});
        joined.candidates.push_back({std::wstring(length, L'.'), 0, PhraseCandidate::Kind::Hiragana, false});
        phrases_.erase(phrases_.begin() + static_cast<std::ptrdiff_t>(i), phrases_.begin() + static_cast<std::ptrdiff_t>(end));
        phrases_.insert(phrases_.begin() + static_cast<std::ptrdiff_t>(i), std::move(joined));
        if (focus_ >= phrases_.size()) focus_ = phrases_.size() - 1;
    }
}

void JapaneseComposer::PutShownFirst() {
    for (auto& phrase : phrases_) {
        if (phrase.form || phrase.selected == 0 || phrase.selected >= phrase.candidates.size()) continue;
        std::rotate(phrase.candidates.begin(), phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected),
                    phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected) + 1);
        phrase.selected = 0;
        phrase.chosenByCheck = true;
    }
}

void JapaneseComposer::TidyFirstPage(PhraseState& phrase) const {
    // The first page goes to words: one date or time, one symbol or emoji,
    // and words with only their first kana apart (は死, よろ敷く: what the
    // reading starts with, then another word) after the others; those move
    // to the start of the second page, in their order. What shows stays
    // first. Nothing leaves the list.
    auto& list = phrase.candidates;
    if (phrase.form || phrase.selected != 0 || list.size() <= pageSize_ || pageSize_ < 3) return;
    // Words the dictionary has for the reading as they are (この間, その後)
    // are words, not pieces.
    std::vector<std::wstring> words;
    if (converter_ && phrase.begin + phrase.length <= conversionReading_.size()) {
        for (const auto& word : converter_->Words(std::wstring_view(conversionReading_).substr(phrase.begin, phrase.length))) {
            words.push_back(word.text);
        }
    }
    const auto hiragana = [](wchar_t c) { return c >= 0x3041 && c <= 0x3096; };
    const auto kanji = [](wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005; };
    const auto pieceOfWords = [&](const PhraseCandidate& c) {
        if (c.kind != PhraseCandidate::Kind::Dictionary || c.name || c.setPhrase || c.chunk || c.length != 0 || c.text.size() < 2 ||
            std::find(words.begin(), words.end(), c.text) != words.end() ||
            !hiragana(c.text[0]) || std::wstring_view(L"おごみ").find(c.text[0]) != std::wstring_view::npos) {
            return false;
        }
        std::size_t kana = 0;
        while (kana < c.text.size() && hiragana(c.text[kana])) ++kana;
        return kana < c.text.size() && kanji(c.text[kana]);
    };
    // A symbol or emoji, from the symbols or from the dictionary (⛲️, ∠):
    // nothing in it that writes words.
    const auto symbolic = [](const PhraseCandidate& c) {
        return c.group == PhraseCandidate::Group::Symbol ||
               (!c.text.empty() && std::none_of(c.text.begin(), c.text.end(), [](wchar_t ch) {
                   return std::iswalnum(ch) != 0 || (ch >= 0x3041 && ch <= 0x30FF) || (ch >= 0x4E00 && ch <= 0x9FFF) ||
                          (ch >= 0xFF10 && ch <= 0xFF5A) || ch == 0x3005;
               }));
    };
    // The next symbols, three at most, start the second page; the forms of
    // what was typed stay at the end.
    constexpr std::size_t kSecondPageSymbols = 3;
    bool date = false;
    std::size_t symbols = 0;
    std::vector<PhraseCandidate> first{std::move(list[0])}, later, rest, typed;
    for (std::size_t i = 1; i < list.size(); ++i) {
        auto& c = list[i];
        if (c.group == PhraseCandidate::Group::Typed) {
            typed.push_back(std::move(c));
            continue;
        }
        bool down = false;
        bool rather = false;  // down, but not to the second page's start
        if (c.group == PhraseCandidate::Group::Date) {
            down = date;
            date = true;
        } else if (symbolic(c)) {
            down = symbols > 0;
            rather = symbols > kSecondPageSymbols;
            ++symbols;
        } else {
            down = pieceOfWords(c);
        }
        if (first.size() < pageSize_ && !down) {
            first.push_back(std::move(c));
        } else if (first.size() < pageSize_ && !rather) {
            later.push_back(std::move(c));
        } else {
            rest.push_back(std::move(c));
        }
    }
    // Too few words to fill the page: those moved down fill it, in order.
    std::size_t moved = 0;
    while (first.size() < pageSize_ && moved < later.size()) first.push_back(std::move(later[moved++]));
    list = std::move(first);
    for (std::size_t i = moved; i < later.size(); ++i) list.push_back(std::move(later[i]));
    for (auto& c : rest) list.push_back(std::move(c));
    for (auto& c : typed) list.push_back(std::move(c));
}

void JapaneseComposer::SelectStyledNumbers() {
    // A number the sentence check or the AI model took as a dictionary word,
    // in its own notation or in the other way StyleKanaNumbers offers (二十
    // or 22 where the style writes 20) is the converter's number as the
    // number style writes it: the first of them.
    for (auto& phrase : phrases_) {
        auto& candidates = phrase.candidates;
        if (phrase.selected >= candidates.size() || !candidates[phrase.selected].reading.empty()) continue;
        // A number alone, or digits and what follows them (22人: 22, 人), or
        // the converter's number; not kanji that only start like one (十分,
        // 一緒).
        const auto& text = candidates[phrase.selected].text;
        const bool digits = std::any_of(text.begin(), text.end(), IsDigit);
        const auto picked = LeadingNumber(text);
        if (!picked || (!digits && !picked->second.empty() && !candidates[phrase.selected].kanaNumber)) continue;
        const auto number = std::find_if(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) {
            return c.kanaNumber && LeadingNumber(c.text) == picked;
        });
        if (number != candidates.end()) phrase.selected = static_cast<std::size_t>(number - candidates.begin());
    }
}

void JapaneseComposer::AddLongerWords(std::size_t i) {
    // A word of the dictionary over this phrase and the next ones (へ|いき:
    // 兵器, 平気, 併記) is offered too, whatever the phrases' breaks, after
    // the phrase's first few: up to this many, over up to this many phrases.
    // Added when the list opens, as the slips are, so that the choices made
    // before (learning, the sentence check) never pick one of them.
    constexpr std::size_t kLongerWords = 4;
    constexpr std::size_t kPhrasesAhead = 3;
    constexpr std::size_t kAfter = 3;
    // A word this costly is too rare to offer this way, unless it is all
    // that was typed (といろ: 十色).
    constexpr std::int64_t kRareWord = 7000;
    const std::wstring& reading = conversionReading_;
    if (!converter_ || i + 1 >= phrases_.size()) return;
    auto& phrase = phrases_[i];
    if (phrase.candidates.empty() || std::any_of(phrase.candidates.begin(), phrase.candidates.end(), [](const PhraseCandidate& c) {
            return c.length > 0 && !c.chunk;
        })) {
        return;
    }
    std::vector<PhraseCandidate> longer;
    for (std::size_t j = i + 1; j < phrases_.size() && j <= i + kPhrasesAhead; ++j) {
        const std::size_t end = phrases_[j].begin + phrases_[j].length;
        if (end > reading.size() || end <= phrase.begin) break;
        for (auto& word : converter_->Words(std::wstring_view(reading).substr(phrase.begin, end - phrase.begin))) {
            if (word.cost > kRareWord && (phrase.begin != 0 || end != reading.size())) continue;
            word.length = end - phrase.begin;
            longer.push_back(std::move(word));
        }
    }
    // A word for all that was typed first (なないろ: 七色 before なな's 7 and
    // 奈々), then by cost.
    const auto whole = [&](const PhraseCandidate& c) { return phrase.begin == 0 && phrase.begin + c.length == reading.size(); };
    std::stable_sort(longer.begin(), longer.end(), [&](const PhraseCandidate& a, const PhraseCandidate& b) {
        if (whole(a) != whole(b)) return whole(a);
        return a.cost < b.cost;
    });
    std::size_t at = std::min(kAfter, phrase.candidates.size());
    std::size_t added = 0;
    for (auto& word : longer) {
        if (added >= kLongerWords) break;
        const bool listed = std::any_of(phrase.candidates.begin(), phrase.candidates.end(),
                                        [&](const PhraseCandidate& c) { return c.text == word.text; });
        if (listed) continue;
        phrase.candidates.insert(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(at++), std::move(word));
        ++added;
    }
    // A date said over phrases (らいしゅうの|きんようびに: 来週の金曜日に,
    // 11/20(金)に) right after the phrase's first, with what follows it in
    // the last phrase as that phrase writes it.
    if (!special_ || !specialOptions_.dates) return;
    const auto now = LocalTime::Now();
    for (std::size_t j = std::min(phrases_.size() - 1, i + kPhrasesAhead); j > i; --j) {
        const std::size_t end = phrases_[j].begin + phrases_[j].length;
        if (end > reading.size() || end <= phrases_[j].begin) continue;
        const std::wstring lastText = PhraseText(phrases_[j]);
        for (std::size_t length = end - phrase.begin; length > phrases_[j].begin - phrase.begin; --length) {
            const std::wstring_view suffix = std::wstring_view(reading).substr(phrase.begin + length, end - phrase.begin - length);
            if (!lastText.ends_with(suffix)) break;
            const auto dates = special_->Dates(SpecialConversions::Language::Japanese,
                                               std::wstring_view(reading).substr(phrase.begin, length), now);
            if (dates.empty()) continue;
            std::size_t place = std::min<std::size_t>(1, phrase.candidates.size());
            for (const auto& date : dates) {
                PhraseCandidate candidate{date + std::wstring(suffix), 0, PhraseCandidate::Kind::Special, false};
                candidate.length = end - phrase.begin;
                phrase.candidates.insert(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(place++), std::move(candidate));
                if (phrase.selected >= place - 1 && phrase.selected != 0) ++phrase.selected;
            }
            return;
        }
    }
}

void JapaneseComposer::AddChunkCandidates(std::size_t i) {
    // The phrase and the ones after it to the end (at most this many), else
    // it and the next one: two other ways to write them as a whole, from the
    // sentences the sentence check read, if they score no more than this
    // below the sentence shown, as the check weighs them (箸を渡る and
    // 構想を寝る do). One that means something else and one written another
    // way (子ども for 子供, わかりました for 分かりました), else two of either.
    // Not one that takes the digits out of a number (3時に: 惨事に), nor one
    // written in katakana the phrase is not in (三時: サンジ).
    constexpr std::size_t kWholeRest = 3;
    constexpr std::size_t kChunks = 2;
    constexpr float kLessLikely = 2.0f;
    if (!chunkCandidates_ || i + 1 >= phrases_.size()) return;
    auto& phrase = phrases_[i];
    if (phrase.chunksTried) return;
    phrase.chunksTried = true;
    if (phrase.candidates.empty() ||
        std::any_of(phrase.candidates.begin(), phrase.candidates.end(), [](const PhraseCandidate& c) { return c.chunk; })) {
        return;
    }
    const auto& checked = lastCheckedSentences_;
    if (checked.reading != conversionReading_ || checked.scores.size() != checked.shown.size()) return;
    // Letters romaji left (otukarw): a slip, which the slip candidates read.
    if (std::any_of(conversionReading_.begin(), conversionReading_.end(),
                    [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= 0xFF21 && c <= 0xFF5A); })) {
        return;
    }
    const std::size_t last = phrases_.size() - i <= kWholeRest ? phrases_.size() - 1 : i + 1;
    std::wstring before, chunk, after;
    for (std::size_t k = 0; k < phrases_.size(); ++k) {
        (k < i ? before : k <= last ? chunk : after) += PhraseText(phrases_[k]);
    }
    const std::wstring shown = before + chunk + after;
    // The sentence shown, or the one the check chose, as the measure.
    const auto at = std::find(checked.shown.begin(), checked.shown.end(), shown);
    const std::size_t base = at != checked.shown.end() ? static_cast<std::size_t>(at - checked.shown.begin()) : checked.chosen;
    if (base >= checked.scores.size()) return;
    const auto kanji = [](std::wstring_view text) {
        std::wstring found;
        for (const wchar_t c : text) {
            if (((c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005) && found.find(c) == std::wstring::npos) found += c;
        }
        return found;
    };
    const auto within = [](const std::wstring& some, const std::wstring& all) {
        return std::all_of(some.begin(), some.end(), [&](wchar_t c) { return all.find(c) != std::wstring::npos; });
    };
    const std::wstring ownKanji = kanji(chunk);
    const auto digits = [](std::wstring_view text) {
        return std::any_of(text.begin(), text.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= 0xFF10 && c <= 0xFF19); });
    };
    const auto katakana = [](std::wstring_view text) {
        std::wstring found;
        for (const wchar_t c : text) {
            if (c >= 0x30A1 && c <= 0x30FA) found += c;
        }
        return found;
    };
    const std::wstring ownKatakana = katakana(chunk);
    // Letters and digits in one width (でしｔ is でしt written wider), and
    // the marks as one (雨. is 雨。 with the other period).
    const auto narrow = [](std::wstring text) {
        for (auto& c : text) {
            if (c >= 0xFF01 && c <= 0xFF5E) c = static_cast<wchar_t>(c - 0xFEE0);
            if (c == L'。') c = L'.';
            if (c == L'、') c = L',';
        }
        return text;
    };
    const std::wstring ownNarrow = narrow(chunk);
    struct Found {
        std::wstring text;
        float score;
        bool written;  // written another way, not another meaning
    };
    std::vector<Found> found;
    for (std::size_t k = 0; k < checked.shown.size(); ++k) {
        const auto& sentence = checked.shown[k];
        if (checked.scores[k] < checked.scores[base] - kLessLikely) continue;
        if (sentence.size() < before.size() + after.size() || !sentence.starts_with(before) || !sentence.ends_with(after)) continue;
        std::wstring text = sentence.substr(before.size(), sentence.size() - before.size() - after.size());
        if (text.empty() || text == chunk ||
            std::any_of(found.begin(), found.end(), [&](const Found& f) { return narrow(f.text) == narrow(text); })) {
            continue;
        }
        if ((digits(chunk) && !digits(text)) || narrow(text) == ownNarrow) continue;
        // Nor one that writes its numbers otherwise only (3千円 for 3,000円):
        // the number style writes them. Of two that differ so, the one that
        // writes them as the phrase does.
        const std::wstring asRead = ForSentenceModel(text);
        if (asRead == ForSentenceModel(chunk)) continue;
        const auto numbers = [](std::wstring_view t) {
            std::wstring out;
            for (const wchar_t c : t) {
                if (IsDigit(c) || c == L',' || c == L'\xFF0C' ||
                    std::wstring_view(L"〇一二三四五六七八九十百千万億兆").find(c) != std::wstring_view::npos) {
                    out += c;
                }
            }
            return out;
        };
        if (const auto same = std::find_if(found.begin(), found.end(), [&](const Found& f) { return ForSentenceModel(f.text) == asRead; });
            same != found.end()) {
            if (numbers(text) == numbers(chunk) && numbers(same->text) != numbers(chunk)) same->text = text;
            continue;
        }
        const std::wstring theirs = kanji(text);
        const bool written = within(theirs, ownKanji) || within(ownKanji, theirs);
        // Written another way: not in katakana the phrase is not in, nor all
        // in kana when it has kanji (はひめまして for は秘めまして).
        if (written && (!within(katakana(text), ownKatakana) || (theirs.empty() && !ownKanji.empty()))) continue;
        found.push_back({std::move(text), checked.scores[k], written});
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.score > b.score; });
    std::vector<const Found*> picked;
    for (const bool written : {false, true}) {
        const auto it = std::find_if(found.begin(), found.end(), [&](const Found& f) { return f.written == written; });
        if (it != found.end()) picked.push_back(&*it);
    }
    for (const auto& f : found) {
        if (picked.size() >= kChunks) break;
        if (std::find(picked.begin(), picked.end(), &f) == picked.end()) picked.push_back(&f);
    }
    std::stable_sort(picked.begin(), picked.end(), [](const Found* a, const Found* b) { return a->score > b->score; });
    if (picked.empty()) return;
    const std::size_t length = phrases_[last].begin + phrases_[last].length - phrase.begin;
    // The phrase's choice first (the check may have chosen one further
    // down), so that the list starts with what is shown.
    if (phrase.selected > 0 && phrase.selected < phrase.candidates.size()) {
        std::rotate(phrase.candidates.begin(), phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected),
                    phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected) + 1);
        phrase.selected = 0;
    }
    std::size_t insertAt = std::min<std::size_t>(1, phrase.candidates.size());
    for (const Found* f : picked) {
        PhraseCandidate candidate{f->text, 0, PhraseCandidate::Kind::Dictionary};
        candidate.length = length;
        candidate.chunk = true;
        phrase.candidates.insert(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(candidate));
        if (phrase.selected >= insertAt) ++phrase.selected;
        ++insertAt;
    }
}

void JapaneseComposer::TakeLongerWord() {
    if (!IsConverted() || focus_ >= phrases_.size()) return;
    const auto& phrase = phrases_[focus_];
    if (phrase.selected >= phrase.candidates.size()) return;
    const auto& chosen = phrase.candidates[phrase.selected];
    if (chosen.length <= phrase.length) return;
    // The phrase takes the word's length, then the word.
    const std::wstring text = chosen.text;
    const bool open = listOpen_;
    ResizeFocus(static_cast<int>(chosen.length) - static_cast<int>(phrase.length));
    if (focus_ >= phrases_.size()) return;
    auto& now = phrases_[focus_];
    const auto found = std::find_if(now.candidates.begin(), now.candidates.end(),
                                    [&](const PhraseCandidate& c) { return c.text == text; });
    if (found != now.candidates.end()) {
        now.selected = static_cast<std::size_t>(found - now.candidates.begin());
    } else {
        now.candidates.insert(now.candidates.begin(), PhraseCandidate{text, 0, PhraseCandidate::Kind::Dictionary});
        now.selected = 0;
    }
    listOpen_ = open;
}

void JapaneseComposer::AddLongVowelWords(std::vector<PhraseCandidate>& candidates, std::wstring_view reading,
                                         bool overSlip) const {
    // Katakana words typed with a vowel for ー (nyuusu: ニュース, baajon:
    // バージョン): the reading with ー, when it is a katakana word, with a
    // particle after it kept as it is.
    if (!converter_ || candidates.empty() || reading.size() < 3) return;
    constexpr std::wstring_view kParticles = L"をがにでのはともへや";
    for (const std::size_t rest : {std::size_t{0}, std::size_t{1}}) {
        if (rest > 0 && (reading.size() < 4 || kParticles.find(reading.back()) == std::wstring_view::npos)) continue;
        const auto word = reading.substr(0, reading.size() - rest);
        const std::wstring drawn = LongVowelReading(word);
        if (drawn == word || !converter_->IsWord(drawn)) continue;
        const auto best = converter_->Best(drawn);
        const auto isKatakana = [](wchar_t c) { return (c >= L'\x30A1' && c <= L'\x30F6') || c == L'ー'; };
        if (!best || best->text.empty() || !std::all_of(best->text.begin(), best->text.end(), isKatakana)) continue;
        std::wstring text = best->text + std::wstring(reading.substr(reading.size() - rest));
        if (std::any_of(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) { return c.text == text; })) {
            return;
        }
        // Second when the keys as typed are no word (にゅうす), otherwise
        // where a slip undone would go.
        // `overSlip`: first, before a slip undone, which changed keys where
        // this changes none.
        const bool typedIsWord = converter_->IsWord(word);
        const std::size_t at = overSlip && !typedIsWord ? 0
                               : std::min(candidates.size(), typedIsWord ? kSlipPosition : std::size_t{1});
        PhraseCandidate candidate{std::move(text), best->cost, PhraseCandidate::Kind::Dictionary, false};
        candidate.rightId = best->rightId;
        candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(at), std::move(candidate));
        return;
    }
}

void JapaneseComposer::AddLoanwords(std::vector<PhraseCandidate>& candidates, std::wstring_view reading) const {
    if (!loanwords_) return;
    // The dictionary did not know the word: the first candidate is the
    // kana as typed ("ぐうぐるで", "ツイッタあ"). The reading, or all of it
    // but a short particle, may be a katakana word.
    if (!candidates.empty() && reading.size() >= kShortestUnknownLoanword &&
        ToHiragana(candidates.front().text) == ToHiragana(reading)) {
        for (std::size_t length = reading.size();
             length >= kShortestUnknownLoanword && reading.size() - length <= kLoanwordParticle; --length) {
            // A word the dictionary knows (ほんと, not haunt; ハート, its
            // katakana read below) is no unknown loanword.
            if (converter_ && converter_->IsWord(std::wstring(reading.substr(0, length)))) break;
            const auto words = loanwords_->Words(reading.substr(0, length), kLoanwords);
            if (words.empty()) continue;
            const std::wstring rest(reading.substr(length));
            std::vector<PhraseCandidate> found;
            for (const auto& word : words) {
                if (word.find(L' ') != std::wstring::npos) continue;  // a description, not the word
                PhraseCandidate english{word + rest, candidates.front().cost, PhraseCandidate::Kind::English, false};
                const auto same = [&](const PhraseCandidate& c) { return c.text == english.text; };
                if (std::none_of(candidates.begin(), candidates.end(), same)) found.push_back(std::move(english));
            }
            candidates.insert(candidates.begin() + 1, found.begin(), found.end());
            return;
        }
    }
    const auto isKatakana = [](wchar_t c) { return (c >= L'\x30A1' && c <= L'\x30F6') || c == L'\x30FC'; };
    for (std::size_t i = 0; i < std::min(candidates.size(), kLoanwordScan); ++i) {
        const auto& candidate = candidates[i];
        if (candidate.kind != PhraseCandidate::Kind::Dictionary) continue;
        std::size_t length = 0;
        while (length < candidate.text.size() && isKatakana(candidate.text[length])) ++length;
        // Only a loanword the reading is first read as (はーと: ハート,
        // heart); katakana further down is another word written so (さくら:
        // サクラ), whose English would be a translation (cherry blossom).
        if (length < kShortestLoanword) return;
        // The word in English, and what follows it as it was ("ミーティングが"
        // -> "meetingが").
        const std::wstring rest = candidate.text.substr(length);
        std::vector<PhraseCandidate> words;
        for (const auto& word : loanwords_->Words(std::wstring_view(candidate.text).substr(0, length), kLoanwords)) {
            // The word itself, not a description of it (Heart symbol, Google
            // Play for ハート, グーグル).
            if (word.find(L' ') != std::wstring::npos) continue;
            PhraseCandidate english{word + rest, candidate.cost, PhraseCandidate::Kind::English, false};
            const auto same = [&](const PhraseCandidate& c) { return c.text == english.text; };
            if (std::none_of(candidates.begin(), candidates.end(), same)) words.push_back(std::move(english));
        }
        candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(i + 1), words.begin(), words.end());
        return;
    }
}

void JapaneseComposer::AddSpecial(std::wstring_view reading, std::vector<PhraseCandidate>& candidates) const {
    if (!special_ || candidates.empty()) return;
    const auto has = [&](const std::wstring& text) {
        return std::any_of(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) { return c.text == text; });
    };
    auto group = PhraseCandidate::Group::Word;
    const auto insert = [&](std::size_t at, const std::vector<std::wstring>& texts, std::wstring_view suffix) {
        at = std::min(at, candidates.size());
        for (const auto& text : texts) {
            PhraseCandidate candidate{text + std::wstring(suffix), candidates.front().cost,
                                      PhraseCandidate::Kind::Special, false};
            candidate.group = group;
            if (has(candidate.text)) continue;
            candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(at++), std::move(candidate));
        }
    };
    // Other forms of a number come right after it, and after a number with
    // a counter as the first candidate writes it (３個: 3個, 三個).
    if (specialOptions_.numbers) {
        std::size_t digits = 0;
        while (digits < reading.size() && IsDigit(reading[digits])) ++digits;
        const auto& first = candidates.front().text;
        // Digits with points or commas between them (1.1.1, 3.14, 1,000):
        // all half-width, all full-width, and with 。 and 、 as typed in
        // Japanese, right after the first.
        const auto mark = [](wchar_t c) {
            return std::wstring_view(L".,\xFF0E\xFF0C\x3002\x3001").find(c) != std::wstring_view::npos;
        };
        const bool marked = digits > 0 && digits < reading.size() && IsDigit(reading.back()) &&
                            std::all_of(reading.begin(), reading.end(), [&](wchar_t c) { return IsDigit(c) || mark(c); });
        if (marked) {
            std::wstring half, full, kuten;
            bool number = true;
            for (std::size_t i = 0; i < reading.size() && number; ++i) {
                const wchar_t c = reading[i];
                if (IsDigit(c)) {
                    const wchar_t ascii = c >= L'\xFF10' ? static_cast<wchar_t>(c - 0xFEE0) : c;
                    half += ascii;
                    full += static_cast<wchar_t>(ascii + 0xFEE0);
                    kuten += ascii;
                    continue;
                }
                // Two marks in a row are no number: left as they are.
                if (i + 1 < reading.size() && !IsDigit(reading[i + 1])) {
                    number = false;
                    continue;
                }
                const bool comma = c == L',' || c == L'\xFF0C' || c == L'\x3001';
                half += comma ? L',' : L'.';
                full += comma ? L'\xFF0C' : L'\xFF0E';
                kuten += comma ? L'\x3001' : L'\x3002';
            }
            if (number) insert(1, {half, full, kuten}, {});
        } else if (digits == reading.size()) {
            insert(1, special_->Numbers(reading), {});
            // Eight digits that are a date (20261115): the date first.
            if (specialOptions_.dates) insert(1, special_->DigitDates(SpecialConversions::Language::Japanese, reading), {});
        } else if (digits > 0 && first.size() > digits && !IsDigit(first[digits]) &&
                   ToHalfWidthAscii(first.substr(0, digits)) == ToHalfWidthAscii(reading.substr(0, digits))) {
            const std::wstring rest = first.substr(digits);
            if (rest.front() != L'.' && rest.front() != L',' && rest.front() != L'\xFF0E' && rest.front() != L'\xFF0C') {
                insert(1, special_->Numbers(reading.substr(0, digits)), rest);
            }
        } else if (digits == 0) {
            // A number read in kana (さん, じゅうご, さんこ): its other forms
            // too (Ⅲ, ③, 参), right after it -- the converter's number, or
            // else a candidate in digits (さん alone is read as a word, with
            // 3 among its candidates).
            auto number = std::find_if(candidates.begin(), candidates.end(),
                                       [](const PhraseCandidate& c) { return c.kanaNumber; });
            if (number == candidates.end()) {
                number = std::find_if(candidates.begin(), candidates.end(), [](const PhraseCandidate& c) {
                    return !c.text.empty() && std::all_of(c.text.begin(), c.text.end(), IsDigit);
                });
            }
            if (number != candidates.end()) {
                // Without the commas between digit groups (1,000 in the
                // official style): the other forms are of all the digits
                // (千, １０００), not of the first group (一,000).
                std::wstring text = KanaNumberText(number->text, NumberStyle::HalfWidth);
                for (std::size_t i = 1; i + 1 < text.size();) {
                    if (text[i] == L',' && IsDigit(text[i - 1]) && IsDigit(text[i + 1])) {
                        text.erase(i, 1);
                    } else {
                        ++i;
                    }
                }
                std::size_t length = 0;
                while (length < text.size() && IsDigit(text[length])) ++length;
                if (length > 0) {
                    insert(static_cast<std::size_t>(number - candidates.begin()) + 1,
                           special_->Numbers(text.substr(0, length)), std::wstring_view(text).substr(length));
                }
            }
        }
    }
    // A unit read in kana (みり after 3, きろぐらむ): its symbol too, right
    // after its katakana (ミリ, mm). Not きろ alone, which is km or kg, nor
    // ぎが and めが.
    if (specialOptions_.numbers) {
        static constexpr std::pair<std::wstring_view, std::wstring_view> kUnits[] = {
            {L"みり", L"mm"}, {L"みりめーとる", L"mm"}, {L"せんち", L"cm"}, {L"せんちめーとる", L"cm"},
            {L"めーとる", L"m"}, {L"きろめーとる", L"km"}, {L"みりぐらむ", L"mg"}, {L"ぐらむ", L"g"},
            {L"きろぐらむ", L"kg"}, {L"みりりっとる", L"mL"}, {L"りっとる", L"L"}, {L"てらばいと", L"TB"},
            {L"ぎがばいと", L"GB"}, {L"めがばいと", L"MB"}, {L"きろばいと", L"KB"}, {L"へるつ", L"Hz"},
            {L"きろわっと", L"kW"}, {L"わっと", L"W"}, {L"ぼると", L"V"},
        };
        const auto unit = std::find_if(std::begin(kUnits), std::end(kUnits), [&](const auto& u) { return u.first == reading; });
        if (unit != std::end(kUnits)) {
            const std::wstring katakana = ToKatakana(std::wstring(reading));
            const auto at = std::find_if(candidates.begin(), candidates.end(),
                                         [&](const PhraseCandidate& c) { return c.text == katakana; });
            insert(at == candidates.end() ? 1 : static_cast<std::size_t>(at - candidates.begin()) + 1,
                   {std::wstring(unit->second)}, {});
        }
    }
    // A date or time word, and what follows it in the phrase as long as the
    // first candidate ends with it too ("きょうは": 今日は, 2026/09/29は).
    // What follows must be a particle or です and the like (いまから,
    // あしたです), not the rest of another word (います: 居ます, いますが,
    // いまさら: 今更).
    if (specialOptions_.dates) {
        const auto now = LocalTime::Now();
        static constexpr std::wstring_view kAfterDate[] = {
            L"は", L"が", L"を", L"に", L"の", L"で", L"と", L"も", L"へ", L"や", L"か", L"ね", L"よ", L"な",
            L"から", L"まで", L"より", L"だ", L"じゃ", L"って", L"こそ", L"しか", L"さえ", L"ごろ", L"くらい", L"ぐらい"};
        for (std::size_t length = reading.size(); length >= 2; --length) {
            const auto suffix = reading.substr(length);
            if (!candidates.front().text.ends_with(suffix)) continue;
            if (!suffix.empty() && std::none_of(std::begin(kAfterDate), std::end(kAfterDate),
                                                [&](std::wstring_view after) { return suffix.starts_with(after); })) {
                continue;
            }
            auto dates = special_->Dates(SpecialConversions::Language::Japanese, reading.substr(0, length), now);
            if (dates.empty()) continue;
            group = PhraseCandidate::Group::Date;
            insert(1, dates, suffix);
            group = PhraseCandidate::Group::Word;
            break;
        }
    }
    // Single kanji the dictionary did not offer, after the words.
    insert(candidates.size(), special_->SingleKanji(reading), {});
    // Symbols and emoticons after them ("やじるし": 矢印, やじるし, ヤジルシ, →).
    if (specialOptions_.symbols) {
        const std::size_t symbolsFrom = candidates.size();
        group = PhraseCandidate::Group::Symbol;
        insert(candidates.size(), special_->Symbols(SpecialConversions::Language::Japanese, reading), {});
        insert(candidates.size(), special_->Emoticons(reading), {});
        group = PhraseCandidate::Group::Word;
        // The reading's first symbol on the first page, after the words read
        // the same: its symbol when it names one (まる: ○, やじるし: →,
        // おんぷ: ♪), otherwise its most used emoji (わらい: 😂). The pack
        // lists a reading's symbols before its emoji. The rest stay last.
        constexpr std::size_t kSymbolAt = 3;
        // Not for a number (3: ⚂) nor when a symbol already comes first
        // (ぱーせんと: %).
        const bool number = std::all_of(reading.begin(), reading.end(), IsDigit);
        const bool symbolFirst = std::none_of(candidates.front().text.begin(), candidates.front().text.end(),
                                              [](wchar_t c) { return c >= L'\x3041'; });
        if (emojiOnFirstPage_ && symbolsFrom < candidates.size() && !number && !symbolFirst) {
            const auto symbol = candidates.begin() + static_cast<std::ptrdiff_t>(symbolsFrom);
            const auto at = candidates.begin() + static_cast<std::ptrdiff_t>(std::min(kSymbolAt, symbolsFrom));
            std::rotate(at, symbol, symbol + 1);
            // Its next most used ones start the second page, when the words
            // fill the first (ほし: ★ ※ ✩ after ☆).
            constexpr std::size_t kSecondPage = 3;
            const std::size_t from = symbolsFrom + 1;
            const std::size_t count = std::min(kSecondPage, candidates.size() - from);
            if (count > 0 && pageSize_ > kSymbolAt + 1 && from > pageSize_) {
                const auto first = candidates.begin() + static_cast<std::ptrdiff_t>(from);
                std::rotate(candidates.begin() + static_cast<std::ptrdiff_t>(pageSize_), first,
                            first + static_cast<std::ptrdiff_t>(count));
            }
        }
    }
}

std::vector<PhraseCandidate> JapaneseComposer::SumCandidates(std::wstring_view reading) const {
    if (!special_ || !specialOptions_.calculator) return {};
    const auto sum = SpecialConversions::Calculate(reading);
    if (!sum) return {};
    // The arithmetic as ASCII, the way it was meant (ー is minus, ・ divide).
    std::wstring expression = ToHalfWidthAscii(reading);
    for (auto& c : expression) {
        if (c == L'\x30FC') c = L'-';
        if (c == L'\x30FB') c = L'/';
    }
    std::vector<PhraseCandidate> candidates{{*sum, 0, PhraseCandidate::Kind::Special, false},
                                            {expression + *sum, 0, PhraseCandidate::Kind::Special, false}};
    for (auto& kana : KanaCandidates(reading)) {
        const bool seen = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const PhraseCandidate& c) { return c.text == kana.text; });
        if (!seen) candidates.push_back(std::move(kana));
    }
    return candidates;
}

std::vector<PhraseCandidate> JapaneseComposer::PostalCandidates(std::wstring_view reading) const {
    if (!postalCodes_ || !specialOptions_.numbers) return {};
    const auto digits = PostalCodeDigits(reading);
    if (!digits) return {};
    constexpr std::size_t kAddresses = 9;
    const auto addresses = postalCodes_->Addresses(*digits, kAddresses);
    if (addresses.empty()) return {};
    // The code as typed stays first (a hyphen as a hyphen), then the
    // addresses and the code with its mark.
    std::wstring typed(reading);
    for (auto& c : typed) {
        if (c == L'\x30FC') c = IsDigit(typed.front()) && typed.front() < 0x80 ? L'-' : L'\xFF0D';
    }
    const std::wstring code = digits->substr(0, 3) + L"-" + digits->substr(3);
    std::vector<PhraseCandidate> candidates{{typed, 0, PhraseCandidate::Kind::Special, false}};
    for (const auto& address : addresses) candidates.push_back({address, 0, PhraseCandidate::Kind::Special, false});
    candidates.push_back({L"\x3012" + code, 0, PhraseCandidate::Kind::Special, false});
    if (typed != code) candidates.push_back({code, 0, PhraseCandidate::Kind::Special, false});
    return candidates;
}

void JapaneseComposer::PreferWordsWritten(std::vector<PhraseCandidate>& candidates) const {
    // A word written just before is likelier again than its homophones
    // (描くことが多い… then かく: 描く, not 書く). A candidate up to the
    // character after its last kanji (描く, 事典が) is looked for in the text
    // before; one found where the first's is not comes first.
    constexpr std::size_t kLooked = 10;
    if (precedingText_.empty() || candidates.size() < 2) return;
    const auto isKanji = [](wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005; };
    const auto stem = [&](const std::wstring& text) -> std::wstring {
        std::size_t last = std::wstring::npos;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (isKanji(text[i])) last = i;
        }
        if (last == std::wstring::npos) return {};
        // From its start (コジン的な is not 的な), to the character after its
        // last kanji.
        const std::size_t end = std::min(text.size(), last + 2);
        return end >= 2 ? text.substr(0, end) : std::wstring{};
    };
    const auto written = [&](const std::wstring& text) {
        const auto part = stem(text);
        return !part.empty() && precedingText_.find(part) != std::wstring::npos;
    };
    // Between homophones in kanji only (書く, 描く): kana or kanji (ような,
    // 様な) is a choice of writing the text before does not make.
    if (candidates.front().kind != PhraseCandidate::Kind::Dictionary || stem(candidates.front().text).empty() ||
        written(candidates.front().text)) {
        return;
    }
    for (std::size_t c = 1; c < std::min(kLooked, candidates.size()); ++c) {
        if (candidates[c].kind != PhraseCandidate::Kind::Dictionary || !candidates[c].reading.empty()) continue;
        if (!written(candidates[c].text)) continue;
        auto chosen = std::move(candidates[c]);
        candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(c));
        candidates.insert(candidates.begin(), std::move(chosen));
        return;
    }
}

}  // namespace tekito::japanese
