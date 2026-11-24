#include "Core/Japanese/Meanings.h"

#include "Core/DataPackPath.h"
#include "Core/Japanese/KanaText.h"

#include <algorithm>
#include <array>

namespace tekito::japanese {
namespace {

constexpr std::size_t kMaxSenses = 4;
constexpr std::size_t kMaxEnglishWord = 48;
// Endings that make a dictionary form from an inflected stem: 会っ(た) ->
// 会う, 食べ(ました) -> 食べる, 高(かった) -> 高い.
constexpr std::array<wchar_t, 11> kEndings{L'う', L'く', L'ぐ', L'す', L'つ', L'ぬ', L'ぶ', L'む', L'る', L'い', L'だ'};

bool IsHiragana(wchar_t c) noexcept { return c >= 0x3041 && c <= 0x309F; }

bool IsLatinWord(std::wstring_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'\'' || c == L'-' || c == L' ';
    });
}

void TrimEnd(std::wstring& text) {
    while (!text.empty() && (text.back() == L' ' || text.back() == L'　' || text.back() == L'。')) text.pop_back();
}

void TrimFront(std::wstring& text) {
    std::size_t at = 0;
    while (at < text.size() && (text[at] == L' ' || text[at] == L'　' || text[at] == L'、')) ++at;
    text.erase(0, at);
}

// The ending of a pointer to another entry ("…を参照", "…参照のこと"), or
// npos: "参照すること" explains a word and is no pointer.
std::size_t ReferenceEnding(std::wstring_view text) {
    for (const std::wstring_view ending :
         {L"を参照のこと", L"を参照されたし", L"を参照", L"も参照", L"参照のこと", L"参照されたし", L"参照"}) {
        if (text.ends_with(ending)) return text.size() - ending.size();
    }
    return std::wstring_view::npos;
}

// A sense from the Japanese Wiktionary without what only points to another
// entry: a pointer in brackets at its end ("（詳細はX参照）") and a last
// sentence that only points ("…の一種。アオイガイを参照。") go. A sense
// that is nothing but a pointer ("「十代」を参照。", "おさがり 参照")
// comes back empty, with the entries it points to in `targets` ("なぜ又は
// なにゆえ", "「盛土（もりど）」および「もりつち」": each), and what it said
// before "詳細は" (鳥の名、詳細は「オオタカ」参照) in `before`.
std::wstring WithoutReference(std::wstring sense, std::vector<std::wstring>& targets, std::wstring& before) {
    targets.clear();
    before.clear();
    if (sense.find(L"参照") == std::wstring::npos) return sense;
    // Brackets that end in a pointer.
    for (std::size_t from = 0;;) {
        const std::size_t close = sense.find_first_of(L"）)", from);
        if (close == std::wstring::npos) break;
        // Its own opening bracket, past any pair inside it ("（「色 (仏教)」も参照）").
        std::size_t open = std::wstring::npos;
        for (std::size_t at = close, depth = 0; at-- > 0;) {
            if (sense[at] == L'）' || sense[at] == L')') {
                ++depth;
            } else if (sense[at] == L'（' || sense[at] == L'(') {
                if (depth == 0) {
                    open = at;
                    break;
                }
                --depth;
            }
        }
        if (open == std::wstring::npos) {
            from = close + 1;
            continue;
        }
        const std::wstring_view inside = std::wstring_view(sense).substr(open + 1, close - open - 1);
        if (ReferenceEnding(inside) == std::wstring_view::npos) {
            from = close + 1;
        } else {
            sense.erase(open, close - open + 1);
            // "…語。（詳細はX参照）。": one full stop, not two.
            if (open > 0 && open < sense.size() && sense[open - 1] == L'。' && sense[open] == L'。') sense.erase(open, 1);
            from = open;
        }
    }
    std::wstring trimmed = sense;
    TrimEnd(trimmed);
    const std::size_t ending = ReferenceEnding(trimmed);
    if (ending == std::wstring::npos) return sense;
    sense = std::move(trimmed);
    const std::size_t stop = sense.rfind(L'。', ending);
    if (stop != std::wstring::npos) return sense.substr(0, stop + 1);
    // Nothing but a pointer: the entries it names.
    std::wstring named = sense.substr(0, ending);
    for (const std::wstring_view lead : {L"詳細は", L"詳しくは"}) {
        if (const auto at = named.find(lead); at != std::wstring::npos) {
            before = named.substr(0, at);
            TrimEnd(before);
            while (!before.empty() && before.back() == L'、') before.pop_back();
            named.erase(0, at + lead.size());
        }
    }
    std::vector<std::wstring> names;
    if (named.find(L'「') != std::wstring::npos) {
        // Each 「…」 is one.
        for (std::size_t at = 0; (at = named.find(L'「', at)) != std::wstring::npos;) {
            const std::size_t end = named.find(L'」', at);
            if (end == std::wstring::npos) break;
            names.push_back(named.substr(at + 1, end - at - 1));
            at = end + 1;
        }
    } else {
        // Without brackets: a note in brackets before it ("(名詞) みっか")
        // goes, and 又は, および and 、 part the names.
        if (!named.empty() && (named.front() == L'(' || named.front() == L'（')) {
            const std::size_t end = named.find_first_of(L")）");
            if (end != std::wstring::npos) named.erase(0, end + 1);
        }
        std::size_t start = 0;
        std::size_t at = 0;
        while (at < named.size()) {
            std::size_t width = 0;
            for (const std::wstring_view part : {L"又は", L"または", L"および", L"、"}) {
                if (named.compare(at, part.size(), part) == 0) width = part.size();
            }
            if (width == 0) {
                ++at;
                continue;
            }
            names.push_back(named.substr(start, at - start));
            at += width;
            start = at;
        }
        names.push_back(named.substr(start));
    }
    for (auto& name : names) {
        // "盛土（もりど）": the word, without its reading.
        if (const auto at = name.find_first_of(L"（("); at != std::wstring::npos && at > 0) name.erase(at);
        TrimFront(name);
        TrimEnd(name);
        if (!name.empty()) targets.push_back(std::move(name));
    }
    return {};
}

}  // namespace

