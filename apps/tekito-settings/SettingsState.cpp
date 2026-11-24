// What the page is sent: the settings and every list, as JSON.

#include "apps/tekito-settings/SettingsApp.h"

namespace tekito::settings {

std::wstring SettingsApp::DataPacksJson() {
    if (!dataPacksJson_.empty()) return dataPacksJson_;
    const auto root = tekito::ExternalLexiconProvider::DataPackRoot();
    std::wstring json = L"[";
    const auto add = [&](const DataPackInfo& pack, const wchar_t* group, bool& valid) {
        tekito::userdata::DataPackStatus status;
        valid = tekito::userdata::ValidateDataPack(tekito::FindDataPack(root, pack.folder), status);
        if (json.size() > 1) json += L',';
        const auto path = status.packPath.empty() ? tekito::FindDataPack(root, pack.folder) : status.packPath;
        const auto name = status.displayName.empty() ? std::wstring(pack.label) : status.displayName;
        json += L"{\"displayName\":\"" + JsonEscape(name) + L"\",\"group\":\"" + group +
                L"\",\"valid\":" + std::wstring(valid ? L"true" : L"false") + L",\"version\":\"" +
                JsonEscape(status.version) + L"\",\"packPath\":\"" + JsonEscape(path.wstring()) +
                L"\",\"reason\":\"" + JsonEscape(status.reason) + L"\",\"source\":\"" +
                JsonEscape(status.source) + L"\",\"license\":\"" + JsonEscape(status.license) +
                L"\",\"noticePath\":\"" + JsonEscape(status.noticePath.wstring()) + L"\"}";
    };
    std::size_t validCount = 0;
    for (const auto& pack : kDataPacks) {
        bool valid = false;
        add(pack, L"english", valid);
        if (valid) ++validCount;
    }
    // Japanese is on only with its required packs; missing optional
    // ones leave it on with less.
    std::size_t japaneseCount = 0;
    japaneseAvailable_ = true;
    for (std::size_t i = 0; i < std::size(kJapaneseDataPacks); ++i) {
        bool valid = false;
        add(kJapaneseDataPacks[i], L"japanese", valid);
        if (valid) ++japaneseCount;
        if (!valid && i < kJapaneseRequiredPacks) japaneseAvailable_ = false;
    }
    dataPackSummary_ = std::to_wstring(validCount) + L" / " + std::to_wstring(std::size(kDataPacks));
    japaneseDataSummary_ =
        std::to_wstring(japaneseCount) + L" / " + std::to_wstring(std::size(kJapaneseDataPacks));
    dataPacksJson_ = json + L']';
    return dataPacksJson_;
}

// The mode Settings shows and sets: the Japanese profile's when Japanese
// is installed, otherwise the English profile's.
const wchar_t* SettingsApp::CurrentModeName() {
    (void)DataPacksJson();
    if (!runtime_) return L"auto";
    if (japaneseAvailable_) {
        const auto mode = runtime_->JapaneseMode();
        if (mode == tekito::InputMode::Mixed) {
            return settings_.mixedTypingEnabled ? L"mixed" : settings_.japaneseEnabled ? L"japanese" : L"auto";
        }
        if (mode == tekito::InputMode::Japanese && !settings_.japaneseEnabled) {
            return settings_.mixedTypingEnabled ? L"mixed" : L"auto";
        }
        return mode == tekito::InputMode::Japanese ? L"japanese"
               : mode == tekito::InputMode::Direct ? L"direct"
                                                   : L"auto";
    }
    return runtime_->Mode() == tekito::InputMode::Direct ? L"direct" : L"auto";
}

std::wstring SettingsApp::DictionaryJson() const {
    std::wstring json = L"[";
    for (const auto& entry : dictionary_.Entries()) {
        if (json.size() > 1) json += L',';
        json += L"{\"id\":" + std::to_wstring(entry.id) + L",\"raw\":\"" +
                JsonEscape(entry.raw) + L"\",\"candidate\":\"" + JsonEscape(entry.candidate) +
                L"\",\"type\":\"Word\",\"policy\":\"" +
                JsonEscape(PolicyName(entry.policyFlags)) + L"\"}";
    }
    return json + L']';
}

// The user's changes to the romaji table, each with the pack's own row.
std::wstring SettingsApp::RomajiJson() const {
    std::wstring json = L"[";
    const auto* pack = PackRomaji();
    for (const auto& change : tekito::japanese::RomajiTable::ReadChanges(tekito::japanese::UserRomajiFile())) {
        const auto* standard = pack ? pack->PackRule(change.input) : nullptr;
        if (json.size() > 1) json += L',';
        json += L"{\"input\":\"" + JsonEscape(change.input) + L"\",\"output\":\"" + JsonEscape(change.output) +
                L"\",\"pending\":\"" + JsonEscape(change.pending) + L"\",\"standard\":\"" +
                JsonEscape(standard ? standard->output : std::wstring()) + L"\"}";
    }
    return json + L']';
}

// What TEKITO learned, each item on its own for Settings to list and
// forget: Japanese conversions (most recent first), English choices and
// words. Read again from the store, as the input method keeps it.
std::wstring SettingsApp::LearnedJson() const {
    std::wstring json = L"[";
    const auto add = [&](std::wstring_view kind, std::wstring_view key, std::wstring_view value,
                         std::wstring_view how, double count) {
        if (json.size() > 1) json += L',';
        json += L"{\"kind\":\"" + JsonEscape(std::wstring(kind)) + L"\",\"key\":\"" + JsonEscape(std::wstring(key)) +
                L"\",\"value\":\"" + JsonEscape(std::wstring(value)) + L"\",\"how\":\"" + std::wstring(how) +
                L"\",\"count\":" + std::to_wstring(static_cast<long long>(count + 0.5)) + L"}";
    };
    if (!repository_) return L"[]";
    tekito::japanese::JapaneseLearningStore japanese;
    if (repository_->LoadJapaneseLearning(japanese)) {
        auto entries = japanese.Entries();
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.lastUsed > b.lastUsed; });
        for (const auto& entry : entries) add(L"ja", entry.reading, entry.surface, L"chosen", entry.selections);
    }
    tekito::UserLearningStore english;
    if (repository_->LoadLearning(english)) {
        std::vector<tekito::UserLearningPreference> preferences(english.Preferences().begin(), english.Preferences().end());
        std::sort(preferences.begin(), preferences.end(), [](const auto& a, const auto& b) { return a.exposure > b.exposure; });
        for (const auto& preference : preferences) {
            const bool undone = english.Undone(preference.rawText, preference.candidate);
            add(L"en", preference.rawText, preference.candidate, undone ? L"undone" : L"chosen", preference.alpha);
        }
        for (const auto& entry : english.Entries()) {
            const auto how = entry.undoCount > entry.candidateSelections ? L"undone"
                             : entry.rawKeeps > entry.candidateSelections ? L"kept"
                                                                          : L"chosen";
            add(L"word", entry.word, {}, how,
                static_cast<double>(entry.candidateSelections + entry.rawKeeps + entry.undoCount));
        }
    }
    return json + L']';
}

