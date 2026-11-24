#pragma once

// Helpers the composer's modules share (JapaneseComposer.cpp and the
// Composer*.cpp files); private to them.

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/KanaSlips.h"
#include "Core/Japanese/KanaText.h"
#include "Core/TypoModel.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/ChunkJudge.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/PostalCodes.h"
#include "Core/Japanese/RomajiTable.h"
#include "Core/Japanese/SentenceReranker.h"
#include "Core/Japanese/AiJudge.h"
#include "Core/EnglishSpelling.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <regex>
#include <map>
#include <cstdlib>
#include <functional>
#include <queue>
#include <set>
#include <cwctype>
#include <limits>

namespace tekito::japanese {
namespace {

// Whether two writings of one reading differ only in how they are written
// (じゃがいも, ジャガイモ; おかわり, お代わり; 一日, 1日; 、 and ，), not in
// which words they are (紙, 髪): wherever they differ, one side is kana,
// both are numbers, or both are marks.
inline bool OnlyNotationDiffers(std::wstring_view a, std::wstring_view b) {
    if (a == b) return true;
    constexpr std::size_t kLongest = 200;
    if (a.size() > kLongest || b.size() > kLongest) return false;
    const auto kana = [](wchar_t c) { return (c >= 0x3041 && c <= 0x30FF) || c == 0xFF70 || (c >= 0xFF66 && c <= 0xFF9F); };
    const auto number = [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= 0xFF10 && c <= 0xFF19) ||
               std::wstring_view(L"〇一二三四五六七八九十百千万").find(c) != std::wstring_view::npos;
    };
    const auto mark = [](wchar_t c) { return std::iswpunct(c) != 0 || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF0F); };
    const auto all = [](std::wstring_view text, auto test) {
        return std::all_of(text.begin(), text.end(), test);
    };
    // The longest common subsequence, then the stretches between its
    // characters.
    std::vector<std::vector<std::uint8_t>> same(a.size() + 1, std::vector<std::uint8_t>(b.size() + 1, 0));
    for (std::size_t i = a.size(); i-- > 0;) {
        for (std::size_t j = b.size(); j-- > 0;) {
            same[i][j] = a[i] == b[j] ? static_cast<std::uint8_t>(same[i + 1][j + 1] + 1)
                                      : std::max(same[i + 1][j], same[i][j + 1]);
        }
    }
    std::size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (i < a.size() && j < b.size() && a[i] == b[j]) {
            ++i;
            ++j;
            continue;
        }
        const std::size_t fromA = i, fromB = j;
        while ((i < a.size() || j < b.size()) && !(i < a.size() && j < b.size() && a[i] == b[j])) {
            if (j == b.size() || (i < a.size() && same[i + 1][j] >= same[i][j + 1])) ++i; else ++j;
        }
        const auto partA = a.substr(fromA, i - fromA);
        const auto partB = b.substr(fromB, j - fromB);
        const bool notation = all(partA, kana) || all(partB, kana) ||
                              (all(partA, number) && all(partB, number)) ||
                              (all(partA, mark) && all(partB, mark));
        if (!notation) return false;
    }
    return true;
}

// A number in kanji numerals or digits as digits (二十二, 22: 22), or
// nothing when it is not one.
inline std::optional<std::wstring> NumeralDigits(std::wstring_view text) {
    if (text.empty()) return std::nullopt;
    if (std::all_of(text.begin(), text.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= 0xFF10 && c <= 0xFF19); })) {
        std::wstring digits;
        for (const wchar_t c : text) digits += c >= 0xFF10 ? static_cast<wchar_t>(c - 0xFEE0) : c;
        return digits;
    }
    constexpr std::wstring_view kDigits = L"〇一二三四五六七八九";
    std::uint64_t total = 0, section = 0, digit = 0;
    bool any = false;
    for (const wchar_t c : text) {
        if (const auto d = kDigits.find(c); d != std::wstring_view::npos) {
            digit = d;
            any = true;
            continue;
        }
        const std::uint64_t unit = c == L'十' ? 10 : c == L'百' ? 100 : c == L'千' ? 1000 : 0;
        const std::uint64_t big = c == L'万' ? 10000ull : c == L'億' ? 100000000ull : c == L'兆' ? 1000000000000ull : 0;
        if (unit) {
            section += (digit ? digit : 1) * unit;
            digit = 0;
        } else if (big) {
            total += (section + digit) * big;
            section = digit = 0;
        } else {
            return std::nullopt;
        }
        any = true;
    }
    if (!any) return std::nullopt;
    return std::to_wstring(total + section + digit);
}

