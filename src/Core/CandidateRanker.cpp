#include "Core/CandidateRanker.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace tekito {

namespace {

std::size_t RawSize(const CorrectionCandidate& candidate) {
    return candidate.rawText.empty() ? candidate.entry.raw.size()
                                     : candidate.rawText.size();
}

// Combines the weighted error cost with frequency and context into one
// continuous score. A strict priority chain (integer distance, then length,
// then frequency, ...) always lets a smaller integer distance or a same-
// length candidate win outright regardless of how implausible it otherwise
// is -- a distance-1 rare word beats a distance-2 common word every time.
// Weighing the signals together instead lets a large frequency or context
// gap outweigh a small error-cost gap, and vice versa.
double CombinedScore(const CorrectionCandidate& candidate) {
    constexpr double kErrorWeight = 10.0;
    constexpr double kFrequencyWeight = 1.0;
    // data/en/phrase's raw scores run roughly 3-50, versus frequency's roughly
    // 0-7 (both are corpus-derived but on different scales), so this must be
    // scaled down to keep context a proportionate vote alongside frequency
    // and error cost rather than one that mechanically dominates both
    // whenever it is nonzero.
    constexpr double kContextWeight = 0.15;
    return -kErrorWeight * candidate.weightedCost +
           kFrequencyWeight * candidate.signals.frequency +
           kContextWeight * candidate.signals.phraseContext;
}

}  // namespace

void CandidateRanker::RankCorrections(std::vector<CorrectionCandidate>& candidates,
                                      std::size_t inputSize) const {
    // The order is built from values each candidate has on its own, so it
    // is a true order: comparing pairs by rules that apply to some pairs
    // only (learning past a threshold, both being valid words) could go
    // round in a circle (A before B before C before A), which sorting
    // cannot follow.
    //
    // 1. A weak exact match goes first, the more frequent first.
    // 2. Learning: a clear preference (at least kLearnedPreferenceThreshold
    //    either way) puts a candidate up or down in steps of
    //    kLearnedPreferenceMargin.
    // 3. Within the same step, candidates that are valid words in conflict
    //    keep their order by context and frequency, and the others by the
    //    combined score; the two runs are merged by the combined score.
    constexpr double kLearnedPreferenceThreshold = 0.20;
    constexpr double kLearnedPreferenceMargin = 0.10;
    const auto learnedStep = [](const CorrectionCandidate& candidate) {
        const double learning = candidate.signals.userLearning;
        if (std::abs(learning) < kLearnedPreferenceThreshold) return 0LL;
        return static_cast<long long>(std::floor(learning / kLearnedPreferenceMargin));
    };
    // Exact ties (no frequency or context data for either): the shape of
    // the slip, so the order never depends on the order found.
    const auto shapeFirst = [inputSize](const CorrectionCandidate& left, const CorrectionCandidate& right) {
        if (left.distance != right.distance) return left.distance < right.distance;
        if (left.repeatedKey != right.repeatedKey) return left.repeatedKey;
        if (left.transposition != right.transposition) return left.transposition;
        if (left.missingCharacter != right.missingCharacter) return left.missingCharacter;
        if (left.extraCharacter != right.extraCharacter) return left.extraCharacter;
        const auto leftSize = RawSize(left);
        const auto rightSize = RawSize(right);
        const auto leftDelta = leftSize > inputSize ? leftSize - inputSize : inputSize - leftSize;
        const auto rightDelta = rightSize > inputSize ? rightSize - inputSize : inputSize - rightSize;
        if (leftDelta != rightDelta) return leftDelta < rightDelta;
        return leftSize < rightSize;
    };
    const auto byScore = [&](const CorrectionCandidate& left, const CorrectionCandidate& right) {
        const auto leftScore = CombinedScore(left);
        const auto rightScore = CombinedScore(right);
        if (leftScore != rightScore) return leftScore > rightScore;
        return shapeFirst(left, right);
    };
    const auto byConflict = [&](const CorrectionCandidate& left, const CorrectionCandidate& right) {
        if (left.signals.contextConfidence != right.signals.contextConfidence) {
            return left.signals.contextConfidence > right.signals.contextConfidence;
        }
        if (left.signals.frequency != right.signals.frequency) return left.signals.frequency > right.signals.frequency;
        return byScore(left, right);
    };
    const auto group = [&](const CorrectionCandidate& left, const CorrectionCandidate& right) {
        if (left.weakExactRecovery != right.weakExactRecovery) return left.weakExactRecovery;
        if (left.weakExactRecovery && left.signals.frequency != right.signals.frequency) {
            return left.signals.frequency > right.signals.frequency;
        }
        return learnedStep(left) > learnedStep(right);
    };
    std::stable_sort(candidates.begin(), candidates.end(), group);
    for (auto first = candidates.begin(); first != candidates.end();) {
        const auto last = std::find_if(first, candidates.end(), [&](const CorrectionCandidate& c) {
            return group(*first, c);
        });
        std::vector<CorrectionCandidate> conflicts;
        std::vector<CorrectionCandidate> others;
        for (auto it = first; it != last; ++it) (it->validWordConflict ? conflicts : others).push_back(std::move(*it));
        std::stable_sort(conflicts.begin(), conflicts.end(), byConflict);
        std::stable_sort(others.begin(), others.end(), byScore);
        auto out = first;
        auto a = conflicts.begin();
        auto b = others.begin();
        while (a != conflicts.end() || b != others.end()) {
            const bool takeConflict = b == others.end() || (a != conflicts.end() && !byScore(*b, *a));
            *out++ = std::move(takeConflict ? *a++ : *b++);
        }
        first = last;
    }
}
}  // namespace tekito
