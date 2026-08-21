// TasomachiVR - dsound.dll proxy.
//
// tasomachi-Win64-Shipping.exe statically imports DSOUND.dll, so dropping a
// dsound.dll next to it gets us loaded before the process entry point runs.
// dsound.dll is not a KnownDLL, so the application directory wins the search.
//
// Static export forwarders (/EXPORT:Foo=dsound.Foo) are not usable here: the
// forwarded module name would resolve back to this very DLL. So every export is
// a real function that resolves the system dsound.dll lazily and tail-calls it.
// Resolution is deliberately NOT done in DllMain (LoadLibrary under the loader
// lock); no export can be called before DllMain has returned anyway.

#include "common.hpp"

#include <mmreg.h> // WAVEFORMATEX, which dsound.h needs and WIN32_LEAN_AND_MEAN hides
#include <dsound.h>

namespace {

struct RealDSound {
    decltype(&::DirectSoundCreate) DirectSoundCreate;
    decltype(&::DirectSoundEnumerateA) DirectSoundEnumerateA;
    decltype(&::DirectSoundEnumerateW) DirectSoundEnumerateW;
    decltype(&::DirectSoundCaptureCreate) DirectSoundCaptureCreate;
    decltype(&::DirectSoundCaptureEnumerateA) DirectSoundCaptureEnumerateA;
    decltype(&::DirectSoundCaptureEnumerateW) DirectSoundCaptureEnumerateW;
    decltype(&::DirectSoundCreate8) DirectSoundCreate8;
    decltype(&::DirectSoundCaptureCreate8) DirectSoundCaptureCreate8;
    decltype(&::DirectSoundFullDuplexCreate) DirectSoundFullDuplexCreate;
    decltype(&::GetDeviceID) GetDeviceID;
    HRESULT(WINAPI* DllCanUnloadNow)();
    HRESULT(WINAPI* DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
};

RealDSound g_real{};
std::once_flag g_resolve_once;

void resolve_real_dsound() {
    wchar_t system_dir[MAX_PATH]{};
    if (GetSystemDirectoryW(system_dir, static_cast<UINT>(std::size(system_dir))) == 0) {
        tasomachivr::log("GetSystemDirectory failed (%lu)", GetLastError());
        return;
    }

    const auto path = tasomachivr::fs::path{system_dir} / L"dsound.dll";
    const HMODULE mod = LoadLibraryW(path.c_str());
    if (mod == nullptr) {
        tasomachivr::log("Could not load the real %ls (%lu)", path.c_str(), GetLastError());
        return;
    }

#define TASOMACHIVR_BIND(name)                                                                        \
    g_real.name = reinterpret_cast<decltype(g_real.name)>(GetProcAddress(mod, #name));             \
    if (g_real.name == nullptr) {                                                                  \
        tasomachivr::log("dsound.dll export missing: %s", #name);                                     \
    }

    TASOMACHIVR_BIND(DirectSoundCreate)
    TASOMACHIVR_BIND(DirectSoundEnumerateA)
    TASOMACHIVR_BIND(DirectSoundEnumerateW)
    TASOMACHIVR_BIND(DirectSoundCaptureCreate)
    TASOMACHIVR_BIND(DirectSoundCaptureEnumerateA)
    TASOMACHIVR_BIND(DirectSoundCaptureEnumerateW)
    TASOMACHIVR_BIND(DirectSoundCreate8)
    TASOMACHIVR_BIND(DirectSoundCaptureCreate8)
    TASOMACHIVR_BIND(DirectSoundFullDuplexCreate)
    TASOMACHIVR_BIND(GetDeviceID)
    TASOMACHIVR_BIND(DllCanUnloadNow)
    TASOMACHIVR_BIND(DllGetClassObject)

#undef TASOMACHIVR_BIND

    tasomachivr::log("System dsound.dll bound at %p", static_cast<void*>(mod));
}

const RealDSound& real() {
    std::call_once(g_resolve_once, resolve_real_dsound);
    return g_real;
}

} // namespace

#define TASOMACHIVR_FORWARD(name, ...)                                                                \
    do {                                                                                           \
        const auto fn = real().name;                                                               \
        if (fn == nullptr) {                                                                       \
            return DSERR_GENERIC;                                                                  \
        }                                                                                          \
        return fn(__VA_ARGS__);                                                                    \
    } while (false)

extern "C" {

HRESULT WINAPI DirectSoundCreate(LPCGUID pcGuidDevice, LPDIRECTSOUND* ppDS, LPUNKNOWN pUnkOuter) {
    TASOMACHIVR_FORWARD(DirectSoundCreate, pcGuidDevice, ppDS, pUnkOuter);
}

HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA pDSEnumCallback, LPVOID pContext) {
    TASOMACHIVR_FORWARD(DirectSoundEnumerateA, pDSEnumCallback, pContext);
}

HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW pDSEnumCallback, LPVOID pContext) {
    TASOMACHIVR_FORWARD(DirectSoundEnumerateW, pDSEnumCallback, pContext);
}

HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID pcGuidDevice, LPDIRECTSOUNDCAPTURE* ppDSC,
                                        LPUNKNOWN pUnkOuter) {
    TASOMACHIVR_FORWARD(DirectSoundCaptureCreate, pcGuidDevice, ppDSC, pUnkOuter);
}

HRESULT WINAPI DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA pDSEnumCallback, LPVOID pContext) {
    TASOMACHIVR_FORWARD(DirectSoundCaptureEnumerateA, pDSEnumCallback, pContext);
}

HRESULT WINAPI DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW pDSEnumCallback, LPVOID pContext) {
    TASOMACHIVR_FORWARD(DirectSoundCaptureEnumerateW, pDSEnumCallback, pContext);
}

