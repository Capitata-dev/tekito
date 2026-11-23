#include "Core/SymSpellIndex.h"

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_set>
#include <utility>

namespace tekito {
namespace {

// Emits every distinct string reachable by deleting 1..maxDepth characters
// from `word` (any positions, any order), via depth-first deletion. `seen`
// dedupes within one call so e.g. deleting position 0 then 2 and deleting
// position 2 then 0 (which can coincide) is only emitted once.
void CollectDeletes(std::string_view word, std::size_t remainingDepth,
                    std::unordered_set<std::string>& seen,
                    const std::function<void(const std::string&)>& emit) {
    if (remainingDepth == 0) return;
    for (std::size_t index = 0; index < word.size(); ++index) {
        std::string variant;
        variant.reserve(word.size() - 1);
        variant.append(word.substr(0, index));
        variant.append(word.substr(index + 1));
        if (seen.insert(variant).second) {
            emit(variant);
            CollectDeletes(variant, remainingDepth - 1, seen, emit);
        }
    }
}

// FNV-1a, 32 bits.
std::uint32_t HashOf(std::string_view text) noexcept {
    std::uint32_t hash = 2166136261u;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

std::size_t BoundedEditDistance(std::string_view left, std::string_view right, std::size_t limit) {
    if (left.size() > right.size() + limit || right.size() > left.size() + limit) {
        return limit + 1;
    }
    // Three rows: the one two back holds what a swap of two letters adds to.
    constexpr std::size_t kStackColumns = 64;
    std::array<std::size_t, 3 * kStackColumns> stack{};
    std::vector<std::size_t> heap;
    std::size_t* rows = stack.data();
    const std::size_t width = right.size() + 1;
    if (width > kStackColumns) {
        heap.resize(3 * width);
        rows = heap.data();
    }
    std::size_t* beforePrevious = rows;
    std::size_t* previous = rows + width;
    std::size_t* current = rows + 2 * width;
    for (std::size_t column = 0; column < width; ++column) previous[column] = column;
    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        std::size_t best = current[0];
        for (std::size_t column = 1; column < width; ++column) {
            current[column] = std::min({current[column - 1] + 1, previous[column] + 1,
                                        previous[column - 1] +
                                            (left[row - 1] == right[column - 1] ? 0 : 1)});
            if (row > 1 && column > 1 && left[row - 1] == right[column - 2] &&
                left[row - 2] == right[column - 1]) {
                current[column] = std::min(current[column], beforePrevious[column - 2] + 1);
            }
            best = std::min(best, current[column]);
        }
        // No later row is below this one's least (a swap costs what the
        // letter-for-letter path before it does), so past limit it stays.
        if (best > limit) return limit + 1;
        std::swap(beforePrevious, previous);
        std::swap(previous, current);
    }
    return previous[right.size()];
}

void SymSpellIndex::Build(std::vector<std::string> words) {
    words_ = std::move(words);
    postings_.clear();
    postings_.reserve(words_.size() * 40);

    std::unordered_set<std::string> seenForWord;
    for (std::uint32_t index = 0; index < words_.size(); ++index) {
        const auto& word = words_[index];
        postings_.push_back({HashOf(word), index});
        seenForWord.clear();
        seenForWord.insert(word);
        CollectDeletes(word, kMaxSupportedDistance, seenForWord,
                       [&](const std::string& variant) { postings_.push_back({HashOf(variant), index}); });
    }
    std::sort(postings_.begin(), postings_.end());
    postings_.erase(std::unique(postings_.begin(), postings_.end()), postings_.end());
    postings_.shrink_to_fit();
}

std::vector<std::string_view> SymSpellIndex::Lookup(std::string_view query,
                                                     std::size_t maxDistance) const {
    std::vector<std::string_view> results;
    if (words_.empty() || query.empty()) return results;
    maxDistance = std::min(maxDistance, kMaxSupportedDistance);

    std::unordered_set<std::uint32_t> candidateIndices;
    const auto considerBucket = [&](const std::string& key) {
        const std::uint32_t hash = HashOf(key);
        auto it = std::lower_bound(postings_.begin(), postings_.end(), Posting{hash, 0});
        for (; it != postings_.end() && it->hash == hash; ++it) candidateIndices.insert(it->word);
    };

    const std::string queryOwned(query);
    considerBucket(queryOwned);
    std::unordered_set<std::string> seenForQuery;
    seenForQuery.insert(queryOwned);
    CollectDeletes(queryOwned, maxDistance, seenForQuery,
                   [&](const std::string& variant) { considerBucket(variant); });

    std::vector<std::pair<std::size_t, std::string_view>> found;
    found.reserve(candidateIndices.size());
    for (const auto wordIndex : candidateIndices) {
        const auto& word = words_[wordIndex];
        const auto distance = BoundedEditDistance(query, word, maxDistance);
        if (distance <= maxDistance) found.emplace_back(distance, word);
    }
    // Nearest first, so a caller that takes only so many keeps the closest;
    // then by spelling, so the order is the same every run (candidateIndices
    // is a hash set).
    std::sort(found.begin(), found.end());
    results.reserve(found.size());
    for (const auto& [distance, word] : found) results.push_back(word);
    return results;
}

}  // namespace tekito