// The latest conversions, newest first, for reporting a wrong one.
std::wstring SettingsApp::RecentJson() const {
    std::wstring json = L"[";
    std::vector<tekito::userdata::RecentConversion> recent;
    if (repository_ && repository_->LoadRecentConversions(recent)) {
        for (const auto& item : recent) {
            if (json.size() > 1) json += L',';
            json += L"{\"id\":" + std::to_wstring(item.id) + L",\"reading\":\"" + JsonEscape(item.reading) +
                    L"\",\"text\":\"" + JsonEscape(item.text) + L"\",\"mode\":\"" + JsonEscape(item.mode) +
                    L"\",\"time\":" + std::to_wstring(item.time) + L"}";
        }
    }
    return json + L']';
}

// The settings that change how Japanese converts, one per line, for a
// report of a wrong conversion.
std::wstring SettingsApp::ReportSettings() const {
    const auto onOff = [](bool on) { return on ? L"on" : L"off"; };
    const wchar_t* keyboards[] = {L"detect", L"JIS", L"US"};
    const wchar_t* digits[] = {L"half-width", L"full-width", L"one digit full-width"};
    const wchar_t* numbers[] = {L"as typed", L"half-width", L"full-width", L"one digit full-width", L"kanji",
                                L"official"};
    const wchar_t* kanji[] = {L"all", L"joyo + jinmei", L"joyo", L"kyoiku"};
    const auto& s = settings_;
    std::wstring text;
    text += L"Live conversion: " + std::wstring(onOff(s.japaneseLiveConversion)) + L"\n";
    text += L"Refine on pause: " + std::wstring(onOff(s.japaneseLiveRefine)) + L"\n";
    text += L"Sentence check: " + std::wstring(onOff(s.japaneseSentenceCheck)) + L"\n";
    text += L"Slip auto-apply: " + std::wstring(onOff(s.japaneseSlipAutoApply)) + L"\n";
    text += L"Predictions: " + std::wstring(onOff(s.japanesePredictionEnabled)) + L"\n";
    text += L"Mix: " + std::wstring(onOff(s.mixedTypingEnabled)) + L"\n";
    text += L"Mix live conversion: " + std::wstring(onOff(s.mixLiveConversion)) + L"\n";
    text += L"Mix English correction: " + std::wstring(onOff(s.mixCorrection)) + L"\n";
    text += L"English without spaces: " + std::wstring(onOff(s.spacelessEnglish)) + L"\n";
    text += L"English without spaces, always: " + std::wstring(onOff(s.spacelessEnglishAlways)) + L"\n";
    text += L"Kana input: " + std::wstring(onOff(s.japaneseKanaInput)) + L"\n";
    text += L"Keyboard: " + std::wstring(keyboards[std::clamp(s.keyboardType, 0, 2)]) + L"\n";
    text += L"Digits: " + std::wstring(digits[std::clamp(s.japaneseDigitWidth, 0, 2)]) + L"\n";
    text += L"Converted numbers: " + std::wstring(numbers[std::clamp(s.japaneseNumberStyle, 0, 5)]) + L"\n";
    text += L"Kanji: " + std::wstring(kanji[std::clamp(s.japaneseKanjiRange, 0, 3)]);
    if (s.japaneseKanjiRange == 3) text += L" (grade " + std::to_wstring(s.japaneseKanjiGrade) + L")";
    if (s.japaneseKanjiRange != 0 && s.japaneseKanjiNames) text += L" + jinmei";
    text += L"\n";
    const auto romaji = tekito::japanese::RomajiTable::ReadChanges(tekito::japanese::UserRomajiFile());
    text += L"Romaji table: " + (romaji.empty() ? std::wstring(L"standard") : std::to_wstring(romaji.size()) + L" changes");
    return text;
}

