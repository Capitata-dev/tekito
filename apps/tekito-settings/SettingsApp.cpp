// The window, its WebView and what it is sent and asked; modes and saving.

#include "apps/tekito-settings/SettingsApp.h"

namespace tekito::settings {

bool SettingsApp::Initialize(HINSTANCE instance) {
    instance_ = instance;
    repository_ = tekito::userdata::CreateDefaultUserDataRepository();
    if (!repository_ || !repository_->Open()) return false;
    (void)repository_->Load(dictionary_);
    (void)repository_->LoadJapaneseUserWords(japaneseWords_);
    (void)repository_->LoadSettings(settings_);
    (void)repository_->LoadLearning(learning_);
    auto startupMode = tekito::userdata::StartMode(settings_, settings_.lastInputMode);
    if (startupMode != tekito::InputMode::Direct) startupMode = tekito::InputMode::Convert;
    runtime_ = std::make_unique<tekito::userdata::RuntimeModeState>(
        startupMode, tekito::userdata::kRuntimeStateName,
        tekito::userdata::StartMode(settings_, settings_.lastJapaneseProfileMode));
    // Back if it was closed (an update stops it).
    if (settings_.desktopCompanion) StartCompanion();

    WNDCLASSW windowClass{};
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = &SettingsApp::WindowProc;
    windowClass.lpszClassName = kClassName;
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TEKITO));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(0, kClassName, WindowTitle(),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME |
                                WS_VISIBLE | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1040, 720,
                            nullptr, nullptr, instance_, this);
    if (hwnd_) {
        const auto icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TEKITO));
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
    return hwnd_ != nullptr;
}

