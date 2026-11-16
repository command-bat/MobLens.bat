// Instalação/remoção da câmera virtual MobLens (requer administrador).
#include "app.h"

// mf_compat.h inclui initguid.h, então vem antes (define FOLDERID_*)
#include "../vcam/mf_compat.h"

#include <knownfolders.h>
#include <shlobj.h>

namespace {

const wchar_t kInprocKey[] =
    L"SOFTWARE\\Classes\\CLSID\\" MOBLENS_CLSID_STR L"\\InprocServer32";

std::wstring InstallDir() {
    wchar_t *pf = nullptr;
    std::wstring dir = L"C:\\Program Files";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr,
                                       &pf))) {
        dir = pf;
    }
    CoTaskMemFree(pf);
    return dir + L"\\MobLens";
}

std::wstring HrText(const wchar_t *what, HRESULT hr) {
    wchar_t buf[256];
    swprintf(buf, 256, L"%ls (0x%08lx)", what, (unsigned long) hr);
    return buf;
}

bool SameFile(const std::wstring &a, const std::wstring &b) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    WIN32_FILE_ATTRIBUTE_DATA fb;
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa)
            || !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb)) {
        return false;
    }
    return fa.nFileSizeLow == fb.nFileSizeLow
        && fa.nFileSizeHigh == fb.nFileSizeHigh
        && CompareFileTime(&fa.ftLastWriteTime, &fb.ftLastWriteTime) == 0;
}

HRESULT CallDllExport(const std::wstring &dll, const char *name) {
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    typedef HRESULT (STDAPICALLTYPE *Fn)();
    Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void *>(
        GetProcAddress(m, name)));
    HRESULT hr = fn ? fn() : HRESULT_FROM_WIN32(GetLastError());
    FreeLibrary(m);
    return hr;
}

HRESULT CreateVirtualCamera(IMcamVirtualCamera **vc,
                            const wchar_t *name = MOBLENS_FRIENDLY_NAME,
                            const wchar_t *clsid = MOBLENS_CLSID_STR) {
    HMODULE m = LoadLibraryW(L"mfsensorgroup.dll");
    auto create = m ? reinterpret_cast<PFN_MFCreateVirtualCamera>(
        reinterpret_cast<void *>(GetProcAddress(m, "MFCreateVirtualCamera")))
        : nullptr;
    if (!create) {
        return E_NOTIMPL;  // requer Windows 11
    }
    return create(MCAM_VirtualCameraType_SoftwareCameraSource,
                  MCAM_VirtualCameraLifetime_System,
                  MCAM_VirtualCameraAccess_AllUsers,
                  name, clsid, nullptr, 0, vc);
}

// Remove a câmera da versão anterior do projeto ("MobileCam")
void RemoveLegacyCamera() {
    const wchar_t kOldClsid[] = L"{7B3C9E21-5A4D-4F1B-9C6E-2D8A1F0B4C73}";
    std::wstring oldKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kOldClsid;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, oldKey.c_str(), 0, KEY_READ, &key)
            != ERROR_SUCCESS) {
        return;  // não instalada
    }
    RegCloseKey(key);

    IMcamVirtualCamera *vc = nullptr;
    if (SUCCEEDED(CreateVirtualCamera(&vc, L"MobileCam", kOldClsid))) {
        vc->Remove();
        vc->Shutdown();
        vc->Release();
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, oldKey.c_str());

    std::wstring dir = InstallDir();
    dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\MobileCam";
    std::wstring dll = dir + L"\\mobilecam_vcam.dll";
    if (!DeleteFileW(dll.c_str())) {
        MoveFileExW(dll.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    RemoveDirectoryW(dir.c_str());
}

bool CopyDll(const std::wstring &src, const std::wstring &dst) {
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        return true;
    }
    // A DLL antiga pode estar carregada pelo serviço de câmera: renomeia
    std::wstring old = dst + L".old";
    DeleteFileW(old.c_str());
    if (MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return CopyFileW(src.c_str(), dst.c_str(), FALSE) != FALSE;
    }
    return false;
}

} // namespace

CameraInstallState GetCameraInstallState() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInprocKey, 0, KEY_READ, &key)
            != ERROR_SUCCESS) {
        return CameraInstallState::Missing;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof(path) - sizeof(wchar_t);
    LONG r = RegQueryValueExW(key, nullptr, nullptr, nullptr,
                              reinterpret_cast<BYTE *>(path), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS
            || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        return CameraInstallState::Missing;
    }
    std::wstring bundled = AppDir() + L"\\" MOBLENS_DLL_NAME;
    return SameFile(path, bundled) ? CameraInstallState::Ok
                                   : CameraInstallState::Outdated;
}

bool InstallCamera(std::wstring &error) {
    std::wstring src = AppDir() + L"\\" MOBLENS_DLL_NAME;
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"Arquivo não encontrado: " + src;
        return false;
    }

    RemoveLegacyCamera();

    std::wstring dir = InstallDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring dst = dir + L"\\" MOBLENS_DLL_NAME;
    // Instalado pelo instalador: o app já está na pasta de destino
    const bool sameFile = !lstrcmpiW(src.c_str(), dst.c_str());
    if (!sameFile && !CopyDll(src, dst)) {
        error = L"Falha ao copiar a DLL para " + dst;
        return false;
    }

    Log(L"instalação: DLL copiada para %ls", dst.c_str());
    HRESULT hr = CallDllExport(dst, "DllRegisterServer");
    Log(L"instalação: DllRegisterServer 0x%08lx", (unsigned long) hr);
    if (FAILED(hr)) {
        error = HrText(L"Falha ao registrar a DLL", hr);
        return false;
    }

    IMcamVirtualCamera *vc = nullptr;
    hr = CreateVirtualCamera(&vc);
    if (FAILED(hr)) {
        error = hr == E_NOTIMPL
              ? L"A câmera virtual requer Windows 11."
              : HrText(L"Falha ao criar a câmera virtual", hr);
        return false;
    }
    Log(L"instalação: MFCreateVirtualCamera ok");
    hr = vc->Start(nullptr);
    Log(L"instalação: Start 0x%08lx", (unsigned long) hr);
    vc->Shutdown();
    vc->Release();
    if (FAILED(hr)) {
        error = HrText(L"Falha ao iniciar a câmera virtual", hr);
        return false;
    }
    return true;
}

bool UninstallCamera(std::wstring &error) {
    IMcamVirtualCamera *vc = nullptr;
    HRESULT hr = CreateVirtualCamera(&vc);
    if (SUCCEEDED(hr)) {
        hr = vc->Remove();
        vc->Shutdown();
        vc->Release();
    }
    if (FAILED(hr)) {
        error = HrText(L"Falha ao remover a câmera virtual", hr);
    }

    std::wstring dir = InstallDir();
    std::wstring dll = dir + L"\\" MOBLENS_DLL_NAME;
    if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CallDllExport(dll, "DllUnregisterServer");
        if (!DeleteFileW(dll.c_str())) {
            MoveFileExW(dll.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    RemoveDirectoryW(dir.c_str());
    return SUCCEEDED(hr);
}