// UTF-8, percent-encoded for a URL's query.
std::wstring SettingsApp::QueryEncoded(std::wstring_view text) {
    std::string utf8;
    if (!text.empty()) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
        utf8.resize(static_cast<std::size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr,
                            nullptr);
    }
    std::wstring out;
    constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    for (const unsigned char c : utf8) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

std::wstring SettingsApp::JapaneseWordsJson() const {
    std::wstring json = L"[";
    for (const auto& word : japaneseWords_) {
        if (json.size() > 1) json += L',';
        json += L"{\"reading\":\"" + JsonEscape(word.reading) + L"\",\"surface\":\"" + JsonEscape(word.surface) +
                L"\",\"kind\":\"" + JsonEscape(tekito::japanese::KindName(word.kind)) + L"\",\"action\":\"" +
                JsonEscape(tekito::japanese::ActionName(word.action)) + L"\"}";
    }
    return json + L']';
}

// "--page <name>" on the command line opens that page (the desktop
// companion's menu). With
// Settings already open, the open window is sent it (kOpenPage).
std::wstring SettingsApp::StartPageHash() {
    static constexpr std::wstring_view kPages[] = {L"general", L"conversion", L"slips", L"notation", L"candidates", L"keys",
                                                    L"dictionary", L"all", L"about", L"japanese", L"mixed", L"auto"};
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::wstring hash;
    for (int i = 1; arguments && i + 1 < count; ++i) {
        if (std::wstring_view(arguments[i]) != L"--page") continue;
        for (const auto page : kPages) {
            if (page == arguments[i + 1]) hash = L"#" + std::wstring(page);
        }
    }
    if (arguments) LocalFree(arguments);
    return hash;
}

