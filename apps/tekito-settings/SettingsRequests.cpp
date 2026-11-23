// The page's requests about settings, modes, apps left alone, learning,
// recent conversions, encryption and Windows.

#include "apps/tekito-settings/SettingsApp.h"

namespace tekito::settings {

bool SettingsApp::HandleSettingsRequest(const std::wstring& type, const std::wstring& requestId, const WebMessage& message) {
    if (type == L"settings.get") {
        Reply(requestId, true);
    } else if (type == L"settings.set") {
        const auto key = message.String(L"key");
        const bool value = message.Bool(L"value");
        const int integerValue = static_cast<int>(message.Number(L"value"));
        const auto previous = settings_;
        bool known = true;
        if (key == L"startMode") settings_.startMode = std::clamp(integerValue, 0, 4);
        else if (key == L"switchOrder") {
            if (tekito::userdata::ValidSwitchOrder(integerValue)) settings_.switchOrder = integerValue;
        }
        else if (key == L"japaneseKanjiNames") settings_.japaneseKanjiNames = value;
        else if (key == L"mixCorrection") settings_.mixCorrection = value;
        else if (key == L"spacelessEnglish") settings_.spacelessEnglish = value;
        else if (key == L"spacelessEnglishAlways") settings_.spacelessEnglishAlways = value;
        else if (key == L"mixSettledMarker") settings_.mixSettledMarker = value;
        else if (key == L"mixEnglishUnderline") settings_.mixEnglishUnderline = value;
        else if (key == L"rainbowAccent") settings_.rainbowAccent = value;
        else if (key == L"catSounds") settings_.catSounds = value;
        else if (key == L"desktopCompanion") settings_.desktopCompanion = value;
        else if (key == L"correctionEnabled") settings_.correctionEnabled = value;
        else if (key == L"commonMisspellingsEnabled") settings_.commonMisspellingsEnabled = value;
        else if (key == L"contextSuggestionsEnabled") settings_.contextSuggestionsEnabled = value;
        else if (key == L"completionEnabled") settings_.completionEnabled = value;
        else if (key == L"candidateWindowEnabled") settings_.candidateWindowEnabled = value;
        else if (key == L"learningEnabled") settings_.learningEnabled = value;
        else if (key == L"secretMode") {
            if (runtime_) runtime_->SetSecret(value);
        }
        else if (key == L"japanesePhoneticSuggestionsEnabled") {
            settings_.japanesePhoneticSuggestionsEnabled = value;
        }
        else if (key == L"socialExpressionRange") {
            settings_.socialExpressionRange = std::clamp(integerValue, 0, 4);
        }
        else if (key == L"socialPersonalization") {
            settings_.socialPersonalization = std::clamp(integerValue, 0, 2);
        }
        else if (key == L"candidateWindowStyle") {
            settings_.candidateWindowStyle = std::clamp(integerValue, 0, 1);
        }
        else if (key == L"candidateWindowScale") {
            settings_.candidateWindowScale = std::clamp(integerValue, 50, 200);
        }
        else if (key == L"candidateRows") {
            settings_.candidateRows = tekito::userdata::UserSettings::ValidCandidateRows(integerValue);
        }
        else if (key == L"aiSettingsControl") {
            settings_.aiSettingsControl = std::clamp(integerValue, 0, 2);
        }
        else if (key == L"periodOnEnter") settings_.periodOnEnter = value;
        else if (key == L"nextWordPrediction") settings_.nextWordPrediction = value;
        else if (key == L"doubledWordCheck") settings_.doubledWordCheck = value;
        else if (key == L"recheckPreviousWord") settings_.recheckPreviousWord = value;
        else if (key == L"enterCommitsAsShown") settings_.enterCommitsAsShown = value;
        else if (key == L"escStepsBack") settings_.escStepsBack = value;
        else if (key == L"showTypedKeys") settings_.showTypedKeys = value;
        else if (key == L"altSwitches") settings_.altSwitches = value;
        else if (key == L"convertFromEnd") settings_.convertFromEnd = value;
        else if (key == L"japaneseKanjiMarker") settings_.japaneseKanjiMarker = value;
        else if (key == L"shiftEnterCommitsToFocus") settings_.shiftEnterCommitsToFocus = value;
        else if (key == L"commitAtPunctuation") settings_.commitAtPunctuation = value;
        else if (key == L"capitalizedCorrection") settings_.capitalizedCorrection = value;
        else if (key == L"doubleSpacePeriod") settings_.doubleSpacePeriod = value;
        else if (key == L"smartPunctuation") settings_.smartPunctuation = value;
        else if (key == L"bracketPairs") settings_.bracketPairs = value;
        else if (key == L"meaningsEnabled") settings_.meaningsEnabled = value;
        else if (key == L"meaningsInUiLanguage") settings_.meaningsInUiLanguage = value;
        else if (key == L"japaneseEnabled") settings_.japaneseEnabled = value;
        else if (key == L"modeIndicatorEnabled") settings_.modeIndicatorEnabled = value;
        else if (key == L"modeUnderlineColors") settings_.modeUnderlineColors = value;
        else if (key == L"modeBadgeOnFocus") settings_.modeBadgeOnFocus = value;
        else if (key == L"modePerApp") settings_.modePerApp = value;
        else if (key == L"advancedSettings") settings_.advancedSettings = value;
        else if (key == L"keepRecentConversions") {
            settings_.keepRecentConversions = value;
            // Off: the kept conversions go too (0: all of them).
            if (!value && repository_) (void)repository_->ForgetRecentConversion(0);
        }
        else if (key == L"dateConversion") settings_.dateConversion = value;
        else if (key == L"numberConversion") settings_.numberConversion = value;
        else if (key == L"symbolConversion") settings_.symbolConversion = value;
        else if (key == L"calculatorEnabled") settings_.calculatorEnabled = value;
        else if (key == L"switchModes") {
            settings_.switchModes = std::clamp(integerValue, 0, 15);
        }
        else if (key == L"japaneseSwitchOrder") {
            settings_.japaneseSwitchOrder = std::clamp(integerValue, 0, 2);
        }
        else if (key == L"japaneseSpaceWidth") {
            settings_.japaneseSpaceWidth = std::clamp(integerValue, 0, 2);
        }
        else if (key == L"japanesePunctuation") {
            settings_.japanesePunctuation = std::clamp(integerValue, 0, 3);
        }
        else if (key == L"englishSpelling") {
            settings_.englishSpelling = std::clamp(integerValue, 0, 2);
        }
        else if (key == L"japaneseNumberStyle") {
            settings_.japaneseNumberStyle = std::clamp(integerValue, 1, 5);
        }
        else if (key == L"japaneseKanjiRange") {
            settings_.japaneseKanjiRange = std::clamp(integerValue, 0, 3);
        }
        else if (key == L"japaneseKanjiGrade") {
            settings_.japaneseKanjiGrade = std::clamp(integerValue, 1, 6);
        }
        else if (key == L"japaneseDigitWidth") {
            settings_.japaneseDigitWidth = std::clamp(integerValue, 0, 2);
        }
        else if (key == L"japaneseLetterWidth") {
            settings_.japaneseLetterWidth = std::clamp(integerValue, 0, 1);
        }
        else if (key == L"japaneseSymbolWidth") {
            settings_.japaneseSymbolWidth = std::clamp(integerValue, 0, 1);
        }
        else if (key == L"japanesePredictionEnabled") settings_.japanesePredictionEnabled = value;
        else if (key == L"japaneseLiveConversion") settings_.japaneseLiveConversion = value;
        else if (key == L"japaneseSentenceCheck") settings_.japaneseSentenceCheck = value;
        else if (key == L"japaneseSentenceModel") {
            // Only a model that is there (Settings downloads it first).
            const int model = std::clamp(integerValue, 0, 2);
            if (model && !tekito::userdata::AiModelPackInstalled(kAiModels[model - 1].pack, AiDataRoot())) {
                known = false;
            } else {
                settings_.japaneseSentenceModel = model;
            }
        }
        else if (key == L"japaneseAiDevice") settings_.japaneseAiDevice = std::clamp(integerValue, 0, 3);
        else if (key == L"japaneseAiGpu") settings_.japaneseAiGpu = integerValue;
        else if (key == L"japaneseSlipAutoApply") settings_.japaneseSlipAutoApply = value;
        else if (key == L"japaneseFirstPageSymbol") settings_.japaneseFirstPageSymbol = value;
        else if (key == L"japaneseTypedForms") settings_.japaneseTypedForms = value;
        else if (key == L"japaneseChunkCandidates") settings_.japaneseChunkCandidates = value;
        else if (key == L"japaneseKanjiVariants") settings_.japaneseKanjiVariants = value;
        else if (key == L"japaneseLiveRefine") settings_.japaneseLiveRefine = value;
        else if (key == L"japaneseKanaInput") settings_.japaneseKanaInput = value;
        else if (key == L"japaneseKanaInputKey") settings_.japaneseKanaInputKey = value;
        else if (key == L"mixSingleVowelKana") settings_.mixSingleVowelKana = value;
        else if (key == L"mixLiveConversion") settings_.mixLiveConversion = value;
        else if (key == L"mixPredictionEnabled") settings_.mixPredictionEnabled = value;
        else if (key == L"mixFirstPageSymbol") settings_.mixFirstPageSymbol = value;
        else if (key == L"japaneseRoughNumbersKanji") settings_.japaneseRoughNumbersKanji = value;
        else if (key == L"noLearningInPrivate") settings_.noLearningInPrivate = value;
        else if (key == L"mixedTypingEnabled") settings_.mixedTypingEnabled = value;
        else if (key == L"japaneseEnglishSpacing") settings_.japaneseEnglishSpacing = value;
        else if (key == L"japaneseIgnoreCapsLock") settings_.japaneseIgnoreCapsLock = value;
        else if (key == L"japaneseNumpadDirect") settings_.japaneseNumpadDirect = value;
        else if (key == L"japaneseCasedWords") settings_.japaneseCasedWords = value;
        else if (key == L"uiLanguage") settings_.uiLanguage = std::clamp(integerValue, 0, 2);
        else if (key == L"toggleKey") {
            settings_.toggleKey = std::clamp(integerValue, 0, 3);
        }
        else if (key == L"altLeftMode") {
            settings_.altLeftMode = std::clamp(integerValue, 0, 4);
        }
        else if (key == L"altRightMode") {
            settings_.altRightMode = std::clamp(integerValue, 0, 4);
        }
        else if (key == L"japaneseLetterDelay") {
            settings_.japaneseLetterDelay = std::clamp(integerValue, 0, 2000);
        }
        else if (key == L"japaneseKanjiDelay") {
            settings_.japaneseKanjiDelay = std::clamp(integerValue, 0, 3000);
        }
        else if (key == L"keyboardType") {
            settings_.keyboardType = std::clamp(integerValue, 0, 2);
        }
        else {
            // Mix's ways of writing, apart from Japanese mode's.
            known = false;
            for (const auto& number : tekito::userdata::kMixStyleNumbers) {
                if (key == number.name) {
                    settings_.*(number.mix) = std::clamp(integerValue, number.min, number.max);
                    known = true;
                }
            }
            for (const auto& on : tekito::userdata::kMixStyleSwitches) {
                if (key == on.name) {
                    settings_.*(on.mix) = value;
                    known = true;
                }
            }
        }
        if (!known) {
            settings_ = previous;
            Reply(requestId, false, Text(L"Unknown setting or startup entry could not be updated.", L"この設定は変更できません。"));
        } else if (!SaveSettings()) {
            settings_ = previous;
            Reply(requestId, false, Text(L"Settings could not be saved.", L"設定を保存できませんでした。"));
        } else {
            if (key == L"uiLanguage") SetWindowTextW(hwnd_, WindowTitle());
            // The companion leaves by itself when it is turned off.
            if (key == L"desktopCompanion" && settings_.desktopCompanion != previous.desktopCompanion) {
                SetCompanionAtStartup(settings_.desktopCompanion);
                if (settings_.desktopCompanion) StartCompanion();
            }
            // The English profile's keyboard layout is part of its
            // registration, which needs administrator rights.
            if (key == L"keyboardType" && settings_.keyboardType != previous.keyboardType) {
                ReregisterInputMethod();
            }
            // Turning Japanese on starts typing Japanese; turning it off
            // leaves Japanese for the English mode used last.
            if (key == L"japaneseEnabled" && japaneseAvailable_ &&
                settings_.japaneseEnabled != previous.japaneseEnabled) {
                std::wstring ignored;
                (void)SetCurrentJapaneseMode(settings_.japaneseEnabled ? tekito::InputMode::Japanese
                                                                       : settings_.japaneseProfileEnglishMode,
                                             ignored);
            }
            // Mix turned off while in it: back to Japanese.
            if (key == L"mixedTypingEnabled" && !settings_.mixedTypingEnabled && runtime_ &&
                runtime_->JapaneseMode() == tekito::InputMode::Mixed) {
                std::wstring ignored;
                (void)SetCurrentJapaneseMode(tekito::InputMode::Japanese, ignored);
            }
            Reply(requestId, true);
        }
    } else if (type == L"mode.set") {
        std::wstring error;
        const auto value = message.String(L"value");
        const auto mode = value == L"direct"     ? tekito::InputMode::Direct
                          : value == L"japanese" ? tekito::InputMode::Japanese
                          : value == L"mixed"    ? tekito::InputMode::Mixed
                                                 : tekito::InputMode::Convert;
        (void)DataPacksJson();
        const bool saved = mode == tekito::InputMode::Japanese && !settings_.japaneseEnabled ? false
                           : mode == tekito::InputMode::Mixed && !settings_.mixedTypingEnabled ? false
                           : japaneseAvailable_ ? SetCurrentJapaneseMode(mode, error)
                           : tekito::TypesJapanese(mode) ? false
                                                         : SetCurrentMode(mode, error);
        Reply(requestId, saved, error);
    } else if (type == L"excludedApps.add") {
        std::wstring error;
        const bool added = AddExcludedApp(message.String(L"name"), error);
        Reply(requestId, added, error);
    } else if (type == L"excludedApps.browse") {
        const auto path = ChooseExecutable();
        std::wstring error;
        const bool added = path.empty() || AddExcludedApp(path.wstring(), error);
        Reply(requestId, added, error);
    } else if (type == L"excludedApps.remove") {
        const auto name = message.String(L"name");
        const auto previous = settings_;
        std::erase_if(settings_.excludedApps, [&](const std::wstring& other) {
            return _wcsicmp(other.c_str(), name.c_str()) == 0;
        });
        if (!SaveSettings()) {
            settings_ = previous;
            Reply(requestId, false, Text(L"The app list could not be saved.", L"アプリの一覧を保存できませんでした。"));
        } else {
            Reply(requestId, true);
        }
    } else if (type == L"learning.clear") {
        // Clears everything learned from the user's choices: word
        // preferences and the expressions they tend to pick.
        if (!repository_ || !repository_->ResetLearning() || !repository_->ResetSocialLearning()) {
            Reply(requestId, false, Text(L"Learning data could not be reset.", L"学習データを消去できませんでした。"));
        } else {
            learning_.Reset();
            runtime_->NotifyLearningChanged();
            Reply(requestId, true);
        }
    } else if (type == L"learning.clearSince") {
        // What was learned or changed in the last hour, or today.
        const bool today = message.String(L"range") == L"today";
        std::int64_t since = static_cast<std::int64_t>(std::time(nullptr)) - 3600;
        if (today) {
            std::tm local{};
            const std::time_t now = std::time(nullptr);
            localtime_s(&local, &now);
            local.tm_hour = local.tm_min = local.tm_sec = 0;
            since = static_cast<std::int64_t>(std::mktime(&local));
        }
        if (!repository_ || !repository_->DeleteLearningSince(since)) {
            Reply(requestId, false, Text(L"Learning data could not be deleted.", L"学習データを削除できませんでした。"));
        } else {
            tekito::UserLearningStore reloaded;
            if (repository_->LoadLearning(reloaded)) learning_ = std::move(reloaded);
            runtime_->NotifyLearningChanged();
            Reply(requestId, true);
        }
    } else if (type == L"japaneseLearning.clear") {
        // Only what was learned from Japanese conversion; English
        // learning has its own button.
        if (!repository_ || !repository_->ResetJapaneseLearning()) {
            Reply(requestId, false, Text(L"Learning data could not be reset.", L"学習データを消去できませんでした。"));
        } else {
            runtime_->NotifyLearningChanged();
            Reply(requestId, true);
        }
    } else if (type == L"recent.list") {
        Reply(requestId, true, {}, L",\"items\":" + RecentJson());
    } else if (type == L"recent.forget") {
        const auto id = static_cast<std::int64_t>(message.Number(L"id"));
        if (!repository_ || !repository_->ForgetRecentConversion(id)) {
            Reply(requestId, false, Text(L"It could not be deleted.", L"削除できませんでした。"));
            return true;
        }
        Reply(requestId, true, {}, L",\"items\":" + RecentJson());
    } else if (type == L"recent.report") {
        // The conversion in the issue form, to review and send in the
        // browser; nothing is sent from here.
        const auto id = static_cast<std::int64_t>(message.Number(L"id"));
        std::vector<tekito::userdata::RecentConversion> recent;
        const auto found = repository_ && repository_->LoadRecentConversions(recent)
                               ? std::find_if(recent.begin(), recent.end(), [&](const auto& item) { return item.id == id; })
                               : recent.end();
        if (found == recent.end()) {
            Reply(requestId, false, Text(L"That conversion is gone.", L"その変換はもうありません。"));
            return true;
        }
        std::wstring url = L"https://github.com/Capitata-dev/tekito/issues/new?template=wrong-conversion.yml";
        url += L"&reading=" + QueryEncoded(found->reading) + L"&got=" + QueryEncoded(found->text);
        const std::string_view version = TEKITO_VERSION_STRING;
        url += L"&mode=" + QueryEncoded(found->mode) +
               L"&version=" + QueryEncoded(std::wstring(version.begin(), version.end())) +
               L"&settings=" + QueryEncoded(ReportSettings());
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Reply(requestId, true);
    } else if (type == L"feedback.open") {
        // A bug report (with the version and settings) or a request,
        // to write and send in the browser; the project and its
        // releases. Nothing is sent from here.
        const auto kind = message.String(L"kind");
        const std::string_view version = TEKITO_VERSION_STRING;
        std::wstring url = L"https://github.com/Capitata-dev/tekito";
        if (kind == L"bug") {
            url += L"/issues/new?template=bug-report.yml&version=" +
                   QueryEncoded(std::wstring(version.begin(), version.end())) +
                   L"&settings=" + QueryEncoded(ReportSettings());
        } else if (kind == L"feature") {
            url += L"/issues/new?template=feature-request.yml";
        } else if (kind == L"releases") {
            url += L"/releases";
        }
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The page could not be opened.", L"ページを開けませんでした。"));
    } else if (type == L"learning.list") {
        Reply(requestId, true, {}, L",\"items\":" + LearnedJson());
    } else if (type == L"learning.forget") {
        const auto kind = message.String(L"kind");
        const auto key = message.String(L"key");
        const auto value = message.String(L"value");
        bool saved = false;
        if (kind == L"ja") {
            tekito::japanese::JapaneseLearningStore japanese;
            saved = repository_->LoadJapaneseLearning(japanese) && japanese.Forget(key, value) &&
                    repository_->SaveJapaneseLearning(japanese);
        } else {
            tekito::UserLearningStore english;
            saved = repository_->LoadLearning(english) &&
                    (kind == L"word" ? english.ForgetWord(key) : english.Forget(key, value)) &&
                    repository_->SaveLearning(english);
            if (saved) learning_ = std::move(english);
        }
        if (!saved) {
            Reply(requestId, false, Text(L"It could not be forgotten.", L"消去できませんでした。"));
            return true;
        }
        runtime_->NotifyLearningChanged();
        Reply(requestId, true, {}, L",\"items\":" + LearnedJson());
    } else if (type == L"encryption.enable" || type == L"encryption.renew") {
        // Settings lets go of the database first, so it changes now when
        // no app typing with TEKITO has it open.
        const auto path = tekito::userdata::DefaultUserDatabasePath();
        if (repository_) repository_->Close();
        const auto code = type == L"encryption.enable" ? tekito::userdata::EnableEncryption(path)
                                                       : tekito::userdata::RenewRecoveryCode(path);
        if (repository_) (void)repository_->Open();
        std::wstring extra;
        if (code) extra = L",\"code\":\"" + std::wstring(code->begin(), code->end()) + L"\"";
        Reply(requestId, code.has_value(),
              code ? L"" : Text(L"The recovery code could not be made.", L"復旧コードを作れませんでした。"), extra);
        PostState();
    } else if (type == L"encryption.disable") {
        const auto path = tekito::userdata::DefaultUserDatabasePath();
        if (repository_) repository_->Close();
        const bool done = tekito::userdata::DisableEncryption(path);
        if (repository_) (void)repository_->Open();
        Reply(requestId, done, done ? L"" : Text(L"Encryption could not be turned off.", L"暗号化をオフにできませんでした。"));
        PostState();
    } else if (type == L"encryption.copy") {
        const bool copied = CopyToClipboard(hwnd_, message.String(L"code"));
        Reply(requestId, copied, copied ? L"" : Text(L"The code could not be copied.", L"コードをコピーできませんでした。"));
    } else if (type == L"encryption.save") {
        const auto file = ChooseRecoveryCodeFile();
        if (file.empty()) {
            Reply(requestId, true);
        } else {
            const bool saved = SaveRecoveryCode(file, message.String(L"code"));
            Reply(requestId, saved, saved ? L"" : Text(L"The code could not be saved.", L"コードを保存できませんでした。"));
        }
    } else if (type == L"diagnostics.copy") {
        const auto text = Diagnostics();
        const bool copied = CopyToClipboard(hwnd_, text);
        Reply(requestId, copied, copied ? L"" : Text(L"Diagnostics could not be copied.", L"診断情報をコピーできませんでした。"));
    } else if (type == L"windows.ctrlShift") {
        const bool done = SetCtrlShiftSwitches(message.Bool(L"on"));
        Reply(requestId, done, done ? L"" : Text(L"Windows' setting could not be changed.", L"Windows の設定を変更できませんでした。"));
        PostState();
    } else if (type == L"windows.default.set" || type == L"windows.default.restore") {
        const bool done = type == L"windows.default.set" ? MakeTekitoDefault() : PutBackDefault();
        Reply(requestId, done, done ? L"" : Text(L"Windows' setting could not be changed.", L"Windows の設定を変更できませんでした。"));
        PostState();
    } else if (type == L"uninstall.open") {
        // TEKITO's uninstaller (what Installed apps runs), which offers
        // to export the words first; Settings closes as it starts.
        // Without it (a build of one's own): Installed apps.
        const auto uninstaller = ExecutableDirectory() / L"TekitoUninstall.exe";
        std::error_code missing;
        const bool there = std::filesystem::is_regular_file(uninstaller, missing);
        const auto result = reinterpret_cast<INT_PTR>(there
            ? ShellExecuteW(hwnd_, L"open", uninstaller.c_str(), nullptr, ExecutableDirectory().c_str(), SW_SHOWNORMAL)
            : ShellExecuteW(hwnd_, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL));
        Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The uninstaller could not be opened.", L"アンインストールを開始できませんでした。"));
        if (result > 32 && there) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    } else if (type == L"logs.open") {
        const auto path = LogFolder();
        const auto result = path.empty() ? 0 : reinterpret_cast<INT_PTR>(ShellExecuteW(
            hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The log folder could not be opened.", L"ログのフォルダーを開けませんでした。"));
    } else if (type == L"license.get") {
        const auto text = LicenseText();
        Reply(requestId, !text.empty(), text.empty() ? Text(L"The license text could not be loaded.", L"ライセンスの本文を読み込めませんでした。") : L"",
              L",\"text\":\"" + JsonEscape(text) + L"\"");
    } else if (type == L"website.open") {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            hwnd_, L"open", kPublisherWebsite, nullptr, nullptr, SW_SHOWNORMAL));
        Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The website could not be opened.", L"Web サイトを開けませんでした。"));
    } else if (type == L"notices.open") {
        const auto path = tekito::ExternalLexiconProvider::DataPackRoot();
        const auto result = path.empty() ? 0 : reinterpret_cast<INT_PTR>(ShellExecuteW(
            hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The data folder could not be opened.", L"データのフォルダーを開けませんでした。"));
    } else {
        return false;
    }
    return true;
}

// Accepts "code", "Code.exe" or a full path; stores the file name.
std::wstring SettingsApp::ExecutableName(std::wstring_view input) {
    auto name = std::filesystem::path(std::wstring(input)).filename().wstring();
    while (!name.empty() && std::iswspace(name.back())) name.pop_back();
    while (!name.empty() && std::iswspace(name.front())) name.erase(name.begin());
    if (name.empty()) return {};
    if (std::filesystem::path(name).extension().empty()) name += L".exe";
    return name;
}

bool SettingsApp::AddExcludedApp(std::wstring_view input, std::wstring& error) {
    const auto name = ExecutableName(input);
    if (name.empty()) {
        error = Text(L"An app name is needed, such as code.exe.", L"アプリ名がありません（例: code.exe）。");
        return false;
    }
    const auto sameName = [&](const std::wstring& other) {
        return _wcsicmp(other.c_str(), name.c_str()) == 0;
    };
    const auto& builtIn = tekito::userdata::kBuiltInExcludedApps;
    if (std::any_of(settings_.excludedApps.begin(), settings_.excludedApps.end(), sameName) ||
        std::any_of(std::begin(builtIn), std::end(builtIn),
                    [&](const wchar_t* app) { return sameName(app); })) {
        return true;
    }
    const auto previous = settings_;
    settings_.excludedApps.push_back(name);
    if (!SaveSettings()) {
        settings_ = previous;
        error = Text(L"The app list could not be saved.", L"アプリの一覧を保存できませんでした。");
        return false;
    }
    return true;
}

}  // namespace tekito::settings