bool MeaningDictionary::Open(const std::filesystem::path& dataRoot) noexcept {
    slang_.Open(FindDataPack(dataRoot, L"japanese-slang") / L"meanings.tsv");
    wiktionary_.Open(FindDataPack(dataRoot, L"japanese-wiktionary") / L"meanings.tsv");
    wordnet_.Open(FindDataPack(dataRoot, L"japanese-wordnet") / L"meanings.tsv");
    english_.Open(FindDataPack(dataRoot, L"dictionary-display") / L"entries.tsv");
    englishInJapanese_.Open(FindDataPack(dataRoot, L"english-meanings-ja") / L"meanings.tsv");
    japaneseInEnglish_.Open(FindDataPack(dataRoot, L"japanese-meanings-en") / L"meanings.tsv");
    forms_.Open(FindDataPack(dataRoot, L"english-forms") / L"forms.tsv");
    return IsOpen();
}

std::optional<Meaning> MeaningDictionary::Lookup(std::wstring_view text, std::wstring_view reading,
                                                 MeaningLanguage language) const {
    if (text.empty()) return std::nullopt;
    try {
        if (IsLatinWord(text)) {
            if (text.size() > kMaxEnglishWord) return Latin(text, language);
            std::wstring lower(text);
            for (wchar_t& c : lower) c = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
            const auto forms = FormsOf(lower);
            if (auto meaning = Latin(text, language)) {
                // A word of its own that is also an irregular form (gone,
                // best): its meaning, with the form too. Regular endings are
                // not looked at here (evening is no form of even).
                if (forms_.IsOpen()) {
                    forms_.ForEachRow(ToUtf8(lower), [&](const std::vector<std::string_view>& fields) {
                        if (fields.size() < 3 || fields[1].empty()) return true;
                        meaning->base = FromUtf8(fields[1]);
                        meaning->form = FromUtf8(fields[2]);
                        return false;
                    });
                }
                return meaning;
            }
            // Another form of a word (went, called, players): that word's
            // meaning, with the form.
            for (const auto& [base, form] : forms) {
                if (auto meaning = Latin(base, language)) {
                    meaning->headword = std::wstring(text);
                    meaning->base = base;
                    meaning->form = form;
                    return meaning;
                }
            }
            return std::nullopt;
        }
        const std::wstring hiragana = ToHiragana(reading);
        if (language == MeaningLanguage::English) {
            if (auto meaning = Japanese(japaneseInEnglish_, text, hiragana)) return meaning;
        }
        for (const SortedTsv* source : {&slang_, &wiktionary_, &wordnet_}) {
            if (auto meaning = Japanese(*source, text, hiragana)) return meaning;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<Meaning> MeaningDictionary::Japanese(const SortedTsv& source, std::wstring_view text,
                                                   std::wstring_view reading) const {
    if (!source.IsOpen()) return std::nullopt;
    // The senses of `word`: with `strict`, only from a row for exactly
    // `wordReading`; otherwise from that row if there is one, else the row
    // that names no reading, else the first. A row that only points
    // elsewhere, to nothing there, gives way to the next (何故: "なぜ又は
    // なにゆえ 参照", then なにゆえ's row).
    const auto find = [&](std::wstring_view word, std::wstring_view wordReading,
                          bool strict) -> std::optional<Meaning> {
        if (strict && wordReading.empty()) return std::nullopt;
        const std::string wanted = ToUtf8(wordReading);
        std::vector<std::pair<int, std::vector<std::string_view>>> rows;
        source.ForEachRow(ToUtf8(word), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() < 3) return true;
            const int r = !wanted.empty() && fields[1] == wanted ? 0 : fields[1].empty() ? 1 : 2;
            if (!strict || r == 0) rows.emplace_back(r, fields);
            return true;
        });
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [rank, chosen] : rows) {
            if (auto meaning = Senses(source, word, chosen)) return meaning;
        }
        return std::nullopt;
    };

    // The candidate as it is, then shorter stems of it with the endings of
    // dictionary forms: 会った -> 会っ, 会 (+う) ; 私の -> 私. Only while a
    // stem keeps a character that is not hiragana: kana alone is too
    // ambiguous to cut (はしった is not はし).
    for (const bool strict : {true, false}) {
        if (auto meaning = find(text, reading, strict)) return meaning;
        std::wstring stem(text);
        std::wstring stemReading(reading);
        while (stem.size() > 1 && IsHiragana(stem.back())) {
            const wchar_t dropped = stem.back();
            stem.pop_back();
            if (!stemReading.empty() && stemReading.back() == dropped) stemReading.pop_back(); else stemReading.clear();
            if (std::all_of(stem.begin(), stem.end(), IsHiragana)) break;
            for (const wchar_t ending : kEndings) {
                const std::wstring form = stem + ending;
                const std::wstring formReading = stemReading.empty() ? std::wstring{} : stemReading + ending;
                if (auto meaning = find(form, formReading, strict)) return meaning;
            }
            if (auto meaning = find(stem, stemReading, strict)) return meaning;
        }
    }
    return std::nullopt;
}

