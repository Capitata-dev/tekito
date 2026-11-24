// The page's requests about the user's words, the dictionaries and romaji.

#include "apps/tekito-settings/SettingsApp.h"

namespace tekito::settings {

bool SettingsApp::HandleWordsRequest(const std::wstring& type, const std::wstring& requestId, const WebMessage& message) {
    if (type == L"dictionary.create" || type == L"dictionary.update") {
        std::wstring error;
        const bool saved = SaveDictionaryFromMessage(message, type == L"dictionary.update", error);
        Reply(requestId, saved, error);
    } else if (type == L"japaneseWords.save") {
        std::wstring error;
        const bool saved = SaveJapaneseWordFromMessage(message, error);
        Reply(requestId, saved, error);
    } else if (type == L"dictionary.save") {
        // The Japanese words as Microsoft IME's word list (for Microsoft
        // IME, Google Japanese Input or ATOK), or the English words as
        // TEKITO's own file; the save dialog's file type picks which.
        const auto [path, imeList] = ChooseDictionaryExportFile();
        if (path.empty()) {
            Reply(requestId, true);  // the dialog was closed
            return true;
        }
        const bool written = imeList ? tekito::userdata::WriteImeDictionary(japaneseWords_, path)
                                     : tekito::userdata::ExportUserDictionary(dictionary_, path);
        Reply(requestId, written, written ? L"" : Text(L"The dictionary could not be exported.", L"辞書を書き出せませんでした。"),
              written ? L",\"saved\":true" : L"");
    } else if (type == L"japaneseWords.import" || type == L"japaneseWords.browse") {
        const auto source = type == L"japaneseWords.browse" ? ChooseImeDictionaryFile() : droppedFile_;
        if (source.empty()) {
            Reply(requestId, type == L"japaneseWords.browse");  // the dialog was closed
            return true;
        }
        // TEKITO's own file holds English words, and takes their place.
        if (tekito::UserDictionary english; tekito::userdata::ImportUserDictionary(source, english)) {
            if (!repository_->Save(english)) {
                Reply(requestId, false, Text(L"The dictionary could not be imported.", L"辞書を読み込めませんでした。"));
                return true;
            }
            dictionary_ = std::move(english);
            runtime_->NotifyDictionaryChanged();
            Reply(requestId, true, {}, L",\"text\":\"" + JsonEscape(Text(L"English words imported.", L"英語の単語を読み込みました。")) + L"\"");
            return true;
        }
        const auto imported = tekito::userdata::ReadImeDictionary(source);
        if (!imported) {
            Reply(requestId, false, Text(L"No words were found in this file.", L"このファイルから単語を読み込めませんでした。"));
            return true;
        }
        auto words = japaneseWords_;
        const auto added = tekito::userdata::MergeImeWords(words, imported->words);
        if (added > 0 && !repository_->SaveJapaneseUserWords(words)) {
            Reply(requestId, false, Text(L"The words could not be saved.", L"単語を保存できませんでした。"));
            return true;
        }
        japaneseWords_ = std::move(words);
        if (added > 0) runtime_->NotifyDictionaryChanged();
        const auto count = std::to_wstring(added);
        const std::wstring text = Text((L"Added " + count + (added == 1 ? L" word." : L" words.")).c_str(),
                                       (count + L" 語を追加しました。").c_str());
        Reply(requestId, true, {}, L",\"text\":\"" + JsonEscape(text) + L"\"");
    } else if (type == L"japaneseWords.delete") {
        auto words = japaneseWords_;
        const auto reading = message.String(L"reading");
        const auto surface = message.String(L"surface");
        std::erase_if(words, [&](const tekito::japanese::UserWord& word) {
            return word.reading == reading && word.surface == surface;
        });
        if (!repository_->SaveJapaneseUserWords(words)) {
            Reply(requestId, false, Text(L"The word could not be deleted.", L"単語を削除できませんでした。"));
        } else {
            japaneseWords_ = std::move(words);
            runtime_->NotifyDictionaryChanged();
            Reply(requestId, true);
        }
    } else if (type == L"romaji.save") {
        const auto input = message.String(L"input");
        const auto output = message.String(L"output");
        const auto pending = message.String(L"pending");
        const auto typed = [](std::wstring_view keys) {
            return !keys.empty() && std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'!' && c <= L'~'; });
        };
        if (!typed(input) || (!pending.empty() && !typed(pending)) || output.find(L'\t') != std::wstring::npos) {
            Reply(requestId, false, Text(L"Keys are letters, digits and marks a keyboard types.",
                                         L"入力には、キーボードで打てる英数字と記号を使えます。"));
            return true;
        }
        auto changes = tekito::japanese::RomajiTable::ReadChanges(tekito::japanese::UserRomajiFile());
        std::erase_if(changes, [&](const auto& change) { return change.input == input; });
        const auto* standard = PackRomaji() ? PackRomaji()->PackRule(input) : nullptr;
        // The pack's own row again is no change at all.
        if (!standard || standard->output != output || standard->pending != pending) {
            changes.push_back({input, output, pending});
        }
        if (!SaveRomaji(changes)) {
            Reply(requestId, false, Text(L"The romaji table could not be saved.", L"ローマ字表を保存できませんでした。"));
        } else {
            Reply(requestId, true);
        }
    } else if (type == L"romaji.delete" || type == L"romaji.reset") {
        auto changes = tekito::japanese::RomajiTable::ReadChanges(tekito::japanese::UserRomajiFile());
        if (type == L"romaji.reset") {
            changes.clear();
        } else {
            const auto input = message.String(L"input");
            std::erase_if(changes, [&](const auto& change) { return change.input == input; });
        }
        if (!SaveRomaji(changes)) {
            Reply(requestId, false, Text(L"The romaji table could not be saved.", L"ローマ字表を保存できませんでした。"));
        } else {
            Reply(requestId, true);
        }
    } else if (type == L"romaji.import" || type == L"romaji.browse") {
        const auto source = type == L"romaji.browse" ? ChooseRomajiFile() : droppedFile_;
        if (source.empty()) {
            Reply(requestId, type == L"romaji.browse");  // the dialog was closed
            return true;
        }
        const auto rows = tekito::japanese::RomajiTable::ParseTable(source);
        if (rows.empty() || !PackRomaji()) {
            Reply(requestId, false, Text(L"No romaji rows were found in this file.",
                                         L"このファイルからローマ字表を読み込めませんでした。"));
            return true;
        }
        if (!SaveRomaji(PackRomaji()->ChangesFor(rows))) {
            Reply(requestId, false, Text(L"The romaji table could not be saved.", L"ローマ字表を保存できませんでした。"));
            return true;
        }
        const auto count = std::to_wstring(rows.size());
        const std::wstring text = Text((L"Read " + count + L" rows.").c_str(), (count + L" 行を読み込みました。").c_str());
        Reply(requestId, true, {}, L",\"text\":\"" + JsonEscape(text) + L"\"");
    } else if (type == L"dictionary.delete") {
        const auto previous = dictionary_;
        if (!dictionary_.Remove(static_cast<std::uint64_t>(message.Number(L"id"))) || !repository_->Save(dictionary_)) {
            dictionary_ = previous;
            Reply(requestId, false, Text(L"The dictionary entry could not be deleted.", L"辞書の単語を削除できませんでした。"));
        } else {
            runtime_->NotifyDictionaryChanged();
            Reply(requestId, true);
        }
    } else if (type == L"dictionary.import") {
        const auto path = ChooseFile(false);
        tekito::UserDictionary imported;
        if (!path.empty() && tekito::userdata::ImportUserDictionary(path, imported) &&
            repository_->Save(imported)) {
            dictionary_ = std::move(imported);
            runtime_->NotifyDictionaryChanged();
            Reply(requestId, true);
        } else if (path.empty()) {
            Reply(requestId, true);
        } else {
            Reply(requestId, false, Text(L"The dictionary could not be imported.", L"辞書を読み込めませんでした。"));
        }
    } else if (type == L"dictionary.export") {
        const auto path = ChooseFile(true);
        if (path.empty()) {
            Reply(requestId, true);
        } else {
            const bool exported = tekito::userdata::ExportUserDictionary(dictionary_, path);
            Reply(requestId, exported, exported ? L"" : Text(L"The dictionary could not be exported.", L"辞書を書き出せませんでした。"));
        }
    } else {
        return false;
    }
    return true;
}

