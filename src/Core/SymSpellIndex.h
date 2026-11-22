#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekito {

// Edit distance where two neighbouring letters swapped (teh, the) are one
// edit, as are a letter put in, left out or hit for another (optimal string
// alignment). Returns limit + 1 for anything past limit, as soon as it is.
[[nodiscard]] std::size_t BoundedEditDistance(std::string_view left, std::string_view right,
                                              std::size_t limit);

// Symmetric-delete fuzzy lookup (Garbe's SymSpell): finds indexed words
// within the given BoundedEditDistance, wherever in the word the edits fall,
// by matching precomputed deletions of the dictionary against deletions of
// the query. Unlike a prefix/first-character scan, this does not require the
// start of the word to be correct.
class SymSpellIndex final {
public:
    static constexpr std::size_t kMaxSupportedDistance = 2;

    // words: lowercase ASCII, ownership taken. Duplicate words are fine.
    void Build(std::vector<std::string> words);

    [[nodiscard]] bool IsBuilt() const noexcept { return !words_.empty(); }

    // Returns indexed words within maxDistance (<= kMaxSupportedDistance) of
    // query, deduplicated, nearest first; the caller ranks results.
    [[nodiscard]] std::vector<std::string_view> Lookup(std::string_view query,
                                                        std::size_t maxDistance) const;

private:
    std::vector<std::string> words_;
    std::unordered_map<std::string, std::vector<std::uint32_t>> deletes_;
};

}  // namespace tekito