int SettingsApp::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void SettingsApp::CreateWebViewStatusControls() {
    fallbackLabel_ = CreateWindowExW(
        0, L"STATIC", Text(L"Loading TEKITO Settings...", L"TEKITO の設定を読み込んでいます…"),
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
        32, 32, 876, 64, hwnd_, nullptr, instance_, nullptr);
    fallbackRetry_ = CreateWindowExW(
        0, L"BUTTON", Text(L"Retry", L"再試行"),
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        410, 112, 120, 32, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRetryWebView)),
        instance_, nullptr);
    if (fallbackLabel_) {
        SendMessageW(fallbackLabel_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
    if (fallbackRetry_) {
        SendMessageW(fallbackRetry_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        ShowWindow(fallbackRetry_, SW_HIDE);
    }
}

void SettingsApp::SetWebViewStatus(std::wstring_view message, bool showRetry) {
    if (fallbackLabel_) {
        SetWindowTextW(fallbackLabel_, std::wstring(message).c_str());
        ShowWindow(fallbackLabel_, SW_SHOW);
    }
    if (fallbackRetry_) ShowWindow(fallbackRetry_, showRetry ? SW_SHOW : SW_HIDE);
    if (controller_) controller_->put_IsVisible(showRetry ? FALSE : TRUE);
}

void SettingsApp::FailWebView(std::wstring_view reason) {
    std::wstring message = Text(L"TEKITO Settings could not be loaded.\r\n",
                                L"TEKITO の設定を開けませんでした。\r\n");
    message += reason;
    SetWebViewStatus(message, true);
}

void SettingsApp::ResetWebView() {
    if (webview_) {
        webview_->remove_WebMessageReceived(webMessageToken_);
        webview_->remove_NavigationCompleted(navigationToken_);
    }
    if (controller_) controller_->Close();
    webview_.Reset();
    webview3_.Reset();
    controller_.Reset();
    webMessageToken_ = {};
    navigationToken_ = {};
}

bool SettingsApp::ValidateSettingsUi(std::wstring& error) const {
    const auto root = ExecutableDirectory() / L"settings-ui";
    if (!std::filesystem::exists(root / L"index.html")) {
        error = Text(L"The local settings package is missing index.html.", L"設定画面のファイル（index.html）が見つかりません。");
        return false;
    }
    for (const auto* icon : {L"auto.ico", L"direct.ico", L"direct-dark.ico", L"japanese.ico",
                             L"japanese-dark.ico", L"mixed.ico", L"tekito.ico"}) {
        const auto path = root / L"assets" / icon;
        std::ifstream file(path, std::ios::binary);
        unsigned char header[4]{};
        if (!std::filesystem::is_regular_file(path) ||
            !file.read(reinterpret_cast<char*>(header), sizeof(header)) ||
            header[0] != 0 || header[1] != 0 || header[2] != 1 || header[3] != 0) {
            error = Text(L"The installed settings package is missing a provided ICO asset.", L"設定画面のアイコンファイルが見つかりません。");
            return false;
        }
    }
    return true;
}

void SettingsApp::InitializeWebView() {
    ResetWebView();
    SetWebViewStatus(Text(L"Loading TEKITO Settings...", L"TEKITO の設定を読み込んでいます…"), false);
    std::wstring uiError;
    if (!ValidateSettingsUi(uiError)) {
        FailWebView(uiError);
        return;
    }
    const auto userData = WebViewUserDataDirectory();
    if (userData.empty()) {
        FailWebView(Text(L"The WebView2 user data folder could not be resolved.", L"WebView2 のデータフォルダーの場所を特定できませんでした。"));
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(userData, error);
    if (error) {
        FailWebView(Text(L"The WebView2 user data folder could not be created.", L"WebView2 のデータフォルダーを作成できませんでした。"));
        return;
    }
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT environmentResult, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(environmentResult) || !environment) {
                    FailWebView(Text(L"WebView2 Runtime is unavailable or could not start.", L"WebView2 Runtime が見つからないか、起動できませんでした。"));
                    return S_OK;
                }
                return environment->CreateCoreWebView2Controller(
                    hwnd_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT controllerResult,
                               ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controllerResult) || !controller) {
                                FailWebView(Text(L"WebView2 could not create the settings view.", L"WebView2 で設定画面を作成できませんでした。"));
                                return S_OK;
                            }
                            controller_ = controller;
                            ApplyTheme();
                            if (FAILED(controller_->get_CoreWebView2(&webview_)) || !webview_) {
                                FailWebView(Text(L"WebView2 returned no settings view.", L"WebView2 から設定画面が返されませんでした。"));
                                return S_OK;
                            }
                            const auto uiFolder = (ExecutableDirectory() / L"settings-ui").wstring();
                            if (FAILED(webview_.As(&webview3_)) ||
                                FAILED(webview3_->SetVirtualHostNameToFolderMapping(
                                    L"tekito.settings", uiFolder.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW))) {
                                FailWebView(Text(L"The local settings package could not be mapped.", L"設定画面のファイルを読み込めませんでした。"));
                                return S_OK;
                            }
                            webview_->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        BOOL success = FALSE;
                                        if (args) args->get_IsSuccess(&success);
                                        if (!success) {
                                            FailWebView(Text(L"The local settings page failed to navigate.", L"設定画面を表示できませんでした。"));
                                        } else {
                                            if (fallbackLabel_) ShowWindow(fallbackLabel_, SW_HIDE);
                                            if (fallbackRetry_) ShowWindow(fallbackRetry_, SW_HIDE);
                                            ResizeWebView();
                                        }
                                        return S_OK;
                                    }).Get(),
                                &navigationToken_);
                            webview_->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR message = nullptr;
                                        droppedFile_ = DroppedFile(args);
                                        if (args && SUCCEEDED(args->TryGetWebMessageAsString(&message)) &&
                                            message) {
                                            HandleWebMessage(message);
                                            CoTaskMemFree(message);
                                        }
                                        return S_OK;
                                    }).Get(),
                                &webMessageToken_);
                            RECT bounds{};
                            GetClientRect(hwnd_, &bounds);
                            controller_->put_Bounds(bounds);
                            controller_->put_IsVisible(TRUE);
                            const HRESULT navigationResult =
                                webview_->Navigate((L"https://tekito.settings/index.html" + StartPageHash()).c_str());
                            if (FAILED(navigationResult)) {
                                FailWebView(Text(L"The local settings page could not be opened.", L"設定画面を開けませんでした。"));
                            }
                            return S_OK;
                        }).Get());
            }).Get());
    if (FAILED(result)) {
        FailWebView(Text(L"WebView2 environment creation failed.", L"WebView2 の準備に失敗しました。"));
    }
}