// A number at the start of `text` and what follows it (22人: 22, 人;
// 二十二人: 22, 人), or nothing when it does not start with one.
inline std::optional<std::pair<std::wstring, std::wstring>> LeadingNumber(std::wstring_view text) {
    for (std::size_t length = text.size(); length > 0; --length) {
        if (const auto digits = NumeralDigits(text.substr(0, length))) {
            return std::pair{*digits, std::wstring(text.substr(length))};
        }
    }
    return std::nullopt;
}

inline std::wstring ChangeCase(std::wstring text, int letterCase) {
    switch (letterCase) {
    case 1:
        for (auto& ch : text) ch = static_cast<wchar_t>(std::towupper(ch));
        break;
    case 2: {
        bool first = true;
        for (auto& ch : text) {
            ch = static_cast<wchar_t>(first ? std::towupper(ch) : std::towlower(ch));
            first = false;
        }
        break;
    }
    case 3:
        for (auto& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
        break;
    default:
        break;
    }
    return text;
}

// Letters the romaji table left as they were (shown full-width).
inline std::size_t LeftoverLetters(std::wstring_view reading) {
    return static_cast<std::size_t>(std::count_if(reading.begin(), reading.end(), [](wchar_t c) {
        return (c >= L'\xFF21' && c <= L'\xFF3A') || (c >= L'\xFF41' && c <= L'\xFF5A');
    }));
}

// Slip candidates for the list (AddSlipCandidates). Any letter may have been
// left out here: the first choice only undoes the likelier slips.
constexpr std::wstring_view kSlipDroppedKeys = L"aiueonkgsztdhbpmyrwjfc";
constexpr std::size_t kSlipPhraseKeys = 16;
// A whole input up to this many keys gets this many whole-input readings.
constexpr std::size_t kWholeSlipKeys = 20;
constexpr std::size_t kWholeSlipCandidates = 2;
constexpr std::size_t kWholeSlipExamined = 6;
constexpr std::size_t kSlipCandidates = 4;
// Keys a phrase needs before slips in it are offered: shorter ones (ko,
// kou) would bring whole other words.
constexpr std::size_t kShortestSlipKeys = 4;
// The likeliest readings get this many spellings each; the rest one.
constexpr std::size_t kSlipSpelledReadings = 2;
constexpr std::size_t kSlipSpellings = 2;
constexpr std::size_t kSlipPosition = 5;
// Costs in the connection matrix's units, against the phrase as typed.
constexpr std::int64_t kSlipMargin = 3000;
constexpr std::int64_t kSlipLikelier = 2000;
// Loanwords (AddLoanwords): the first candidates that may start in
// katakana, how long the katakana must be, and the English words offered.
constexpr std::size_t kLoanwordScan = 5;
// Content words of the text committed lately kept for the language model.
constexpr std::size_t kContextWords = 8;
constexpr std::size_t kShortestLoanword = 2;
constexpr std::size_t kLoanwords = 3;
// A reading the dictionary did not know is looked up from this many kana,
// with at most this many after the word (a particle).
constexpr std::size_t kShortestUnknownLoanword = 3;
constexpr std::size_t kLoanwordParticle = 2;

inline bool IsAlphanumeric(KanaForm form) {
    return form == KanaForm::FullWidthAlphanumeric || form == KanaForm::HalfWidthAlphanumeric;
}

// Keys that write a URL, an email address, a path or a mention, which
// stay as typed: https://…, www.…, name@example.com, @name, ./src, ~/x,
// /home/x, C:\x.
inline bool IsProtectedSpan(std::wstring_view keys) {
    static const std::wregex pattern(
        LR"(^(?:(?:https?|ftp)://\S+|www\.\S+\.\S+|[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}|@[A-Za-z0-9_][A-Za-z0-9_.\-]*|(?:\./|\.\./|~/)\S*|/[A-Za-z0-9_.\-]+(?:/\S*)?|[A-Za-z]:\\\S*)$)");
    return !keys.empty() && std::regex_match(keys.begin(), keys.end(), pattern);
}

// The same, or one still being typed: name@ and its domain so far
// (kanta@capitata), a URL after its :// ; a file name (report.pdf); a
// version (v1.0.0, ver1.2, version 2 written together, 1.3.1-pre.3) and an
// ordinal (1st, 2nd, 3rd, 21st). Shown and committed as typed.
inline bool IsAddressTyping(std::wstring_view keys) {
    static const std::wregex pattern(LR"(^(?:[A-Za-z0-9._%+\-]+@(?:[A-Za-z][A-Za-z0-9.\-]*)?|(?:https?|ftp)://\S*|[vV](?:er(?:sion)?)?[0-9]+(?:\.[0-9]+)*\.?(?:-[A-Za-z0-9.]*)?|[0-9]+(?:\.[0-9]+){2,}-[A-Za-z0-9.]*|[0-9]*(?:1st|2nd|3rd|[0-9]th))$)");
    static const std::wregex file(
        LR"(^[A-Za-z0-9_\-]+(?:\.[A-Za-z0-9_\-]+)*\.(?:pdf|docx?|xlsx?|pptx?|txt|csv|md|json|xml|ya?ml|html?|css|js|ts|tsx|jsx|py|cpp|hpp|h|cs|java|go|rs|sh|ps1|bat|exe|dll|msi|zip|7z|gz|png|jpe?g|gif|svg|webp|mp3|mp4|mov|wav|log|ini|toml)$)",
        std::regex::icase);
    return IsProtectedSpan(keys) ||
           (!keys.empty() && (std::regex_match(keys.begin(), keys.end(), pattern) || std::regex_match(keys.begin(), keys.end(), file)));
}

inline bool IsDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'\xFF10' && c <= L'\xFF19');
}

