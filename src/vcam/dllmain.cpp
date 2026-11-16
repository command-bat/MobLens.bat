// Ponto de entrada da DLL, fábrica de classes COM e auto-registro.
#include "vcam.h"

#include <cwchar>

HMODULE g_module = nullptr;
volatile LONG g_objectCount = 0;
volatile LONG g_lockCount = 0;

const VideoFormat kFormats[] = {
    {1920, 1080, 30},  // padrão
    {1280, 720, 30},
    {3840, 2160, 30},
    {2560, 1440, 30},
    {960, 540, 30},
    {640, 480, 30},
    {640, 360, 30},
};
const DWORD kFormatCount = sizeof(kFormats) / sizeof(kFormats[0]);

HRESULT CreateVideoMediaType(const VideoFormat &fmt, IMFMediaType **out) {
    IMFMediaType *type = nullptr;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        return hr;
    }
    const UINT32 frameSize = fmt.width * fmt.height * 3 / 2;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(type, MF_MT_FRAME_SIZE, fmt.width, fmt.height);
    MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fmt.fps, 1);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, fmt.width);
    type->SetUINT32(MF_MT_SAMPLE_SIZE, frameSize);
    type->SetUINT32(MF_MT_AVG_BITRATE, frameSize * 8 * fmt.fps);
    *out = type;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Fábrica de classes
// ---------------------------------------------------------------------------
namespace {

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Objeto estático: sem contagem real de referências
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid,
                                void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        return CreateActivator(riid, ppv);
    }

    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&g_lockCount);
        } else {
            InterlockedDecrement(&g_lockCount);
        }
        return S_OK;
    }
};

ClassFactory g_factory;

HRESULT SetRegString(HKEY root, const wchar_t *subkey, const wchar_t *name,
                     const wchar_t *value) {
    HKEY key = nullptr;
    LONG r = RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_WRITE, nullptr, &key, nullptr);
    if (r != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(r);
    }
    r = RegSetValueExW(key, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(value),
                       (DWORD) ((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(r);
}

const wchar_t kClsidKey[] = L"SOFTWARE\\Classes\\CLSID\\" MOBLENS_CLSID_STR;
const wchar_t kInprocKey[] =
    L"SOFTWARE\\Classes\\CLSID\\" MOBLENS_CLSID_STR L"\\InprocServer32";

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (clsid != CLSID_MobLensSource) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return (g_objectCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        return E_FAIL;
    }
    HRESULT hr = SetRegString(HKEY_LOCAL_MACHINE, kClsidKey, nullptr,
                              L"MobLens Virtual Camera Source");
    if (SUCCEEDED(hr)) {
        hr = SetRegString(HKEY_LOCAL_MACHINE, kInprocKey, nullptr, path);
    }
    if (SUCCEEDED(hr)) {
        hr = SetRegString(HKEY_LOCAL_MACHINE, kInprocKey, L"ThreadingModel",
                          L"Both");
    }
    return hr;
}

STDAPI DllUnregisterServer() {
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    if (r == ERROR_FILE_NOT_FOUND) {
        return S_OK;
    }
    return HRESULT_FROM_WIN32(r);
}