std::wstring SettingsApp::SettingsStateJson() {
    const auto mode = CurrentModeName();
    const auto registeredDll = RegisteredTsfDll();
    const auto tsf = !registeredDll.empty() && std::filesystem::exists(registeredDll) ? L"Loaded" : L"Unavailable";
    const auto packs = DataPacksJson();
    std::wstring json = L"{\"version\":\"" + std::wstring(kVersion) + L"\",\"settings\":{\"startMode\":" + std::to_wstring(settings_.startMode);
    json += L",\"switchOrder\":" + std::to_wstring(settings_.switchOrder);
    json += L",\"japaneseKanjiNames\":";
    json += settings_.japaneseKanjiNames ? L"true" : L"false";
    json += L",\"mixSettledMarker\":";
    json += settings_.mixSettledMarker ? L"true" : L"false";
    json += L",\"mixEnglishUnderline\":";
    json += settings_.mixEnglishUnderline ? L"true" : L"false";
    json += L",\"rainbowAccent\":";
    json += settings_.rainbowAccent ? L"true" : L"false";
    json += L",\"catSounds\":";
    json += settings_.catSounds ? L"true" : L"false";
    json += L",\"desktopCompanion\":";
    json += settings_.desktopCompanion ? L"true" : L"false";
    json += L",\"mixCorrection\":";
    json += settings_.mixCorrection ? L"true" : L"false";
    json += L",\"spacelessEnglish\":";
    json += settings_.spacelessEnglish ? L"true" : L"false";
    json += L",\"spacelessEnglishAlways\":";
    json += settings_.spacelessEnglishAlways ? L"true" : L"false";
    json += L",\"correctionEnabled\":";
    json += settings_.correctionEnabled ? L"true" : L"false";
    json += L",\"commonMisspellingsEnabled\":";
    json += settings_.commonMisspellingsEnabled ? L"true" : L"false";
    json += L",\"contextSuggestionsEnabled\":";
    json += settings_.contextSuggestionsEnabled ? L"true" : L"false";
    json += L",\"completionEnabled\":";
    json += settings_.completionEnabled ? L"true" : L"false";
    json += L",\"candidateWindowEnabled\":";
    json += settings_.candidateWindowEnabled ? L"true" : L"false";
    json += L",\"learningEnabled\":";
    json += settings_.learningEnabled ? L"true" : L"false";
    // Secret mode lives in the session's shared state, never saved.
    json += L",\"secretMode\":";
    json += runtime_ && runtime_->Secret() ? L"true" : L"false";
    json += L",\"japanesePhoneticSuggestionsEnabled\":";
    json += settings_.japanesePhoneticSuggestionsEnabled ? L"true" : L"false";
    json += L",\"socialExpressionRange\":" + std::to_wstring(settings_.socialExpressionRange);
    json += L",\"socialPersonalization\":" + std::to_wstring(settings_.socialPersonalization);
    json += L",\"candidateWindowStyle\":" + std::to_wstring(settings_.candidateWindowStyle);
    json += L",\"candidateWindowScale\":" + std::to_wstring(settings_.candidateWindowScale);
    json += L",\"candidateRows\":" + std::to_wstring(settings_.candidateRows);
    json += L",\"japaneseSwitchOrder\":" + std::to_wstring(settings_.japaneseSwitchOrder);
    json += L",\"switchModes\":" + std::to_wstring(settings_.switchModes);
    json += L",\"mixPredictionEnabled\":";
    json += settings_.mixPredictionEnabled ? L"true" : L"false";
    json += L",\"mixFirstPageSymbol\":";
    json += settings_.mixFirstPageSymbol ? L"true" : L"false";
    for (const auto& number : tekito::userdata::kMixStyleNumbers) {
        json += L",\"" + std::wstring(number.name) + L"\":" + std::to_wstring(settings_.*(number.mix));
    }
    for (const auto& on : tekito::userdata::kMixStyleSwitches) {
        json += L",\"" + std::wstring(on.name) + L"\":" + (settings_.*(on.mix) ? L"true" : L"false");
    }
    json += L",\"modeIndicatorEnabled\":";
    json += settings_.modeIndicatorEnabled ? L"true" : L"false";
    json += L",\"modeUnderlineColors\":";
    json += settings_.modeUnderlineColors ? L"true" : L"false";
    json += L",\"modeBadgeOnFocus\":";
    json += settings_.modeBadgeOnFocus ? L"true" : L"false";
    json += L",\"modePerApp\":";
    json += settings_.modePerApp ? L"true" : L"false";
    json += L",\"dateConversion\":";
    json += settings_.dateConversion ? L"true" : L"false";
    json += L",\"numberConversion\":";
    json += settings_.numberConversion ? L"true" : L"false";
    json += L",\"symbolConversion\":";
    json += settings_.symbolConversion ? L"true" : L"false";
    json += L",\"calculatorEnabled\":";
    json += settings_.calculatorEnabled ? L"true" : L"false";
    json += L",\"japaneseEnabled\":";
    json += settings_.japaneseEnabled ? L"true" : L"false";
    json += L",\"meaningsEnabled\":";
    json += settings_.meaningsEnabled ? L"true" : L"false";
    json += L",\"meaningsInUiLanguage\":";
    json += settings_.meaningsInUiLanguage ? L"true" : L"false";
    json += L",\"toggleKey\":" + std::to_wstring(settings_.toggleKey);
    json += L",\"altLeftMode\":" + std::to_wstring(settings_.altLeftMode);
    json += L",\"altRightMode\":" + std::to_wstring(settings_.altRightMode);
    json += L",\"keyboardType\":" + std::to_wstring(settings_.keyboardType);
    json += L",\"periodOnEnter\":";
    json += settings_.periodOnEnter ? L"true" : L"false";
    json += L",\"nextWordPrediction\":";
    json += settings_.nextWordPrediction ? L"true" : L"false";
    json += L",\"doubledWordCheck\":";
    json += settings_.doubledWordCheck ? L"true" : L"false";
    json += L",\"recheckPreviousWord\":";
    json += settings_.recheckPreviousWord ? L"true" : L"false";
    json += L",\"enterCommitsAsShown\":";
    json += settings_.enterCommitsAsShown ? L"true" : L"false";
    json += L",\"escStepsBack\":";
    json += settings_.escStepsBack ? L"true" : L"false";
    json += L",\"showTypedKeys\":";
    json += settings_.showTypedKeys ? L"true" : L"false";
    json += L",\"altSwitches\":";
    json += settings_.altSwitches ? L"true" : L"false";
    json += L",\"convertFromEnd\":";
    json += settings_.convertFromEnd ? L"true" : L"false";
    json += L",\"japaneseLetterDelay\":" + std::to_wstring(settings_.japaneseLetterDelay);
    json += L",\"japaneseKanjiDelay\":" + std::to_wstring(settings_.japaneseKanjiDelay);
    json += L",\"japaneseKanjiMarker\":";
    json += settings_.japaneseKanjiMarker ? L"true" : L"false";
    json += L",\"shiftEnterCommitsToFocus\":";
    json += settings_.shiftEnterCommitsToFocus ? L"true" : L"false";
    json += L",\"commitAtPunctuation\":";
    json += settings_.commitAtPunctuation ? L"true" : L"false";
    json += L",\"capitalizedCorrection\":";
    json += settings_.capitalizedCorrection ? L"true" : L"false";
    json += L",\"doubleSpacePeriod\":";
    json += settings_.doubleSpacePeriod ? L"true" : L"false";
    json += L",\"bracketPairs\":";
    json += settings_.bracketPairs ? L"true" : L"false";
    json += L",\"smartPunctuation\":";
    json += settings_.smartPunctuation ? L"true" : L"false";
    json += L",\"japaneseSpaceWidth\":" + std::to_wstring(settings_.japaneseSpaceWidth);
    json += L",\"japanesePunctuation\":" + std::to_wstring(settings_.japanesePunctuation);
    json += L",\"englishSpelling\":" + std::to_wstring(settings_.englishSpelling);
    json += L",\"japaneseNumberStyle\":" + std::to_wstring(settings_.japaneseNumberStyle);
    json += L",\"japaneseRoughNumbersKanji\":";
    json += settings_.japaneseRoughNumbersKanji ? L"true" : L"false";
    json += L",\"japaneseKanjiRange\":" + std::to_wstring(settings_.japaneseKanjiRange);
    json += L",\"japaneseKanjiGrade\":" + std::to_wstring(settings_.japaneseKanjiGrade);
    json += L",\"japaneseDigitWidth\":" + std::to_wstring(settings_.japaneseDigitWidth);
    json += L",\"japaneseSymbolWidth\":" + std::to_wstring(settings_.japaneseSymbolWidth);
    json += L",\"japaneseLetterWidth\":" + std::to_wstring(settings_.japaneseLetterWidth);
    json += L",\"advancedSettings\":";
    json += settings_.advancedSettings ? L"true" : L"false";
    json += L",\"keepRecentConversions\":";
    json += settings_.keepRecentConversions ? L"true" : L"false";
    json += L",\"japanesePredictionEnabled\":";
    json += settings_.japanesePredictionEnabled ? L"true" : L"false";
    json += L",\"japaneseEnglishSpacing\":";
    json += settings_.japaneseEnglishSpacing ? L"true" : L"false";
    json += L",\"mixedTypingEnabled\":";
    json += settings_.mixedTypingEnabled ? L"true" : L"false";
    json += L",\"japaneseLiveConversion\":";
    json += settings_.japaneseLiveConversion ? L"true" : L"false";
    json += L",\"japaneseSentenceCheck\":";
    json += settings_.japaneseSentenceCheck ? L"true" : L"false";
    json += L",\"japaneseSentenceModel\":" + std::to_wstring(settings_.japaneseSentenceModel);
    json += L",\"japaneseAiDevice\":" + std::to_wstring(settings_.japaneseAiDevice);
    json += L",\"japaneseAiGpu\":" + std::to_wstring(settings_.japaneseAiGpu);
    json += L",\"japaneseSlipAutoApply\":";
    json += settings_.japaneseSlipAutoApply ? L"true" : L"false";
    json += L",\"japaneseFirstPageSymbol\":";
    json += settings_.japaneseFirstPageSymbol ? L"true" : L"false";
    json += L",\"japaneseTypedForms\":";
    json += settings_.japaneseTypedForms ? L"true" : L"false";
    json += L",\"japaneseChunkCandidates\":";
    json += settings_.japaneseChunkCandidates ? L"true" : L"false";
    json += L",\"japaneseKanjiVariants\":";
    json += settings_.japaneseKanjiVariants ? L"true" : L"false";
    json += L",\"japaneseLiveRefine\":";
    json += settings_.japaneseLiveRefine ? L"true" : L"false";
    json += L",\"japaneseKanaInput\":";
    json += settings_.japaneseKanaInput ? L"true" : L"false";
    json += L",\"japaneseKanaInputKey\":";
    json += settings_.japaneseKanaInputKey ? L"true" : L"false";
    json += L",\"mixSingleVowelKana\":";
    json += settings_.mixSingleVowelKana ? L"true" : L"false";
    json += L",\"mixLiveConversion\":";
    json += settings_.mixLiveConversion ? L"true" : L"false";
    json += L",\"noLearningInPrivate\":";
    json += settings_.noLearningInPrivate ? L"true" : L"false";
    json += L",\"japaneseIgnoreCapsLock\":";
    json += settings_.japaneseIgnoreCapsLock ? L"true" : L"false";
    json += L",\"japaneseNumpadDirect\":";
    json += settings_.japaneseNumpadDirect ? L"true" : L"false";
    json += L",\"japaneseCasedWords\":";
    json += settings_.japaneseCasedWords ? L"true" : L"false";
    json += L",\"uiLanguage\":" + std::to_wstring(settings_.uiLanguage);
    json += L",\"excludedApps\":" + NameListJson(settings_.excludedApps);
    json += L",\"builtInExcludedApps\":" +
            NameListJson({std::begin(tekito::userdata::kBuiltInExcludedApps),
                          std::end(tekito::userdata::kBuiltInExcludedApps)});
    json += L"},\"mode\":\"" + std::wstring(mode) + L"\",\"runtime\":{\"tsf\":\"" +
            tsf + L"\",\"dataPacks\":\"" + dataPackSummary_ + L"\",\"japaneseData\":\"" +
            japaneseDataSummary_ + L"\",\"japanese\":" + (japaneseAvailable_ ? L"true" : L"false") +
            L",\"japaneseKeyboard\":" +
            (tekito::userdata::UsesJapaneseKeyboard(settings_.keyboardType) ? L"true" : L"false");
    // Windows' own input settings Settings can change.
    json += std::wstring(L",\"ctrlShiftSwitches\":") + (CtrlShiftSwitches() ? L"true" : L"false") +
            L",\"tekitoDefault\":" + (IsTekito(DefaultKeyboard()) ? L"true" : L"false") +
            L",\"defaultSaved\":" +
            (ReadUserString(kTekitoWindowsKey, L"PreviousDefault") ? L"true" : L"false");
    if (!japaneseAvailable_) {
        // What adding Japanese downloads, when Settings can.
        const auto info = ReadJapaneseDataInfo();
        json += L",\"japaneseDataBytes\":" + std::to_wstring(info ? info->bytes : 0) + L",\"japaneseAddable\":" +
                (info ? L"true" : L"false") + L",\"japaneseInstalling\":" + (japaneseInstalling_ ? L"true" : L"false") +
                L",\"japaneseDataError\":\"" + JsonEscape(japaneseDataError_) + L"\"";
    }
    json += L"},\"learning\":{\"enabled\":";
    json += settings_.learningEnabled ? L"true" : L"false";
    json += L",\"count\":" + std::to_wstring(learning_.Entries().size() + learning_.PreferenceCount()) + L"},\"dictionary\":";
    json += DictionaryJson();
    json += L",\"japaneseWords\":" + JapaneseWordsJson();
    json += L",\"romaji\":" + RomajiJson();
    json += L",\"packs\":" + packs;
    json += L",\"postalCodes\":" + PostalCodesJson();
    json += L",\"aiModels\":" + AiModelsJson();
    json += L",\"encryption\":" + EncryptionJson();
    json += L",\"appearance\":{\"accent\":\"" + AccentColorHex() + L"\",\"systemLanguage\":\"" +
            std::wstring(tekito::userdata::UseJapaneseUi(0) ? L"ja" : L"en") + L"\"}}";
    return json;
}