// Title bar and the WebView's pre-paint background follow the Windows
// app theme, so a dark page never flashes white while it loads.
void SettingsApp::ApplyTheme() {
    const BOOL dark = AppsUseDarkTheme() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    ComPtr<ICoreWebView2Controller2> controller2;
    if (controller_ && SUCCEEDED(controller_.As(&controller2))) {
        const COREWEBVIEW2_COLOR background =
            dark ? COREWEBVIEW2_COLOR{255, 10, 16, 20} : COREWEBVIEW2_COLOR{255, 238, 243, 246};
        controller2->put_DefaultBackgroundColor(background);
    }
}

SettingsApp::Generations SettingsApp::CurrentGenerations() const {
    if (!runtime_) return {};
    return {runtime_->ModeGeneration(), runtime_->SettingsGeneration(),
            runtime_->DictionaryGeneration(), runtime_->LearningGeneration()};
}

// Picks up changes made by other processes and pushes the new state to
// the page. Changes this window made itself are already reflected in its
// reply, so the generations are re-read after every handled request.
void SettingsApp::CheckForExternalChanges() {
    if (!runtime_ || !repository_ || !webview_) return;
    const auto now = CurrentGenerations();
    if (now == seenGenerations_) return;
    if (now.settings != seenGenerations_.settings) {
        (void)repository_->LoadSettings(settings_);
    }
    if (now.dictionary != seenGenerations_.dictionary) {
        tekito::UserDictionary reloaded;
        if (repository_->Load(reloaded)) dictionary_ = std::move(reloaded);
    }
    if (now.learning != seenGenerations_.learning) {
        tekito::UserLearningStore reloaded;
        if (repository_->LoadLearning(reloaded)) learning_ = std::move(reloaded);
    }
    seenGenerations_ = now;
    PostState();
}

void SettingsApp::PostState() {
    if (!webview_) return;
    const auto message = L"{\"type\":\"state.changed\",\"state\":" + SettingsStateJson() + L"}";
    webview_->PostWebMessageAsString(message.c_str());
}

void SettingsApp::ResizeWebView() {
    RECT bounds{};
    GetClientRect(hwnd_, &bounds);
    if (controller_) controller_->put_Bounds(bounds);
    if (fallbackLabel_) {
        const int width = std::max(280L, bounds.right - bounds.left - 64);
        SetWindowPos(fallbackLabel_, nullptr, 32, 32, width, 64,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        if (fallbackRetry_) {
            SetWindowPos(fallbackRetry_, nullptr, (width - 120) / 2 + 32, 112,
                         120, 32, SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }
}

std::wstring SettingsApp::ResponseJson(bool ok, std::wstring_view error) {
    return L"{\"ok\":" + std::wstring(ok ? L"true" : L"false") +
           L",\"error\":\"" + JsonEscape(error) +
           L"\",\"state\":" + SettingsStateJson() + L"}";
}

// `extra` is additional JSON members for the reply, e.g. L",\"text\":\"...\"".
void SettingsApp::Reply(std::wstring_view requestId, bool ok, std::wstring_view error,
           std::wstring_view extra) {
    if (!webview_ || requestId.empty()) return;
    auto response = ResponseJson(ok, error);
    response.insert(response.size() - 1, extra);
    const auto message = L"{\"requestId\":\"" + JsonEscape(requestId) + L"\"," + response.substr(1);
    webview_->PostWebMessageAsString(message.c_str());
}

// The TEKITO license agreement, embedded in this executable.
std::wstring SettingsApp::LicenseText() const {
    const HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_LICENSE_TEXT), RT_RCDATA);
    const HGLOBAL loaded = resource ? LoadResource(instance_, resource) : nullptr;
    const DWORD size = resource ? SizeofResource(instance_, resource) : 0;
    const auto* bytes = loaded && size ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    if (!bytes) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), nullptr, 0);
    std::wstring text(static_cast<std::size_t>(std::max(count, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), text.data(), count);
    return text;
}

void SettingsApp::HandleWebMessage(std::wstring_view message) {
    HandleRequest(message);
    seenGenerations_ = CurrentGenerations();
}

