#include "UserData/AiModelInstall.h"

#include "Core/Japanese/AiJudge.h"
#include "UserData/DataPackValidation.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace tekito::userdata {
namespace {

// Unpacks `zip` into `into` with Windows' own tar.
bool Unpack(const std::filesystem::path& zip, const std::filesystem::path& into) {
    std::error_code ignored;
    std::filesystem::remove_all(into, ignored);
    std::filesystem::create_directories(into, ignored);
    wchar_t system[MAX_PATH]{};
    if (GetSystemDirectoryW(system, MAX_PATH) == 0) return false;
    std::wstring command = L"\"" + (std::filesystem::path(system) / L"tar.exe").wstring() + L"\" -xf \"" +
                           zip.wstring() + L"\" -C \"" + into.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        return false;
    }
    constexpr DWORD kUnpackTimeout = 10 * 60 * 1000;
    const bool finished = WaitForSingleObject(process.hProcess, kUnpackTimeout) == WAIT_OBJECT_0;
    DWORD exitCode = 1;
    if (!finished) TerminateProcess(process.hProcess, 1);
    else GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return finished && exitCode == 0;
}

// Removes `directory`, trying again for a moment while the helper that
// was told to stop still has its model mapped.
bool RemoveWhenFree(const std::filesystem::path& directory) {
    std::error_code error;
    for (int tries = 0; tries < 20; ++tries) {
        // What the NPU's compiler keeps (npu-cache) is read-only.
        for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(it->path().c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
            }
        }
        error.clear();
        std::filesystem::remove_all(directory, error);
        if (!error || !std::filesystem::exists(directory, error)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

// Moves `from` to `to`, trying again for a moment while the helper that was
// told to stop still has its model mapped.
bool MoveWhenFree(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    for (int tries = 0; tries < 20; ++tries) {
        error.clear();
        std::filesystem::rename(from, to, error);
        if (!error) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

// Whether what was unpacked is plain files and folders only: no link or
// junction that leads elsewhere.
bool OnlyPlainFiles(const std::filesystem::path& root) {
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return !error;
}

// Whether each file the manifest gives a SHA-256 for ("sha256": {"model.data":
// "...", ...}) has it; "file" and "index" ValidateDataPack checks.
bool FilesMatchManifest(const std::filesystem::path& pack) {
    std::ifstream in(pack / L"manifest.json", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto key = manifest.find("\"sha256\"");
    const auto open = key == std::string::npos ? key : manifest.find('{', key);
    const auto close = open == std::string::npos ? open : manifest.find('}', open);
    if (close == std::string::npos) return false;
    const std::string hashes = manifest.substr(open + 1, close - open - 1);
    std::vector<std::string> strings;
    for (std::size_t at = hashes.find('"'); at != std::string::npos;) {
        const auto end = hashes.find('"', at + 1);
        if (end == std::string::npos) return false;
        strings.push_back(hashes.substr(at + 1, end - at - 1));
        at = hashes.find('"', end + 1);
    }
    if (strings.size() % 2 != 0) return false;
    for (std::size_t i = 0; i < strings.size(); i += 2) {
        const auto& name = strings[i];
        if (name == "file" || name == "index") continue;
        const std::wstring file(name.begin(), name.end());
        // A file name of the pack's own (model.data), never a path.
        if (file.empty() || file == L"." || file == L".." || file.find_first_of(L"\\/:") != std::wstring::npos) {
            return false;
        }
        if (FileSha256(pack / file) != strings[i + 1]) return false;
    }
    return true;
}

// The zip, held open so it cannot be changed between its check and its
// unpacking (others may still read it).
struct HeldFile {
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit HeldFile(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~HeldFile() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
    HeldFile(const HeldFile&) = delete;
    HeldFile& operator=(const HeldFile&) = delete;
};

}  // namespace

bool IsPlainPackId(std::wstring_view packId) noexcept {
    // A pack's name is one folder name of its own (japanese-core,
    // ai-runtime): never a path, so nothing outside the data folder is
    // removed or replaced.
    if (packId.empty() || packId.size() > 64 || packId.front() == L'-') return false;
    for (const wchar_t c : packId) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')) return false;
    }
    return true;
}

namespace {

// Puts each unpacked pack (`from`/<id>) in place of the one there, all or
// none: the ones there are moved aside first, and put back if any new one
// cannot be placed.
AiModelInstallError PutInPlace(const std::filesystem::path& from, const std::vector<std::wstring>& packIds,
                               const std::filesystem::path& dataRoot) {
    std::error_code error;
    std::vector<std::wstring> movedAside, placed;
    const auto aside = [&](const std::wstring& id) { return dataRoot / (id + L".old"); };
    const auto undo = [&](AiModelInstallError result) {
        for (const auto& id : placed) (void)RemoveWhenFree(dataRoot / id);
        for (const auto& id : movedAside) (void)MoveWhenFree(aside(id), dataRoot / id);
        return result;
    };
    for (const auto& id : packIds) {
        const auto target = dataRoot / id;
        if (std::filesystem::exists(aside(id), error) && !RemoveWhenFree(aside(id))) {
            return undo(AiModelInstallError::InUse);
        }
        if (std::filesystem::exists(target, error)) {
            if (!MoveWhenFree(target, aside(id))) return undo(AiModelInstallError::InUse);
            movedAside.push_back(id);
        }
        if (!MoveWhenFree(from / id, target)) return undo(AiModelInstallError::Unpack);
        placed.push_back(id);
    }
    for (const auto& id : movedAside) (void)RemoveWhenFree(aside(id));
    return AiModelInstallError::None;
}

// Checks `zip` and unpacks it into `into`, the zip held so it is the one
// checked; and checks what came out (`packIds`, each whole).
AiModelInstallError CheckAndUnpack(const std::filesystem::path& zip, std::string_view sha256,
                                   const std::filesystem::path& into, const std::vector<std::wstring>& packIds) {
    const HeldFile held(zip);
    if (held.handle == INVALID_HANDLE_VALUE || FileSha256(zip) != sha256) return AiModelInstallError::Checksum;
    if (!Unpack(zip, into) || !OnlyPlainFiles(into)) return AiModelInstallError::Unpack;
    // Every pack checked before any is put in place.
    for (const auto& packId : packIds) {
        DataPackStatus status;
        if (!ValidateDataPack(into / packId, status) || status.packId != packId || !FilesMatchManifest(into / packId)) {
            return AiModelInstallError::Invalid;
        }
    }
    return AiModelInstallError::None;
}

}  // namespace

AiModelInstallError InstallAiModelPack(const std::filesystem::path& zip, std::string_view sha256,
                                       std::wstring_view packId, const std::filesystem::path& dataRoot) {
    if (!IsPlainPackId(packId)) return AiModelInstallError::Invalid;
    const std::vector<std::wstring> ids{std::wstring(packId)};
    const auto unpacked = dataRoot / (ids.front() + L".download");
    const auto done = [&](AiModelInstallError result) {
        std::error_code ignored;
        std::filesystem::remove_all(unpacked, ignored);
        return result;
    };
    if (const auto checked = CheckAndUnpack(zip, sha256, unpacked, ids); checked != AiModelInstallError::None) {
        return done(checked);
    }
    StopAiHelper();
    return done(PutInPlace(unpacked, ids, dataRoot));
}

AiModelInstallError InstallDataPacks(const std::filesystem::path& zip, std::string_view sha256,
                                     const std::vector<std::wstring>& packIds, const std::filesystem::path& dataRoot) {
    if (packIds.empty()) return AiModelInstallError::Invalid;
    for (const auto& packId : packIds) {
        if (!IsPlainPackId(packId)) return AiModelInstallError::Invalid;
    }
    std::error_code error;
    std::filesystem::create_directories(dataRoot, error);
    const auto unpacked = dataRoot / L"japanese-data.download";
    const auto done = [&](AiModelInstallError result) {
        std::error_code ignored;
        std::filesystem::remove_all(unpacked, ignored);
        return result;
    };
    if (const auto checked = CheckAndUnpack(zip, sha256, unpacked, packIds); checked != AiModelInstallError::None) {
        return done(checked);
    }
    return done(PutInPlace(unpacked, packIds, dataRoot));
}

bool RemoveAiModelPack(std::wstring_view packId, const std::filesystem::path& dataRoot) {
    if (!IsPlainPackId(packId)) return false;
    const auto target = dataRoot / std::wstring(packId);
    std::error_code error;
    if (!std::filesystem::exists(target, error)) return true;
    StopAiHelper();
    return RemoveWhenFree(target);
}

bool AiModelPackInstalled(std::wstring_view packId, const std::filesystem::path& dataRoot) {
    if (!IsPlainPackId(packId)) return false;
    std::error_code error;
    const auto pack = dataRoot / std::wstring(packId);
    return std::filesystem::is_regular_file(pack / L"manifest.json", error) &&
           std::filesystem::is_regular_file(pack / L"model.onnx", error);
}

bool AiRuntimeInstalled(const std::filesystem::path& dataRoot) {
    std::error_code error;
    const auto pack = dataRoot / std::wstring(japanese::aihelper::kRuntimePack);
    // "protocol": "TKA3" in its manifest (an older pack ran llama.cpp).
    std::ifstream in(pack / L"manifest.json", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto key = manifest.find("\"protocol\"");
    const auto value = key == std::string::npos ? key : manifest.find('"', manifest.find(':', key) + 1);
    if (value == std::string::npos ||
        manifest.compare(value + 1, japanese::aihelper::kProtocol.size(), japanese::aihelper::kProtocol) != 0) {
        return false;
    }
    return std::filesystem::is_regular_file(pack / L"manifest.json", error) &&
           std::filesystem::is_regular_file(pack / L"TekitoAI.exe", error);
}

bool AiRuntimeHasWindowsMl(const std::filesystem::path& dataRoot) {
    std::error_code error;
    const auto pack = dataRoot / std::wstring(japanese::aihelper::kRuntimePack);
    return AiRuntimeInstalled(dataRoot) &&
           std::filesystem::is_regular_file(pack / L"Microsoft.Windows.AI.MachineLearning.dll", error);
}

bool AiRuntimeRunsSettings(const std::filesystem::path& dataRoot) {
    if (!AiRuntimeInstalled(dataRoot)) return false;
    std::ifstream in(dataRoot / std::wstring(japanese::aihelper::kRuntimePack) / L"manifest.json", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto key = manifest.find("\"models\"");
    const auto end = key == std::string::npos ? key : manifest.find(']', key);
    return end != std::string::npos && manifest.substr(key, end - key).find("\"settings\"") != std::string::npos;
}

void StopAiHelper() { japanese::aihelper::Stop(); }

}  // namespace tekito::userdata
