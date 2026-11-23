#pragma once

// The Settings window. Its code by area:
//   SettingsApp.cpp        the window, its WebView, what it is sent and
//                          asked; modes and saving
//   SettingsState.cpp      what the page is sent: the settings and every
//                          list, as JSON
//   SettingsRequests.cpp   the page's requests about settings, modes, apps
//                          left alone, learning, recent conversions,
//                          encryption and Windows
//   SettingsWords.cpp      the page's requests about the user's words, the
//                          dictionaries and romaji
//   SettingsDownloads.cpp  the page's requests about downloads (postal
//                          codes, the Japanese data, the AI models and what
//                          runs them), and each download finishing
//   SettingsFiles.cpp      files the user picks or drops

#include "apps/tekito-settings/SettingsShared.h"
#include "apps/tekito-settings/SettingsPacks.h"
#include "apps/tekito-settings/SettingsWindows.h"
#include "apps/tekito-settings/WebMessage.h"

namespace tekito::settings {

class SettingsApp final {
public:
    bool Initialize(HINSTANCE instance);
    int Run();

private:
    struct Generations {
        std::uint32_t mode{0};
        std::uint32_t settings{0};
        std::uint32_t dictionary{0};
        std::uint32_t learning{0};
        bool operator==(const Generations&) const = default;
    };

    // The model is used as soon as it is installed.
    // The Japanese data this build installs, as setup left it beside
    // TEKITO.exe (japanese-data.json): where it is, its SHA-256, its size and
    // its packs. Not there (setup from before, or a build of one's own):
    // Japanese is added by setup only.
    struct JapaneseDataInfo {
        std::wstring url;
        std::string sha256;
        unsigned long long bytes{0};
        std::vector<std::wstring> packs;
    };

    static std::filesystem::path PostalCodeDirectory() {
        return tekito::ExternalLexiconProvider::PackDirectory(L"japanese-zipcode");
    }
    static std::filesystem::path AiDataRoot() { return tekito::ExternalLexiconProvider::DataPackRoot(); }
    // Messages shown to the user follow UserSettings::uiLanguage.
    bool Japanese() const { return tekito::userdata::UseJapaneseUi(settings_.uiLanguage); }
    const wchar_t* Text(const wchar_t* english, const wchar_t* japanese) const {
        return Japanese() ? japanese : english;
    }
    const wchar_t* WindowTitle() const { return Text(L"TEKITO Settings", L"TEKITO 設定"); }

    // SettingsApp.cpp
    void CreateWebViewStatusControls();
    void SetWebViewStatus(std::wstring_view message, bool showRetry);
    void FailWebView(std::wstring_view reason);
    void ResetWebView();
    bool ValidateSettingsUi(std::wstring& error) const;
    void InitializeWebView();
    void ApplyTheme();
    Generations CurrentGenerations() const;
    void CheckForExternalChanges();
    void PostState();
    void ResizeWebView();
    std::wstring ResponseJson(bool ok, std::wstring_view error = {});
    void Reply(std::wstring_view requestId, bool ok, std::wstring_view error = {},
               std::wstring_view extra = {});
    std::wstring LicenseText() const;
    void HandleWebMessage(std::wstring_view message);
    void HandleRequest(std::wstring_view json);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    std::wstring Diagnostics() const;
    void ReregisterInputMethod();
    bool SaveSettings();
    bool SetCurrentMode(tekito::InputMode mode, std::wstring& error);
    bool SetCurrentJapaneseMode(tekito::InputMode mode, std::wstring& error);

    // SettingsState.cpp
    std::wstring DataPacksJson();
    const wchar_t* CurrentModeName();
    std::wstring DictionaryJson() const;
    std::wstring RomajiJson() const;
    std::wstring LearnedJson() const;
    std::wstring RecentJson() const;
    std::wstring ReportSettings() const;
    static std::wstring QueryEncoded(std::wstring_view text);
    std::wstring JapaneseWordsJson() const;
    static std::wstring StartPageHash();
    std::wstring SettingsStateJson();
    static std::wstring NameListJson(const std::vector<std::wstring>& names);
    std::wstring AiModelsJson() const;
    std::wstring PostalCodesJson() const;
    static std::wstring EncryptionJson();

