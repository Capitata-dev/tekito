#include "Tsf/TextService.h"
#include "Tsf/TextServiceShared.h"

// The text service itself: COM, activation, settings, input modes, the sinks
// it advises, the language bar and display attributes. Keys, English input,
// the composition and the candidate list are in the TextService*.cpp files by
// area.

namespace tekito::tsf {

TextService::TextService()
    : userLearningProvider_(userLearning_),
      socialLearningProvider_(socialLearning_),
      settingsRepository_(userdata::CreateDefaultUserDataRepository()),
      processIsElevated_(CurrentProcessIsElevated()),
      processName_(CurrentProcessName()) {
    processPrefersPassThrough_ = IsBuiltInPassThroughProcess(processName_);
    ++g_objectCount;
    Trace(L"TextService ctor");
}

TextService::~TextService() {
    Trace(L"TextService dtor");
    Deactivate();
    --g_objectCount;
}

HRESULT TextService::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor) {
        *object = static_cast<ITfTextInputProcessor*>(this);
    } else if (riid == IID_ITfTextInputProcessorEx) {
        *object = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (riid == IID_ITfKeyEventSink) {
        *object = static_cast<ITfKeyEventSink*>(this);
    } else if (riid == IID_ITfCompositionSink) {
        *object = static_cast<ITfCompositionSink*>(this);
    } else if (riid == IID_ITfDisplayAttributeProvider) {
        *object = static_cast<ITfDisplayAttributeProvider*>(this);
    } else if (riid == IID_ITfDisplayAttributeInfo) {
        *object = static_cast<ITfDisplayAttributeInfo*>(this);
    } else if (riid == IID_ITfThreadMgrEventSink) {
        *object = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (riid == IID_ITfTextEditSink) {
        *object = static_cast<ITfTextEditSink*>(this);
    } else if (riid == IID_ITfTextLayoutSink) {
        *object = static_cast<ITfTextLayoutSink*>(this);
    } else if (riid == IID_ITfCompartmentEventSink) {
        *object = static_cast<ITfCompartmentEventSink*>(this);
    } else if (riid == IID_ITfInputProcessorProfileActivationSink) {
        *object = static_cast<ITfInputProcessorProfileActivationSink*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG TextService::AddRef() { return ++refCount_; }

ULONG TextService::Release() {
    const ULONG value = --refCount_;
    if (value == 0) delete this;
    return value;
}

HRESULT TextService::Activate(ITfThreadMgr* threadManager, TfClientId clientId) {
    return ActivateEx(threadManager, clientId, 0);
}

HRESULT TextService::ActivateEx(ITfThreadMgr* threadManager, TfClientId clientId, DWORD flags) {
    Trace(L"ActivateEx");
    if (!threadManager) return E_INVALIDARG;
    Deactivate();

    threadManager_ = threadManager;
    threadManager_->AddRef();
    clientId_ = clientId;
    activateFlags_ = flags;

    ResetEngine();
    runtimeMode_.reset();
    userDictionary_ = {};
    userLearning_ = {};
    socialLearning_ = {};
    japaneseLearning_.Clear();
    userSettings_ = {};
    userLearningProvider_.SetEnabled(true);

    if (settingsRepository_ && settingsRepository_->Open()) {
        if (settingsRepository_->Load(userDictionary_)) {
            if (!settingsRepository_->LoadLearning(userLearning_)) {
                Trace(L"ActivateEx UserLearning load skipped");
            }
            if (!settingsRepository_->LoadSocialLearning(socialLearning_)) {
                Trace(L"ActivateEx SocialLearning load skipped");
            }
            if (!settingsRepository_->LoadJapaneseLearning(japaneseLearning_)) {
                Trace(L"ActivateEx JapaneseLearning load skipped");
            }
            if (!settingsRepository_->LoadJapaneseUserWords(japaneseUserWords_)) {
                Trace(L"ActivateEx Japanese user words load skipped");
            }
            Trace(L"ActivateEx UserDictionary loaded");
        } else {
            Trace(L"ActivateEx UserDictionary load skipped");
        }

        if (settingsRepository_->LoadSettings(userSettings_)) {
            settingsLoaded_ = true;
            ApplySettings();
        } else {
            settingsRepository_->Close();
            Trace(L"ActivateEx UserSettings load skipped");
        }
    } else {
        Trace(L"ActivateEx UserData repository open skipped");
    }

    // The English profile has only Auto and Direct.
    auto startupMode = userdata::StartMode(userSettings_, userSettings_.lastInputMode);
    if (startupMode != InputMode::Direct) startupMode = InputMode::Convert;
    const auto japaneseStartupMode = userdata::StartMode(userSettings_, userSettings_.lastJapaneseProfileMode);
    runtimeMode_ = std::make_unique<userdata::RuntimeModeState>(
        startupMode, userdata::kRuntimeStateName, japaneseStartupMode);
    UpdateActiveProfile();
    activationSettling_ = japaneseProfile_;
    mode_ = SharedMode();
    state_.SetInputMode(mode_ == InputMode::Convert ? InputMode::Convert : InputMode::Direct);
    runtimeModeGeneration_ = runtimeMode_->ModeGeneration();
    runtimeSettingsGeneration_ = runtimeMode_->SettingsGeneration();
    runtimeDictionaryGeneration_ = runtimeMode_->DictionaryGeneration();
    runtimeLearningGeneration_ = runtimeMode_->LearningGeneration();

    // Loading the language data takes seconds; never do it on the host
    // application's thread. Until it is ready, keys pass through untouched.
    ResetEngine();
    if (IsDefaultEngineDataLoaded()) {
        EnsureEngine();
    } else {
        StartEngineDataPreload();
    }

    ComPtr<ITfKeystrokeMgr> keystrokeManager;
    HRESULT hr = threadManager_->QueryInterface(IID_PPV_ARGS(keystrokeManager.Put()));
    if (FAILED(hr)) {
        Trace(L"ActivateEx QueryInterface ITfKeystrokeMgr failed");
        // Not left half active: what was set up is undone.
        (void)Deactivate();
        return hr;
    }

    hr = keystrokeManager->AdviseKeyEventSink(clientId_, this, TRUE);
    if (FAILED(hr)) {
        Trace(L"ActivateEx AdviseKeyEventSink failed");
        (void)Deactivate();
        return hr;
    }

    UpdateToggleKey(userdata::PreservedSwitchKey(userSettings_.toggleKey, userSettings_.keyboardType));
    UpdateKanaInputKey(userSettings_.japaneseKanaInputKey && JapaneseModeAvailable());
    UpdateAltHook(userSettings_.altSwitches);
    AdviseThreadManagerSinks();
    SyncModeCompartments();

    RegisterLangBarItem();
    candidateWindow_.Initialize(g_moduleInstance, [this](std::size_t index) {
        OnCandidateSelected(index);
    });
    candidateElement_ = new (std::nothrow) CandidateListElement(threadManager_, [this](std::size_t index) {
        OnCandidateSelected(index);
    });
    candidateWindow_.SetListObserver(
        [this](const std::vector<Candidate>& candidates, std::size_t selected, std::size_t pageStart,
               std::size_t visible) {
            return !candidateElement_ || candidateElement_->OnListShown(candidates, selected, pageStart, visible);
        },
        [this]() {
            if (candidateElement_) candidateElement_->OnListHidden();
        });
    Trace(L"ActivateEx ready");
    return S_OK;
}

HRESULT TextService::Deactivate() {
    Trace(L"Deactivate");
    CancelJapaneseRefine();
    StopWatchingAiAnswer();
    StopJapaneseRedisplay();
    // A composition still open is committed as it stands (and its underline
    // taken off) before the text service goes, rather than left behind.
    if (composition_ && compositionContext_ && clientId_ != TF_CLIENTID_NULL) {
        KeyInput input{};
        input.type = japanese_.IsComposing() ? KeyInput::Type::JapaneseCommit : KeyInput::Type::EndComposition;
        if (auto* session = new (std::nothrow) KeyEditSession(this, compositionContext_, input)) {
            HRESULT sessionResult = E_FAIL;
            (void)compositionContext_->RequestEditSession(clientId_, session, TF_ES_SYNC | TF_ES_READWRITE,
                                                          &sessionResult);
            session->Release();
        }
    }
    if (settingsLoaded_) SyncRuntimeState();
    if (settingsLoaded_) {
        userdata::UserSettings latestSettings = userSettings_;
        if (settingsRepository_) (void)settingsRepository_->LoadSettings(latestSettings);
        latestSettings.lastInputMode = runtimeMode_ ? runtimeMode_->Mode() : state_.Mode();
        if (runtimeMode_) latestSettings.lastJapaneseProfileMode = runtimeMode_->JapaneseMode();
        latestSettings.japaneseProfileEnglishMode = userSettings_.japaneseProfileEnglishMode;
        userSettings_ = latestSettings;
        if (!settingsRepository_ || !settingsRepository_->SaveSettings(latestSettings)) {
            Trace(L"Deactivate UserSettings save failed");
        }
        if (!settingsRepository_->SaveLearning(userLearning_)) {
            Trace(L"Deactivate UserLearning save failed");
        }
        if (!settingsRepository_->SaveSocialLearning(socialLearning_)) {
            Trace(L"Deactivate SocialLearning save failed");
        }
        if (!settingsRepository_->SaveJapaneseLearning(japaneseLearning_)) {
            Trace(L"Deactivate JapaneseLearning save failed");
        }
        if (settingsRepository_) settingsRepository_->Close();
        settingsLoaded_ = false;
    }
    candidateWindow_.Hide();
    candidateWindow_.SetListObserver(nullptr, nullptr);
    if (candidateElement_) {
        candidateElement_->Detach();
        candidateElement_->Release();
        candidateElement_ = nullptr;
    }
    state_.Reset();
    rawText_.clear();
    japanese_.Clear();
    UnregisterLangBarItem();
    UnadviseContextSinks();
    UnadviseThreadManagerSinks();
    UpdateToggleKey(0);
    UpdateKanaInputKey(false);
    UpdateAltHook(false);

    if (threadManager_ && clientId_ != TF_CLIENTID_NULL) {
        ComPtr<ITfKeystrokeMgr> keystrokeManager;
        if (SUCCEEDED(threadManager_->QueryInterface(IID_PPV_ARGS(keystrokeManager.Put())))) {
            keystrokeManager->UnadviseKeyEventSink(clientId_);
        }
    }

    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (compositionContext_) {
        compositionContext_->Release();
        compositionContext_ = nullptr;
    }
    if (threadManager_) {
        threadManager_->Release();
        threadManager_ = nullptr;
    }
    clientId_ = TF_CLIENTID_NULL;
    activateFlags_ = 0;
    runtimeMode_.reset();
    runtimeModeGeneration_ = 0;
    runtimeSettingsGeneration_ = 0;
    runtimeDictionaryGeneration_ = 0;
    runtimeLearningGeneration_ = 0;
    return S_OK;
}

void TextService::SyncRuntimeState() {
    if (!runtimeMode_) return;
    const auto settingsGeneration = runtimeMode_->SettingsGeneration();
    if (settingsGeneration != runtimeSettingsGeneration_ && settingsLoaded_ &&
        settingsRepository_) {
        userdata::UserSettings loadedSettings;
        if (settingsRepository_->LoadSettings(loadedSettings)) {
            userSettings_ = std::move(loadedSettings);
            ApplySettings();
            // Japanese, or mixed typing, was turned off in Settings.
            if (const auto available = AvailableMode(mode_); available != mode_) ChangeInputMode(available);
        }
        runtimeSettingsGeneration_ = settingsGeneration;
    }

    const auto dictionaryGeneration = runtimeMode_->DictionaryGeneration();
    if (dictionaryGeneration != runtimeDictionaryGeneration_ && settingsLoaded_ &&
        settingsRepository_) {
        UserDictionary loadedDictionary;
        if (settingsRepository_->Load(loadedDictionary)) {
            // The engine reads userDictionary_ by reference; refresh it in
            // place rather than rebuilding (a rebuild reloads every Data
            // Pack and stalls typing for seconds).
            userDictionary_ = std::move(loadedDictionary);
            if (candidateEngine_) candidateEngine_->RefreshUserDictionary();
        }
        std::vector<japanese::UserWord> loadedWords;
        if (settingsRepository_->LoadJapaneseUserWords(loadedWords)) {
            japaneseUserWords_ = std::move(loadedWords);
            if (japaneseProfile_) RefreshJapaneseUserWords();
        }
        // The romaji table's changes are kept with the user's words.
        if (japaneseProfile_) japanese::ReloadUserRomaji();
        runtimeDictionaryGeneration_ = dictionaryGeneration;
    }

    const auto learningGeneration = runtimeMode_->LearningGeneration();
    if (learningGeneration != runtimeLearningGeneration_ && settingsLoaded_ &&
        settingsRepository_) {
        // The learning providers read these stores by reference, so
        // replacing their contents is enough; no engine rebuild.
        UserLearningStore loadedLearning;
        if (settingsRepository_->LoadLearning(loadedLearning)) {
            userLearning_ = std::move(loadedLearning);
        }
        SocialLearningStore loadedSocialLearning;
        if (settingsRepository_->LoadSocialLearning(loadedSocialLearning)) {
            socialLearning_ = std::move(loadedSocialLearning);
        }
        japanese::JapaneseLearningStore loadedJapaneseLearning;
        if (settingsRepository_->LoadJapaneseLearning(loadedJapaneseLearning)) {
            japaneseLearning_ = std::move(loadedJapaneseLearning);
        }
        runtimeLearningGeneration_ = learningGeneration;
    }

    const auto modeGeneration = runtimeMode_->ModeGeneration();
    if (modeGeneration != runtimeModeGeneration_) {
        // Each app keeps the mode it was left in, if Settings says so; a mode
        // changed here is still shared, for apps that start next.
        if (!userSettings_.modePerApp) SetInputMode(SharedMode());
        // Secret mode turned on or off shows on the button too.
        if (modeLangBarItem_) modeLangBarItem_->NotifyUpdate();
        runtimeModeGeneration_ = modeGeneration;
    }
}

bool TextService::EnsureEngine() {
    if (candidateEngine_) return true;
    if (!IsDefaultEngineDataLoaded()) return false;
    candidateEngine_ = CreateDefaultConversionEngine(userDictionary_, userLearningProvider_,
                                                     socialLearningProvider_);
    if (!candidateEngine_) {
        Trace(L"Engine creation failed");
        return false;
    }
    englishSpelling_ = std::make_unique<EnglishSpelling>(*candidateEngine_);
    englishSpelling_->SetVariety(static_cast<EnglishVariety>(std::clamp(userSettings_.englishSpelling, 0, 2)));
    japanese_.SetEnglishSpelling(englishSpelling_.get());
    return true;
}

void TextService::ResetEngine() {
    japanese_.SetEnglishSpelling(nullptr);
    englishSpelling_.reset();
    candidateEngine_.reset();
}

void TextService::ApplySettings() {
    UpdateAltHook(userSettings_.altSwitches);
    userLearningProvider_.SetEnabled(userSettings_.learningEnabled);
    socialLearningProvider_.SetProfile(userSettings_.socialExpressionRange,
                                       userSettings_.socialPersonalization);
    const bool wasPassingThrough = ProcessPassesThrough();
    processExcludedByUser_ = std::any_of(
        userSettings_.excludedApps.begin(), userSettings_.excludedApps.end(),
        [&](const std::wstring& name) { return _wcsicmp(name.c_str(), processName_.c_str()) == 0; });
    if (!wasPassingThrough && ProcessPassesThrough() && state_.IsActive()) {
        FinishCompositionLater();
    }
    if (threadManager_) {
        UpdateToggleKey(userdata::PreservedSwitchKey(userSettings_.toggleKey, userSettings_.keyboardType));
        UpdateKanaInputKey(userSettings_.japaneseKanaInputKey && JapaneseModeAvailable());
    }
    userdata::ApplyJapaneseSettings(japanese_, userSettings_, &japaneseLearning_, mode_ == InputMode::Mixed);
    if (englishSpelling_) {
        englishSpelling_->SetVariety(static_cast<EnglishVariety>(std::clamp(userSettings_.englishSpelling, 0, 2)));
    }
    const auto rows = static_cast<std::size_t>(userSettings_.candidateRows);
    state_.SetPageSizes(rows ? rows : 5, rows ? rows : 10);
    state_.SetDoubleSpacePeriod(userSettings_.doubleSpacePeriod);
    japanesePage_ = userdata::JapanesePageSize(userSettings_);
}

void TextService::SetInputMode(InputMode mode) {
    // Only the Japanese profile has Japanese and Mix, each when it is on.
    mode = AvailableMode(mode);
    englishSegment_ = false;
    const bool finishWord = (mode != InputMode::Convert && state_.IsActive()) ||
                            (mode != mode_ && japanese_.IsComposing());
    mode_ = mode;
    japanese::AttachMixedTyping(japanese_, mode == InputMode::Mixed);
    userdata::ApplyModeSettings(japanese_, userSettings_, mode == InputMode::Mixed);
    state_.SetInputMode(mode == InputMode::Convert ? InputMode::Convert : InputMode::Direct);
    if (japaneseProfile_) {
        userSettings_.lastJapaneseProfileMode = mode;
        if (!TypesJapanese(mode)) userSettings_.japaneseProfileEnglishMode = mode;
    } else {
        userSettings_.lastInputMode = mode;
    }
    if (finishWord) FinishCompositionLater();
    SyncModeCompartments();
    if (modeLangBarItem_) modeLangBarItem_->NotifyUpdate();
}

void TextService::ChangeInputMode(InputMode mode) {
    activationSettling_ = false;
    if (runtimeMode_) {
        if (japaneseProfile_) {
            runtimeMode_->SetJapaneseMode(mode);
        } else {
            runtimeMode_->SetMode(mode == InputMode::Direct ? InputMode::Direct : InputMode::Convert);
        }
        runtimeModeGeneration_ = runtimeMode_->ModeGeneration();
    }
    SetInputMode(mode);
}

void TextService::UpdateToggleKey(int key) {
    if (ProcessPassesThrough()) key = 0;
    if (key == preservedToggleKey_ || !threadManager_ || clientId_ == TF_CLIENTID_NULL) return;
    ComPtr<ITfKeystrokeMgr> keystrokeManager;
    if (FAILED(threadManager_->QueryInterface(IID_PPV_ARGS(keystrokeManager.Put())))) return;

    TF_PRESERVEDKEY preserved{};
    if (ToggleKeyFor(preservedToggleKey_, preserved)) {
        keystrokeManager->UnpreserveKey(GUID_TekitoToggleKey, &preserved);
    }
    preservedToggleKey_ = 0;
    if (ToggleKeyFor(key, preserved)) {
        constexpr wchar_t kDescription[] = L"Turn TEKITO on or off";
        const HRESULT hr = keystrokeManager->PreserveKey(
            clientId_, GUID_TekitoToggleKey, &preserved, kDescription,
            static_cast<ULONG>(std::size(kDescription) - 1));
        if (SUCCEEDED(hr)) {
            preservedToggleKey_ = key;
        } else {
            TraceHr(L"PreserveKey failed", hr);
        }
    }
}

void TextService::UpdateKanaInputKey(bool on) {
    if (ProcessPassesThrough() || !japaneseProfile_) on = false;
    if (on == kanaInputKeyPreserved_ || !threadManager_ || clientId_ == TF_CLIENTID_NULL) return;
    ComPtr<ITfKeystrokeMgr> keystrokeManager;
    if (FAILED(threadManager_->QueryInterface(IID_PPV_ARGS(keystrokeManager.Put())))) return;
    for (const auto& key : kKanaInputKeys) {
        if (on) {
            constexpr wchar_t kDescription[] = L"Kana input on or off";
            (void)keystrokeManager->PreserveKey(clientId_, GUID_TekitoKanaInputKey, &key, kDescription,
                                                static_cast<ULONG>(std::size(kDescription) - 1));
        } else {
            (void)keystrokeManager->UnpreserveKey(GUID_TekitoKanaInputKey, &key);
        }
    }
    kanaInputKeyPreserved_ = on;
}

void TextService::ToggleKanaInput(ITfContext* context) {
    // What is being typed is committed as it stands first, as a mode key does.
    if (context && japanese_.IsComposing()) {
        KeyInput commit{};
        commit.type = KeyInput::Type::JapaneseCommit;
        [[maybe_unused]] const auto ignored = RequestKeyEditSession(context, commit);
    }
    userSettings_.japaneseKanaInput = !userSettings_.japaneseKanaInput;
    // Kept, and every app told: they read the settings again.
    if (settingsRepository_ && settingsRepository_->SaveSettings(userSettings_) && runtimeMode_) {
        runtimeMode_->NotifySettingsChanged();
        runtimeSettingsGeneration_ = runtimeMode_->SettingsGeneration();
    }
}

void TextService::AdviseThreadManagerSinks() {
    if (!threadManager_) return;
    if (threadManagerSinkCookie_ == TF_INVALID_COOKIE) {
        (void)AdviseSink(threadManager_, IID_ITfThreadMgrEventSink,
                         static_cast<ITfThreadMgrEventSink*>(this), threadManagerSinkCookie_);
    }
    if (openCloseSinkCookie_ == TF_INVALID_COOKIE) {
        if (auto compartment = OpenCloseCompartment(threadManager_)) {
            (void)AdviseSink(compartment.Get(), IID_ITfCompartmentEventSink,
                             static_cast<ITfCompartmentEventSink*>(this), openCloseSinkCookie_);
        }
    }
    if (conversionSinkCookie_ == TF_INVALID_COOKIE) {
        if (auto compartment = ConversionCompartment(threadManager_)) {
            (void)AdviseSink(compartment.Get(), IID_ITfCompartmentEventSink,
                             static_cast<ITfCompartmentEventSink*>(this), conversionSinkCookie_);
        }
    }
    if (profileSinkCookie_ == TF_INVALID_COOKIE) {
        (void)AdviseSink(threadManager_, IID_ITfInputProcessorProfileActivationSink,
                         static_cast<ITfInputProcessorProfileActivationSink*>(this),
                         profileSinkCookie_);
    }

    ComPtr<ITfDocumentMgr> focus;
    ComPtr<ITfContext> top;
    if (SUCCEEDED(threadManager_->GetFocus(focus.Put())) && focus &&
        SUCCEEDED(focus->GetTop(top.Put())) && top) {
        AdviseContextSinks(top.Get());
    }
}

void TextService::UnadviseThreadManagerSinks() {
    UnadviseSink(threadManager_, threadManagerSinkCookie_);
    UnadviseSink(threadManager_, profileSinkCookie_);
    auto compartment = OpenCloseCompartment(threadManager_);
    UnadviseSink(compartment.Get(), openCloseSinkCookie_);
    auto conversion = ConversionCompartment(threadManager_);
    UnadviseSink(conversion.Get(), conversionSinkCookie_);
}

void TextService::AdviseContextSinks(ITfContext* context) {
    if (!context || sinkContext_.Get() == context) return;
    UnadviseContextSinks();
    context->AddRef();
    sinkContext_.Attach(context);
    (void)AdviseSink(context, IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this),
                     textEditSinkCookie_);
    (void)AdviseSink(context, IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink*>(this),
                     textLayoutSinkCookie_);
}

void TextService::UnadviseContextSinks() {
    UnadviseSink(sinkContext_.Get(), textEditSinkCookie_);
    UnadviseSink(sinkContext_.Get(), textLayoutSinkCookie_);
    sinkContext_.Reset();
}

void TextService::NoteConversion(std::wstring reading, std::wstring text) {
    constexpr std::size_t kKept = 30;
    if (!userSettings_.keepRecentConversions || privateField_ || reading.empty() || text.empty() || !settingsRepository_) {
        return;
    }
    userdata::RecentConversion conversion;
    conversion.reading = std::move(reading);
    conversion.text = std::move(text);
    conversion.mode = mode_ == InputMode::Mixed ? L"Mix" : L"Japanese";
    conversion.time = static_cast<std::int64_t>(std::time(nullptr));
    (void)settingsRepository_->AddRecentConversion(conversion, kKept);
}

void TextService::OpenSettings() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_moduleInstance, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        Trace(L"OpenSettings could not resolve the TSF module path");
        return;
    }
    // TEKITO.exe sits beside the DLL, both installed and in a build folder.
    const auto settings =
        std::filesystem::path(modulePath, modulePath + length).parent_path() / L"TEKITO.exe";
    const auto result = ShellExecuteW(nullptr, L"open", settings.c_str(), L"--settings",
                                      settings.parent_path().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        Trace(L"OpenSettings ShellExecute failed");
    }
}