void SettingsApp::HandleRequest(std::wstring_view json) {
    const auto message = WebMessage::Parse(json);
    const auto type = message.String(L"type");
    const auto requestId = message.String(L"requestId");
    // Each area answers its own requests (SettingsRequests.cpp,
    // SettingsWords.cpp, SettingsDownloads.cpp).
    if (HandleSettingsRequest(type, requestId, message)) return;
    if (HandleWordsRequest(type, requestId, message)) return;
    (void)HandleDownloadsRequest(type, requestId, message);
}

LRESULT CALLBACK SettingsApp::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<SettingsApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<SettingsApp*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_CREATE) {
        self->seenGenerations_ = self->CurrentGenerations();
        self->ApplyTheme();
        self->CreateWebViewStatusControls();
        self->InitializeWebView();
        SetTimer(hwnd, kStateWatchTimer, kStateWatchIntervalMs, nullptr);
        return 0;
    }
    if (message == WM_COPYDATA) {
        // A page to open, from a second "TEKITO.exe --page <page>": "#"
        // and a page's name, nothing else.
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!data || data->dwData != kOpenPage || !data->lpData || data->cbData > 64 * sizeof(wchar_t)) return FALSE;
        const std::wstring hash(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
        if (hash.size() < 2 || hash[0] != L'#' ||
            !std::all_of(hash.begin() + 1, hash.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; })) {
            return FALSE;
        }
        if (self->webview_) self->webview_->ExecuteScript((L"location.hash = '" + hash + L"';").c_str(), nullptr);
        return TRUE;
    }
    if (message == kAiWindowsMlInstalled) {
        self->FinishWindowsMlInstall(static_cast<tekito::userdata::AiModelInstallError>(wParam), static_cast<int>(lParam));
        return 0;
    }
    if (message == kJapaneseDataInstalled) {
        self->FinishJapaneseDataInstall(static_cast<tekito::userdata::AiModelInstallError>(wParam));
        return 0;
    }
    if (message == kAiNpuPrepared) {
        self->FinishNpuPrepare(wParam != 0);
        return 0;
    }
    if (message == kAiModelInstalled) {
        self->FinishAiModelInstall(std::unique_ptr<AiModelDownload>(reinterpret_cast<AiModelDownload*>(lParam)));
        return 0;
    }
    if (message == kPostalDownloaded) {
        self->FinishPostalDownload(std::unique_ptr<PostalDownload>(reinterpret_cast<PostalDownload*>(lParam)));
        return 0;
    }
    if (message == WM_TIMER && wParam == kStateWatchTimer) {
        self->CheckForExternalChanges();
        return 0;
    }
    if (message == WM_SETTINGCHANGE && lParam &&
        std::wstring_view(reinterpret_cast<const wchar_t*>(lParam)) == L"ImmersiveColorSet") {
        self->ApplyTheme();
        self->PostState();  // the accent color may have changed
        return 0;
    }
    if (message == WM_SIZE) {
        self->ResizeWebView();
        return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 760;
        limits->ptMinTrackSize.y = 520;
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == kRetryWebView && HIWORD(wParam) == BN_CLICKED) {
            self->InitializeWebView();
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(hwnd, kStateWatchTimer);
        self->ResetWebView();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Plain-text report for bug reports. It names components, versions and
// paths only; nothing the user typed is in it.
std::wstring SettingsApp::Diagnostics() const {
    const auto root = tekito::ExternalLexiconProvider::DataPackRoot();
    const auto registeredDll = RegisteredTsfDll();
    const bool registered = !registeredDll.empty() && std::filesystem::exists(registeredDll);
    std::wstring text = L"TEKITO\r\nVersion " + std::wstring(kVersion) + L"\r\n\r\n";
    text += L"Input method: " + std::wstring(registered ? L"Registered" : L"Not registered");
    text += L"\r\nLearning: " + std::wstring(settings_.learningEnabled ? L"On" : L"Off") + L" (" +
            std::to_wstring(learning_.Entries().size() + learning_.PreferenceCount()) +
            L" preferences)";
    text += L"\r\n\r\nData Packs";
    for (const auto& pack : kDataPacks) {
        tekito::userdata::DataPackStatus status;
        const bool valid = tekito::userdata::ValidateDataPack(tekito::FindDataPack(root, pack.folder), status);
        text += L"\r\n" + std::wstring(pack.folder) + L": " +
                (valid ? status.version : L"Unavailable (" + status.reason + L")");
    }
    text += L"\r\n\r\nPaths\r\nData: " + root.wstring();
    text += L"\r\nUser data: " + tekito::userdata::DefaultUserDatabasePath().wstring();
    text += L"\r\nInput method DLL: " + (registeredDll.empty() ? L"-" : registeredDll);
    text += L"\r\nLog folder: " + LogFolder().wstring();
    return text;
}

// Registers the input method again (regsvr32, elevated) so a changed
// keyboard type reaches the English profile's layout. Declining the
// prompt keeps the old layout until TEKITO is next installed.
void SettingsApp::ReregisterInputMethod() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    const auto dll =
        std::filesystem::path(modulePath, modulePath + length).parent_path() / L"Tekito.Tsf.dll";
    // Only a DLL in Program Files, which only administrators can change:
    // one in a folder the user's own programs can write to is never run
    // elevated.
    PWSTR programFiles = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles))) return;
    std::wstring protectedRoot = programFiles;
    CoTaskMemFree(programFiles);
    if (!protectedRoot.empty() && protectedRoot.back() != L'\\') protectedRoot += L'\\';
    const std::wstring dllPath = dll.wstring();
    if (dllPath.size() <= protectedRoot.size() ||
        CompareStringOrdinal(dllPath.c_str(), static_cast<int>(protectedRoot.size()), protectedRoot.c_str(),
                             static_cast<int>(protectedRoot.size()), TRUE) != CSTR_EQUAL) {
        return;
    }
    wchar_t system[MAX_PATH]{};
    if (!std::filesystem::exists(dll) || GetSystemDirectoryW(system, MAX_PATH) == 0) return;
    const auto regsvr32 = std::filesystem::path(system) / L"regsvr32.exe";
    const std::wstring parameters = L"/s \"" + dll.wstring() + L"\"";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = hwnd_;
    info.lpVerb = L"runas";
    info.lpFile = regsvr32.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return;
    if (info.hProcess) {
        WaitForSingleObject(info.hProcess, 30000);
        CloseHandle(info.hProcess);
    }
}

