// Stream de vídeo da câmera virtual: entrega amostras NV12 no ritmo de
// fps do formato escolhido, atendendo as requisições (RequestSample) do Frame Server.
#include "vcam.h"

#include <cstring>

MediaStream::MediaStream() {
    InitializeCriticalSection(&m_cs);
    InterlockedIncrement(&g_objectCount);
}

MediaStream::~MediaStream() {
    Shutdown();
    DeleteCriticalSection(&m_cs);
    InterlockedDecrement(&g_objectCount);
}

HRESULT MediaStream::Initialize(MediaSource *source, IMFStreamDescriptor *sd) {
    if (!source || !sd) {
        return E_INVALIDARG;
    }

    m_source = source;
    m_source->AddRef();
    m_sd = sd;
    m_sd->AddRef();

    HRESULT hr = MFCreateEventQueue(&m_queue);
    if (FAILED(hr)) {
        return hr;
    }

    hr = MFCreateAttributes(&m_attributes, 8);
    if (FAILED(hr)) {
        return hr;
    }
    m_attributes->SetGUID(MCAM_MF_DEVICESTREAM_STREAM_CATEGORY,
                          MCAM_PINNAME_VIDEO_CAPTURE);
    m_attributes->SetUINT32(MCAM_MF_DEVICESTREAM_STREAM_ID, 0);
    m_attributes->SetUINT32(MCAM_MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    m_attributes->SetUINT32(MCAM_MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                            MCAM_MFFrameSourceTypes_Color);

    IMFMediaTypeHandler *handler = nullptr;
    hr = m_sd->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) {
        IMFMediaType *type = nullptr;
        if (SUCCEEDED(handler->GetCurrentMediaType(&type))) {
            SetMediaType(type);
            type->Release();
        }
        handler->Release();
    }

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    m_timer = CreateWaitableTimerExW(nullptr, nullptr,
                                     CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_ALL_ACCESS);
    if (!m_timer) {
        m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    if (!m_timer) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

HRESULT MediaStream::SetMediaType(IMFMediaType *type) {
    if (!type) {
        return E_INVALIDARG;
    }
    UINT32 w = 0;
    UINT32 h = 0;
    HRESULT hr = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    if (FAILED(hr)) {
        return hr;
    }
    if (!w || !h || w > MOBLENS_MAX_WIDTH || h > MOBLENS_MAX_HEIGHT
            || (w & 1) || (h & 1)) {
        return MF_E_INVALIDMEDIATYPE;
    }

    EnterCriticalSection(&m_cs);
    if (m_type) {
        m_type->Release();
    }
    m_type = type;
    m_type->AddRef();
    m_width = w;
    m_height = h;
    UINT32 num = 0;
    UINT32 den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den))
            && num && den && num / den >= 1 && num / den <= 120) {
        m_fps = num / den;
    }
    LeaveCriticalSection(&m_cs);

    return S_OK;
}

HRESULT MediaStream::GetAttributes(IMFAttributes **attrs) {
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

bool MediaStream::IsActive() {
    EnterCriticalSection(&m_cs);
    bool active = m_state == MCAM_MF_STREAM_STATE_RUNNING;
    LeaveCriticalSection(&m_cs);
    return active;
}

HRESULT MediaStream::Start(const PROPVARIANT *startTime) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        m_state = MCAM_MF_STREAM_STATE_RUNNING;
        hr = m_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK,
                                         startTime);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

HRESULT MediaStream::Stop(bool sendEvent) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        m_state = MCAM_MF_STREAM_STATE_STOPPED;
        ClearRequests();
        m_reader.SetActive(false);
        if (sendEvent) {
            hr = m_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK,
                                             nullptr);
        }
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