void TextService::RegisterLangBarItem() {
    if (langBarItemMgr_) return;

    ComPtr<ITfLangBarItemMgr> manager;
    if (!threadManager_ ||
        FAILED(threadManager_->QueryInterface(IID_PPV_ARGS(manager.Put())))) {
        Trace(L"RegisterLangBarItem manager query failed");
        return;
    }

    auto* item = new (std::nothrow) ModeLangBarItem(
        g_moduleInstance,
        [this]() { return mode_; },
        [this](InputMode mode) {
            ChangeInputMode(mode);
            ShowModeIndicator(nullptr);
        },
        [this]() { return ToggledMode(); },
        [this]() { return JapaneseModeAvailable(); },
        [this]() { OpenSettings(); },
        [this]() { return userdata::UseJapaneseUi(userSettings_.uiLanguage); },
        [this]() { return MixedModeAvailable(); },
        [this]() { return runtimeMode_ && runtimeMode_->Secret(); },
        [this](bool on) {
            if (runtimeMode_) runtimeMode_->SetSecret(on);
        },
        [this]() { return privateWindowShown_; });
    if (!item) return;

    ComPtr<ModeLangBarItem> ownedItem(item);
    if (FAILED(manager->AddItem(ownedItem.Get()))) {
        Trace(L"RegisterLangBarItem AddItem failed");
        return;
    }

    Trace(L"RegisterLangBarItem AddItem succeeded");
    langBarItemMgr_ = std::move(manager);
    modeLangBarItem_ = std::move(ownedItem);
}

