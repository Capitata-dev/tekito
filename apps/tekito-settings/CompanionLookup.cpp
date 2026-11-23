#include "apps/tekito-settings/CompanionLookup.h"

#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/Meanings.h"

#include <algorithm>
#include <cwctype>

namespace tekito::companion {
namespace {

// Longer than this is cut: a word or a phrase is what it reads.
constexpr std::size_t kLongest = 32;
// Senses said, at most.
constexpr std::size_t kSenses = 2;

bool IsLatin(std::wstring_view text) {
    return std::all_of(text.begin(), text.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'\'' || c == L'-' || c == L' ';
    });
}

bool HasKanji(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF); });
}

std::wstring Trimmed(std::wstring_view text) {
    const auto space = [](wchar_t c) { return std::iswspace(c) || c == L'　'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    // The first line only.
    if (const auto end = text.find_first_of(L"\r\n"); end != std::wstring_view::npos) text = text.substr(0, end);
    return std::wstring(text.substr(0, kLongest));
}

std::wstring Senses(const japanese::Meaning& meaning) {
    std::wstring body;
    for (std::size_t i = 0; i < meaning.senses.size() && i < kSenses; ++i) {
        if (!body.empty()) body += L'\n';
        body += meaning.senses[i];
    }
    return body;
}

}  // namespace

Lookup LookUp(std::wstring_view given, bool japaneseUi) {
    const std::wstring text = Trimmed(given);
    if (text.empty()) return {};
    const auto& meanings = japanese::ProcessMeanings();
    const auto notFound = japaneseUi ? L"読みと意味は見つかりませんでした" : L"No reading or meaning was found";
    if (IsLatin(text)) {
        // An English word: its meaning, in Japanese for Japanese Settings.
        const auto meaning = meanings.Lookup(text, L"", japaneseUi ? japanese::MeaningLanguage::Japanese
                                                                   : japanese::MeaningLanguage::Word);
        if (!meaning) return {text, japaneseUi ? L"意味は見つかりませんでした" : L"No meaning was found"};
        std::wstring title = text;
        // Another form of a word: which (went: go).
        if (!meaning->base.empty()) title += L" (" + meaning->base + L")";
        return {title, Senses(*meaning)};
    }
    // Japanese: how it reads, when it has kanji, and what it means.
    std::wstring reading;
    if (HasKanji(text)) {
        if (const auto* converter = japanese::ProcessConverter()) {
            if (auto guess = converter->ReadingOf(text)) reading = std::move(guess->reading);
        }
    }
    const auto meaning = meanings.Lookup(text, reading, japaneseUi ? japanese::MeaningLanguage::Word
                                                                   : japanese::MeaningLanguage::English);
    std::wstring title = text;
    if (!reading.empty() && reading != text) title += L"（" + reading + L"）";
    if (meaning) return {title, Senses(*meaning)};
    if (!reading.empty()) return {title, japaneseUi ? L"意味は見つかりませんでした" : L"No meaning was found"};
    return {title, notFound};
}

}  // namespace tekito::companion
