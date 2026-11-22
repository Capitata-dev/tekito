#include "UserData/PostalCodeImport.h"

#include "UserData/DataPackValidation.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace tekito::userdata {
namespace {

constexpr wchar_t kDataFile[] = L"zipcodes.tsv";
// Japan Post's data has about 120,000 rows; far fewer is some other file.
constexpr std::size_t kFewestRows = 50000;

std::wstring Wide(std::string_view text, bool strict) {
    if (text.empty()) return {};
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int length = MultiByteToWideChar(CP_UTF8, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, flags, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

// One CSV line's fields; quotes are taken off.
std::vector<std::wstring> CsvFields(std::wstring_view line) {
    std::vector<std::wstring> fields(1);
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const wchar_t c = line[i];
        if (c == L'"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == L'"') {
                fields.back().push_back(L'"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (c == L',' && !quoted) {
            fields.emplace_back();
        } else {
            fields.back().push_back(c);
        }
    }
    return fields;
}

// The town as a place: notes in full-width parentheses go, and a note in
// place of a town ("not listed below", "a block number follows": the code
// covers the rest of the city) leaves none.
std::wstring Town(std::wstring town) {
    if (const auto note = town.find(L'\xFF08'); note != std::wstring::npos) town.erase(note);
    if (town == L"\x4EE5\x4E0B\x306B\x63B2\x8F09\x304C\x306A\x3044\x5834\x5408" || town.ends_with(L"\x306E\x6B21\x306B\x756A\x5730\x304C\x304F\x308B\x5834\x5408")) town.clear();
    return town;
}

bool IsCode(std::wstring_view text) {
    if (text.size() != 7) return false;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// The most a file to import may be (Japan Post's: about 2 MB zipped, 18 MB
// as CSV).
constexpr std::uintmax_t kMostZipBytes = 32ull * 1024 * 1024;
constexpr std::uintmax_t kMostCsvBytes = 128ull * 1024 * 1024;

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

// A number in manifest.json after "key": (the manifests TEKITO writes).
std::size_t JsonNumber(const std::string& json, std::string_view key) {
    const auto at = json.find("\"" + std::string(key) + "\"");
    if (at == std::string::npos) return 0;
    const auto colon = json.find(':', at);
    if (colon == std::string::npos) return 0;
    return static_cast<std::size_t>(std::strtoull(json.c_str() + colon + 1, nullptr, 10));
}

// The CSV inside Japan Post's zip, unpacked with Windows' own tar.
std::filesystem::path UnpackCsv(const std::filesystem::path& zip, const std::filesystem::path& into) {
    std::error_code ignored;
    std::filesystem::remove_all(into, ignored);
    std::filesystem::create_directories(into, ignored);
    wchar_t system[MAX_PATH]{};
    if (GetSystemDirectoryW(system, MAX_PATH) == 0) return {};
    std::wstring command = L"\"" + (std::filesystem::path(system) / L"tar.exe").wstring() + L"\" -xf \"" +
                           zip.wstring() + L"\" -C \"" + into.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        return {};
    }
    constexpr DWORD kUnpackTimeout = 60000;
    const bool finished = WaitForSingleObject(process.hProcess, kUnpackTimeout) == WAIT_OBJECT_0;
    DWORD exitCode = 1;
    if (!finished) TerminateProcess(process.hProcess, 1);
    else GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    // Unpacked whole, or not used: part of a CSV is not the data.
    if (!finished || exitCode != 0) return {};
    // tar.exe (bsdtar) leaves out members with .. and takes the leading /
    // off absolute ones; what is read must still be a plain file inside
    // `into`, not a link out of it.
    const auto root = std::filesystem::weakly_canonical(into, ignored).wstring();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(into, ignored)) {
        if (entry.is_symlink(ignored) || !entry.is_regular_file(ignored)) continue;
        if (_wcsicmp(entry.path().extension().c_str(), L".csv") != 0) continue;
        const auto path = std::filesystem::weakly_canonical(entry.path(), ignored).wstring();
        if (path.size() > root.size() && _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 &&
            (path[root.size()] == L'\\' || path[root.size()] == L'/')) {
            return entry.path();
        }
    }
    return {};
}

std::wstring MonthOf(const std::filesystem::path& file) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(file, error);
    std::time_t time = std::time(nullptr);
    if (!error) {
        time = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(written));
    }
    std::tm local{};
    localtime_s(&local, &time);
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%04d.%02d", local.tm_year + 1900, local.tm_mon + 1);
    return buffer;
}

