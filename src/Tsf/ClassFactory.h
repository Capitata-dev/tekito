#pragma once

#include <unknwn.h>
#include <atomic>

namespace tekito::tsf {

class ClassFactory final : public IClassFactory {
public:
    // A factory held by a client keeps the DLL loaded (DllCanUnloadNow).
    ClassFactory() noexcept;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) override;
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override;

private:
    ~ClassFactory();
    std::atomic<ULONG> refCount_{1};
};

}  // namespace tekito::tsf