// The romaji table as its pack has it, for the user's changes over it.
const tekito::japanese::RomajiTable* SettingsApp::PackRomaji() {
    static const auto table =
        tekito::japanese::RomajiTable::Load(tekito::ExternalLexiconProvider::PackDirectory(L"japanese-romaji"));
    return table.get();
}

bool SettingsApp::SaveRomaji(const std::vector<tekito::japanese::RomajiTable::Change>& changes) {
    if (!tekito::japanese::RomajiTable::WriteChanges(tekito::japanese::UserRomajiFile(), changes)) return false;
    runtime_->NotifyDictionaryChanged();
    return true;
}

// A reading in hiragana: katakana becomes hiragana; anything else is
// not a reading.
std::optional<std::wstring> SettingsApp::HiraganaReading(std::wstring_view text) {
    std::wstring reading;
    for (wchar_t c : text) {
        if (c >= 0x30A1 && c <= 0x30F6) c = static_cast<wchar_t>(c - 0x60);
        const bool kana = (c >= 0x3041 && c <= 0x3096) || c == 0x30FC || c == 0x309D || c == 0x309E;
        if (!kana) return std::nullopt;
        reading.push_back(c);
    }
    if (reading.empty()) return std::nullopt;
    return reading;
}

// Adds a word, or replaces the one named by originalReading and
// originalSurface.
bool SettingsApp::SaveJapaneseWordFromMessage(const WebMessage& message, std::wstring& error) {
    const auto reading = HiraganaReading(message.String(L"reading"));
    const auto surface = message.String(L"surface");
    const auto kind = tekito::japanese::KindFromName(message.String(L"kind"));
    const auto action = tekito::japanese::ActionFromName(message.String(L"action"))
                            .value_or(tekito::japanese::UserWordAction::First);
    if (!reading) {
        error = Text(L"The reading is needed in hiragana.", L"読みはひらがなで登録できます。");
        return false;
    }
    if (surface.empty() || !kind) {
        error = Text(L"The word is empty.", L"単語が空欄です。");
        return false;
    }
    const auto originalReading = message.String(L"originalReading");
    const auto originalSurface = message.String(L"originalSurface");
    auto words = japaneseWords_;
    std::erase_if(words, [&](const tekito::japanese::UserWord& word) {
        return (word.reading == originalReading && word.surface == originalSurface) ||
               (word.reading == *reading && word.surface == surface);
    });
    words.push_back({*reading, surface, *kind, action});
    if (!repository_->SaveJapaneseUserWords(words)) {
        error = Text(L"The word could not be saved.", L"単語を保存できませんでした。");
        return false;
    }
    japaneseWords_ = std::move(words);
    runtime_->NotifyDictionaryChanged();
    return true;
}

