#include "UserData/UserDictionaryFile.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace tekito::userdata {
namespace {

constexpr std::wstring_view kHeader = L"TEKITO_USER_DICTIONARY_V1";
// What a file to import may hold at most: more is not a dictionary of words.
constexpr std::uintmax_t kMostBytes = 32ull * 1024 * 1024;
constexpr std::size_t kMostLineChars = 16 * 1024;
constexpr std::size_t kMostEntries = 200'000;

std::wstring Hex(std::wstring_view value) {
    std::wostringstream output;
    output << std::uppercase << std::hex << std::setfill(L'0');
    for (const wchar_t ch : value) output << std::setw(4) << static_cast<unsigned int>(ch);
    return output.str();
}

bool HexValue(wchar_t value, unsigned int& result) {
    if (value >= L'0' && value <= L'9') result = static_cast<unsigned int>(value - L'0');
    else if (value >= L'a' && value <= L'f') result = static_cast<unsigned int>(value - L'a' + 10);
    else if (value >= L'A' && value <= L'F') result = static_cast<unsigned int>(value - L'A' + 10);
    else return false;
    return true;
}

bool Unhex(std::wstring_view value, std::wstring& result) {
    if (value.size() % 4 != 0) return false;
    result.clear();
    result.reserve(value.size() / 4);
    for (std::size_t index = 0; index < value.size(); index += 4) {
        unsigned int code = 0;
        for (std::size_t offset = 0; offset < 4; ++offset) {
            unsigned int digit = 0;
            if (!HexValue(value[index + offset], digit)) return false;
            code = (code << 4) | digit;
        }
        result.push_back(static_cast<wchar_t>(code));
    }
    return true;
}

bool ParseLine(std::wstring_view line, UserDictionaryEntry& entry) {
    std::wstring_view fields[6];
    for (auto& field : fields) {
        const auto separator = line.find(L'\t');
        field = separator == std::wstring_view::npos ? line : line.substr(0, separator);
        line = separator == std::wstring_view::npos ? std::wstring_view{} : line.substr(separator + 1);
    }
    if (fields[0].empty() || fields[3].empty() || fields[4].empty() || fields[5].empty()) return false;
    try {
        entry.id = std::stoull(std::wstring(fields[0]));
        if (!Unhex(fields[1], entry.raw) || !Unhex(fields[2], entry.candidate)) return false;
        entry.policyFlags = static_cast<std::uint32_t>(std::stoul(std::wstring(fields[3])));
        // Each a 0 or a 1, nothing else: a file that says otherwise is not
        // one this wrote.
        const auto flag = [](std::wstring_view field) { return field == L"0" || field == L"1"; };
        if (!flag(fields[4]) || !flag(fields[5])) return false;
        entry.caseSensitive = fields[4] == L"1";
        entry.enabled = fields[5] == L"1";
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

bool ExportUserDictionary(const UserDictionary& dictionary,
                          const std::filesystem::path& path) noexcept {
    try {
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        // Written beside it first: an export that stops part way leaves the
        // earlier one as it was.
        auto partial = path;
        partial += L".partial";
        bool written = false;
        {
            std::wofstream output(partial, std::ios::binary | std::ios::trunc);
            if (output) {
                output << kHeader << L'\n'
                       << L"id\traw_hex\tcandidate_hex\tpolicy_flags\tcase_sensitive\tenabled\n";
                for (const auto& entry : dictionary.Entries()) {
                    output << entry.id << L'\t' << Hex(entry.raw) << L'\t' << Hex(entry.candidate) << L'\t'
                           << entry.policyFlags << L'\t' << (entry.caseSensitive ? 1 : 0) << L'\t'
                           << (entry.enabled ? 1 : 0) << L'\n';
                }
                output.flush();
                written = static_cast<bool>(output);
                output.close();
                written = written && !output.fail();
            }
        }
        // Moved over the earlier one once all of it is on disk.
        if (!written ||
            !MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ignored;
            std::filesystem::remove(partial, ignored);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ImportUserDictionary(const std::filesystem::path& path,
                          UserDictionary& dictionary) noexcept {
    try {
        std::error_code error;
        const auto bytes = std::filesystem::file_size(path, error);
        if (error || bytes > kMostBytes) return false;
        std::wifstream input(path, std::ios::binary);
        std::wstring line;
        // A line as written here, or with Windows' line ending.
        const auto next = [&] {
            if (!std::getline(input, line)) return false;
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            return true;
        };
        if (!input || !next() || line != kHeader || !next()) {
            return false;
        }
        UserDictionary loaded;
        std::size_t count = 0;
        while (next()) {
            if (line.empty()) continue;
            if (line.size() > kMostLineChars || ++count > kMostEntries) return false;
            UserDictionaryEntry entry;
            if (!ParseLine(line, entry) || !loaded.Add(std::move(entry))) return false;
        }
        if (!input.eof()) return false;
        dictionary = std::move(loaded);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace tekito::userdata
