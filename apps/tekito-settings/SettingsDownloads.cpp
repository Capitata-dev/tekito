// The page's requests about downloads: postal codes, the Japanese data,
// the AI models and what runs them; and each download finishing.

#include "apps/tekito-settings/SettingsApp.h"

namespace tekito::settings {

bool SettingsApp::HandleDownloadsRequest(const std::wstring& type, const std::wstring& requestId, const WebMessage& message) {
    if (type == L"postalCodes.open") {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            hwnd_, L"open", kPostalCodePage, nullptr, nullptr, SW_SHOWNORMAL));
        Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The page could not be opened.", L"ページを開けませんでした。"));
    } else if (type == L"postalCodes.import" || type == L"postalCodes.browse") {
        const auto source = type == L"postalCodes.browse" ? ChoosePostalCodeFile() : droppedFile_;
        if (source.empty()) {
            Reply(requestId, type == L"postalCodes.browse");  // the dialog was closed
            return true;
        }
        tekito::userdata::PostalCodeImportError error{};
        const auto imported = tekito::userdata::ImportPostalCodes(source, PostalCodeDirectory(), error);
        if (imported) runtime_->NotifyDictionaryChanged();
        Reply(requestId, imported.has_value(), imported ? L"" : PostalCodeErrorText(error));
    } else if (type == L"postalCodes.download") {
        if (postalDownloading_) {
            Reply(requestId, false);
            return true;
        }
        postalDownloading_ = true;
        wchar_t folder[MAX_PATH]{};
        GetTempPathW(MAX_PATH, folder);
        auto* download = new PostalDownload{
            requestId, std::filesystem::path(folder) / (L"tekito-utf_ken_all-" + std::to_wstring(GetCurrentProcessId()) + L".zip")};
        std::thread([hwnd = hwnd_, download] {
            for (const auto* url : kPostalCodeZips) {
                download->ok = SUCCEEDED(URLDownloadToFileW(nullptr, url, download->file.c_str(), 0, nullptr)) &&
                               PostalZipChecked(url, download->file);
                if (download->ok) break;
            }
            if (!PostMessageW(hwnd, kPostalDownloaded, 0, reinterpret_cast<LPARAM>(download))) delete download;
        }).detach();
    } else if (type == L"aiModel.download") {
        const int model = static_cast<int>(message.Number(L"model"));
        if (aiDownloading_ || model < 1 || model > 2) {
            Reply(requestId, false);
            return true;
        }
        aiDownloading_ = model;
        aiError_.clear();
        auto* download = new AiModelDownload{model};
        std::thread([hwnd = hwnd_, download, root = AiDataRoot()] {
            download->error = DownloadAiModel(download->model, root);
            if (!PostMessageW(hwnd, kAiModelInstalled, 0, reinterpret_cast<LPARAM>(download))) delete download;
        }).detach();
        Reply(requestId, true);
        PostState();
    } else if (type == L"japaneseData.install") {
        // The user agreed to download the Japanese data (Settings' dialog).
        const auto info = ReadJapaneseDataInfo();
        if (japaneseInstalling_ || japaneseAvailable_ || !info) {
            Reply(requestId, false, Text(L"Japanese can be added by running setup again.",
                                         L"日本語はセットアップからも追加できます。もう一度セットアップを実行してください。"));
            return true;
        }
        japaneseInstalling_ = true;
        japaneseDataError_.clear();
        std::thread([hwnd = hwnd_, info = *info, root = AiDataRoot()] {
            wchar_t folder[MAX_PATH]{};
            GetTempPathW(MAX_PATH, folder);
            const auto file = std::filesystem::path(folder) /
                              (L"tekito-japanese-data-" + std::to_wstring(GetCurrentProcessId()) + L".zip");
            const auto error = FAILED(URLDownloadToFileW(nullptr, info.url.c_str(), file.c_str(), 0, nullptr))
                                   ? tekito::userdata::AiModelInstallError::Download
                                   : tekito::userdata::InstallDataPacks(file, info.sha256, info.packs, root);
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
            PostMessageW(hwnd, kJapaneseDataInstalled, static_cast<WPARAM>(error), 0);
        }).detach();
        Reply(requestId, true);
        PostState();
    } else if (type == L"ai.score") {
        // Home's AI: the texts (one a line) scored after the user's words.
        const auto texts = message.String(L"texts");
        std::vector<std::wstring> lines;
        for (std::size_t start = 0; start <= texts.size();) {
            const auto end = std::min(texts.find(L'\n', start), texts.size());
            if (end > start) lines.push_back(texts.substr(start, end - start));
            start = end + 1;
        }
        if (lines.empty() || lines.size() > 256 || !SettingsAiReady()) {
            Reply(requestId, false, Text(L"AI is not ready.", L"AI を使えません。"));
            return true;
        }
        auto* ask = new ScoredAsk{std::wstring(requestId)};
        // Where the user's AI runs; the GPU or the NPU only with Windows ML.
        const auto root = AiDataRoot();
        const int chosen = std::clamp(settings_.japaneseAiDevice, 0, 3);
        const auto device = chosen >= 2 && tekito::userdata::AiRuntimeHasWindowsMl(root)
                                ? static_cast<tekito::japanese::AiDevice>(chosen)
                                : tekito::japanese::AiDevice::Cpu;
        std::thread([hwnd = hwnd_, ask, prompt = message.String(L"prompt"), base = message.String(L"base"), lines, device,
                     gpu = static_cast<std::uint32_t>(settings_.japaneseAiGpu)] {
            if (auto scores = ScoreTexts(prompt, base, lines, device, gpu)) {
                ask->scores = std::move(*scores);
                ask->ok = true;
            }
            if (!PostMessageW(hwnd, kAiScored, 0, reinterpret_cast<LPARAM>(ask))) delete ask;
        }).detach();
    } else if (type == L"settingsAi.download") {
        // The user agreed to download Settings' AI model by itself.
        if (aiDownloading_ || settingsAiDownloading_) {
            Reply(requestId, false);
            return true;
        }
        settingsAiDownloading_ = true;
        aiError_.clear();
        std::thread([hwnd = hwnd_, root = AiDataRoot()] {
            auto* download = new AiModelDownload{0};
            download->error = UpdateAiRuntime(root);
            if (download->error == tekito::userdata::AiModelInstallError::None &&
                !tekito::userdata::AiModelPackInstalled(kSettingsAi.pack, root)) {
                download->error = DownloadAiPack(kSettingsAi, root);
            }
            if (!PostMessageW(hwnd, kAiModelInstalled, 0, reinterpret_cast<LPARAM>(download))) delete download;
        }).detach();
        Reply(requestId, true);
        PostState();
    } else if (type == L"aiRuntime.windowsMl") {
        // The user agreed to Microsoft's terms for the GPU or the NPU.
        const int device = static_cast<int>(message.Number(L"device"));
        if (windowsMlDownloading_ || aiDownloading_ || (device != 2 && device != 3)) {
            Reply(requestId, false);
            return true;
        }
        windowsMlDownloading_ = true;
        npuError_.clear();
        std::thread([hwnd = hwnd_, device, root = AiDataRoot()] {
            const auto error = DownloadAiRuntimeWindowsMl(root);
            PostMessageW(hwnd, kAiWindowsMlInstalled, static_cast<WPARAM>(error), device);
        }).detach();
        Reply(requestId, true);
        PostState();
    } else if (type == L"windowsMl.terms") {
        const auto file = ExecutableDirectory() / L"settings-ui" / L"notices" / L"windows-ml-LICENSE.txt";
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The terms could not be opened.", L"条項を開けませんでした。"));
    } else if (type == L"aiNpu.prepare") {
        if (npuPreparing_) {
            Reply(requestId, false);
            return true;
        }
        npuPreparing_ = true;
        npuError_.clear();
        std::thread([hwnd = hwnd_] { PostMessageW(hwnd, kAiNpuPrepared, PrepareNpu() ? 1 : 0, 0); }).detach();
        Reply(requestId, true);
        PostState();
    } else if (type == L"aiModel.remove") {
        const int model = static_cast<int>(message.Number(L"model"));
        if (aiDownloading_ || model < 1 || model > 2) {
            Reply(requestId, false);
            return true;
        }
        if (settings_.japaneseSentenceModel == model) {
            settings_.japaneseSentenceModel = 0;
            (void)SaveSettings();
        }
        bool removed = tekito::userdata::RemoveAiModelPack(kAiModels[model - 1].pack, AiDataRoot());
        // With the last model, what runs it goes too.
        if (removed && std::none_of(std::begin(kAiModels), std::end(kAiModels), [](const AiModelInfo& info) {
                return tekito::userdata::AiModelPackInstalled(info.pack, AiDataRoot());
            })) {
            removed = tekito::userdata::RemoveAiModelPack(kAiRuntime.pack, AiDataRoot());
        }
        Reply(requestId, removed,
              removed ? L"" : Text(L"The AI model could not be removed. Close the apps you type in and try again.",
                                   L"AI モデルを削除できませんでした。入力中のアプリを閉じてから、もう一度お試しください。"));
        PostState();
    } else if (type == L"postalCodes.remove") {
        const bool removed = tekito::userdata::RemovePostalCodes(PostalCodeDirectory());
        if (removed) runtime_->NotifyDictionaryChanged();
        Reply(requestId, removed, removed ? L"" : Text(L"The postal codes could not be removed.", L"郵便番号を削除できませんでした。"));
    } else {
        return false;
    }
    return true;
}

