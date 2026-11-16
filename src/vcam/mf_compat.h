// Compatibilidade de cabeçalhos entre Windows SDK (MSVC) e mingw-w64.
// Tudo que pode faltar no mingw-w64 é definido aqui com os valores oficiais
// do Windows SDK.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <initguid.h>
#include <unknwn.h>
#include <cguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>
#include <devpropdef.h>


// ---------------------------------------------------------------------------
// GUIDs de atributos de stream de dispositivo (mfidl.h / ksmedia.h)
// ---------------------------------------------------------------------------
// Definidos com nomes próprios para não colidir com os do SDK.
DEFINE_GUID(MCAM_MF_DEVICESTREAM_STREAM_CATEGORY,
    0x2939e7b8, 0xa62e, 0x4579, 0xb6, 0x74, 0xd4, 0x07, 0x3d, 0xfa, 0xbb, 0xba);
DEFINE_GUID(MCAM_MF_DEVICESTREAM_STREAM_ID,
    0x11bd5120, 0xd124, 0x446b, 0x88, 0xe6, 0x17, 0x06, 0x02, 0x57, 0xff, 0xf9);
DEFINE_GUID(MCAM_MF_DEVICESTREAM_FRAMESERVER_SHARED,
    0x1cb378e9, 0xb279, 0x41d4, 0xaf, 0x97, 0x34, 0xa2, 0x43, 0xe6, 0x83, 0x20);
DEFINE_GUID(MCAM_MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
    0x17145fd1, 0x1b2b, 0x423c, 0x80, 0x01, 0x2b, 0x68, 0x33, 0xed, 0x35, 0x88);
DEFINE_GUID(MCAM_PINNAME_VIDEO_CAPTURE,
    0xfb6c4281, 0x0353, 0x11d1, 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba);
DEFINE_GUID(MCAM_KSCATEGORY_VIDEO_CAMERA,
    0xe5323777, 0xf976, 0x4f5b, 0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44);

DEFINE_GUID(MCAM_MFSampleExtension_Token,
    0x8294da66, 0xf328, 0x4805, 0xb5, 0x51, 0x00, 0xde, 0xb4, 0xc5, 0x7a, 0x61);

#define MCAM_MFFrameSourceTypes_Color 0x0001

// ---------------------------------------------------------------------------
// IKsControl (ks.h / ksproxy.h). Interface obrigatória para fontes de câmera
// do Frame Server. Declarada aqui com nome próprio (layout idêntico).
// ---------------------------------------------------------------------------
struct MCAM_KSIDENTIFIER {
    GUID Set;
    ULONG Id;
    ULONG Flags;
};

DEFINE_GUID(MCAM_IID_IKsControl,
    0x28f54685, 0x06fd, 0x11d2, 0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96);

struct IMcamKsControl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(
        MCAM_KSIDENTIFIER *Property, ULONG PropertyLength,
        LPVOID PropertyData, ULONG DataLength, ULONG *BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(
        MCAM_KSIDENTIFIER *Method, ULONG MethodLength,
        LPVOID MethodData, ULONG DataLength, ULONG *BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(
        MCAM_KSIDENTIFIER *Event, ULONG EventLength,
        LPVOID EventData, ULONG DataLength, ULONG *BytesReturned) = 0;
};

// ---------------------------------------------------------------------------
// IMFMediaSourceEx / IMFMediaStream2 (mfidl.h, Windows 8+ / 10+)
// ---------------------------------------------------------------------------
DEFINE_GUID(MCAM_IID_IMFMediaSourceEx,
    0x3c9b2eb9, 0x86d5, 0x4514, 0xa3, 0x94, 0xf5, 0x66, 0x64, 0xf9, 0xf0, 0xd8);
DEFINE_GUID(MCAM_IID_IMFMediaStream2,
    0xc5bc37d6, 0x75c7, 0x46a1, 0xa1, 0x32, 0x81, 0xb5, 0xf7, 0x23, 0xc2, 0x0f);

struct IMcamMediaSourceEx : public IMFMediaSource {
    virtual HRESULT STDMETHODCALLTYPE GetSourceAttributes(
        IMFAttributes **ppAttributes) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamAttributes(
        DWORD dwStreamIdentifier, IMFAttributes **ppAttributes) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetD3DManager(IUnknown *pManager) = 0;
};

enum MCAM_MF_STREAM_STATE {
    MCAM_MF_STREAM_STATE_STOPPED = 0,
    MCAM_MF_STREAM_STATE_PAUSED = 1,
    MCAM_MF_STREAM_STATE_RUNNING = 2,
};

struct IMcamMediaStream2 : public IMFMediaStream {
    virtual HRESULT STDMETHODCALLTYPE SetStreamState(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamState(int *value) = 0;
};

// ---------------------------------------------------------------------------
// Virtual Camera API (mfvirtualcamera.h, Windows 11 build 22000+), carregada
// dinamicamente de mfsensorgroup.dll.
// ---------------------------------------------------------------------------
DEFINE_GUID(MCAM_IID_IMFVirtualCamera,
    0x1c08a864, 0xef6c, 0x4c75, 0xaf, 0x59, 0x5f, 0x2d, 0x68, 0xda, 0x95, 0x63);

enum MCAM_VirtualCameraType { MCAM_VirtualCameraType_SoftwareCameraSource = 0 };
enum MCAM_VirtualCameraLifetime {
    MCAM_VirtualCameraLifetime_Session = 0,
    MCAM_VirtualCameraLifetime_System = 1,
};
enum MCAM_VirtualCameraAccess {
    MCAM_VirtualCameraAccess_CurrentUser = 0,
    MCAM_VirtualCameraAccess_AllUsers = 1,
};

struct IMcamVirtualCamera : public IMFAttributes {
    virtual HRESULT STDMETHODCALLTYPE AddDeviceSourceInfo(LPCWSTR info) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProperty(const DEVPROPKEY *key,
        DEVPROPTYPE type, const BYTE *data, ULONG size) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddRegistryEntry(LPCWSTR name,
        LPCWSTR subkey, DWORD regType, const BYTE *data, ULONG size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(IMFAsyncCallback *callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource **src) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendCameraProperty(REFGUID set, ULONG id,
        ULONG flags, void *payload, ULONG payloadLength, void *data,
        ULONG dataLength, ULONG *dataWritten) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncEvent(REFGUID set, ULONG id,
        ULONG flags, HANDLE event, IUnknown **syncObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncSemaphore(REFGUID set, ULONG id,
        ULONG flags, HANDLE semaphore, LONG count, IUnknown **syncObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};

typedef HRESULT (STDAPICALLTYPE *PFN_MFCreateVirtualCamera)(
    int type, int lifetime, int access, LPCWSTR friendlyName,
    LPCWSTR sourceId, const GUID *categories, ULONG categoryCount,
    IMcamVirtualCamera **virtualCamera);

typedef HRESULT (STDAPICALLTYPE *PFN_MFIsVirtualCameraTypeSupported)(
    int type, BOOL *supported);