HRESULT MediaStream::Shutdown() {
    EnterCriticalSection(&m_cs);
    if (m_shutdown) {
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }
    m_shutdown = true;
    m_state = MCAM_MF_STREAM_STATE_STOPPED;
    ClearRequests();
    if (m_queue) {
        m_queue->Shutdown();
    }
    LeaveCriticalSection(&m_cs);

    // A thread de entrega usa m_cs, então é finalizada fora da seção crítica
    if (m_thread) {
        SetEvent(m_stopEvent);
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_timer) {
        CloseHandle(m_timer);
        m_timer = nullptr;
    }
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }

    m_reader.Close();

    if (m_type) { m_type->Release(); m_type = nullptr; }
    if (m_attributes) { m_attributes->Release(); m_attributes = nullptr; }
    if (m_sd) { m_sd->Release(); m_sd = nullptr; }
    if (m_queue) { m_queue->Release(); m_queue = nullptr; }
    if (m_source) {
        // quebra a referência circular fonte <-> stream
        MediaSource *source = m_source;
        m_source = nullptr;
        source->Release();
    }
    return S_OK;
}

void MediaStream::ClearRequests() {
    while (!m_requests.empty()) {
        IUnknown *token = m_requests.front();
        m_requests.pop_front();
        if (token) {
            token->Release();
        }
    }
}

DWORD WINAPI MediaStream::ThreadProc(LPVOID param) {
    static_cast<MediaStream *>(param)->Run();
    return 0;
}

void MediaStream::Run() {
    LARGE_INTEGER freq;
    LARGE_INTEGER next;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    LONGLONG period = freq.QuadPart / m_fps;
    HANDLE handles[2] = {m_stopEvent, m_timer};

    for (;;) {
        period = freq.QuadPart / m_fps;  // o formato pode mudar
        next.QuadPart += period;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG remaining = next.QuadPart - now.QuadPart;
        if (remaining < -period) {
            // muito atrasado: ressincroniza o relógio
            next = now;
            remaining = 0;
        }

        LARGE_INTEGER due;
        due.QuadPart = remaining > 0
                     ? -(remaining * 10000000 / freq.QuadPart)
                     : -1;
        if (due.QuadPart == 0) {
            due.QuadPart = -1;
        }
        SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);

        DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0 + 1) {
            break;  // stop
        }
        DeliverSample();
    }
}

void MediaStream::DeliverSample() {
    EnterCriticalSection(&m_cs);
    if (m_shutdown || m_state != MCAM_MF_STREAM_STATE_RUNNING
            || m_requests.empty() || !m_width || !m_height) {
        LeaveCriticalSection(&m_cs);
        return;
    }

    IUnknown *token = m_requests.front();
    m_requests.pop_front();

    const UINT32 w = m_width;
    const UINT32 h = m_height;
    const DWORD size = w * h * 3 / 2;

    IMFMediaBuffer *buffer = nullptr;
    IMFSample *sample = nullptr;
    HRESULT hr = MFCreate2DMediaBuffer(w, h, MFVideoFormat_NV12.Data1, FALSE,
                                       &buffer);
    if (SUCCEEDED(hr)) {
        IMF2DBuffer *buffer2d = nullptr;
        BYTE *scanline0 = nullptr;
        LONG pitch = 0;
        hr = buffer->QueryInterface(IID_IMF2DBuffer,
                                    reinterpret_cast<void **>(&buffer2d));
        if (SUCCEEDED(hr)) {
            hr = buffer2d->Lock2D(&scanline0, &pitch);
        }
        if (SUCCEEDED(hr)) {
            if (pitch == (LONG) w) {
                m_reader.Fill(scanline0, w, h);
            } else {
                // stride diferente da largura: copia linha a linha
                static thread_local std::vector<uint8_t> scratch;
                scratch.resize(size);
                m_reader.Fill(scratch.data(), w, h);
                for (UINT32 y = 0; y < h * 3 / 2; ++y) {
                    memcpy(scanline0 + (LONG_PTR) y * pitch,
                           scratch.data() + (size_t) y * w, w);
                }
            }
            buffer2d->Unlock2D();
            DWORD len = 0;
            buffer2d->GetContiguousLength(&len);
            buffer->SetCurrentLength(len ? len : size);
        }
        if (buffer2d) {
            buffer2d->Release();
        }
    }

    if (SUCCEEDED(hr)) {
        hr = MFCreateSample(&sample);
    }
    if (SUCCEEDED(hr)) {
        hr = sample->AddBuffer(buffer);
    }
    if (SUCCEEDED(hr)) {
        sample->SetSampleTime(MFGetSystemTime());
        sample->SetSampleDuration(10000000 / m_fps);
        if (token) {
            sample->SetUnknown(MCAM_MFSampleExtension_Token, token);
        }
        hr = m_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample);
    }
    if (FAILED(hr)) {
        m_queue->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
    }

    if (sample) {
        sample->Release();
    }
    if (buffer) {
        buffer->Release();
    }
    if (token) {
        token->Release();
    }
    LeaveCriticalSection(&m_cs);
}

