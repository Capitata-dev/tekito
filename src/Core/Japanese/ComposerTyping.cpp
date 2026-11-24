#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/ComposerShared.h"

// Typing: keys becoming kana, the caret, deleting, and how what is being
// typed shows.

namespace tekito::japanese {

void JapaneseComposer::Insert(wchar_t key) {
    lastCommit_.reset();
    markLeft_ = false;
    if (letterDelayMs_ || kanjiDelayMs_) lastKeyAt_ = Now();
    mixKanaShown_ = false;
    if (aiJudge_ && !chunkJudge_ && !Typing()) aiJudge_->Warm();
    // Mixed typing: a capital is a letter of the chunk like any other; the
    // chunk keeps it for when it is English.
    if (chunkJudge_) {
        // A capital after kana typed without a space (きょうはZ) starts an
        // English word: the kana before it is a Japanese chunk of its own,
        // as if Space had come between, not letters with it (kyouhaZ).
        // Not after a single kana (iPhone, eBay) nor digits alone (100GB), nor
        // in a word shown as letters or with a capital already.
        const auto kanaTyped = [&] {
            const std::wstring reading = Reading();
            return std::count_if(reading.begin(), reading.end(), [](wchar_t c) { return c >= 0x3041 && c <= 0x30FF; }) >= 2;
        };
        if (key >= L'A' && key <= L'Z' && !Live() && Typing() && !IsConverted() && pending_.empty() &&
            units_.size() >= 2 && caret_ == units_.size() && !TypedLetters() && kanaTyped() &&
            std::none_of(typedCase_.begin(), typedCase_.end(), [](wchar_t c) { return c >= L'A' && c <= L'Z'; })) {
            Convert();
        }
        // A converted chunk joins the sentence, and a new one starts.
        if (IsConverted() && !Live()) SettleChunk();
        if (!Typing()) {
            typedCase_.clear();
            typedCaseKept_ = true;
        }
        if (caret_ != units_.size()) typedCaseKept_ = false;
        // Keys taken back since (Backspace) are no longer typed.
        typedCase_.resize(std::min(typedCase_.size(), Keys(true).size()));
        typedCase_.push_back(key);
        if (key >= L'A' && key <= L'Z') key = static_cast<wchar_t>(key - L'A' + L'a');
    }
    if (chunkJudge_ && !Live()) {
        phrases_.clear();
        listOpen_ = false;
        if (!JoinSoundMark(key) && !FeedAscii(key)) {
            RejoinConsonant(key);
            Feed(key);
        }
        SplitAfterCapitals();
        UpdatePredictions();
        return;
    }
    if (Live()) {
        // Typing on after picking keeps what was picked.
        if (IsConverted() && !preview_) SettleAll();
        kanaView_ = false;
        listOpen_ = false;
        if (!JoinSoundMark(key) && !FeedAscii(key)) {
            RejoinConsonant(key);
            Feed(key);
        }
        // Typed inside the text (the caret moved back): kana until the end.
        kanaView_ = caret_ != units_.size();
        UpdateLive();
        UpdatePredictions();
        return;
    }
    // The caller commits a converted text before typing on; if it did not,
    // typing continues from the typed kana.
    phrases_.clear();
    listOpen_ = false;
    if (!JoinSoundMark(key) && !FeedAscii(key)) {
        RejoinConsonant(key);
        Feed(key);
    }
    UpdatePredictions();
}

void JapaneseComposer::SplitAfterCapitals() {
    if (!chunkJudge_ || !table_ || !Typing() || IsConverted() || caret_ != units_.size() || !typedCaseKept_) return;
    const std::wstring typed = typedCase_;
    const auto upper = [](wchar_t c) { return c >= L'A' && c <= L'Z'; };
    // Typed with a capital in its first letters (NHK, Zoom, iPhone).
    if (typed.size() < 3 || std::none_of(typed.begin(), typed.begin() + 2, upper)) return;
    const auto lowerCase = [](wchar_t c) { return c >= L'a' && c <= L'z'; };
    // Where the English ends: after two capitals or more (NHK, URL, OK), or
    // after a word as it is written (Zoom, GitHub), the longest.
    std::size_t end = 0;
    std::size_t capitals = 0;
    while (capitals < typed.size() && (upper(typed[capitals]) || (capitals > 0 && typed[capitals] >= L'0' && typed[capitals] <= L'9'))) {
        ++capitals;
    }
    const auto countUpper = std::count_if(typed.begin(), typed.begin() + static_cast<std::ptrdiff_t>(capitals), upper);
    std::wstring lower = typed;
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    if (countUpper >= 2 && capitals < typed.size()) {
        end = capitals;
    } else {
        for (std::size_t at = typed.size() - 1; at >= 2 && end == 0; --at) {
            const std::wstring word = typed.substr(0, at);
            if (!std::all_of(word.begin(), word.end(), [&](wchar_t c) { return upper(c) || lowerCase(c); }) ||
                std::none_of(word.begin(), word.end(), upper)) {
                continue;
            }
            if (chunkJudge_->EnglishWritten(lower.substr(0, at)) == word) end = at;
        }
        // Not while the keys may still be an English word (Zoomed).
        if (end > 0 && chunkJudge_->MayStartEnglishWord(lower)) end = 0;
    }
    if (end == 0 || end >= typed.size()) return;
    // What follows reads as kana, a whole syllable at least (no, not n).
    const std::wstring rest = typed.substr(end);
    if (!std::all_of(rest.begin(), rest.end(), lowerCase)) return;
    const auto tokens = ParseRomaji(*table_, rest);
    if (tokens.empty() || std::any_of(tokens.begin(), tokens.end(), [](const RomajiToken& t) { return t.leftover; })) return;
    // The English a chunk of its own, as Space settles it; the rest typed
    // again as the next chunk.
    ClearChunk();
    typedCase_.clear();
    for (const wchar_t c : typed.substr(0, end)) Insert(c);
    Convert();
    if (IsConverted()) SettleChunk();
    for (const wchar_t c : rest) Insert(c);
}

bool JapaneseComposer::JoinSoundMark(wchar_t key) {
    // かな入力: ゛ and ゜ go onto the kana before them (か゛: が, は゜: ぱ).
    if ((key != L'\x309B' && key != L'\x309C') || !pending_.empty() || caret_ == 0) return false;
    auto& before = units_[caret_ - 1];
    if (before.kana.empty()) return false;
    static constexpr std::wstring_view kPlain = L"かきくけこさしすせそたちつてとはひふへほはひふへほうカキクケコサシスセソタチツテトハヒフヘホハヒフヘホウ";
    static constexpr std::wstring_view kMarked = L"がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽゔガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポヴ";
    // ゜ only for the は row, the second time it is listed.
    const std::size_t from = key == L'\x309C' ? 20 : 0;
    const std::size_t to = key == L'\x309C' ? 25 : 20;
    const wchar_t last = before.kana.back();
    for (std::size_t row = 0; row < 2; ++row) {
        const std::size_t offset = row * 26;
        for (std::size_t i = from; i < to; ++i) {
            if (kPlain[offset + i] == last) {
                before.kana.back() = kMarked[offset + i];
                before.keys += key;
                return true;
            }
        }
        // う: ゔ, with ゛.
        if (key == L'\x309B' && kPlain[offset + 25] == last) {
            before.kana.back() = kMarked[offset + 25];
            before.keys += key;
            return true;
        }
    }
    return false;
}

bool JapaneseComposer::KanaTyped() const {
    return std::any_of(units_.begin(), units_.end(), [](const Unit& unit) {
        return std::any_of(unit.keys.begin(), unit.keys.end(), [](wchar_t c) { return c >= 0x3000; });
    });
}

void JapaneseComposer::RejoinConsonant(wchar_t key) {
    // A consonant left on its own, the caret moved back to right after it,
    // takes what is typed there (ktoba, then o after the k: ことば).
    if (!table_ || !pending_.empty() || caret_ == 0) return;
    const Unit& before = units_[caret_ - 1];
    const bool lone = !before.ascii && !before.keys.empty() && before.keys == before.kana &&
                      std::all_of(before.keys.begin(), before.keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; });
    if (lone && (table_->HasLongerInput(before.keys + key) || table_->Find(before.keys + key))) {
        pending_ = before.keys;
        units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(caret_ - 1));
        --caret_;
    }
}