// The downloaded file is imported, then deleted.
void SettingsApp::FinishPostalDownload(std::unique_ptr<PostalDownload> download) {
    postalDownloading_ = false;
    if (!download->ok) {
        Reply(download->requestId, false,
              Text(L"The postal codes could not be downloaded. Check the connection and try again.",
                   L"郵便番号をダウンロードできませんでした。接続を確認して、もう一度お試しください。"));
        return;
    }
    tekito::userdata::PostalCodeImportError error{};
    const auto imported = tekito::userdata::ImportPostalCodes(download->file, PostalCodeDirectory(), error);
    std::error_code ignored;
    std::filesystem::remove(download->file, ignored);
    if (imported) runtime_->NotifyDictionaryChanged();
    Reply(download->requestId, imported.has_value(), imported ? L"" : PostalCodeErrorText(error));
}

std::optional<SettingsApp::JapaneseDataInfo> SettingsApp::ReadJapaneseDataInfo() {
    std::ifstream in(ExecutableDirectory() / L"japanese-data.json", std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.rfind("\xEF\xBB\xBF", 0) == 0) bytes.erase(0, 3);
    const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
    const auto json = WebMessage::Parse(text);
    JapaneseDataInfo info;
    info.url = json.String(L"url");
    const auto sha = json.String(L"sha256");
    for (const wchar_t c : sha) info.sha256 += static_cast<char>(std::towupper(c));
    info.bytes = static_cast<unsigned long long>(std::max<std::int64_t>(json.Number(L"size"), 0));
    const auto packs = json.String(L"packs");
    for (std::size_t start = 0; start <= packs.size();) {
        const auto end = std::min(packs.find(L',', start), packs.size());
        if (end > start) info.packs.push_back(packs.substr(start, end - start));
        start = end + 1;
    }
    // Only TEKITO's releases, over HTTPS. The repository moved from the
    // Canta360 account to the Capitata-dev organization; a file written
    // before the move names the old address, which GitHub forwards.
    const bool fromRelease = info.url.starts_with(L"https://github.com/Capitata-dev/tekito/releases/download/") ||
                             info.url.starts_with(L"https://github.com/Canta360/tekito/releases/download/");
    if (!fromRelease || info.sha256.size() != 64 || info.packs.empty()) {
        return std::nullopt;
    }
    return info;
}

