// Files the user picks or drops.

#include "apps/tekito-settings/SettingsApp.h"

namespace tekito::settings {

std::filesystem::path SettingsApp::ChooseFile(bool save) {
    wchar_t buffer[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"TEKITO User Dictionary (*.tsv)\0*.tsv\0All files\0*.*\0";
    dialog.Flags = OFN_PATHMUSTEXIST |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))
               ? std::filesystem::path(buffer)
               : std::filesystem::path{};
}

// The file dropped on the page, sent with the message.
std::filesystem::path SettingsApp::DroppedFile(ICoreWebView2WebMessageReceivedEventArgs* args) {
    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> withObjects;
    ComPtr<ICoreWebView2ObjectCollectionView> objects;
    UINT32 count = 0;
    if (!args || FAILED(args->QueryInterface(IID_PPV_ARGS(&withObjects))) ||
        FAILED(withObjects->get_AdditionalObjects(&objects)) || !objects ||
        FAILED(objects->get_Count(&count)) || count == 0) {
        return {};
    }
    ComPtr<IUnknown> first;
    ComPtr<ICoreWebView2File> file;
    LPWSTR path = nullptr;
    if (FAILED(objects->GetValueAtIndex(0, &first)) || FAILED(first.As(&file)) ||
        FAILED(file->get_Path(&path)) || !path) {
        return {};
    }
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::filesystem::path SettingsApp::ChooseImeDictionaryFile() {
    wchar_t buffer[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Text (*.txt)\0*.txt;*.tsv;*.csv\0All files\0*.*\0";
    dialog.lpstrTitle = Text(L"Choose a dictionary to import", L"読み込む辞書を選択");
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(buffer) : std::filesystem::path{};
}

// Where to export, and whether as Microsoft IME's word list (the Japanese
// words; offered while Japanese is installed) or TEKITO's own file (the
// English words).
std::pair<std::filesystem::path, bool> SettingsApp::ChooseDictionaryExportFile() {
    wchar_t buffer[MAX_PATH] = L"TEKITO";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    const bool japanese = japaneseAvailable_;
    dialog.lpstrFilter = japanese
        ? Text(L"Japanese words, Microsoft IME format, for other IMEs (*.txt)\0*.txt\0English words, TEKITO format (*.tsv)\0*.tsv\0",
               L"日本語の単語 - Microsoft IME 形式、ほかの IME 用 (*.txt)\0*.txt\0英語の単語 - TEKITO 形式 (*.tsv)\0*.tsv\0")
        : Text(L"English words, TEKITO format (*.tsv)\0*.tsv\0", L"英語の単語 - TEKITO 形式 (*.tsv)\0*.tsv\0");
    dialog.lpstrDefExt = japanese ? L"txt" : L"tsv";
    dialog.lpstrTitle = Text(L"Export the dictionary", L"辞書を書き出す");
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return {};
    return {std::filesystem::path(buffer), japanese && dialog.nFilterIndex == 1};
}

std::filesystem::path SettingsApp::ChooseRecoveryCodeFile() {
    wchar_t buffer[MAX_PATH] = L"TEKITO recovery code";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Text (*.txt)\0*.txt\0";
    dialog.lpstrDefExt = L"txt";
    dialog.lpstrTitle = Text(L"Save the recovery code", L"復旧コードを保存");
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&dialog) ? std::filesystem::path(buffer) : std::filesystem::path{};
}

// The code, and what it is for, in the user's language.
bool SettingsApp::SaveRecoveryCode(const std::filesystem::path& file, const std::wstring& code) {
    const std::wstring text = std::wstring(Text(L"TEKITO recovery code", L"TEKITO 復旧コード")) + L"\r\n\r\n" + code +
                              L"\r\n\r\n" +
                              Text(L"With this code, your dictionary, learning and settings can be recovered on another PC "
                                   L"or after Windows is reinstalled. Keep it somewhere safe, away from this PC.",
                                   L"このコードがあれば、別の PC や Windows の再インストール後でも、辞書・学習内容・設定を"
                                   L"取り出せます。この PC 以外の安全な場所に保管してください。") +
                              L"\r\n";
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return static_cast<bool>(out);
}

std::filesystem::path SettingsApp::ChooseRomajiFile() {
    wchar_t buffer[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Text (*.txt)\0*.txt;*.tsv\0All files\0*.*\0";
    dialog.lpstrTitle = Text(L"Choose a romaji table saved from another input method",
                             L"ほかの IME から書き出したローマ字表を選ぶ");
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(buffer) : std::filesystem::path{};
}

std::filesystem::path SettingsApp::ChoosePostalCodeFile() {
    wchar_t buffer[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"utf_ken_all.zip\0*.zip;*.csv\0";
    dialog.lpstrTitle = Text(L"Choose Japan Post's postal code data", L"日本郵便の郵便番号データを選ぶ");
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(buffer) : std::filesystem::path{};
}

std::filesystem::path SettingsApp::ChooseExecutable() {
    wchar_t buffer[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Apps (*.exe)\0*.exe\0";
    dialog.lpstrTitle = Text(L"Choose an app where TEKITO stays off", L"TEKITO をオフにするアプリを選ぶ");
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(buffer)
                                     : std::filesystem::path{};
}

}  // namespace tekito::settings