bool JapaneseComposer::FeedAscii(wchar_t key) {
    // As in Google Japanese Input: a capital starts letters as typed, in
    // half-width, until the text is committed. Digits and symbols stay as
    // typed too; a small letter after two or more capitals (NHKno) goes
    // back to kana.
    if (!Typing()) {
        asciiMode_ = false;
        capitals_ = 0;
    }
    const bool capital = key >= L'A' && key <= L'Z';
    if (capital && !asciiMode_) {
        if (table_ && (table_->Find(std::wstring(1, key)) || table_->HasLongerInput(std::wstring(1, key)))) return false;
        FlushAll();
        asciiMode_ = true;
    }
    if (!asciiMode_) return false;
    const bool small = key >= L'a' && key <= L'z';
    if (small && (capitals_ > 1 || LettersWordDone(key))) {
        asciiMode_ = false;
        capitals_ = 0;
        return false;
    }
    if (key < L'!' || key > L'~') {
        asciiMode_ = false;
        capitals_ = 0;
        return false;
    }
    capitals_ = capital ? capitals_ + 1 : 0;
    units_.insert(units_.begin() + static_cast<std::ptrdiff_t>(caret_),
                  Unit{std::wstring(1, key), std::wstring(1, key), true});
    ++caret_;
    TakeCasedWord();
    return true;
}