bool SettingsApp::SaveSettings() {
    if (runtime_) settings_.lastInputMode = runtime_->Mode();
    const bool saved = repository_ && repository_->SaveSettings(settings_);
    if (saved && runtime_) runtime_->NotifySettingsChanged();
    return saved;
}

bool SettingsApp::SetCurrentMode(tekito::InputMode mode, std::wstring& error) {
    const auto previousSettings = settings_;
    const auto previousMode = runtime_ ? runtime_->Mode() : settings_.lastInputMode;
    if (runtime_) runtime_->SetMode(mode);
    settings_.lastInputMode = mode;
    if (!repository_ || !repository_->SaveSettings(settings_)) {
        settings_ = previousSettings;
        if (runtime_) runtime_->SetMode(previousMode);
        error = Text(L"The current mode could not be saved.", L"入力モードを保存できませんでした。");
        return false;
    }
    if (runtime_) runtime_->NotifySettingsChanged();
    return true;
}

bool SettingsApp::SetCurrentJapaneseMode(tekito::InputMode mode, std::wstring& error) {
    const auto previousSettings = settings_;
    const auto previousMode = runtime_ ? runtime_->JapaneseMode() : settings_.lastJapaneseProfileMode;
    if (runtime_) runtime_->SetJapaneseMode(mode);
    settings_.lastJapaneseProfileMode = mode;
    if (!tekito::TypesJapanese(mode)) settings_.japaneseProfileEnglishMode = mode;
    if (!repository_ || !repository_->SaveSettings(settings_)) {
        settings_ = previousSettings;
        if (runtime_) runtime_->SetJapaneseMode(previousMode);
        error = Text(L"The current mode could not be saved.", L"入力モードを保存できませんでした。");
        return false;
    }
    if (runtime_) runtime_->NotifySettingsChanged();
    return true;
}

}  // namespace tekito::settings