// The senses of one row of `source` for `word`, pointers left out or
// followed one step.
std::optional<Meaning> MeaningDictionary::Senses(const SortedTsv& source, std::wstring_view word,
                                                 const std::vector<std::string_view>& row) const {
    Meaning meaning{std::wstring(word), {}};
    const auto add = [&](std::wstring sense) {
        if (sense.empty() || meaning.senses.size() >= kMaxSenses) return;
        if (std::find(meaning.senses.begin(), meaning.senses.end(), sense) == meaning.senses.end()) {
            meaning.senses.push_back(std::move(sense));
        }
    };
    // A sense that only points to other entries ("「十代」を参照。") is
    // their senses, one step: a pointer there is not followed.
    std::vector<std::wstring> pointed;
    std::wstring fallback;
    std::vector<std::wstring> targets;
    std::wstring before;
    for (std::size_t i = 2; i < row.size(); ++i) {
        if (row[i].empty()) continue;
        std::wstring sense = WithoutReference(FromUtf8(row[i]), targets, before);
        if (!sense.empty()) {
            add(std::move(sense));
            continue;
        }
        for (auto& target : targets) {
            if (target != word) pointed.push_back(std::move(target));
        }
        if (fallback.empty()) fallback = before;
    }
    for (const auto& name : pointed) {
        // The row that names no reading, else the first.
        std::vector<std::string_view> other;
        source.ForEachRow(ToUtf8(name), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() < 3) return true;
            if (other.empty() || fields[1].empty()) other = fields;
            return !fields[1].empty();
        });
        for (std::size_t i = 2; i < other.size(); ++i) {
            if (!other[i].empty()) add(WithoutReference(FromUtf8(other[i]), targets, before));
        }
    }
    if (meaning.senses.empty()) add(std::move(fallback));
    if (meaning.senses.empty()) return std::nullopt;
    return meaning;
}

std::optional<Meaning> MeaningDictionary::Latin(std::wstring_view word, MeaningLanguage language) const {
    if (language == MeaningLanguage::Japanese && word.size() <= kMaxEnglishWord) {
        std::wstring lower(word);
        for (wchar_t& c : lower) c = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        if (auto meaning = Japanese(englishInJapanese_, lower, {})) return meaning;
    }
    return English(word);
}

std::vector<std::string> MeaningDictionary::PartsOfSpeech(std::wstring_view word) const {
    // english-forms: the word's row with no base, "parts:noun verb".
    std::vector<std::string> parts;
    if (!forms_.IsOpen()) return parts;
    forms_.ForEachRow(ToUtf8(word), [&](const std::vector<std::string_view>& fields) {
        if (fields.size() < 3 || !fields[1].empty() || !fields[2].starts_with("parts:")) return true;
        std::string_view names = fields[2].substr(6);
        while (!names.empty()) {
            const std::size_t space = names.find(' ');
            parts.emplace_back(names.substr(0, space));
            if (space == std::string_view::npos) break;
            names.remove_prefix(space + 1);
        }
        return false;
    });
    return parts;
}

