// Fonte de mídia (IMFMediaSourceEx) da câmera virtual MobLens.
#include "vcam.h"

MediaSource::MediaSource() {
    InitializeCriticalSection(&m_cs);
    InterlockedIncrement(&g_objectCount);
}

MediaSource::~MediaSource() {
    Shutdown();
    if (m_attributes) {
        m_attributes->Release();
    }
    if (m_mfStarted) {
        MFShutdown();
    }
    DeleteCriticalSection(&m_cs);
    InterlockedDecrement(&g_objectCount);
}

HRESULT MediaSource::Initialize(IMFAttributes *activatorAttributes) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        return hr;
    }
    m_mfStarted = true;

    hr = MFCreateAttributes(&m_attributes, 16);
    if (FAILED(hr)) {
        return hr;
    }
    if (activatorAttributes) {
        activatorAttributes->CopyAllItems(m_attributes);
    }

    hr = MFCreateEventQueue(&m_queue);
    if (FAILED(hr)) {
        return hr;
    }

    IMFMediaType *types[16] = {};
    DWORD typeCount = 0;
    for (DWORD i = 0; i < kFormatCount && i < 16; ++i) {
        hr = CreateVideoMediaType(kFormats[i], &types[typeCount]);
        if (FAILED(hr)) {
            break;
        }
        ++typeCount;
    }

    IMFStreamDescriptor *sd = nullptr;
    IMFMediaTypeHandler *handler = nullptr;
    if (SUCCEEDED(hr)) {
        hr = MFCreateStreamDescriptor(0, typeCount, types, &sd);
    }
    if (SUCCEEDED(hr)) {
        hr = sd->GetMediaTypeHandler(&handler);
    }
    if (SUCCEEDED(hr)) {
        hr = handler->SetCurrentMediaType(types[0]);
    }
    if (SUCCEEDED(hr)) {
        sd->SetGUID(MCAM_MF_DEVICESTREAM_STREAM_CATEGORY,
                    MCAM_PINNAME_VIDEO_CAPTURE);
        sd->SetUINT32(MCAM_MF_DEVICESTREAM_STREAM_ID, 0);
        sd->SetUINT32(MCAM_MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        sd->SetUINT32(MCAM_MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                      MCAM_MFFrameSourceTypes_Color);
        hr = MFCreatePresentationDescriptor(1, &sd, &m_pd);
    }
    if (SUCCEEDED(hr)) {
        hr = m_pd->SelectStream(0);
    }
    if (SUCCEEDED(hr)) {
        m_stream = new (std::nothrow) MediaStream();
        hr = m_stream ? m_stream->Initialize(this, sd) : E_OUTOFMEMORY;
    }

    if (handler) {
        handler->Release();
    }
    if (sd) {
        sd->Release();
    }
    for (DWORD i = 0; i < typeCount; ++i) {
        types[i]->Release();
    }

    return hr;
}

// ----- IUnknown -------------------------------------------------------------

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator
            || riid == IID_IMFMediaSource || riid == MCAM_IID_IMFMediaSourceEx) {
        *ppv = static_cast<IMcamMediaSourceEx *>(this);
    } else if (riid == IID_IMFGetService) {
        *ppv = static_cast<IMFGetService *>(this);
    } else if (riid == MCAM_IID_IKsControl) {
        *ppv = static_cast<IMcamKsControl *>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaSource::AddRef() {
    return InterlockedIncrement(&m_ref);
}

STDMETHODIMP_(ULONG) MediaSource::Release() {
    ULONG ref = InterlockedDecrement(&m_ref);
    if (!ref) {
        delete this;
    }
    return ref;
}

// ----- IMFMediaEventGenerator ----------------------------------------------

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent **event) {
    IMFMediaEventQueue *queue = nullptr;
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        queue = m_queue;
        queue->AddRef();
    }
    LeaveCriticalSection(&m_cs);
    if (FAILED(hr)) {
        return hr;
    }
    hr = queue->GetEvent(flags, event);
    queue->Release();
    return hr;
}

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_queue->BeginGetEvent(cb, state);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult *result,
                                      IMFMediaEvent **event) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_queue->EndGetEvent(result, event);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID ext,
                                     HRESULT status, const PROPVARIANT *value) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_queue->QueueEventParamVar(met, ext, status, value);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

// ----- IMFMediaSource -------------------------------------------------------