inline bool IsDigits(std::wstring_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), IsDigit);
}

// A decimal point or comma between two digits as wide as they are: 1.1 and
// １．１, not 1．1 (half-width digits) or １.１.
inline std::wstring MarksAsDigits(std::wstring text) {
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        const wchar_t left = text[i - 1];
        const wchar_t right = text[i + 1];
        const bool halfDigits = left >= L'0' && left <= L'9' && right >= L'0' && right <= L'9';
        const bool fullDigits = left >= L'\xFF10' && left <= L'\xFF19' && right >= L'\xFF10' && right <= L'\xFF19';
        if (!halfDigits && !fullDigits) continue;
        auto& mark = text[i];
        if (mark == L'.' || mark == L'\xFF0E') mark = halfDigits ? L'.' : L'\xFF0E';
        if (mark == L',' || mark == L'\xFF0C') mark = halfDigits ? L',' : L'\xFF0C';
    }
    return text;
}

// Digits joined by marks are a code (090-1234-5678, 12:30, 2026/11/13,
// 192.168.0.1): its marks as wide as its digits, a dash a hyphen (not ー),
// the ・ of a date a slash; and, joined by a dash, colon or slash, all its
// digits as wide as the first.
inline std::wstring CodeMarks(std::wstring text) {
    const auto digit = [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'\xFF10' && c <= L'\xFF19'); };
    constexpr std::wstring_view kDashes = L"-\x30FC\xFF0D\x2015\x2010";
    constexpr std::wstring_view kColons = L":\xFF1A";
    constexpr std::wstring_view kSlashes = L"/\xFF0F\x30FB";
    constexpr std::wstring_view kPoints = L".\xFF0E,\xFF0C";
    const auto mark = [&](wchar_t c) {
        return kDashes.find(c) != std::wstring_view::npos || kColons.find(c) != std::wstring_view::npos ||
               kSlashes.find(c) != std::wstring_view::npos || kPoints.find(c) != std::wstring_view::npos;
    };
    for (std::size_t start = 0; start < text.size();) {
        if (!digit(text[start])) {
            ++start;
            continue;
        }
        std::size_t end = start;
        while (end < text.size() && (digit(text[end]) || (mark(text[end]) && end + 1 < text.size() && digit(text[end + 1])))) ++end;
        // The groups of digits and the marks between them.
        std::vector<std::size_t> groups;
        bool slashes = false, joined = false, others = false;
        std::size_t length = 0;
        for (std::size_t i = start; i <= end; ++i) {
            if (i < end && digit(text[i])) {
                ++length;
                continue;
            }
            groups.push_back(length);
            length = 0;
            if (i == end) break;
            if (kSlashes.find(text[i]) != std::wstring_view::npos) slashes = true;
            else others = true;
            if (kPoints.find(text[i]) == std::wstring_view::npos) joined = true;
        }
        const auto value = [&](std::size_t group) {
            std::size_t at = start;
            for (std::size_t g = 0; g < group; ++g) at += groups[g] + 1;
            int n = 0;
            for (std::size_t k = 0; k < groups[group]; ++k) {
                const wchar_t c = text[at + k];
                n = n * 10 + (c >= L'\xFF10' ? c - L'\xFF10' : c - L'0');
            }
            return n;
        };
        // A date: month and day (11/13), with a year before them (2026/11/13).
        bool date = slashes && !others && (groups.size() == 2 || groups.size() == 3);
        if (date) {
            const std::size_t m = groups.size() - 2;
            date = groups[m] <= 2 && groups[m + 1] <= 2 && value(m) >= 1 && value(m) <= 12 && value(m + 1) >= 1 &&
                   value(m + 1) <= 31 &&
                   (groups.size() == 3 ? groups[0] == 2 || groups[0] == 4 : groups[m] == 2 || groups[m + 1] == 2);
        }
        const bool half = text[start] <= L'9';
        // A plus before it (+81-80-...) as wide as its digits.
        if (joined && start > 0 && (text[start - 1] == L'+' || text[start - 1] == L'\xFF0B') &&
            (start == 1 || !digit(text[start - 2]))) {
            text[start - 1] = half ? L'+' : L'\xFF0B';
        }
        for (std::size_t i = start; i < end; ++i) {
            wchar_t& c = text[i];
            if (digit(c)) {
                if (joined && half && c >= L'\xFF10') c = static_cast<wchar_t>(c - 0xFEE0);
                if (joined && !half && c <= L'9') c = static_cast<wchar_t>(c + 0xFEE0);
            } else if (kDashes.find(c) != std::wstring_view::npos) {
                c = half ? L'-' : L'\xFF0D';
            } else if (kColons.find(c) != std::wstring_view::npos) {
                c = half ? L':' : L'\xFF1A';
            } else if (kSlashes.find(c) != std::wstring_view::npos) {
                if (date) c = half ? L'/' : L'\xFF0F';
            } else if (c == L'.' || c == L'\xFF0E') {
                c = half ? L'.' : L'\xFF0E';
            } else if (c == L',' || c == L'\xFF0C') {
                c = half ? L',' : L'\xFF0C';
            }
        }
        start = end;
    }
    return text;
}