bool JapaneseComposer::LettersWordDone(wchar_t key) const {
    if (!casedWords_ || !writtenAs_ || !splitter_ || caret_ == 0 || !units_[caret_ - 1].ascii) return false;
    std::size_t first = caret_;
    while (first > 0 && units_[first - 1].ascii) --first;
    std::wstring letters;
    for (std::size_t u = first; u < caret_; ++u) {
        const wchar_t c = units_[u].keys.empty() ? L'\0' : units_[u].keys.front();
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'))) return false;
        letters += c;
    }
    if (letters.size() < 2) return false;
    std::wstring lower = letters;
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    const auto written = writtenAs_(lower);
    const bool word = (written && *written == letters) || splitter_->EnglishWritten(lower) == letters;
    return word && std::any_of(letters.begin(), letters.end(), [](wchar_t c) { return c >= L'A' && c <= L'Z'; }) &&
           !splitter_->MayStartEnglishWord(lower + key);
}

void JapaneseComposer::TakeCasedWord() {
    // A word written with a small letter first and a capital inside
    // (iPhone, macOS, eBay), typed so: once the letters after the capital
    // spell it with the small ones read as kana before it (い, まc), those
    // are letters too, as typed. Only for the word exactly as written.
    if (!casedWords_ || !writtenAs_ || caret_ == 0) return;
    std::size_t first = caret_;
    while (first > 0 && units_[first - 1].ascii) --first;
    const auto letter = [](wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); };
    if (first == 0 || units_[first].keys.empty() || units_[first].keys.front() < L'A' || units_[first].keys.front() > L'Z') return;
    std::wstring letters;
    for (std::size_t u = first; u < caret_; ++u) {
        if (units_[u].keys.size() != 1 || !letter(units_[u].keys.front())) return;
        letters += units_[u].keys;
    }
    const auto small = [](const std::wstring& keys) {
        return !keys.empty() && std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; });
    };
    constexpr std::size_t kMostUnits = 4;
    std::wstring before;
    for (std::size_t k = 1; k <= kMostUnits && k <= first; ++k) {
        const std::size_t from = first - k;
        if (units_[from].ascii || !small(units_[from].keys)) return;
        before.insert(0, units_[from].keys);
        const std::wstring word = before + letters;
        std::wstring lower = word;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        const auto written = writtenAs_(lower);
        if (!written || *written != word) continue;
        // The kana before them as they were, for the live conversion's
        // phrases kept from before.
        std::size_t changedAt = 0;
        for (std::size_t u = 0; u < from; ++u) changedAt += units_[u].kana.size();
        for (std::size_t u = from; u < first; ++u) {
            units_[u].kana = units_[u].keys;
            units_[u].ascii = true;
        }
        while (settledPhrases_ > 0 && phrases_[settledPhrases_ - 1].begin + phrases_[settledPhrases_ - 1].length > changedAt) {
            --settledPhrases_;
        }
        settledLength_ = settledPhrases_ ? phrases_[settledPhrases_ - 1].begin + phrases_[settledPhrases_ - 1].length : 0;
        return;
    }
}

