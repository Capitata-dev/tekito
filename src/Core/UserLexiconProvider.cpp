#include "Core/UserLexiconProvider.h"

#include <algorithm>
#include <cwctype>

namespace tekito {
namespace {

// A word the user added as a word (written as it is typed, letters only):
// a slip in it is put right to it, as with any dictionary word.
bool IsUserWord(const UserDictionaryEntry& entry) {
    if (entry.caseSensitive || entry.raw.size() < 3 || entry.raw.size() != entry.candidate.size()) return false;
    return std::equal(entry.raw.begin(), entry.raw.end(), entry.candidate.begin(), [](wchar_t typed, wchar_t written) {
        return std::iswalpha(typed) && std::towlower(typed) == std::towlower(written);
    });
}

}  // namespace

UserLexiconProvider::UserLexiconProvider(const UserDictionary& dictionary)
    : dictionary_(dictionary) {
    Refresh();
}

void UserLexiconProvider::Refresh() {
    const auto userEntries = dictionary_.Entries();
    rawTexts_.clear();
    candidateTexts_.clear();
    entries_.clear();
    rawTexts_.reserve(userEntries.size());
    candidateTexts_.reserve(userEntries.size());
    entries_.reserve(userEntries.size());

    for (const auto& userEntry : userEntries) {
        if (!userEntry.enabled) continue;
        rawTexts_.push_back(userEntry.raw);
        candidateTexts_.push_back(userEntry.candidate);

        entries_.push_back({rawTexts_.back(), candidateTexts_.back(),
                            SemanticLabel::Original, SemanticLabel::Standard,
                            false, IsUserWord(userEntry), CandidatePolicySuggestOnly,
                            userEntry.policyFlags, CandidateSourceUserDictionary,
                            userEntry.caseSensitive});
    }
}

std::span<const LexiconEntry> UserLexiconProvider::Entries() const noexcept {
    return entries_;
}

}  // namespace tekito
