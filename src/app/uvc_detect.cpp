// Detecta celular Android conectado no modo USB "Webcam" (Android 14+).
// Nesse modo o celular é uma webcam UVC comum e o Windows a usa sem o
// MobLens e sem depuração USB.
#include "app.h"

#include <mfapi.h>
#include <mfidl.h>

#include <cwctype>

namespace {

// Vendor IDs USB de fabricantes de celulares Android
const wchar_t *const kPhoneVendors[] = {
    L"18d1",  // Google
    L"04e8",  // Samsung
    L"22b8",  // Motorola
    L"2717",  // Xiaomi
    L"2a70",  // OnePlus
    L"0fce",  // Sony
    L"1004",  // LG
    L"12d1",  // Huawei
    L"22d9",  // Oppo / Realme
    L"2d95",  // Vivo
};

std::wstring Lower(std::wstring s) {
    for (wchar_t &c : s) {
        c = (wchar_t) towlower(c);
    }
    return s;
}

std::wstring GetString(IMFActivate *dev, REFGUID key) {
    wchar_t *value = nullptr;
    UINT32 len = 0;
    std::wstring out;
    if (SUCCEEDED(dev->GetAllocatedString(key, &value, &len)) && value) {
        out = value;
        CoTaskMemFree(value);
    }
    return out;
}

}  // namespace

std::wstring FindPhoneWebcam() {
    IMFAttributes *attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1))) {
        return L"";
    }
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate **devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &devices, &count);
    attrs->Release();
    if (FAILED(hr)) {
        return L"";
    }

    std::wstring found;
    for (UINT32 i = 0; i < count; ++i) {
        if (found.empty()) {
            std::wstring link = Lower(GetString(
                devices[i],
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK));
            for (const wchar_t *vid : kPhoneVendors) {
                if (link.find(std::wstring(L"usb#vid_") + vid)
                        != std::wstring::npos) {
                    found = GetString(devices[i],
                                      MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
                    if (found.empty()) {
                        found = L"Celular";
                    }
                    break;
                }
            }
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    if (!found.empty()) {
        Log(L"Celular no modo Webcam (UVC) detectado: %ls", found.c_str());
    }
    return found;
}