void TextService::UnregisterLangBarItem() {
    if (langBarItemMgr_ && modeLangBarItem_) {
        langBarItemMgr_->RemoveItem(modeLangBarItem_.Get());
    }
    modeLangBarItem_.Reset();
    langBarItemMgr_.Reset();
}

HRESULT TextService::OnSetFocus(BOOL foreground) {
    if (foreground) {
        SyncRuntimeState();
    }
    if (!foreground) {
        candidateWindow_.Hide();
    }
    return S_OK;
}

HRESULT TextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }

HRESULT TextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }

HRESULT TextService::OnPushContext(ITfContext*) { return S_OK; }

HRESULT TextService::OnPopContext(ITfContext* context) {
    if (context && context == sinkContext_.Get()) UnadviseContextSinks();
    // The context the composition is in goes away: nothing more can be
    // written there, and the typing and its list go with it.
    if (context && context == compositionContext_) {
        if (composition_) {
            composition_->Release();
            composition_ = nullptr;
        }
        compositionContext_->Release();
        compositionContext_ = nullptr;
        compositionText_.clear();
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        symbolComposition_ = false;
        japanese_.Clear();
    }
    return S_OK;
}

HRESULT TextService::OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr*) {
    japanese_.ForgetCommit();
    altTap_.reset();
    // Another field: whether it is private is seen as it is typed in.
    if (std::exchange(privateWindowShown_, false) && modeLangBarItem_) modeLangBarItem_->NotifyUpdate();
    badgeOnNextKey_ = focus != nullptr;
    typedTail_.clear();
    // Light or dark taskbar: the mode icon follows when focus moves.
    if (modeLangBarItem_) modeLangBarItem_->RefreshTheme();
    ComPtr<ITfContext> top;
    if (focus && SUCCEEDED(focus->GetTop(top.Put())) && top) {
        AdviseContextSinks(top.Get());
    } else {
        UnadviseContextSinks();
    }

    // A word left behind in a document that lost focus is committed as typed.
    if (compositionContext_) {
        ComPtr<ITfDocumentMgr> owner;
        if (FAILED(compositionContext_->GetDocumentMgr(owner.Put())) || owner.Get() != focus) {
            FinishCompositionLater();
        }
    } else {
        candidateWindow_.Hide();
    }
    return S_OK;
}