// Replaces `target` with `fresh`. An application may still have the old
// file mapped; it is renamed aside, and removed once nothing uses it.
bool Replace(const std::filesystem::path& fresh, const std::filesystem::path& target) {
    std::error_code error;
    if (std::filesystem::exists(target, error)) {
        const auto aside = target.wstring() + L".old-" + std::to_wstring(GetTickCount64());
        if (!MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    }
    return MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void RemoveOldFiles(const std::filesystem::path& packDirectory) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(packDirectory, error)) {
        if (entry.path().filename().wstring().find(L".old-") != std::wstring::npos) {
            std::filesystem::remove(entry.path(), error);
        }
    }
}

}  // namespace

std::optional<PostalCodeData> InstalledPostalCodes(const std::filesystem::path& packDirectory) {
    DataPackStatus status;
    if (!ValidateDataPack(packDirectory, status) || !status.valid) return std::nullopt;
    const auto count = JsonNumber(ReadAll(packDirectory / L"manifest.json"), "entry_count");
    return PostalCodeData{count, status.version};
}

std::optional<PostalCodeData> BuildPostalCodePack(std::string_view csvUtf8, std::wstring version,
                                                  const std::filesystem::path& packDirectory,
                                                  PostalCodeImportError& error) {
    if (csvUtf8.starts_with("\xEF\xBB\xBF")) csvUtf8.remove_prefix(3);
    const std::wstring text = Wide(csvUtf8, true);
    if (text.empty()) {
        error = csvUtf8.empty() ? PostalCodeImportError::Unreadable : PostalCodeImportError::ShiftJis;
        return std::nullopt;
    }
    // "code \t address" in UTF-8: std::string orders bytewise.
    std::set<std::string> rows;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring_view line(text.data() + start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        const auto fields = CsvFields(line);
        if (fields.size() < 9 || !IsCode(fields[2])) continue;
        rows.insert(Utf8(fields[2]) + "\t" + Utf8(fields[6] + fields[7] + Town(fields[8])));
    }
    if (rows.size() < kFewestRows) {
        error = PostalCodeImportError::NotPostalCodes;
        return std::nullopt;
    }

    std::error_code fileError;
    std::filesystem::create_directories(packDirectory, fileError);
    RemoveOldFiles(packDirectory);
    // Every file written beside the pack first, and put in place only once
    // all of them are: a write that fails leaves the pack as it was.
    const auto fresh = packDirectory / (std::wstring(kDataFile) + L".new");
    const auto freshNotice = packDirectory / L"NOTICE.new";
    const auto freshManifest = packDirectory / L"manifest.json.new";
    const auto failed = [&] {
        for (const auto& file : {fresh, freshNotice, freshManifest}) std::filesystem::remove(file, fileError);
        error = PostalCodeImportError::WriteFailed;
        return std::nullopt;
    };
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        for (const auto& row : rows) out << row << '\n';
        out.close();
        if (!out) return failed();
    }
    {
        std::ofstream notice(freshNotice, std::ios::binary | std::ios::trunc);
        notice << "Postal codes and the addresses they cover, from Japan Post's postal\n"
                  "code data (https://www.post.japanpost.jp/zipcode/download.html),\n"
                  "added in TEKITO Settings. Japan Post claims no copyright in this data.\n";
        notice.close();
        if (!notice) return failed();
    }
    const auto hash = FileSha256(fresh);
    if (hash.empty()) return failed();
    std::ofstream manifest(freshManifest, std::ios::binary | std::ios::trunc);
    manifest << "{\n"
             << "  \"pack_id\": \"japanese-zipcode\",\n"
             << "  \"display_name\": \"Japanese Postal Codes\",\n"
             << "  \"schema_version\": 1,\n"
             << "  \"version\": \"" << JsonEscape(Utf8(version)) << "\",\n"
             << "  \"language\": \"ja-JP\",\n"
             << "  \"type\": \"japanese-zipcode\",\n"
             << "  \"format\": \"sorted-tsv-v1\",\n"
             << "  \"file\": \"zipcodes.tsv\",\n"
             << "  \"entry_count\": " << rows.size() << ",\n"
             << "  \"sha256\": {\"file\": \"" << hash << "\"},\n"
             << "  \"license\": \"Japan-Post-Postal-Code-Data\",\n"
             << "  \"source\": \"Japan Post utf_ken_all.zip, added in Settings\",\n"
             << "  \"notice_file\": \"NOTICE\"\n"
             << "}\n";
    manifest.close();
    if (!manifest) return failed();
    // The data first, then what describes it: until the manifest is
    // replaced, the old one's hash does not match and the pack is not used.
    if (!Replace(fresh, packDirectory / kDataFile) ||
        !MoveFileExW(freshNotice.c_str(), (packDirectory / L"NOTICE").c_str(), MOVEFILE_REPLACE_EXISTING) ||
        !MoveFileExW(freshManifest.c_str(), (packDirectory / L"manifest.json").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return failed();
    }
    error = PostalCodeImportError::None;
    return PostalCodeData{rows.size(), std::move(version)};
}