// The Japanese data in place: TEKITO's Japanese keyboard added (its
// English one taken out, as setup does: the Japanese one types English
// too), and Japanese turned on.
void SettingsApp::FinishJapaneseDataInstall(tekito::userdata::AiModelInstallError error) {
    japaneseInstalling_ = false;
    using tekito::userdata::AiModelInstallError;
    dataPacksJson_.clear();
    (void)DataPacksJson();
    if (error == AiModelInstallError::None && japaneseAvailable_) {
        japaneseDataError_.clear();
        if (ChangeKeyboard(kJapaneseTip, false)) (void)ChangeKeyboard(kEnglishTip, true);
        settings_.japaneseEnabled = true;
        (void)SaveSettings();
    } else {
        japaneseDataError_ = error == AiModelInstallError::Download
                                 ? Text(L"The Japanese data could not be downloaded. Check the connection and try again.",
                                        L"日本語のデータをダウンロードできませんでした。接続を確認して、もう一度お試しください。")
                             : error == AiModelInstallError::InUse
                                 ? Text(L"The Japanese data could not be put in place. Close the apps you type in and try again.",
                                        L"日本語のデータを配置できませんでした。入力中のアプリを閉じてから、もう一度お試しください。")
                                 : Text(L"The downloaded Japanese data was damaged. Try again.",
                                        L"ダウンロードした日本語のデータが壊れていました。もう一度お試しください。");
    }
    PostState();
}