// The reading a phrase is learned under. Right after digits it is learned
// apart ("#こ"): 3こ stays 3個 even when こ alone is usually 子. The mark
// keeps it out of predictions, which look up typed kana.
inline std::wstring LearningReading(std::wstring_view reading, std::size_t begin, std::size_t length) {
    std::wstring key(reading.substr(begin, length));
    if (begin > 0 && IsDigit(reading[begin - 1]) && !key.empty() && !IsDigit(key.front())) key.insert(0, 1, L'#');
    return key;
}

// A number in digits for a word read in kana (いち: 1, the dictionary's
// likeliest) comes after the first two words; not a number read whole
// (さんびゃく: 300).
constexpr std::size_t kDigitsPosition = 2;
inline void PutDigitsAfterWords(std::vector<PhraseCandidate>& candidates) {
    std::vector<PhraseCandidate> digits;
    for (std::size_t i = 0; i < std::min(candidates.size(), kDigitsPosition); ++i) {
        if (candidates[i].kind == PhraseCandidate::Kind::Dictionary && IsDigits(candidates[i].text) &&
            !candidates[i].kanaNumber) {
            digits.push_back(std::move(candidates[i]));
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(i--));
        }
    }
    const auto at = candidates.begin() + static_cast<std::ptrdiff_t>(std::min(candidates.size(), kDigitsPosition));
    candidates.insert(at, std::make_move_iterator(digits.begin()), std::make_move_iterator(digits.end()));
}