std::optional<PostalCodeData> ImportPostalCodes(const std::filesystem::path& source,
                                                const std::filesystem::path& packDirectory,
                                                PostalCodeImportError& error) {
    error = PostalCodeImportError::Unreadable;
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(source, fileError)) return std::nullopt;
    auto csv = source;
    // A folder of this run's own (another Settings may be importing too).
    const auto unpacked = std::filesystem::temp_directory_path(fileError) /
                          (L"TEKITO-zipcode-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()));
    const bool zipped = _wcsicmp(source.extension().c_str(), L".zip") == 0;
    const auto done = [&](std::optional<PostalCodeData> result) {
        if (zipped) std::filesystem::remove_all(unpacked, fileError);
        return result;
    };
    if (zipped) {
        // Japan Post's zip is about 2 MB, its CSV about 18 MB: much more is
        // not that data, and is not read into memory.
        if (std::filesystem::file_size(source, fileError) > kMostZipBytes) return std::nullopt;
        csv = UnpackCsv(source, unpacked);
        if (csv.empty()) return done(std::nullopt);
    }
    const auto size = std::filesystem::file_size(csv, fileError);
    if (fileError || size > kMostCsvBytes) return done(std::nullopt);
    const auto text = ReadAll(csv);
    return done(BuildPostalCodePack(text, MonthOf(source), packDirectory, error));
}

bool RemovePostalCodes(const std::filesystem::path& packDirectory) {
    std::error_code error;
    // Without its manifest the pack is gone; the data may still be mapped
    // by an application, so it is renamed aside.
    std::filesystem::remove(packDirectory / L"manifest.json", error);
    if (error) return false;
    const auto data = packDirectory / kDataFile;
    if (std::filesystem::exists(data, error)) {
        const auto aside = data.wstring() + L".old-" + std::to_wstring(GetTickCount64());
        MoveFileExW(data.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    std::filesystem::remove(packDirectory / L"NOTICE", error);
    RemoveOldFiles(packDirectory);
    std::filesystem::remove(packDirectory, error);  // only when empty
    return true;
}

}  // namespace tekito::userdata