std::vector<std::pair<std::wstring, std::wstring>> MeaningDictionary::FormsOf(std::wstring_view lower) const {
    std::vector<std::pair<std::wstring, std::wstring>> forms;
    if (forms_.IsOpen()) {
        forms_.ForEachRow(ToUtf8(lower), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() >= 3 && !fields[1].empty()) forms.emplace_back(FromUtf8(fields[1]), FromUtf8(fields[2]));
            return true;
        });
    }
    if (!forms.empty() || lower.size() < 4) return forms;
    const std::wstring word(lower);
    const auto ends = [&](std::wstring_view end) { return word.size() > end.size() + 1 && word.ends_with(end); };
    const auto cut = [&](std::size_t n) { return word.substr(0, word.size() - n); };
    // A doubled last consonant (running, bigger, stopped): one of it.
    const auto undoubled = [&](std::size_t n) {
        const std::wstring stem = cut(n);
        return stem.size() >= 3 && stem.back() == stem[stem.size() - 2] ? stem.substr(0, stem.size() - 1) : std::wstring{};
    };
    // The word's parts of speech; one the dictionary does not have is taken
    // as any (the Japanese meanings name none).
    const auto is = [&](const std::wstring& base, std::initializer_list<std::string_view> wanted) {
        const auto parts = PartsOfSpeech(base);
        if (parts.empty()) return true;
        return std::any_of(parts.begin(), parts.end(), [&](const std::string& part) {
            return std::find(wanted.begin(), wanted.end(), part) != wanted.end();
        });
    };
    const auto add = [&](const std::wstring& base, std::wstring_view form) {
        if (base.size() < 2) return;
        if (std::any_of(forms.begin(), forms.end(), [&](const auto& f) { return f.first == base; })) return;
        forms.emplace_back(base, std::wstring(form));
    };
    if (ends(L"s") && !ends(L"ss")) {
        std::vector<std::wstring> bases;
        if (ends(L"ies")) bases.push_back(cut(3) + L"y");
        if (ends(L"es")) bases.push_back(cut(2));
        bases.push_back(cut(1));
        for (const auto& base : bases) {
            const bool noun = is(base, {"noun"});
            const bool verb = is(base, {"verb"});
            if (!noun && !verb) continue;
            const auto parts = PartsOfSpeech(base);
            const bool both = parts.empty() || (std::find(parts.begin(), parts.end(), "noun") != parts.end() &&
                                                std::find(parts.begin(), parts.end(), "verb") != parts.end());
            add(base, both ? L"plural+third-person" : noun ? L"plural" : L"third-person");
        }
    }
    if (ends(L"ed")) {
        for (const auto& base : {ends(L"ied") ? cut(3) + L"y" : std::wstring{}, cut(2), cut(1), undoubled(2)}) {
            if (!base.empty() && is(base, {"verb"})) add(base, L"past+past-participle");
        }
    }
    if (ends(L"ing")) {
        for (const auto& base : {cut(3), cut(3) + L"e", undoubled(3)}) {
            if (!base.empty() && is(base, {"verb"})) add(base, L"present-participle");
        }
    }
    for (const auto& [end, form] : {std::pair{std::wstring_view(L"er"), std::wstring_view(L"comparative")},
                                    std::pair{std::wstring_view(L"est"), std::wstring_view(L"superlative")}}) {
        if (!ends(end)) continue;
        const std::size_t n = end.size();
        for (const auto& base : {ends(std::wstring(L"i") + std::wstring(end)) ? cut(n + 1) + L"y" : std::wstring{}, cut(n),
                                 cut(n - 1), undoubled(n)}) {
            if (!base.empty() && is(base, {"adj", "adv"})) add(base, form);
        }
    }
    return forms;
}

std::optional<Meaning> MeaningDictionary::English(std::wstring_view word) const {
    if (!english_.IsOpen() || word.size() > kMaxEnglishWord) return std::nullopt;
    std::string key = "external:";
    for (const wchar_t c : word) key.push_back(static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c));
    std::optional<Meaning> meaning;
    // entries.tsv: key, word, pronunciation, part of speech, senses joined
    // with " / ", example.
    english_.ForEachRow(key, [&](const std::vector<std::string_view>& fields) {
        if (fields.size() < 5 || fields[4].empty()) return true;
        meaning = Meaning{FromUtf8(fields[1]), {}};
        std::string_view senses = fields[4];
        while (!senses.empty() && meaning->senses.size() < kMaxSenses) {
            const std::size_t split = senses.find(" / ");
            meaning->senses.push_back(FromUtf8(senses.substr(0, split)));
            if (split == std::string_view::npos) break;
            senses.remove_prefix(split + 3);
        }
        return false;
    });
    return meaning;
}

}  // namespace tekito::japanese