    // SettingsRequests.cpp
    bool HandleSettingsRequest(const std::wstring& type, const std::wstring& requestId, const WebMessage& message);
    static std::wstring ExecutableName(std::wstring_view input);
    bool AddExcludedApp(std::wstring_view input, std::wstring& error);

    // SettingsWords.cpp
    bool HandleWordsRequest(const std::wstring& type, const std::wstring& requestId, const WebMessage& message);
    static const tekito::japanese::RomajiTable* PackRomaji();
    bool SaveRomaji(const std::vector<tekito::japanese::RomajiTable::Change>& changes);
    static std::optional<std::wstring> HiraganaReading(std::wstring_view text);
    bool SaveJapaneseWordFromMessage(const WebMessage& message, std::wstring& error);
    std::uint32_t PolicyFlagsFromName(std::wstring_view name) const;
    bool SaveDictionaryFromMessage(const WebMessage& message, bool update,
                                   std::wstring& error);
    std::uint64_t NextDictionaryId() const;

    // SettingsDownloads.cpp
    bool HandleDownloadsRequest(const std::wstring& type, const std::wstring& requestId, const WebMessage& message);
    void FinishPostalDownload(std::unique_ptr<PostalDownload> download);
    static std::optional<JapaneseDataInfo> ReadJapaneseDataInfo();
    void FinishJapaneseDataInstall(tekito::userdata::AiModelInstallError error);
    static bool SettingsAiReady();
    void FinishAiModelInstall(std::unique_ptr<AiModelDownload> download);
    void FinishWindowsMlInstall(tekito::userdata::AiModelInstallError error, int device);
    void FinishNpuPrepare(bool ready);
    std::wstring PostalCodeErrorText(tekito::userdata::PostalCodeImportError error) const;

    // SettingsFiles.cpp
    std::filesystem::path ChooseFile(bool save);
    static std::filesystem::path DroppedFile(ICoreWebView2WebMessageReceivedEventArgs* args);
    std::filesystem::path ChooseImeDictionaryFile();
    std::pair<std::filesystem::path, bool> ChooseDictionaryExportFile();
    std::filesystem::path ChooseRecoveryCodeFile();
    bool SaveRecoveryCode(const std::filesystem::path& file, const std::wstring& code);
    std::filesystem::path ChooseRomajiFile();
    std::filesystem::path ChoosePostalCodeFile();
    std::filesystem::path ChooseExecutable();

    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND fallbackLabel_{};
    HWND fallbackRetry_{};
    std::unique_ptr<tekito::userdata::IUserDataRepository> repository_;
    std::unique_ptr<tekito::userdata::RuntimeModeState> runtime_;
    tekito::UserDictionary dictionary_;
    std::vector<tekito::japanese::UserWord> japaneseWords_;
    tekito::UserLearningStore learning_;
    tekito::userdata::UserSettings settings_{};
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    EventRegistrationToken webMessageToken_{};
    // A file dropped on the page comes with its message (postal codes).
    std::filesystem::path droppedFile_;
    bool postalDownloading_{false};
    // The AI model being downloaded (1, 2), 0 for none, and why the last
    // one failed.
    int aiDownloading_{0};
    std::wstring aiError_;
    // The NPU being made ready, and why it (or Windows ML) last could not be.
    bool npuPreparing_{false};
    bool windowsMlDownloading_{false};
    bool japaneseInstalling_{false};
    bool settingsAiDownloading_{false};
    std::wstring japaneseDataError_;
    std::wstring npuError_;
    EventRegistrationToken navigationToken_{};
    std::wstring dataPackSummary_{L"Unavailable"};
    std::wstring japaneseDataSummary_{L"0 / 0"};
    bool japaneseAvailable_{false};
    std::wstring dataPacksJson_;
    ComPtr<ICoreWebView2_3> webview3_;
    Generations seenGenerations_{};
};

}  // namespace tekito::settings