void JapaneseComposer::Feed(wchar_t key) {
    if (!table_) {
        Emit(std::wstring(1, key), std::wstring(1, key));
        return;
    }
    // Each pass either settles the key or shortens the pending keys, so this
    // ends; the bound only guards against a malformed table.
    for (int guard = 0; guard < 64; ++guard) {
        std::wstring keys = pending_ + key;
        if (table_->HasLongerInput(keys)) {
            pending_ = std::move(keys);
            return;
        }
        if (const RomajiRule* rule = table_->Find(keys)) {
            const std::size_t kept = keys.ends_with(rule->pending) ? rule->pending.size() : 0;
            Emit(keys.substr(0, keys.size() - kept), rule->output);
            pending_ = rule->pending;
            return;
        }
        if (pending_.empty()) {
            // Not in the table at all (a capital letter, say): as typed.
            Emit(std::wstring(1, key), std::wstring(1, key));
            return;
        }
        FlushOnce();
    }
    Emit(std::wstring(1, key), std::wstring(1, key));
}

// Settles the pending keys as far as the table allows: the longest leading
// run that is a row becomes its kana, and the rest is typed again.
void JapaneseComposer::FlushOnce() {
    if (pending_.empty()) return;
    const std::wstring pending = std::move(pending_);
    pending_.clear();
    for (std::size_t length = pending.size(); length > 0; --length) {
        const std::wstring_view head(pending.data(), length);
        const RomajiRule* rule = table_ ? table_->Find(head) : nullptr;
        // A rule that leaves keys pending cannot settle anything here.
        if (!rule || !rule->pending.empty()) continue;
        Emit(std::wstring(head), rule->output);
        for (wchar_t rest : std::wstring_view(pending).substr(length)) Feed(rest);
        return;
    }
    Emit(pending.substr(0, 1), pending.substr(0, 1));
    for (wchar_t rest : std::wstring_view(pending).substr(1)) Feed(rest);
}

void JapaneseComposer::FlushAll() {
    for (int guard = 0; guard < 64 && !pending_.empty(); ++guard) FlushOnce();
    if (!pending_.empty()) {
        Emit(pending_, pending_);
        pending_.clear();
    }
}

void JapaneseComposer::Emit(std::wstring keys, std::wstring kana) {
    units_.insert(units_.begin() + static_cast<std::ptrdiff_t>(caret_),
                  {std::move(keys), std::move(kana), false, letterDelayMs_ ? Now() : 0});
    ++caret_;
}

