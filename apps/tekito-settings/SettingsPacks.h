#pragma once

// The AI models, what runs them and the data packs: what there is, where it
// comes from, and installing it.

#include "apps/tekito-settings/SettingsShared.h"

namespace tekito::settings {

// The GPUs the AI model can run on (DirectML's adapters, not software
// ones), each once: japanese::GpuId and its name.
inline std::vector<std::pair<std::uint32_t, std::wstring>> AiGpus() {
    std::vector<std::pair<std::uint32_t, std::wstring>> gpus;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return gpus;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        const auto id = tekito::japanese::GpuId(desc.VendorId, desc.DeviceId);
        if (std::none_of(gpus.begin(), gpus.end(), [&](const auto& gpu) { return gpu.first == id; })) {
            gpus.emplace_back(id, desc.Description);
        }
    }
    return gpus;
}

// Whether the PC has an NPU (DXCore lists it; Windows 11 24H2 or later).
inline bool HasNpu() {
    const HMODULE dxcore = LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxcore) return false;
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<Create>(GetProcAddress(dxcore, "DXCoreCreateAdapterFactory"));
    bool found = false;
    Microsoft::WRL::ComPtr<IDXCoreAdapterFactory> factory;
    if (create && SUCCEEDED(create(IID_PPV_ARGS(&factory)))) {
        const GUID npu[] = {DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU};
        Microsoft::WRL::ComPtr<IDXCoreAdapterList> list;
        found = SUCCEEDED(factory->CreateAdapterList(1, npu, IID_PPV_ARGS(&list))) && list->GetAdapterCount() > 0;
    }
    factory.Reset();
    FreeLibrary(dxcore);
    return found;
}

// Has TekitoAI.exe make the NPU's provider ready (Windows ML downloads it
// from Microsoft when it is not on the PC yet); true when an NPU is ready.
inline bool PrepareNpu() {
    std::wstring command = L"\"" + tekito::japanese::aihelper::HelperPath() + L"\" --devices --prepare npu";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0;
}

// The AI models (japanese::AiModel 1 and 2) Settings downloads, from
// TEKITO's releases, each checked against its SHA-256
// (scripts/ja/build-japanese-ai-packs.py prints them), and the version its
// manifest gives: a copy there of an older version is offered to be
// replaced (nothing is looked up online for it).
struct AiModelInfo {
    const wchar_t* pack;
    const wchar_t* url;
    const char* sha256;
    unsigned long long bytes;
    const wchar_t* version;
};
constexpr AiModelInfo kAiModels[] = {
    {L"japanese-ai-light", L"https://github.com/Capitata-dev/tekito/releases/download/ai-models-2/TEKITO-japanese-ai-light.zip",
     "44A8FA9CD516DC169CC0F03449E946B299AE44C440C8EFB027128A834819DD04", 358021668ull, L"2026.11"},
    {L"japanese-ai-strong", L"https://github.com/Capitata-dev/tekito/releases/download/ai-models-2/TEKITO-japanese-ai-strong.zip",
     "23EECE917B66F960BB7DE9FA44C6AA5CC78B49652B63BA7FE3B1256870E6FFFC", 1587553900ull, L"2026.11"},
};
// What runs them (TekitoAI.exe and ONNX Runtime, on the processor),
// downloaded with the first model and removed with the last; and the copy
// with Windows ML in its place (GPUs and NPUs), downloaded once the user
// chooses one and agrees to Microsoft's terms. Both also run Settings'
// model (ai-models-2's copies do not, and are replaced).
constexpr AiModelInfo kAiRuntime{
    L"ai-runtime", L"https://github.com/Capitata-dev/tekito/releases/download/ai-models-3/TEKITO-ai-runtime.zip",
    "AF216815E7B721890841A558D0F4F157DC67C6887F29817D72342482DC8ED6C5", 6380188ull, L"2026.11"};
constexpr AiModelInfo kAiRuntimeWindowsMl{
    L"ai-runtime", L"https://github.com/Capitata-dev/tekito/releases/download/ai-models-3/TEKITO-ai-runtime-windows-ml.zip",
    "003B574CB4874F37415B29A3B214CC555032D27197702A10140DE8195DB3AE14", 18131689ull, L"2026.11"};