// A choice for each part of a sentence (sizes: how many each part has), in
// order of the sum of the choices' ranks, the first choices first; `visit`
// takes each and returns false to stop. The sentence check of a conversion
// (parts: phrases) and of a mixed sentence (parts: chunks) share it.
// Each pick of one choice per part, likeliest first, until `visit` says
// stop: by the sum of the ranks picked, or with `deltas` (how much more
// each choice costs than its part's first, rising along each part) by the
// sum of those, so a second choice nearly as likely as the first comes
// before one far behind it.
template <class Visit>
inline void ForPicksByRank(const std::vector<std::size_t>& sizes, Visit visit,
                    const std::vector<std::vector<std::int64_t>>* deltas = nullptr) {
    using Picks = std::vector<std::size_t>;
    const auto weight = [&](const Picks& picks) {
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < picks.size(); ++i) {
            sum += deltas ? (*deltas)[i][picks[i]] : static_cast<std::int64_t>(picks[i]);
        }
        return sum;
    };
    using Entry = std::pair<std::int64_t, Picks>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    std::set<Picks> seen{Picks(sizes.size(), 0)};
    queue.push({0, Picks(sizes.size(), 0)});
    while (!queue.empty()) {
        const Picks picks = queue.top().second;
        queue.pop();
        if (!visit(picks)) return;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (picks[i] + 1 >= sizes[i]) continue;
            Picks next = picks;
            ++next[i];
            if (seen.insert(next).second) queue.push({weight(next), std::move(next)});
        }
    }
}

// The text of a conversion's phrases with their next candidates in, by the
// sum of their ranks: `count` texts at most, the first as converted.
// Candidates for part of the input only (not another reading of it).


inline std::vector<std::wstring> TopConversions(const std::vector<Phrase>& phrases, std::size_t count) {
    constexpr std::size_t kListed = 8;
    std::vector<std::wstring> texts;
    std::vector<std::vector<const std::wstring*>> lists;
    for (const auto& phrase : phrases) {
        std::vector<const std::wstring*> list;
        for (const auto& candidate : phrase.candidates) {
            if (list.size() >= kListed) break;
            if (!candidate.reading.empty()) continue;
            if (std::none_of(list.begin(), list.end(), [&](const std::wstring* t) { return *t == candidate.text; })) {
                list.push_back(&candidate.text);
            }
        }
        if (list.empty() || *list.front() != phrase.candidates.front().text) {
            std::wstring text;
            for (const auto& other : phrases) {
                if (!other.candidates.empty()) text += other.candidates.front().text;
            }
            return {text};
        }
        lists.push_back(std::move(list));
    }
    std::vector<std::size_t> sizes;
    for (const auto& list : lists) sizes.push_back(list.size());
    ForPicksByRank(sizes, [&](const std::vector<std::size_t>& picks) {
        std::wstring text;
        for (std::size_t i = 0; i < lists.size(); ++i) text += *lists[i][picks[i]];
        if (std::find(texts.begin(), texts.end(), text) == texts.end()) texts.push_back(std::move(text));
        return texts.size() < count;
    });
    return texts;
}