HRESULT TextService::OnEndEdit(ITfContext* context, TfEditCookie readOnlyCookie,
                               ITfEditRecord* editRecord) {
    if (!composition_ || !editRecord || context != compositionContext_) return S_OK;
    BOOL selectionChanged = FALSE;
    if (FAILED(editRecord->GetSelectionStatus(&selectionChanged)) || !selectionChanged) {
        return S_OK;
    }

    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(readOnlyCookie, TF_DEFAULT_SELECTION, 1, &selection,
                                     &fetched)) ||
        fetched != 1 || !selection.range) {
        return S_OK;
    }
    ComPtr<ITfRange> selectionRange(selection.range);
    ComPtr<ITfRange> compositionRange;
    if (FAILED(composition_->GetRange(compositionRange.Put()))) return S_OK;

    // A click or another edit moved the caret out of the word.
    LONG startOrder = 0;
    LONG endOrder = 0;
    if (SUCCEEDED(selectionRange->CompareStart(readOnlyCookie, compositionRange.Get(),
                                               TF_ANCHOR_START, &startOrder)) &&
        SUCCEEDED(selectionRange->CompareEnd(readOnlyCookie, compositionRange.Get(),
                                             TF_ANCHOR_END, &endOrder)) &&
        (startOrder < 0 || endOrder > 0)) {
        FinishCompositionLater();
    }
    return S_OK;
}