std::uint32_t SettingsApp::PolicyFlagsFromName(std::wstring_view name) const {
    if (name == L"Protect original") return tekito::CandidatePolicyProtect;
    if (name == L"Correct spelling") return tekito::CandidatePolicyCorrect;
    if (name == L"Normalize") return tekito::CandidatePolicyNormalize;
    if (name == L"Expand abbreviation") return tekito::CandidatePolicyExpand;
    if (name == L"Context only") return tekito::CandidatePolicyContext;
    return tekito::CandidatePolicySuggestOnly;
}

bool SettingsApp::SaveDictionaryFromMessage(const WebMessage& message, bool update,
                               std::wstring& error) {
    const auto raw = message.String(L"raw");
    const auto candidate = message.String(L"candidate");
    if (raw.empty() || candidate.empty()) {
        error = Text(L"Input and output are required.", L"入力と変換後の両方が必要です。");
        return false;
    }
    const auto previous = dictionary_;
    const auto id = update ? static_cast<std::uint64_t>(message.Number(L"id")) : NextDictionaryId();
    if (update && id == 0) {
        error = Text(L"The dictionary entry was not found.", L"辞書の単語が見つかりませんでした。");
        return false;
    }
    if (update) (void)dictionary_.Remove(id);
    if (!dictionary_.Add({id, raw, candidate,
                          PolicyFlagsFromName(message.String(L"policy")),
                          message.Bool(L"caseSensitive"),
                          message.Bool(L"enabled", true)}) ||
        !repository_->Save(dictionary_)) {
        dictionary_ = previous;
        error = Text(L"The dictionary entry could not be saved.", L"辞書の単語を保存できませんでした。");
        return false;
    }
    runtime_->NotifyDictionaryChanged();
    return true;
}

std::uint64_t SettingsApp::NextDictionaryId() const {
    std::uint64_t id = 1;
    for (const auto& entry : dictionary_.Entries()) id = std::max(id, entry.id + 1);
    return id;
}

}  // namespace tekito::settings