// Written with a capital letter: a name (Zoom, GitHub, API).
inline bool HasCapital(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'A' && c <= L'Z'; });
}



// A number in kanji numerals as it is read (2005: 二千五, 10: 十, 120000:
// 十二万).
inline std::wstring KanjiNumber(std::uint64_t value) {
    if (value == 0) return L"〇";
    constexpr std::wstring_view kDigits = L"〇一二三四五六七八九";
    constexpr std::pair<std::uint64_t, wchar_t> kBig[] = {{1000000000000ULL, L'兆'}, {100000000ULL, L'億'}, {10000ULL, L'万'}};
    const auto small = [&](std::uint64_t n) {
        std::wstring text;
        constexpr std::pair<std::uint64_t, wchar_t> kUnits[] = {{1000, L'千'}, {100, L'百'}, {10, L'十'}};
        for (const auto& [unit, mark] : kUnits) {
            const auto digit = n / unit;
            n %= unit;
            if (digit == 0) continue;
            if (digit > 1) text += kDigits[digit];
            text += mark;
        }
        if (n > 0) text += kDigits[n];
        return text;
    };
    std::wstring text;
    for (const auto& [unit, mark] : kBig) {
        const auto part = value / unit;
        value %= unit;
        if (part > 0) text += small(part) + mark;
    }
    return text + small(value);
}



// The vowel a kana ends in (か: あ, しゅ's ゅ: う), or 0.
inline wchar_t VowelOf(wchar_t kana) {
    constexpr std::wstring_view kRows[] = {
        L"あぁかがさざただなはばぱまやゃらわ", L"いぃきぎしじちぢにひびぴみり", L"うぅくぐすずつづぬふぶぷむゆゅる",
        L"えぇけげせぜてでねへべぺめれ", L"おぉこごそぞとどのほぼぽもよょろを"};
    constexpr wchar_t kVowels[] = {L'あ', L'い', L'う', L'え', L'お'};
    for (std::size_t r = 0; r < std::size(kRows); ++r) {
        if (kRows[r].find(kana) != std::wstring_view::npos) return kVowels[r];
    }
    return 0;
}

// `reading` with each vowel that only draws out the one before written ー,
// as katakana words are read in the dictionary (にゅうす: にゅーす, ばあじょん:
// ばーじょん, こんぴゅうたあ: こんぴゅーたー); おう and えい too (しょう:
// しょー). The same reading when there is none.
inline std::wstring LongVowelReading(std::wstring_view reading) {
    std::wstring out(reading);
    for (std::size_t i = 1; i < out.size(); ++i) {
        const wchar_t vowel = VowelOf(out[i - 1]);
        const wchar_t c = out[i];
        if (vowel != 0 && (c == vowel || (vowel == L'お' && c == L'う') || (vowel == L'え' && c == L'い'))) {
            out[i] = L'ー';
        }
    }
    return out;
}



// Whether one text has an English word where the other has Japanese (Take
// and たけ): another word, not another way to write it.
inline bool LanguageDiffers(std::wstring_view a, std::wstring_view b) {
    const auto letters = [](std::wstring_view text) {
        std::wstring kept;
        for (const wchar_t c : text) {
            if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) kept.push_back(static_cast<wchar_t>(std::towlower(c)));
        }
        return kept;
    };
    return letters(a) != letters(b);
}


// As many characters as a few sentences: the AI model reads them all before
// each sentence it is asked about.
constexpr std::size_t kRecentCharacters = 120;


inline bool IsJapaneseLetter(wchar_t c) {
    return (c >= L'\x3041' && c <= L'\x30FA') || c == L'\x30FC' || (c >= L'\x3400' && c <= L'\x4DBF') ||
           (c >= L'\x4E00' && c <= L'\x9FFF') || c == L'\x3005';
}

inline bool IsAsciiWordChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
}


}  // namespace
}  // namespace tekito::japanese