std::wstring SettingsApp::NameListJson(const std::vector<std::wstring>& names) {
    std::wstring json = L"[";
    for (const auto& name : names) {
        if (json.size() > 1) json += L',';
        json += L'"' + JsonEscape(name) + L'"';
    }
    return json + L']';
}

std::wstring SettingsApp::AiModelsJson() const {
    std::wstring json = L"{";
    const wchar_t* names[] = {L"light", L"strong"};
    for (std::size_t i = 0; i < std::size(kAiModels); ++i) {
        // An older version there: downloading it again replaces it
        // (and what runs it, when it is older).
        const auto root = AiDataRoot();
        const bool installed = tekito::userdata::AiModelPackInstalled(kAiModels[i].pack, root);
        const bool modelOld = installed && AiPackOutdated(kAiModels[i], root);
        const bool runtimeOld = installed && tekito::userdata::AiRuntimeInstalled(root) &&
                                AiPackOutdated(AiRuntimeFor(root), root);
        const auto updateBytes = (modelOld ? kAiModels[i].bytes : 0ull) +
                                 (runtimeOld ? AiRuntimeFor(root).bytes : 0ull);
        json += std::wstring(i ? L"," : L"") + L"\"" + names[i] + L"\":{\"installed\":" +
                (installed ? L"true" : L"false") + L",\"bytes\":" + std::to_wstring(kAiModels[i].bytes) +
                L",\"update\":" + (updateBytes ? L"true" : L"false") + L",\"updateBytes\":" +
                std::to_wstring(updateBytes) + L"}";
    }
    json += std::wstring(L",\"runtime\":{\"installed\":") +
            (tekito::userdata::AiRuntimeInstalled(AiDataRoot()) ? L"true" : L"false") +
            L",\"bytes\":" + std::to_wstring(kAiRuntime.bytes) + L",\"windowsMl\":" +
            (tekito::userdata::AiRuntimeHasWindowsMl(AiDataRoot()) ? L"true" : L"false") +
            L",\"windowsMlBytes\":" + std::to_wstring(kAiRuntimeWindowsMl.bytes) + L",\"windowsMlDownloading\":" +
            (windowsMlDownloading_ ? L"true" : L"false") + L"}";
    json += L",\"downloading\":" + std::to_wstring(aiDownloading_) + L",\"error\":\"" + JsonEscape(aiError_) + L"\"";
    // Where it can run: the GPUs (ids as the setting keeps them) and
    // whether there is an NPU, looked up once.
    static const auto gpus = AiGpus();
    static const bool npu = HasNpu();
    json += L",\"gpus\":[";
    for (std::size_t i = 0; i < gpus.size(); ++i) {
        json += std::wstring(i ? L"," : L"") + L"{\"id\":" + std::to_wstring(static_cast<int>(gpus[i].first)) +
                L",\"name\":\"" + JsonEscape(gpus[i].second) + L"\"}";
    }
    json += std::wstring(L"],\"npu\":") + (npu ? L"true" : L"false") + L",\"npuPreparing\":" +
            (npuPreparing_ ? L"true" : L"false") + L",\"npuError\":\"" + JsonEscape(npuError_) + L"\"}";
    return json;
}

std::wstring SettingsApp::PostalCodesJson() const {
    const auto installed = tekito::userdata::InstalledPostalCodes(PostalCodeDirectory());
    if (!installed) return L"{\"installed\":false}";
    return L"{\"installed\":true,\"version\":\"" + JsonEscape(installed->version) + L"\",\"count\":" +
           std::to_wstring(installed->count) + L"}";
}

// Encryption of the user's database: asked for, and done.
std::wstring SettingsApp::EncryptionJson() {
    const auto path = tekito::userdata::DefaultUserDatabasePath();
    return std::wstring(L"{\"wanted\":") + (tekito::userdata::EncryptionWanted(path) ? L"true" : L"false") +
           L",\"encrypted\":" + (tekito::userdata::DatabaseEncrypted(path) ? L"true" : L"false") + L"}";
}

}  // namespace tekito::settings