std::uint64_t JapaneseComposer::Now() const {
    if (clock_) return clock_();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::size_t JapaneseComposer::FreshUnits() const {
    // Only while typing at the end: inside the text, kana as they are.
    if (letterDelayMs_ == 0 || caret_ != units_.size()) return 0;
    const std::uint64_t now = Now();
    std::size_t fresh = 0;
    for (auto unit = units_.rbegin(); unit != units_.rend(); ++unit) {
        if (unit->ascii || unit->typedAt == 0 || now - unit->typedAt >= letterDelayMs_ || unit->keys.empty()) break;
        ++fresh;
    }
    return fresh;
}

bool JapaneseComposer::ConversionWaits() const {
    const std::uint32_t wait = std::max(letterDelayMs_, kanjiDelayMs_);
    if (wait == 0 || lastKeyAt_ == 0 || !Typing() || caret_ != units_.size()) return false;
    return Now() - lastKeyAt_ < wait;
}

std::wstring JapaneseComposer::DisplayTyping() const {
    std::wstring text = RenderTyping(true);
    const std::size_t fresh = FreshUnits();
    if (fresh == 0) return text;
    // The last units' kana (before the keys still pending) as their keys.
    std::size_t kana = 0;
    std::wstring keys;
    for (std::size_t i = units_.size() - fresh; i < units_.size(); ++i) {
        kana += units_[i].kana.size();
        keys += units_[i].keys;
    }
    const std::size_t pending = PendingText().size();
    if (kana + pending > text.size()) return text;
    const std::size_t at = text.size() - pending - kana;
    return text.substr(0, at) + keys + text.substr(text.size() - pending);
}

std::optional<std::uint32_t> JapaneseComposer::NextDisplayChange() const {
    const std::uint64_t now = Now();
    std::optional<std::uint64_t> next;
    const auto consider = [&](std::uint64_t at) {
        if (at > now && (!next || at < *next)) next = at;
    };
    if (FreshUnits() > 0) {
        for (std::size_t i = units_.size() - FreshUnits(); i < units_.size(); ++i) consider(units_[i].typedAt + letterDelayMs_);
    }
    if (ConversionWaits()) consider(lastKeyAt_ + std::max(letterDelayMs_, kanjiDelayMs_));
    if (!next) return std::nullopt;
    return static_cast<std::uint32_t>(*next - now);
}

std::size_t JapaneseComposer::ReadingOffset(std::size_t units) const {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < units && i < units_.size(); ++i) offset += units_[i].kana.size();
    return offset;
}

std::size_t JapaneseComposer::SplitAt(std::size_t offset) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < units_.size(); ++i) {
        if (at == offset) return i;
        const std::size_t size = units_[i].kana.size();
        if (offset < at + size) {
            // "きゃ" split into "き" and "ゃ", each with the keys that type it.
            Unit head{{}, units_[i].kana.substr(0, offset - at)};
            Unit tail{{}, units_[i].kana.substr(offset - at)};
            for (Unit* part : {&head, &tail}) {
                auto keys = table_ ? table_->KeysFor(part->kana) : std::wstring{};
                part->keys = keys.empty() ? part->kana : std::move(keys);
            }
            units_[i] = std::move(head);
            units_.insert(units_.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(tail));
            return i + 1;
        }
        at += size;
    }
    return units_.size();
}

void JapaneseComposer::MoveCaret(int delta) {
    if ((IsConverted() && !IsLivePreview()) || !Typing()) return;
    LeaveLivePreview();
    FlushAll();
    predictions_.clear();
    chosenPrediction_.reset();
    const auto total = static_cast<long long>(ReadingOffset(units_.size()));
    const auto target = std::clamp(static_cast<long long>(ReadingOffset(caret_)) + delta, 0LL, total);
    caret_ = SplitAt(static_cast<std::size_t>(target));
    ShowLiveAtEnd();
}

std::size_t JapaneseComposer::CaretOffset() const {
    // After the settled chunks of the sentence (Mix).
    std::size_t settled = 0;
    const auto segments = Segments();
    if (IsConverted()) {
        // At the end, after the space English is shown with (Mix).
        std::size_t end = 0;
        for (const auto& segment : segments) end += segment.text.size();
        return end;
    }
    if (!segments.empty() && segments.front().settled) settled = segments.front().text.size();
    if (const auto address = AddressShown()) return settled + address->size();
    if (const auto live = MixLiveText()) return settled + (ConversionWaits() ? DisplayTyping().size() : live->size());
    if (const auto letters = LettersShown()) return settled + letters->size();
    if (FreshUnits() > 0) return settled + DisplayTyping().size();
    return settled + ReadingOffset(caret_) + pending_.size();
}

void JapaneseComposer::Delete() {
    if ((IsConverted() && !IsLivePreview()) || !Typing()) return;
    LeaveLivePreview();
    FlushAll();
    if (caret_ >= units_.size()) {
        ShowLiveAtEnd();
        return;
    }
    auto& next = units_[caret_];
    next.kana.erase(0, 1);
    if (next.kana.empty()) {
        units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(caret_));
    } else {
        auto keys = table_ ? table_->KeysFor(next.kana) : std::wstring{};
        next.keys = keys.empty() ? next.kana : std::move(keys);
    }
    ShowLiveAtEnd();
}

