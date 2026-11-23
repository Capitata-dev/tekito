#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// Predictions while typing, and forgetting or hiding what was learned.

namespace tekito::japanese {

void JapaneseComposer::UpdatePredictions() {
    predictions_.clear();
    chosenPrediction_.reset();
    if (!predictionEnabled_ || !IsTyping()) return;
    // Complete kana only (no key waiting to become one), at least two of
    // them, typed at the end.
    const std::wstring reading = Reading();
    if (!pending_.empty() || reading.size() < 2 || LeftoverLetters(reading) > 0 || caret_ != units_.size()) {
        return;
    }
    constexpr std::size_t kPredictions = 5;
    if (learning_) {
        for (auto& entry : learning_->StartingWith(reading, kPredictions)) {
            predictions_.push_back({std::move(entry.reading), std::move(entry.surface), 0});
        }
    }
    if (converter_) {
        for (auto& prediction : converter_->Predict(reading, kPredictions, userDictionary_)) {
            if (predictions_.size() >= kPredictions) break;
            const bool seen = std::any_of(predictions_.begin(), predictions_.end(),
                                          [&](const Prediction& p) { return p.text == prediction.text; });
            if (!seen) predictions_.push_back(std::move(prediction));
        }
    }
    // Only kanji in the range set (KeepKanjiInRange).
    if (kanjiSets_ && kanjiRange_ != KanjiRange::All) {
        std::erase_if(predictions_, [&](const Prediction& p) { return !kanjiSets_->Allows(p.text, kanjiRange_, kanjiGrade_, kanjiNames_); });
    }
}

void JapaneseComposer::NextPrediction() {
    if (predictions_.empty()) return;
    chosenPrediction_ = chosenPrediction_ ? (*chosenPrediction_ + 1) % predictions_.size() : 0;
}

void JapaneseComposer::PreviousPrediction() {
    if (predictions_.empty()) return;
    const std::size_t count = predictions_.size();
    chosenPrediction_ = chosenPrediction_ ? (*chosenPrediction_ + count - 1) % count : count - 1;
}

bool JapaneseComposer::CanForgetChosen() const {
    if (!learning_) return false;
    if (IsTyping() && chosenPrediction_ && *chosenPrediction_ < predictions_.size()) {
        const auto& prediction = predictions_[*chosenPrediction_];
        return learning_->Contains(prediction.reading, prediction.text);
    }
    if (!IsConverted() || preview_ || !listOpen_ || focus_ >= phrases_.size()) return false;
    const auto& phrase = phrases_[focus_];
    if (phrase.form || phrase.selected >= phrase.candidates.size()) return false;
    return learning_->Contains(LearningReading(conversionReading_, phrase.begin, phrase.length),
                               phrase.candidates[phrase.selected].text);
}

bool JapaneseComposer::ForgetChosen() {
    if (!CanForgetChosen()) return false;
    if (IsTyping()) {
        const auto at = static_cast<std::ptrdiff_t>(*chosenPrediction_);
        learning_->Forget(predictions_[*chosenPrediction_].reading, predictions_[*chosenPrediction_].text);
        predictions_.erase(predictions_.begin() + at);
        if (predictions_.empty()) {
            chosenPrediction_.reset();
        } else if (*chosenPrediction_ >= predictions_.size()) {
            chosenPrediction_ = predictions_.size() - 1;
        }
        return true;
    }
    const auto& phrase = phrases_[focus_];
    return learning_->Forget(LearningReading(conversionReading_, phrase.begin, phrase.length),
                             phrase.candidates[phrase.selected].text);
}

bool JapaneseComposer::CanHideChosen() const {
    if (!IsConverted() || preview_ || !listOpen_ || focus_ >= phrases_.size()) return false;
    const auto& phrase = phrases_[focus_];
    if (phrase.form || phrase.selected >= phrase.candidates.size() || phrase.candidates.size() < 2) return false;
    if (phrase.begin + phrase.length > conversionReading_.size()) return false;
    const auto& chosen = phrase.candidates[phrase.selected];
    // A word of the dictionary, not the kana typed, a date or a slip undone.
    if (chosen.kind != PhraseCandidate::Kind::Dictionary || chosen.length > 0 || chosen.chunk || !chosen.reading.empty() ||
        chosen.slip || chosen.text.empty()) {
        return false;
    }
    return !CanForgetChosen();
}

std::optional<std::pair<std::wstring, std::wstring>> JapaneseComposer::HideChosen() {
    if (!CanHideChosen()) return std::nullopt;
    auto& phrase = phrases_[focus_];
    std::wstring reading = conversionReading_.substr(phrase.begin, phrase.length);
    std::wstring text = phrase.candidates[phrase.selected].text;
    // The kana both end with (the particle after the word) left off.
    const auto hiragana = [](wchar_t c) { return c >= L'\x3041' && c <= L'\x309F'; };
    while (reading.size() > 1 && text.size() > 1 && reading.back() == text.back() && hiragana(text.back())) {
        reading.pop_back();
        text.pop_back();
    }
    phrase.candidates.erase(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(phrase.selected));
    if (phrase.selected >= phrase.candidates.size()) phrase.selected = phrase.candidates.size() - 1;
    return std::pair{std::move(reading), std::move(text)};
}

void JapaneseComposer::ChoosePrediction(std::size_t index) {
    if (index < predictions_.size()) chosenPrediction_ = index;
}

}  // namespace tekito::japanese