HRESULT TextService::OnLayoutChange(ITfContext* context, TfLayoutCode code, ITfContextView*) {
    if (!context || context != compositionContext_ || !candidateWindow_.IsShown()) return S_OK;
    if (code == TF_LC_DESTROY) {
        candidateWindow_.Hide();
        return S_OK;
    }
    KeyInput input{};
    input.type = KeyInput::Type::Reposition;
    auto* session = new (std::nothrow) KeyEditSession(this, context, input);
    if (!session) return S_OK;
    HRESULT sessionResult = E_FAIL;
    (void)context->RequestEditSession(clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READ,
                                      &sessionResult);
    session->Release();
    return S_OK;
}

HRESULT TextService::OnCompositionTerminated(TfEditCookie, ITfComposition* composition) {
    Trace(L"OnCompositionTerminated");
    // An earlier composition ending while another one goes on leaves the
    // one that goes on alone.
    if (composition_ && composition && composition_ != composition) return S_OK;
    if (composition_ == composition) {
        composition_->Release();
        composition_ = nullptr;
        NoteTyped(compositionText_);
        compositionText_.clear();
    }
    if (compositionContext_) {
        compositionContext_->Release();
        compositionContext_ = nullptr;
    }
    candidateWindow_.Hide();
    state_.Reset();
    rawText_.clear();
    symbolComposition_ = false;
    japanese_.Clear();
    return S_OK;
}