std::optional<std::wstring> JapaneseComposer::TypedWithCapitals(std::wstring_view keys) const {
    if (!chunkJudge_ || !typedCaseKept_ || typedCase_.size() < keys.size()) return std::nullopt;
    const std::wstring_view typed = std::wstring_view(typedCase_).substr(0, keys.size());
    bool capital = false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const bool upper = typed[i] >= L'A' && typed[i] <= L'Z';
        capital = capital || upper;
        if ((upper ? static_cast<wchar_t>(typed[i] - L'A' + L'a') : typed[i]) != keys[i]) return std::nullopt;
    }
    if (!capital) return std::nullopt;
    return std::wstring(typed);
}

void JapaneseComposer::Backspace() {
    if (chunkJudge_ && !Typing() && !settled_.empty()) {
        const auto chunk = settled_.back();
        settled_.pop_back();
        RecountSentence();
        for (const wchar_t key : chunk.typed) Insert(key);
        return;
    }
    // Mixed typing: what was typed after the caret is kept no longer.
    if (chunkJudge_ && caret_ != units_.size()) typedCaseKept_ = false;
    if (Live() && IsConverted()) {
        // Live: one kana less, and the rest converted again.
        if (!preview_) SettleAll();
        if (!pending_.empty()) {
            pending_.pop_back();
        } else if (caret_ > 0) {
            auto& last = units_[caret_ - 1];
            last.kana.pop_back();
            if (last.kana.empty()) {
                units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(caret_) - 1);
                --caret_;
            } else {
                auto keys = table_ ? table_->KeysFor(last.kana) : std::wstring{};
                last.keys = keys.empty() ? last.kana : std::move(keys);
            }
        }
        UpdateLive();
        UpdatePredictions();
        return;
    }
    if (IsConverted()) {
        Cancel();
        return;
    }
    if (!pending_.empty()) {
        pending_.pop_back();
        UpdatePredictions();
        return;
    }
    if (caret_ == 0) return;
    auto& last = units_[caret_ - 1];
    last.kana.pop_back();
    if (last.kana.empty()) {
        units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(caret_) - 1);
        --caret_;
        UpdatePredictions();
        return;
    }
    // "きゃ" lost its "ゃ": keep what "き" is typed as, for F9 and F10.
    auto keys = table_ ? table_->KeysFor(last.kana) : std::wstring{};
    last.keys = keys.empty() ? last.kana : std::move(keys);
    UpdatePredictions();
}

void JapaneseComposer::Cancel() {
    if (IsConverted()) {
        phrases_.clear();
        listOpen_ = false;
        if (Live()) {
            // Live: the kana, until typing brings the conversion back.
            ResetLive();
            kanaView_ = true;
            UpdatePredictions();
        }
        return;
    }
    ClearChunk();
}

void JapaneseComposer::StepBack() {
    const bool keysShown = keysShown_ && IsConverted() && phrases_.size() == 1 &&
                           phrases_.front().form == KanaForm::HalfWidthAlphanumeric;
    keysShown_ = false;
    // What is shown (Mix's live conversion too), for a stage that would look
    // the same.
    const auto Shown = [this] {
        std::wstring text;
        for (const auto& segment : Segments()) text += segment.text;
        return text;
    };
    if (!escStepsBack_ || keysShown) {
        Cancel();
        if (keysShown) ClearChunk();
        return;
    }
    if (IsConverted()) {
        const auto shown = Shown();
        Cancel();
        if (Shown() != shown) return;
    }
    // Mix live conversion: the kana, until typing brings it back.
    if (!mixKanaShown_ && MixLiveText()) {
        const auto shown = Shown();
        mixKanaShown_ = true;
        if (Shown() != shown) return;
    }
    // The kana: the keys typed next, half-width as typed.
    if (Typing()) {
        const auto shown = Shown();
        Transliterate(KanaForm::HalfWidthAlphanumeric);
        if (Shown() != shown) {
            keysShown_ = true;
            return;
        }
    }
    ClearChunk();
}

