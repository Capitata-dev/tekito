// TEKITO.exe's Settings mode: the command-line tasks (the installer's and
// uninstaller's), then the window (SettingsApp.h).

#include "apps/tekito-settings/SettingsApp.h"

using namespace tekito::settings;

namespace {

// "--export-dictionaries <folder>" (the uninstaller's): the user's words
// saved into the folder, with no window: the Japanese words as Microsoft
// IME's word list, the English words as TEKITO's own file, each only when
// there are some. 0 when every list was written, 2 when one was not.
std::optional<int> ExportDictionariesFromCommandLine() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::filesystem::path folder;
    for (int i = 1; arguments && i + 1 < count; ++i) {
        if (std::wstring_view(arguments[i]) == L"--export-dictionaries") folder = arguments[i + 1];
    }
    if (arguments) LocalFree(arguments);
    if (folder.empty()) return std::nullopt;
    const auto repository = tekito::userdata::CreateDefaultUserDataRepository();
    if (!repository || !repository->Open()) return 2;
    tekito::UserDictionary english;
    std::vector<tekito::japanese::UserWord> japanese;
    if (!repository->Load(english) || !repository->LoadJapaneseUserWords(japanese)) return 2;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    bool written = true;
    if (!japanese.empty()) {
        written = tekito::userdata::WriteImeDictionary(japanese, folder / L"TEKITO Japanese words.txt") && written;
    }
    if (!english.Entries().empty()) {
        written = tekito::userdata::ExportUserDictionary(english, folder / L"TEKITO English words.tsv") && written;
    }
    return written ? 0 : 2;
}

// "--erase-user-data" (the uninstaller's, removing the user's data): the
// words, learning, settings and recent conversions erased from the user's
// database, which apps that still have TEKITO loaded keep open so it cannot
// be deleted until they close. 0 when erased, 2 when not.
std::optional<int> EraseUserDataFromCommandLine() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool erase = false;
    for (int i = 1; arguments && i < count; ++i) erase = erase || std::wstring_view(arguments[i]) == L"--erase-user-data";
    if (arguments) LocalFree(arguments);
    if (!erase) return std::nullopt;
    const auto repository = tekito::userdata::CreateDefaultUserDataRepository();
    if (!repository || !repository->Open()) return 2;
    return repository->EraseAll() ? 0 : 2;
}

// "--rescue <user.db> <user.key> <recovery code> <plain.db>": everything
// readable from an encrypted database, with its recovery code, written to a
// plain one (any PC; the files may come from another). 0 when written, 2
// when the code does not open the key.
std::optional<int> RescueFromCommandLine() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::optional<int> result;
    for (int i = 1; arguments && i + 4 < count; ++i) {
        if (std::wstring_view(arguments[i]) != L"--rescue") continue;
        const std::wstring code = arguments[i + 3];
        result = tekito::userdata::RescueDatabase(arguments[i + 1], arguments[i + 2], std::string(code.begin(), code.end()),
                                                  arguments[i + 4])
                     ? 0
                     : 2;
        break;
    }
    if (arguments) LocalFree(arguments);
    return result;
}

// "--install-ai light|strong" (the installer's, when the user chose AI):
// downloads and installs the model, and what runs it, with no window, and
// turns it on. 0 when it is installed, 3 when it could not be downloaded,
// 4 when it could not be installed.
std::optional<int> InstallAiFromCommandLine() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    int model = 0;
    for (int i = 1; arguments && i + 1 < count; ++i) {
        if (std::wstring_view(arguments[i]) != L"--install-ai") continue;
        const std::wstring_view which = arguments[i + 1];
        model = which == L"light" ? 1 : which == L"strong" ? 2 : -1;
    }
    if (arguments) LocalFree(arguments);
    if (model == 0) return std::nullopt;
    if (model < 0) return 4;
    using tekito::userdata::AiModelInstallError;
    const auto error = DownloadAiModel(model, tekito::ExternalLexiconProvider::DataPackRoot());
    if (error == AiModelInstallError::Download) return 3;
    if (error != AiModelInstallError::None) return 4;
    const auto repository = tekito::userdata::CreateDefaultUserDataRepository();
    tekito::userdata::UserSettings settings;
    if (!repository || !repository->Open() || !repository->LoadSettings(settings)) return 4;
    settings.japaneseSentenceModel = model;
    return repository->SaveSettings(settings) ? 0 : 4;
}

}  // namespace

int RunTekitoSettings(HINSTANCE instance) {
    if (const auto companion = RunCompanionFromCommandLine()) return *companion;
    if (const auto exported = ExportDictionariesFromCommandLine()) return *exported;
    if (const auto erased = EraseUserDataFromCommandLine()) return *erased;
    if (const auto rescued = RescueFromCommandLine()) return *rescued;
    if (const auto installed = InstallAiFromCommandLine()) return *installed;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\TEKITO.Settings.Singleton");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kClassName, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (singleInstance) CloseHandle(singleInstance);
        if (comInitialized) CoUninitialize();
        return 0;
    }
    SettingsApp app;
    if (!app.Initialize(instance)) {
        MessageBoxW(nullptr, L"TEKITO Settings failed to start.", L"TEKITO",
                    MB_OK | MB_ICONERROR);
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
        if (comInitialized) CoUninitialize();
        return 1;
    }
    const int result = app.Run();
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    if (comInitialized) CoUninitialize();
    return result;
}