HRESULT TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** enumInfo) {
    if (!enumInfo) return E_INVALIDARG;
    std::vector<ITfDisplayAttributeInfo*> infos{static_cast<ITfDisplayAttributeInfo*>(this)};
    for (int i = 0; i < kJapaneseUnderlineCount; ++i) {
        if (auto* info = CreateJapaneseUnderline(static_cast<JapaneseUnderline>(i))) infos.push_back(info);
    }
    *enumInfo = new (std::nothrow) DisplayAttributeInfoEnum(infos);
    // The enumerator holds its own references to the underlines made here.
    for (std::size_t i = 1; i < infos.size(); ++i) infos[i]->Release();
    return *enumInfo ? S_OK : E_OUTOFMEMORY;
}

HRESULT TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) {
    if (!info) return E_INVALIDARG;
    *info = nullptr;
    if (guid == GUID_TekitoDisplayAttribute) {
        *info = static_cast<ITfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    for (int i = 0; i < kJapaneseUnderlineCount; ++i) {
        const auto underline = static_cast<JapaneseUnderline>(i);
        if (guid == JapaneseUnderlineGuid(underline)) {
            *info = CreateJapaneseUnderline(underline);
            return *info ? S_OK : E_OUTOFMEMORY;
        }
    }
    return E_INVALIDARG;
}

HRESULT TextService::GetGUID(GUID* guid) {
    if (!guid) return E_INVALIDARG;
    *guid = GUID_TekitoDisplayAttribute;
    return S_OK;
}

HRESULT TextService::GetDescription(BSTR* description) {
    if (!description) return E_INVALIDARG;
    *description = SysAllocString(L"TEKITO composing underline");
    return *description ? S_OK : E_OUTOFMEMORY;
}

HRESULT TextService::GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) {
    if (!attribute) return E_INVALIDARG;
    *attribute = {};
    attribute->crText.type = TF_CT_NONE;
    attribute->crBk.type = TF_CT_NONE;
    attribute->lsStyle = TF_LS_DOT;
    attribute->fBoldLine = FALSE;
    attribute->crLine.type = TF_CT_SYSCOLOR;
    attribute->crLine.nIndex = COLOR_WINDOWTEXT;
    attribute->bAttr = TF_ATTR_INPUT;
    return S_OK;
}

HRESULT TextService::SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) {
    return E_NOTIMPL;
}

HRESULT TextService::Reset() {
    return S_OK;
}

}  // namespace tekito::tsf