std::wstring JapaneseComposer::ReportReading() const {
    std::wstring reading;
    for (const auto& chunk : settled_) {
        std::wstring kana;
        if (!chunk.isEnglish && table_) {
            for (const auto& token : ParseRomaji(*table_, chunk.keys)) kana += token.kana;
        }
        reading += (kana.empty() ? chunk.typed : ToHiragana(kana)) + L" ";
    }
    return reading + ToHiragana(Reading());
}

std::wstring JapaneseComposer::Reading() const {
    // Keys the table did not turn into kana are full-width, as in Microsoft
    // IME ("ｋ" while "ka" is being typed); letters after a capital are not.
    std::wstring kana;
    for (const auto& unit : units_) kana += unit.ascii ? unit.kana : ToFullWidthAscii(unit.kana);
    return ApplyTypingStyle(std::move(kana));
}

std::wstring JapaneseComposer::PendingText() const {
    // After an address being typed, as typed (kanta@c).
    if (preview_ && !phrases_.empty() && phrases_.back().address) return pending_;
    std::wstring text = ToFullWidthAscii(pending_);
    for (auto& ch : text) {
        const bool letter = (ch >= L'\xFF21' && ch <= L'\xFF3A') || (ch >= L'\xFF41' && ch <= L'\xFF5A');
        if (ch >= L'\xFF01' && ch <= L'\xFF5E' && (letter ? halfWidthLetters_ : halfWidthSymbols_)) {
            ch = static_cast<wchar_t>(ch - 0xFEE0);
        }
    }
    return text;
}

std::optional<std::pair<std::wstring, std::wstring>> JapaneseComposer::BracketPair(wchar_t key) const {
    if (!table_) return std::nullopt;
    constexpr std::pair<wchar_t, wchar_t> kPairs[] = {
        {L'\x300C', L'\x300D'}, {L'\x300E', L'\x300F'}, {L'\xFF08', L'\xFF09'}, {L'\x3010', L'\x3011'},
        {L'\xFF3B', L'\xFF3D'}, {L'\xFF5B', L'\xFF5D'}, {L'\x3008', L'\x3009'}, {L'\x300A', L'\x300B'},
        {L'\x3014', L'\x3015'}, {L'(', L')'},           {L'[', L']'},           {L'{', L'}'}};
    std::wstring typed;
    for (const auto& token : ParseRomaji(*table_, std::wstring(1, key))) {
        if (token.leftover) return std::nullopt;
        typed += token.kana;
    }
    typed = ApplyTypingStyle(ToFullWidthAscii(typed));
    if (typed.size() != 1) return std::nullopt;
    for (const auto& [open, close] : kPairs) {
        if (typed.front() == open) return std::pair{typed, std::wstring(1, close)};
    }
    return std::nullopt;
}

std::vector<RomajiToken> JapaneseComposer::ParseRomaji(const RomajiTable& table, std::wstring_view keys) {
    JapaneseComposer typing(&table);
    for (const wchar_t key : keys) typing.Feed(key);
    typing.FlushAll();
    std::vector<RomajiToken> tokens;
    tokens.reserve(typing.units_.size());
    for (auto& unit : typing.units_) {
        const bool leftover = LeftoverLetters(ToFullWidthAscii(unit.kana)) > 0;
        tokens.push_back({unit.keys.size(), std::move(unit.kana), leftover});
    }
    return tokens;
}

std::wstring JapaneseComposer::Keys(bool includePending) const {
    std::wstring keys;
    for (const auto& unit : units_) keys += unit.keys;
    if (includePending) keys += pending_;
    return keys;
}

