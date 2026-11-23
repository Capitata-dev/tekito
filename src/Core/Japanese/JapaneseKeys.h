#pragma once

#include "Core/Japanese/JapaneseComposer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tekito::japanese {

// What each key does while typing Japanese, apart from the text service:
// the TSF text service and the testbed both turn a key into a KeyCommand
// with TranslateKey, carry it out with ApplyKey, and show CandidateListFor,
// so they behave alike. They only write the result where it goes.

// A key pressed in Japanese mode.
struct KeyPress {
    enum class Key : std::uint8_t {
        Other,      // anything not below: the application's
        Character,  // a key that types `character`
        Space,
        Convert,     // Henkan
        NonConvert,  // Muhenkan
        Enter,
        Backspace,
        Escape,
        Tab,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        PageUp,
        PageDown,
        Delete,
        Insert,
        F6,
        F7,
        F8,
        F9,
        F10,
    };
    Key key{Key::Other};
    // Character: what the key types.
    wchar_t character{0};
    // 1-9 for those digit keys (the top row or the number pad), else 0:
    // with the candidate list open they choose from it.
    int digit{0};
    bool shift{false};
    // Ctrl, Alt or Windows held: a shortcut for the application.
    bool command{false};
    // Ctrl held (without Alt).
    bool control{false};
};

// What a key does to the composition.
struct KeyCommand {
    enum class Action : std::uint8_t {
        Insert,             // types `character`
        Backspace,
        Cancel,             // Esc: back to kana, or drop what was typed
        Convert,            // Space, Henkan: convert, then the next candidate
        CycleKana,          // Muhenkan
        Transliterate,      // F6-F10, to `form`
        NextCandidate,      // `delta` candidates on (one when 0)
        PreviousCandidate,  // `delta` candidates back (one when 0)
        MoveFocus,          // to the phrase `delta` away
        Resize,             // the focused phrase by `delta`
        SelectCandidate,    // `index` in the focused phrase's list
        ChooseRow,          // a click on row `index` of the list shown
        NextPrediction,
        PreviousPrediction,
        ClearPrediction,
        MoveCaret,          // by `delta` kana before conversion
        Delete,
        Commit,             // Enter, or anything that ends the composition
        InsertOutside,      // types `character` with no composition (a full-width space)
        UndoCommit,         // Ctrl+Backspace right after committing: the text comes back
        ForgetChosen,       // Ctrl+Delete: the chosen prediction or candidate is unlearned
        Reconvert,          // Henkan with nothing typed: the selected text, converted again
        ShowCandidates,     // Tab while typing: converted, with the candidate list open
        CommitToFocus,      // Shift+Enter: the phrases up to the focused one
        Nothing,            // a key the list takes with nothing to do (a digit in its columns)
    };
    Action action{Action::Commit};
    wchar_t character{0};
    int delta{0};
    std::size_t index{0};
    KanaForm form{KanaForm::Hiragana};
    // Commit only: the key still goes on to the application afterwards
    // (a shortcut, an arrow that leaves the text).
    bool letKeyThrough{false};
};

struct KeyOptions {
    // Space outside a composition types a full-width space (Shift the other).
    bool fullWidthSpace{true};
    // Rows of the candidate list's first page (1-9 choose from it); the
    // pages after it are kListColumns columns of as many rows.
    std::size_t pageSize{9};
    // Shift+Enter commits up to the focused phrase (CommitToFocus).
    bool shiftEnterToFocus{false};
};

// What `key` does now; nothing when it is the application's.
[[nodiscard]] std::optional<KeyCommand> TranslateKey(const JapaneseComposer& composer, const KeyPress& key,
                                                     const KeyOptions& options);

// What carrying out a command produced besides the composition itself.
struct KeyOutcome {
    // Text committed: it goes before the composition, which ends there.
    std::optional<std::wstring> committed;
    // Text typed with no composition.
    std::wstring outside;
    // Text to take back out of the document, before the composition.
    std::optional<std::wstring> uncommitted;
    // Ctrl+Delete hid a word (JapaneseComposer::HideChosen): its reading
    // and how it is written, for the user's words.
    std::optional<std::pair<std::wstring, std::wstring>> hidden;
};

KeyOutcome ApplyKey(JapaneseComposer& composer, const KeyCommand& command);

// The list to show beside the composition: the predictions while typing,
// or the focused phrase's candidates once its list is open.
struct CandidateList {
    enum class Kind : std::uint8_t { None, Predictions, Candidates };
    struct Row {
        std::wstring text;
        // The reading it was converted from, for its meaning.
        std::wstring reading;
        // The number shown beside it (1-9 on each page).
        std::uint32_t number{0};
        // Read with a slip undone, or a spelling Mozc corrects (Mix: an
        // English word spelled otherwise than typed).
        bool suggestion{false};
        // The first of the phrase's own candidates after its chunk
        // candidates (PhraseCandidate::chunk): a line above it.
        bool groupStart{false};
        // Mix: the English keys as typed, beside a word spelled otherwise.
        bool asTyped{false};
        // Mix: slang offered for an English word; the empty row that takes
        // out a word typed twice.
        bool slang{false};
        bool repeat{false};
        // The same as another row but for its width: the wider or the
        // narrower of the two (None: no such row).
        enum class Width : std::uint8_t { None, Full, Half };
        Width width{Width::None};
    };
    Kind kind{Kind::None};
    std::vector<Row> rows;
    std::optional<std::size_t> selected;
    // The page shown: `count` rows from `pageStart`.
    std::size_t pageStart{0};
    std::size_t count{0};
    // The pages after the first: columns of this many rows, down then
    // across (0 on the first page, a single column).
    std::size_t columnRows{0};
    // The page shown (from 0) and how many there are (0: not paged).
    std::size_t page{0};
    std::size_t pages{0};
};

// The list's pages: the first, `pageSize` in one column; then pages of
// kListColumns columns of `pageSize`, down each column then across.
inline constexpr std::size_t kListColumns = 3;
struct ListPage {
    std::size_t start{0};
    std::size_t count{0};
    bool columns{false};
    // Which page (from 0) of how many.
    std::size_t index{0};
    std::size_t pages{1};
};
[[nodiscard]] ListPage ListPageOf(std::size_t index, std::size_t size, std::size_t pageSize) noexcept;

// Marks the rows the same as another but for width (Row::width).
void MarkWidths(std::vector<CandidateList::Row>& rows);

[[nodiscard]] CandidateList CandidateListFor(const JapaneseComposer& composer, std::size_t pageSize);

}  // namespace tekito::japanese