inline tekito::userdata::AiModelInstallError DownloadAiPack(const AiModelInfo& pack, const std::filesystem::path& root,
                                                            double from = 0.0, double to = 1.0);

// The copy of what runs the models this Settings would download: with
// Windows ML when that is the one there.
inline const AiModelInfo& AiRuntimeFor(const std::filesystem::path& root) {
    return tekito::userdata::AiRuntimeHasWindowsMl(root) ? kAiRuntimeWindowsMl : kAiRuntime;
}

// Whether a copy of `pack` is there but older than the one this Settings
// downloads.
inline bool AiPackOutdated(const AiModelInfo& pack, const std::filesystem::path& root) {
    const auto installed = tekito::userdata::AiPackVersion(pack.pack, root);
    return !installed.empty() && tekito::userdata::OlderVersion(installed, pack.version);
}

// Whether what runs the models is too old: it does not run Settings'
// model, or is an older version.
inline bool AiRuntimeOutdated(const std::filesystem::path& root) {
    return !tekito::userdata::AiRuntimeRunsSettings(root) || AiPackOutdated(AiRuntimeFor(root), root);
}

// What runs the models, when it is not there or is too old.
inline tekito::userdata::AiModelInstallError UpdateAiRuntime(const std::filesystem::path& root) {
    if (!AiRuntimeOutdated(root)) return tekito::userdata::AiModelInstallError::None;
    return DownloadAiPack(AiRuntimeFor(root), root, 0.0, 0.05);
}

// Downloads what runs the models when it is not there yet, then `model`
// (1 or 2), and installs them into `root`.
inline tekito::userdata::AiModelInstallError DownloadAiModel(int model, const std::filesystem::path& root) {
    using tekito::userdata::AiModelInstallError;
    if (const auto error = UpdateAiRuntime(root); error != AiModelInstallError::None) return error;
    if (const auto error = DownloadAiPack(kAiModels[model - 1], root, 0.05, 1.0); error != AiModelInstallError::None) return error;
    return AiModelInstallError::None;
}

// What runs the models, with Windows ML (in place of the processor's copy).
inline tekito::userdata::AiModelInstallError DownloadAiRuntimeWindowsMl(const std::filesystem::path& root) {
    return DownloadAiPack(kAiRuntimeWindowsMl, root);
}

// Setup's progress file (TEKITO_PROGRESS_FILE, which install.ps1 sets for
// "--install-ai"): "ai <fraction>" as the model comes. Nothing otherwise.
inline void NoteAiProgress(double done) {
    static const std::wstring file = [] {
        std::wstring path(MAX_PATH, L'?');
        path.resize(GetEnvironmentVariableW(L"TEKITO_PROGRESS_FILE", path.data(), static_cast<DWORD>(path.size())));
        return path;
    }();
    if (file.empty()) return;
    char text[32]{};
    sprintf_s(text, "ai %.3f", std::clamp(done, 0.0, 1.0));
    std::ofstream out(std::filesystem::path(file), std::ios::binary | std::ios::trunc);
    out << text;
}

// How much of a download has come, told on as part of `from` to `to`.
class DownloadProgress final : public IBindStatusCallback {
public:
    DownloadProgress(double from, double to) : from_(from), to_(to) {}
    STDMETHODIMP QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(IBindStatusCallback)) {
            *object = static_cast<IBindStatusCallback*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    // On the stack for the one download: nothing to free.
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    STDMETHODIMP GetPriority(LONG*) override { return E_NOTIMPL; }
    STDMETHODIMP OnLowResource(DWORD) override { return S_OK; }
    STDMETHODIMP OnProgress(ULONG progress, ULONG total, ULONG, LPCWSTR) override {
        if (total == 0) return S_OK;
        const int permille = static_cast<int>(1000.0 * progress / total);
        if (permille != told_) {
            told_ = permille;
            NoteAiProgress(from_ + (to_ - from_) * progress / total);
        }
        return S_OK;
    }
    STDMETHODIMP OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    STDMETHODIMP GetBindInfo(DWORD*, BINDINFO*) override { return E_NOTIMPL; }
    STDMETHODIMP OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    STDMETHODIMP OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }

private:
    double from_;
    double to_;
    int told_{-1};
};

// Downloads one pack and installs it into `root`; how much has come is
// told as part of `from` to `to` (NoteAiProgress).
inline tekito::userdata::AiModelInstallError DownloadAiPack(const AiModelInfo& pack, const std::filesystem::path& root,
                                                            double from, double to) {
    using tekito::userdata::AiModelInstallError;
    wchar_t folder[MAX_PATH]{};
    GetTempPathW(MAX_PATH, folder);
    const auto file = std::filesystem::path(folder) /
                      (L"tekito-" + std::wstring(pack.pack) + L"-" + std::to_wstring(GetCurrentProcessId()) + L".zip");
    DownloadProgress progress(from, to);
    const auto error = FAILED(URLDownloadToFileW(nullptr, pack.url, file.c_str(), 0, &progress))
                           ? AiModelInstallError::Download
                           : tekito::userdata::InstallAiModelPack(file, pack.sha256, pack.pack, root);
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    return error;
}

// The distributor's site, opened by the About page. Fixed here rather than
// taken from the page, so the page cannot ask the host to open arbitrary URLs.
constexpr wchar_t kPublisherWebsite[] = L"https://capitata.dev";

// The Data Packs a full installation has, in the order About lists them.
struct DataPackInfo {
    const wchar_t* folder;
    const wchar_t* label;
};
// The Japanese ones, installed only with Japanese input. The first two are
// what Japanese needs at all; the rest add to it.
constexpr DataPackInfo kJapaneseDataPacks[] = {
    {L"japanese-core", L"Japanese Dictionary"},
    {L"japanese-romaji", L"Romaji Table"},
    {L"japanese-lm", L"Japanese Word Pairs"},
    {L"japanese-loanwords", L"Loanwords"},
    {L"japanese-english", L"Katakana and English"},
    {L"japanese-names", L"Proper Nouns (Wikidata)"},
    {L"japanese-jmdict", L"Set Phrases and Names (JMdict)"},
    {L"japanese-sudachi", L"Japanese Nouns (SudachiDict)"},
    {L"japanese-places", L"Place Names (Japan Post)"},
    {L"japanese-wiktionary", L"Japanese Meanings (Wiktionary)"},
    {L"japanese-wordnet", L"Japanese Meanings (WordNet)"},
};
constexpr std::size_t kJapaneseRequiredPacks = 2;
// Where Japan Post offers its postal code data (UTF-8), which Settings adds.
constexpr wchar_t kPostalCodePage[] = L"https://www.post.japanpost.jp/service/search/zipcode/download/utf-zip.html";
// Japan Post's whole-country file in UTF-8, fetched only after the user
// agrees to it in Settings: the copy on TEKITO's releases (kept current by
// .github/workflows/postal-codes.yml), else Japan Post's own.
constexpr const wchar_t* kPostalCodeZips[] = {
    L"https://github.com/Capitata-dev/tekito/releases/download/postal-codes/utf_ken_all.zip",
    L"https://www.post.japanpost.jp/service/search/zipcode/download/utf/zip/utf_ken_all.zip",
};
constexpr DataPackInfo kDataPacks[] = {
    {L"standard-english", L"Standard English"},
    {L"wikipedia-common-misspellings", L"Common Misspellings"},
    {L"frequency", L"Frequency"},
    {L"phrase", L"Phrase / N-gram"},
    {L"dictionary-display", L"Dictionary Display"},
    {L"proper-nouns", L"Place Names"},
    {L"english-names", L"Names (Wikidata)"},
    {L"slang", L"Slang"},
    {L"wiktionary-slang", L"Extended Slang"},
    {L"pronunciation", L"Pronunciation"},
    {L"emoji", L"Emoji"},
    {L"social-expression", L"Social Expressions"},
    {L"japanese-phonetic", L"Japanese Phonetic Suggestions"},
    {L"special-conversions", L"Special Conversions"},
    {L"kaomoji", L"Kaomoji and ASCII Art"},
    {L"qwerty-typo-catalog", L"QWERTY Typo Evaluation"},
};

}  // namespace tekito::settings
