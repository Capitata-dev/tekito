#include "Core/Japanese/JapaneseKeys.h"

#include "Core/Japanese/KanaText.h"

#include <algorithm>

namespace tekito::japanese {
namespace {

using Key = KeyPress::Key;
using Action = KeyCommand::Action;

KeyCommand Make(Action action) {
    KeyCommand command;
    command.action = action;
    return command;
}

KeyCommand WithDelta(Action action, int delta) {
    auto command = Make(action);
    command.delta = delta;
    return command;
}

// Keys that move the caret or focus: they settle the text and go on to the
// application.
bool IsCaretKey(Key key) {
    switch (key) {
    case Key::Left: case Key::Right: case Key::Up: case Key::Down:
    case Key::Home: case Key::End: case Key::PageUp: case Key::PageDown:
    case Key::Delete: case Key::Insert: case Key::Tab:
        return true;
    default:
        return false;
    }
}

}  // namespace

std::optional<KeyCommand> TranslateKey(const JapaneseComposer& composer, const KeyPress& key,
                                       const KeyOptions& options) {
    const bool composing = composer.IsComposing();
    if (!composing && key.key == Key::Backspace && key.control && !key.shift && composer.LastCommit()) {
        return Make(Action::UndoCommit);
    }
    if (composing && key.key == Key::Delete && key.control && !key.shift &&
        (composer.CanForgetChosen() || composer.CanHideChosen())) {
        return Make(Action::ForgetChosen);
    }
    if (key.command) {
        // Shortcuts act on the committed text.
        if (!composing) return std::nullopt;
        auto command = Make(Action::Commit);
        command.letKeyThrough = true;
        return command;
    }
    // Predictions while typing (also while the live conversion is shown).
    if (composer.IsTyping() && !composer.Predictions().empty()) {
        const bool chosen = composer.ChosenPrediction().has_value();
        switch (key.key) {
        case Key::Tab:
            return Make(key.shift ? Action::PreviousPrediction : Action::NextPrediction);
        case Key::Down:
            return Make(Action::NextPrediction);
        case Key::Up:
            if (chosen) return Make(Action::PreviousPrediction);
            break;
        case Key::Escape:
            if (chosen) return Make(Action::ClearPrediction);
            break;
        default:
            break;
        }
    }
    // Live: the arrows edit the typed text, as before conversion.
    if (composer.IsLivePreview() && !key.shift) {
        switch (key.key) {
        case Key::Left:
            return WithDelta(Action::MoveCaret, -1);
        case Key::Right:
            return WithDelta(Action::MoveCaret, 1);
        case Key::Home:
            return WithDelta(Action::MoveCaret, -100000);
        case Key::End:
            return WithDelta(Action::MoveCaret, 100000);
        case Key::Delete:
            return Make(Action::Delete);
        default:
            break;
        }
    }
    // The mark left at punctuation (CommitBeforeMark): Space and Henkan
    // show its other forms, Backspace drops it, Esc steps back; any other
    // key commits it and goes on to the application, as it would have
    // with the mark committed.
    if (composer.MarkLeftUntouched()) {
        switch (key.key) {
        case Key::Character:
        case Key::Space:
        case Key::Convert:
        case Key::Backspace:
        case Key::Escape:
            break;
        default: {
            auto command = Make(Action::Commit);
            command.letKeyThrough = true;
            return command;
        }
        }
    }
    if (composer.IsConverted()) {
        switch (key.key) {
        case Key::Space:
            return Make(key.shift ? Action::PreviousCandidate : Action::Convert);
        case Key::Down:
            return Make(Action::NextCandidate);
        case Key::Up:
            return Make(Action::PreviousCandidate);
        case Key::PageDown:
        case Key::PageUp: {
            // With the list open, a page at a time: the first page, then
            // the pages of columns after it.
            if (!composer.IsCandidateListOpen() || !composer.FocusedCandidates()) break;
            const std::size_t size = composer.FocusedCandidates()->size();
            const std::size_t at = composer.FocusedSelection();
            const auto page = ListPageOf(at, size, options.pageSize);
            if (key.key == Key::PageDown) {
                const std::size_t next = page.start + page.count;
                if (next >= size) break;
                return WithDelta(Action::NextCandidate, static_cast<int>(next - at));
            }
            if (page.start == 0) break;
            const auto before = ListPageOf(page.start - 1, size, options.pageSize);
            return WithDelta(Action::PreviousCandidate, static_cast<int>(at - before.start));
        }
        case Key::Left:
        case Key::Right: {
            // In the columns, the column beside: as far as the list goes,
            // back into the first page from the first column.
            const auto* candidates = composer.IsCandidateListOpen() ? composer.FocusedCandidates() : nullptr;
            const std::size_t pageSize = std::max<std::size_t>(options.pageSize, 1);
            if (!key.shift && candidates && composer.FocusedSelection() >= pageSize && candidates->size() > pageSize) {
                const std::size_t at = composer.FocusedSelection();
                if (key.key == Key::Right) {
                    const std::size_t to = std::min(at + pageSize, candidates->size() - 1);
                    // Past the last column, on to the first as Space goes.
                    return WithDelta(Action::NextCandidate, static_cast<int>(to == at ? 1 : to - at));
                }
                return WithDelta(Action::PreviousCandidate, static_cast<int>(std::min(at, pageSize)));
            }
            return WithDelta(key.shift ? Action::Resize : Action::MoveFocus, key.key == Key::Left ? -1 : 1);
        }
        default:
            break;
        }
        // With the list open, 1-9 choose from the first page, the one
        // numbered.
        if (key.digit >= 1 && key.digit <= 9 && composer.IsCandidateListOpen() && !key.shift) {
            // In the columns, unnumbered, a digit stays with the list.
            if (composer.FocusedSelection() >= std::max<std::size_t>(options.pageSize, 1)) return Make(Action::Nothing);
            const auto* candidates = composer.FocusedCandidates();
            const std::size_t index = static_cast<std::size_t>(key.digit - 1);
            if (candidates && index < candidates->size()) {
                auto command = Make(Action::SelectCandidate);
                command.index = index;
                return command;
            }
        }
    }
    if (composing && !composer.IsConverted()) {
        // Before conversion the arrows edit the typed text, as in Microsoft IME.
        switch (key.key) {
        case Key::Left:
            return WithDelta(Action::MoveCaret, -1);
        case Key::Right:
            return WithDelta(Action::MoveCaret, 1);
        case Key::Home:
            return WithDelta(Action::MoveCaret, -100000);
        case Key::End:
            return WithDelta(Action::MoveCaret, 100000);
        case Key::Delete:
            return Make(Action::Delete);
        default:
            break;
        }
    }
    switch (key.key) {
    case Key::Space:
    case Key::Convert:
        if (composing) return Make(Action::Convert);
        // The caller finds the text (JapaneseComposer::Reconvert).
        if (key.key == Key::Convert) return Make(Action::Reconvert);
        if (key.key == Key::Space) {
            const bool fullWidth = options.fullWidthSpace != key.shift;
            // A half-width space is the application's own.
            if (!fullWidth) return std::nullopt;
            auto command = Make(Action::InsertOutside);
            command.character = L'\x3000';
            return command;
        }
        return std::nullopt;
    case Key::NonConvert:
        if (!composing) return std::nullopt;
        return Make(Action::CycleKana);
    case Key::Enter:
        if (!composing) return std::nullopt;
        if (key.shift && options.shiftEnterToFocus && composer.CanCommitToFocus()) return Make(Action::CommitToFocus);
        return Make(Action::Commit);
    case Key::Backspace:
        if (!composing) return std::nullopt;
        return Make(Action::Backspace);
    case Key::Escape:
        if (!composing) return std::nullopt;
        return Make(Action::Cancel);
    case Key::F6:
    case Key::F7:
    case Key::F8:
    case Key::F9:
    case Key::F10: {
        if (!composing) return std::nullopt;
        constexpr KanaForm forms[] = {KanaForm::Hiragana, KanaForm::Katakana, KanaForm::HalfWidthKatakana,
                                      KanaForm::FullWidthAlphanumeric, KanaForm::HalfWidthAlphanumeric};
        auto command = Make(Action::Transliterate);
        command.form = forms[static_cast<int>(key.key) - static_cast<int>(Key::F6)];
        return command;
    }
    default:
        break;
    }
    // Tab while typing shows the candidates rather than ending the text with
    // a tab (the predictions come first, above, when there are some); once
    // converted it steps through them.
    if (key.key == Key::Tab && (composer.IsTyping() || composer.IsConverted())) {
        if (composer.IsConverted() && !composer.IsLivePreview()) {
            // With the list open: Tab goes into the columns after the first
            // page, Shift+Tab back to its top.
            const auto* candidates = composer.IsCandidateListOpen() ? composer.FocusedCandidates() : nullptr;
            const std::size_t pageSize = std::max<std::size_t>(options.pageSize, 1);
            if (candidates && candidates->size() > pageSize) {
                const std::size_t at = composer.FocusedSelection();
                if (!key.shift && at < pageSize) return WithDelta(Action::NextCandidate, static_cast<int>(pageSize - at));
                if (key.shift && at >= pageSize) return WithDelta(Action::PreviousCandidate, static_cast<int>(at));
            }
            return Make(key.shift ? Action::PreviousCandidate : Action::NextCandidate);
        }
        return Make(Action::ShowCandidates);
    }
    if (IsCaretKey(key.key)) {
        if (!composing) return std::nullopt;
        auto command = Make(Action::Commit);
        command.letKeyThrough = true;
        return command;
    }
    if (key.key == Key::Character && key.character > 0x20 && key.character != 0x7F) {
        auto command = Make(Action::Insert);
        command.character = key.character;
        return command;
    }
    return std::nullopt;
}

KeyOutcome ApplyKey(JapaneseComposer& composer, const KeyCommand& command) {
    KeyOutcome outcome;
    switch (command.action) {
    case Action::ShowCandidates:
        composer.ShowCandidates();
        break;
    case Action::Reconvert:
        break;
    case Action::ForgetChosen:
        // A learned one is forgotten; one never learned is hidden.
        if (!composer.ForgetChosen()) outcome.hidden = composer.HideChosen();
        break;
    case Action::UndoCommit:
        if (const auto* text = composer.LastCommit()) {
            std::wstring committed = *text;
            if (composer.UndoCommit()) outcome.uncommitted = std::move(committed);
        }
        break;
    case Action::Insert:
        // After a conversion, typing on commits it and starts anew (live
        // conversion types on).
        // A mark left at punctuation goes before what is typed next.
        if (composer.CommitsBeforeTyping() || composer.MarkLeft()) outcome.committed = composer.Commit();
        composer.Insert(command.character);
        // A punctuation mark commits the text up to it, converted as Space
        // converts it (live conversion and Mix convert on Enter themselves);
        // the mark stays for its other forms.
        if (composer.EndsAtPunctuation()) {
            const std::wstring before = composer.CommitBeforeMark(command.character);
            if (!before.empty() || !composer.IsComposing()) {
                outcome.committed = outcome.committed.value_or(std::wstring{}) + before;
            }
        }
        break;
    case Action::Backspace:
        if (composer.MarkLeftUntouched()) {
            composer.DropMark();
            break;
        }
        composer.Backspace();
        break;
    case Action::Cancel:
        composer.StepBack();
        break;
    case Action::Convert:
        composer.Convert();
        break;
    case Action::CycleKana:
        composer.CycleKana();
        break;
    case Action::Transliterate:
        composer.Transliterate(command.form);
        break;
    case Action::NextCandidate:
        for (int i = 0; i < std::max(command.delta, 1); ++i) composer.NextCandidate();
        break;
    case Action::PreviousCandidate:
        for (int i = 0; i < std::max(command.delta, 1); ++i) composer.PreviousCandidate();
        break;
    case Action::MoveFocus:
        composer.MoveFocus(command.delta);
        break;
    case Action::Resize:
        composer.ResizeFocus(command.delta);
        break;
    case Action::SelectCandidate:
        composer.SelectCandidate(command.index);
        break;
    case Action::ChooseRow:
        if (composer.IsTyping()) {
            // A click on a prediction commits it.
            composer.ChoosePrediction(command.index);
            outcome.committed = composer.Commit();
        } else {
            composer.SelectCandidate(command.index);
        }
        break;
    case Action::NextPrediction:
        composer.NextPrediction();
        break;
    case Action::PreviousPrediction:
        composer.PreviousPrediction();
        break;
    case Action::ClearPrediction:
        composer.ClearPredictionChoice();
        break;
    case Action::MoveCaret:
        composer.MoveCaret(command.delta);
        break;
    case Action::Delete:
        composer.Delete();
        break;
    case Action::Commit:
        outcome.committed = composer.IsComposing() ? composer.Commit() : std::wstring{};
        break;
    case Action::CommitToFocus:
        outcome.committed = composer.CommitToFocus();
        break;
    case Action::InsertOutside:
        outcome.outside = std::wstring(1, command.character);
        break;
    case Action::Nothing:
        break;
    }
    return outcome;
}

// Rows the same but for width (ＫＯＵＥＮ and KOUEN, ３千円 and 3千円, ｺｳｴﾝ
// and コウエン): the full-width ASCII one is the wider, the half-width
// katakana one and the ASCII one beside a full-width twin the narrower.
// Katakana and hiragana as usually written take no mark.
void MarkWidths(std::vector<CandidateList::Row>& rows) {
    using Width = CandidateList::Row::Width;
    const auto has = [](const std::wstring& text, wchar_t from, wchar_t to) {
        return std::any_of(text.begin(), text.end(), [&](wchar_t c) { return c >= from && c <= to; });
    };
    std::vector<std::wstring> keys;
    keys.reserve(rows.size());
    for (const auto& row : rows) keys.push_back(ToHalfWidthKatakana(row.text));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        bool twin = false;
        bool fullTwin = false;
        for (std::size_t j = 0; j < rows.size(); ++j) {
            if (j == i || keys[j] != keys[i] || rows[j].text == rows[i].text) continue;
            twin = true;
            fullTwin = fullTwin || has(rows[j].text, 0xFF01, 0xFF5E);
        }
        if (!twin) continue;
        if (has(rows[i].text, 0xFF01, 0xFF5E)) {
            rows[i].width = Width::Full;
        } else if (has(rows[i].text, 0xFF61, 0xFF9F) || (fullTwin && has(rows[i].text, 0x21, 0x7E))) {
            rows[i].width = Width::Half;
        }
    }
}