HRESULT WINAPI DirectSoundCreate8(LPCGUID pcGuidDevice, LPDIRECTSOUND8* ppDS8,
                                  LPUNKNOWN pUnkOuter) {
    TASOMACHIVR_FORWARD(DirectSoundCreate8, pcGuidDevice, ppDS8, pUnkOuter);
}

HRESULT WINAPI DirectSoundCaptureCreate8(LPCGUID pcGuidDevice, LPDIRECTSOUNDCAPTURE8* ppDSC8,
                                         LPUNKNOWN pUnkOuter) {
    TASOMACHIVR_FORWARD(DirectSoundCaptureCreate8, pcGuidDevice, ppDSC8, pUnkOuter);
}

HRESULT WINAPI DirectSoundFullDuplexCreate(LPCGUID pcGuidCaptureDevice, LPCGUID pcGuidRenderDevice,
                                           LPCDSCBUFFERDESC pcDSCBufferDesc,
                                           LPCDSBUFFERDESC pcDSBufferDesc, HWND hWnd, DWORD dwLevel,
                                           LPDIRECTSOUNDFULLDUPLEX* ppDSFD,
                                           LPDIRECTSOUNDCAPTUREBUFFER8* ppDSCBuffer8,
                                           LPDIRECTSOUNDBUFFER8* ppDSBuffer8, LPUNKNOWN pUnkOuter) {
    TASOMACHIVR_FORWARD(DirectSoundFullDuplexCreate, pcGuidCaptureDevice, pcGuidRenderDevice,
                     pcDSCBufferDesc, pcDSBufferDesc, hWnd, dwLevel, ppDSFD, ppDSCBuffer8,
                     ppDSBuffer8, pUnkOuter);
}

HRESULT WINAPI GetDeviceID(LPCGUID pGuidSrc, LPGUID pGuidDest) {
    TASOMACHIVR_FORWARD(GetDeviceID, pGuidSrc, pGuidDest);
}

HRESULT WINAPI DllCanUnloadNow() {
    const auto fn = real().DllCanUnloadNow;
    return fn != nullptr ? fn() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    const auto fn = real().DllGetClassObject;
    return fn != nullptr ? fn(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

} // extern "C"