std::wstring JapaneseComposer::KeysFor(std::size_t begin, std::size_t length) const {
    // After conversion (including a slip undone), phrase offsets index
    // conversionReading_ and readingKeys_ maps them back to the typed keys.
    const std::size_t end = begin + length;
    if (!readingKeys_.empty() && end < readingKeys_.size()) {
        const std::size_t firstKey = readingKeys_[begin];
        const std::size_t endKey = readingKeys_[end];
        if (firstKey != std::wstring::npos && endKey != std::wstring::npos && endKey >= firstKey) {
            const std::wstring typed = Keys(false);
            if (endKey <= typed.size()) return typed.substr(firstKey, endKey - firstKey);
        }
        // Inside a corrected edit there is no clean key span; do not fall
        // back to walking units_ by corrected-reading offsets (wrong keys).
        if (conversionReading_ != Reading()) return {};
    }
    std::wstring keys;
    std::size_t at = 0;
    for (const auto& unit : units_) {
        const std::size_t unitEnd = at + unit.kana.size();
        if (unitEnd > begin && at < begin + length) keys += unit.keys;
        at = unitEnd;
    }
    return keys;
}

std::wstring JapaneseComposer::ApplyTypingStyle(std::wstring text) const {
    const bool comma = punctuation_ == PunctuationStyle::CommaPeriod ||
                       punctuation_ == PunctuationStyle::CommaKuten;
    const bool period = punctuation_ == PunctuationStyle::CommaPeriod ||
                        punctuation_ == PunctuationStyle::ToutenPeriod;
    // With one digit full-width: whether the number at `i` has one digit
    // only (marks between digits join them: 1.5 has two).
    const auto oneDigit = [&](std::size_t i) {
        const auto joins = [&](std::size_t at) {
            return at > 0 && at + 1 < text.size() && IsDigit(text[at - 1]) && IsDigit(text[at + 1]) &&
                   std::wstring_view(L"。．.、，,").find(text[at]) != std::wstring_view::npos;
        };
        const bool before = i > 0 && (IsDigit(text[i - 1]) || joins(i - 1));
        const bool after = i + 1 < text.size() && (IsDigit(text[i + 1]) || joins(i + 1));
        return !before && !after;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto& ch = text[i];
        const bool wide = ch >= L'\xFF01' && ch <= L'\xFF5E';
        const bool digit = ch >= L'\xFF10' && ch <= L'\xFF19';
        const bool letter = (ch >= L'\xFF21' && ch <= L'\xFF3A') || (ch >= L'\xFF41' && ch <= L'\xFF5A');
        const bool halfDigit = halfWidthDigits_ && !(singleDigitFull_ && oneDigit(i));
        const bool half = digit ? halfDigit : letter ? halfWidthLetters_ : halfWidthSymbols_;
        if (wide && half) ch = static_cast<wchar_t>(ch - 0xFEE0);
        if (singleDigitFull_ && ch >= L'0' && ch <= L'9' && oneDigit(i)) ch = static_cast<wchar_t>(ch + 0xFEE0);
        // Between digits: a decimal point or a thousands comma.
        const bool betweenDigits = i > 0 && i + 1 < text.size() && IsDigit(text[i - 1]) && IsDigit(text[i + 1]);
        if (betweenDigits && (ch == L'。' || ch == L'．')) {
            ch = halfWidthDigits_ ? L'.' : L'．';
        } else if (betweenDigits && (ch == L'、' || ch == L'，')) {
            ch = halfWidthDigits_ ? L',' : L'，';
        } else if (comma && ch == L'、') {
            ch = L'，';
        } else if (period && ch == L'。') {
            ch = L'．';
        }
    }
    return text;
}

std::wstring JapaneseComposer::RenderTyping(bool includePending) const {
    std::wstring kana = Reading();
    // Pending keys show at the caret, where the kana they make will go.
    if (includePending) kana.insert(ReadingOffset(caret_), PendingText());
    // Digits joined by marks as a code (090-1234, not 090ー1234).
    return CodeMarks(TypingForm() == KanaForm::Katakana ? ToKatakana(kana) : kana);
}

std::wstring JapaneseComposer::TypedKeys() const {
    std::wstring keys;
    for (const auto& chunk : settled_) keys += chunk.typed + L" ";
    keys += typedCaseKept_ && typedCase_.size() == Keys(true).size() ? typedCase_ : Keys(true);
    while (!keys.empty() && keys.back() == L' ') keys.pop_back();
    return keys;
}

}  // namespace tekito::japanese