// ----- IUnknown -------------------------------------------------------------

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator
            || riid == IID_IMFMediaStream || riid == MCAM_IID_IMFMediaStream2) {
        *ppv = static_cast<IMcamMediaStream2 *>(this);
    } else if (riid == MCAM_IID_IKsControl) {
        *ppv = static_cast<IMcamKsControl *>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaStream::AddRef() {
    return InterlockedIncrement(&m_ref);
}

STDMETHODIMP_(ULONG) MediaStream::Release() {
    ULONG ref = InterlockedDecrement(&m_ref);
    if (!ref) {
        delete this;
    }
    return ref;
}

// ----- IMFMediaEventGenerator ----------------------------------------------

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent **event) {
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
    // GetEvent pode bloquear: chamado fora da seção crítica
    hr = queue->GetEvent(flags, event);
    queue->Release();
    return hr;
}

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_queue->BeginGetEvent(cb, state);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult *result,
                                      IMFMediaEvent **event) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_queue->EndGetEvent(result, event);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID ext,
                                     HRESULT status, const PROPVARIANT *value) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_queue->QueueEventParamVar(met, ext, status, value);
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

// ----- IMFMediaStream -------------------------------------------------------

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource **source) {
    if (!source) {
        return E_POINTER;
    }
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        hr = m_source->QueryInterface(IID_IMFMediaSource,
                                      reinterpret_cast<void **>(source));
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor **sd) {
    if (!sd) {
        return E_POINTER;
    }
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        *sd = m_sd;
        m_sd->AddRef();
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown *token) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        if (m_state != MCAM_MF_STREAM_STATE_RUNNING) {
            hr = MF_E_INVALIDREQUEST;
        } else if (m_requests.size() >= 16) {
            hr = MF_E_NOTACCEPTING;
        } else {
            if (token) {
                token->AddRef();
            }
            m_requests.push_back(token);
        }
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

// ----- IMFMediaStream2 ------------------------------------------------------

STDMETHODIMP MediaStream::SetStreamState(int state) {
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    bool changed = SUCCEEDED(hr) && state != m_state;
    LeaveCriticalSection(&m_cs);
    if (!changed) {
        return hr;
    }

    switch (state) {
        case MCAM_MF_STREAM_STATE_RUNNING: {
            PROPVARIANT start;
            PropVariantInit(&start);
            start.vt = VT_I8;
            start.hVal.QuadPart = MFGetSystemTime();
            return Start(&start);
        }
        case MCAM_MF_STREAM_STATE_STOPPED:
            return Stop(true);
        case MCAM_MF_STREAM_STATE_PAUSED:
            EnterCriticalSection(&m_cs);
            m_state = MCAM_MF_STREAM_STATE_PAUSED;
            LeaveCriticalSection(&m_cs);
            return S_OK;
        default:
            return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(int *state) {
    if (!state) {
        return E_POINTER;
    }
    EnterCriticalSection(&m_cs);
    HRESULT hr = CheckShutdown();
    if (SUCCEEDED(hr)) {
        *state = m_state;
    }
    LeaveCriticalSection(&m_cs);
    return hr;
}

// ----- IKsControl (nenhuma propriedade de hardware suportada) ---------------

STDMETHODIMP MediaStream::KsProperty(MCAM_KSIDENTIFIER *, ULONG, LPVOID, ULONG,
                                     ULONG *) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaStream::KsMethod(MCAM_KSIDENTIFIER *, ULONG, LPVOID, ULONG,
                                   ULONG *) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaStream::KsEvent(MCAM_KSIDENTIFIER *, ULONG, LPVOID, ULONG,
                                  ULONG *) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