STDMETHODIMP MediaSource::GetCharacteristics(DWORD *characteristics) {
    if (!characteristics) {
        return E_POINTER;
    }
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        *characteristics = MFMEDIASOURCE_IS_LIVE;
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(
        IMFPresentationDescriptor **pd) {
    if (!pd) {
        return E_POINTER;
    }
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_pd->Clone(pd);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor *pd,
                                const GUID *timeFormat,
                                const PROPVARIANT *startPosition) {
    (void) startPosition;  // fonte ao vivo: posição ignorada
    if (!pd) {
        return E_INVALIDARG;
    }
    if (timeFormat && *timeFormat != GUID_NULL) {
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    }

    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) {
        LeaveCriticalSection(&m_cs);
        return hr;
    }

    // Descobre se o stream 0 foi selecionado e com qual formato
    bool selected = false;
    IMFMediaType *type = nullptr;
    DWORD count = 0;
    hr = pd->GetStreamDescriptorCount(&count);
    for (DWORD i = 0; SUCCEEDED(hr) && i < count; ++i) {
        BOOL isSelected = FALSE;
        IMFStreamDescriptor *sd = nullptr;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &isSelected, &sd))) {
            continue;
        }
        DWORD id = 0;
        sd->GetStreamIdentifier(&id);
        if (id == 0 && isSelected) {
            selected = true;
            IMFMediaTypeHandler *handler = nullptr;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&handler))) {
                handler->GetCurrentMediaType(&type);
                handler->Release();
            }
        }
        sd->Release();
    }

    PROPVARIANT startTime;
    PropVariantInit(&startTime);
    startTime.vt = VT_I8;
    startTime.hVal.QuadPart = MFGetSystemTime();

    if (SUCCEEDED(hr) && selected) {
        if (type) {
            hr = m_stream->SetMediaType(type);
        }
        if (SUCCEEDED(hr)) {
            hr = m_queue->QueueEventParamUnk(
                m_streamAnnounced ? MEUpdatedStream : MENewStream, GUID_NULL,
                S_OK, static_cast<IMFMediaStream *>(m_stream));
            m_streamAnnounced = true;
        }
    } else if (SUCCEEDED(hr)) {
        m_stream->Stop(false);
    }

    if (SUCCEEDED(hr)) {
        hr = m_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK,
                                         &startTime);
    }
    if (SUCCEEDED(hr) && selected) {
        hr = m_stream->Start(&startTime);
    }
    if (FAILED(hr)) {
        m_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, hr, nullptr);
    }

    if (type) {
        type->Release();
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaSource::Stop() {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_stream->Stop(true);
    }
    if (SUCCEEDED(hr)) {
        hr = m_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK,
                                         nullptr);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaSource::Pause() {
    // Câmera ao vivo: pausa não suportada
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP MediaSource::Shutdown() {
    EnterCriticalSection(&m_cs);
    if (m_shutdown) {
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }
    m_shutdown = true;
    MediaStream *stream = m_stream;
    m_stream = nullptr;
    if (m_queue) {
        m_queue->Shutdown();
        m_queue->Release();
        m_queue = nullptr;
    }
    if (m_pd) {
        m_pd->Release();
        m_pd = nullptr;
    }
    LeaveCriticalSection(&m_cs);

    if (stream) {
        stream->Shutdown();
        stream->Release();
    }
    return S_OK;
}

// ----- IMFMediaSourceEx -----------------------------------------------------

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes **attrs) {
    if (!attrs) {
        return E_POINTER;
    }
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        *attrs = m_attributes;
        m_attributes->AddRef();
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD id, IMFAttributes **attrs) {
    if (!attrs) {
        return E_POINTER;
    }
    if (id != 0) {
        return MF_E_INVALIDSTREAMNUMBER;
    }
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_stream->GetAttributes(attrs);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown *manager) {
    // As amostras são geradas em memória de sistema; o gerenciador D3D não
    // é necessário.
    (void) manager;
    return CheckShutdown();
}

// ----- IMFGetService --------------------------------------------------------

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID *ppv) {
    if (ppv) {
        *ppv = nullptr;
    }
    return MF_E_UNSUPPORTED_SERVICE;
}

// ----- IKsControl -----------------------------------------------------------

STDMETHODIMP MediaSource::KsProperty(MCAM_KSIDENTIFIER *, ULONG, LPVOID, ULONG,
                                     ULONG *) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(MCAM_KSIDENTIFIER *, ULONG, LPVOID, ULONG,
                                   ULONG *) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(MCAM_KSIDENTIFIER *, ULONG, LPVOID, ULONG,
                                  ULONG *) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