// Settings' AI model and what runs it are there.
bool SettingsApp::SettingsAiReady() {
    const auto root = AiDataRoot();
    return tekito::userdata::AiModelPackInstalled(kSettingsAi.pack, root) && tekito::userdata::AiRuntimeRunsSettings(root);
}

void SettingsApp::FinishAiModelInstall(std::unique_ptr<AiModelDownload> download) {
    aiDownloading_ = 0;
    settingsAiDownloading_ = false;
    using tekito::userdata::AiModelInstallError;
    switch (download->error) {
    case AiModelInstallError::None:
        // 0: Settings' own model alone.
        if (download->model > 0) {
            settings_.japaneseSentenceModel = download->model;
            (void)SaveSettings();
        }
        break;
    case AiModelInstallError::Download:
        aiError_ = Text(L"The AI model could not be downloaded. Check the connection and try again.",
                        L"AI モデルをダウンロードできませんでした。接続を確認して、もう一度お試しください。");
        break;
    case AiModelInstallError::InUse:
        aiError_ = Text(L"The AI model could not be replaced. Close the apps you type in and try again.",
                        L"AI モデルを置き換えられませんでした。入力中のアプリを閉じてから、もう一度お試しください。");
        break;
    default:
        aiError_ = Text(L"The downloaded AI model was damaged. Try again.",
                        L"ダウンロードした AI モデルが壊れていました。もう一度お試しください。");
        break;
    }
    PostState();
}

// Windows ML there: the AI model runs where the user chose (the NPU made
// ready first).
void SettingsApp::FinishWindowsMlInstall(tekito::userdata::AiModelInstallError error, int device) {
    windowsMlDownloading_ = false;
    if (error == tekito::userdata::AiModelInstallError::None) {
        settings_.japaneseAiDevice = device;
        (void)SaveSettings();
        if (device == 3 && !npuPreparing_) {
            npuPreparing_ = true;
            std::thread([hwnd = hwnd_] { PostMessageW(hwnd, kAiNpuPrepared, PrepareNpu() ? 1 : 0, 0); }).detach();
        }
    } else {
        npuError_ = error == tekito::userdata::AiModelInstallError::Download
                        ? Text(L"It could not be downloaded. Check the connection and try again.",
                               L"ダウンロードできませんでした。接続を確認して、もう一度お試しください。")
                        : Text(L"It could not be installed. Close the apps you type in and try again.",
                               L"インストールできませんでした。入力中のアプリを閉じてから、もう一度お試しください。");
    }
    PostState();
}

// The NPU ready, or not: the AI model goes back to running where it
// chooses.
void SettingsApp::FinishNpuPrepare(bool ready) {
    npuPreparing_ = false;
    if (!ready) {
        npuError_ = Text(L"The NPU could not be prepared. Check the connection and try again.",
                         L"NPU を準備できませんでした。接続を確認して、もう一度お試しください。");
        if (settings_.japaneseAiDevice == 3) {
            settings_.japaneseAiDevice = 0;
            (void)SaveSettings();
        }
    }
    PostState();
}

std::wstring SettingsApp::PostalCodeErrorText(tekito::userdata::PostalCodeImportError error) const {
    using tekito::userdata::PostalCodeImportError;
    switch (error) {
    case PostalCodeImportError::ShiftJis:
        return Text(L"This is the Shift_JIS version. Download the UTF-8 version instead.",
                    L"Shift_JIS 形式のファイルです。UTF-8 形式をダウンロードしてください。");
    case PostalCodeImportError::NotPostalCodes:
        return Text(L"This is not Japan Post's postal code data.", L"日本郵便の郵便番号データではないようです。");
    case PostalCodeImportError::WriteFailed:
        return Text(L"The postal codes could not be saved.", L"郵便番号を保存できませんでした。");
    default:
        return Text(L"The file could not be read.", L"ファイルを読み込めませんでした。");
    }
}

}  // namespace tekito::settings
