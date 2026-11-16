// MobLens - fonte de mídia de câmera virtual (Media Foundation).
#pragma once

#include "mf_compat.h"

#include <cstdint>
#include <deque>
#include <new>
#include <vector>

#include "../common/moblens_ids.h"
#include "../common/moblens_shm.h"

// CLSID da fonte de mídia (mesmo valor de MOBLENS_CLSID_STR)
DEFINE_GUID(CLSID_MobLensSource,
    0x4e8d2a61, 0x93b7, 0x4c5e, 0xa1, 0xf2, 0x6b, 0x0c, 0x9d, 0x3e, 0x7a, 0x58);

extern HMODULE g_module;
extern volatile LONG g_objectCount;
extern volatile LONG g_lockCount;

// Formatos anunciados para os aplicativos (NV12)
struct VideoFormat {
    UINT32 width;
    UINT32 height;
    UINT32 fps;
};
extern const VideoFormat kFormats[];
extern const DWORD kFormatCount;

HRESULT CreateVideoMediaType(const VideoFormat &fmt, IMFMediaType **type);

// ---------------------------------------------------------------------------
// Leitor da memória compartilhada alimentada pelo scrcpy
// ---------------------------------------------------------------------------
class FrameReader {
public:
    FrameReader();
    ~FrameReader();

    // Preenche dst (NV12 contíguo, stride = width) com o quadro mais recente
    // do celular, ou com uma imagem "sem sinal" se não houver produtor.
    void Fill(uint8_t *dst, UINT32 width, UINT32 height);
    void SetActive(bool active);
    void Close();

private:
    bool TryOpen();
    bool ReadShared(UINT32 width, UINT32 height);
    void DrawPlaceholder(uint8_t *dst, UINT32 width, UINT32 height);

    HANDLE m_mapping = nullptr;
    struct moblens_shm_header *m_header = nullptr;
    ULONGLONG m_lastOpenAttempt = 0;
    std::vector<uint8_t> m_frame;  // último quadro válido
    UINT32 m_frameWidth = 0;
    UINT32 m_frameHeight = 0;
    ULONGLONG m_lastGoodTick = 0;
    UINT32 m_tick = 0;
};

class MediaSource;

// ---------------------------------------------------------------------------
// Stream de vídeo (único) da fonte
// ---------------------------------------------------------------------------
class MediaStream : public IMcamMediaStream2, public IMcamKsControl {
public:
    MediaStream();

    HRESULT Initialize(MediaSource *source, IMFStreamDescriptor *sd);
    HRESULT Start(const PROPVARIANT *startTime);
    HRESULT Stop(bool sendEvent);
    HRESULT Shutdown();
    HRESULT SetMediaType(IMFMediaType *type);
    HRESULT GetAttributes(IMFAttributes **attrs);
    bool IsActive();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent **event) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult *result, IMFMediaEvent **event) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status,
                            const PROPVARIANT *value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource **source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor **sd) override;
    STDMETHODIMP RequestSample(IUnknown *token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(int state) override;
    STDMETHODIMP GetStreamState(int *state) override;

    // IKsControl
    STDMETHODIMP KsProperty(MCAM_KSIDENTIFIER *p, ULONG pl, LPVOID d, ULONG dl,
                            ULONG *ret) override;
    STDMETHODIMP KsMethod(MCAM_KSIDENTIFIER *p, ULONG pl, LPVOID d, ULONG dl,
                          ULONG *ret) override;
    STDMETHODIMP KsEvent(MCAM_KSIDENTIFIER *p, ULONG pl, LPVOID d, ULONG dl,
                         ULONG *ret) override;

private:
    ~MediaStream();

    static DWORD WINAPI ThreadProc(LPVOID param);
    void Run();
    void DeliverSample();
    void ClearRequests();
    HRESULT CheckShutdown() const {
        return m_shutdown ? MF_E_SHUTDOWN : S_OK;
    }

    volatile LONG m_ref = 1;
    CRITICAL_SECTION m_cs;
    bool m_shutdown = false;
    int m_state = MCAM_MF_STREAM_STATE_STOPPED;
    MediaSource *m_source = nullptr;  // referência forte, liberada no Shutdown
    IMFMediaEventQueue *m_queue = nullptr;
    IMFStreamDescriptor *m_sd = nullptr;
    IMFAttributes *m_attributes = nullptr;
    IMFMediaType *m_type = nullptr;
    UINT32 m_width = 0;
    UINT32 m_height = 0;
    volatile UINT32 m_fps = MOBLENS_FPS;
    std::deque<IUnknown *> m_requests;
    HANDLE m_thread = nullptr;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_timer = nullptr;
    FrameReader m_reader;
};

// ---------------------------------------------------------------------------
// Fonte de mídia
// ---------------------------------------------------------------------------
class MediaSource : public IMcamMediaSourceEx, public IMFGetService,
                    public IMcamKsControl {
public:
    MediaSource();
    HRESULT Initialize(IMFAttributes *activatorAttributes);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent **event) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult *result, IMFMediaEvent **event) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status,
                            const PROPVARIANT *value) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD *characteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor **pd) override;
    STDMETHODIMP Start(IMFPresentationDescriptor *pd, const GUID *timeFormat,
                       const PROPVARIANT *startPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes **attrs) override;
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes **attrs) override;
    STDMETHODIMP SetD3DManager(IUnknown *manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID *ppv) override;

    // IKsControl
    STDMETHODIMP KsProperty(MCAM_KSIDENTIFIER *p, ULONG pl, LPVOID d, ULONG dl,
                            ULONG *ret) override;
    STDMETHODIMP KsMethod(MCAM_KSIDENTIFIER *p, ULONG pl, LPVOID d, ULONG dl,
                          ULONG *ret) override;
    STDMETHODIMP KsEvent(MCAM_KSIDENTIFIER *p, ULONG pl, LPVOID d, ULONG dl,
                         ULONG *ret) override;

private:
    ~MediaSource();
    HRESULT CheckShutdown() const {
        return m_shutdown ? MF_E_SHUTDOWN : S_OK;
    }

    volatile LONG m_ref = 1;
    CRITICAL_SECTION m_cs;
    bool m_shutdown = false;
    bool m_mfStarted = false;
    bool m_streamAnnounced = false;
    IMFMediaEventQueue *m_queue = nullptr;
    IMFPresentationDescriptor *m_pd = nullptr;
    IMFAttributes *m_attributes = nullptr;
    MediaStream *m_stream = nullptr;
};

// ---------------------------------------------------------------------------
// Ativador (IMFActivate) - é o objeto COM criado pelo Frame Server
// ---------------------------------------------------------------------------
HRESULT CreateActivator(REFIID riid, void **ppv);