ListPage ListPageOf(std::size_t index, std::size_t size, std::size_t pageSize) noexcept {
    pageSize = std::max<std::size_t>(pageSize, 1);
    const std::size_t grid = kListColumns * pageSize;
    const std::size_t pages = size <= pageSize ? 1 : 1 + (size - pageSize + grid - 1) / grid;
    if (index < pageSize || size <= pageSize) return {0, std::min(pageSize, size), false, 0, pages};
    const std::size_t page = (index - pageSize) / grid;
    const std::size_t start = pageSize + page * grid;
    return {start, std::min(grid, size - start), true, page + 1, pages};
}

CandidateList CandidateListFor(const JapaneseComposer& composer, std::size_t pageSize) {
    pageSize = std::max<std::size_t>(pageSize, 1);
    CandidateList list;
    const auto& predictions = composer.Predictions();
    if (composer.IsTyping() && !predictions.empty()) {
        list.kind = CandidateList::Kind::Predictions;
        for (std::size_t i = 0; i < predictions.size(); ++i) {
            list.rows.push_back({predictions[i].text, predictions[i].reading, static_cast<std::uint32_t>(i + 1)});
        }
        list.selected = composer.ChosenPrediction();
        list.count = list.rows.size();
        return list;
    }
    const auto* candidates = composer.FocusedCandidates();
    if (!composer.IsCandidateListOpen() || !candidates || candidates->empty()) return list;
    list.kind = CandidateList::Kind::Candidates;
    const std::wstring reading = composer.FocusedReading();
    for (std::size_t i = 0; i < candidates->size(); ++i) {
        const auto& candidate = (*candidates)[i];
        // The first page numbered; the columns after it not, as digits
        // choose from the first page only.
        list.rows.push_back({candidate.text, reading, static_cast<std::uint32_t>(i < pageSize ? i + 1 : 0),
                             candidate.slip || candidate.spellingCorrection || candidate.englishCorrection,
                             i > 0 && (*candidates)[i - 1].chunk && !candidate.chunk, candidate.asTyped});
    }
    MarkWidths(list.rows);
    const std::size_t selected = std::min(composer.FocusedSelection(), candidates->size() - 1);
    list.selected = selected;
    const auto page = ListPageOf(selected, list.rows.size(), pageSize);
    list.pageStart = page.start;
    list.count = page.count;
    list.columnRows = page.columns ? pageSize : 0;
    list.page = page.index;
    list.pages = page.pages;
    return list;
}

}  // namespace tekito::japanese
