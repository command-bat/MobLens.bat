// MobLens - webcam virtual DirectShow (Windows 10, 8 e 7).
//
// No Windows 11 a webcam é criada com MFCreateVirtualCamera (src/vcam). Nas
// versões anteriores essa API não existe; este filtro de captura DirectShow
// é registrado na categoria "Video Capture Sources" e é carregado dentro de
// cada aplicativo que abre a câmera (Zoom, Discord, Chrome/Edge, OBS...).
// Os quadros vêm da mesma memória compartilhada (moblens_shm.h), no espaço de
// nomes da sessão ("Local\"), pois o filtro roda com os privilégios do
// usuário.
//
// Implementação independente das "baseclasses" do SDK.
#include <windows.h>
#include <dshow.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "../common/moblens_ids.h"
#include "../common/moblens_shm.h"

#ifndef KSPROPERTY_SUPPORT_GET
#define KSPROPERTY_SUPPORT_GET 1
#endif

namespace {

// {B1F4C7D2-6A3E-4E85-9C1B-7D2E5F8A3C64}
const CLSID CLSID_MobLensDShow = {
    0xb1f4c7d2, 0x6a3e, 0x4e85,
    {0x9c, 0x1b, 0x7d, 0x2e, 0x5f, 0x8a, 0x3c, 0x64}};

// Subtipos de vídeo: {FOURCC-0000-0010-8000-00AA00389B71}
constexpr DWORD FourCC(char a, char b, char c, char d) {
    return (DWORD) (BYTE) a | ((DWORD) (BYTE) b << 8)
         | ((DWORD) (BYTE) c << 16) | ((DWORD) (BYTE) d << 24);
}
GUID SubtypeFromFourCC(DWORD fcc) {
    GUID g = {fcc, 0x0000, 0x0010,
              {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return g;
}

const REFERENCE_TIME kFrameTime = 10000000 / MOBLENS_FPS;  // 100 ns

enum class Pixel { NV12, I420, YUY2 };

struct Format {
    UINT32 width;
    UINT32 height;
    Pixel pixel;
};

struct PixelInfo {
    DWORD fourcc;
    WORD bits;
};

PixelInfo Info(Pixel p) {
    switch (p) {
        case Pixel::NV12: return {FourCC('N', 'V', '1', '2'), 12};
        case Pixel::I420: return {FourCC('I', '4', '2', '0'), 12};
        default:          return {FourCC('Y', 'U', 'Y', '2'), 16};
    }
}

// Ordem = preferência (o primeiro é o padrão)
const UINT32 kSizes[][2] = {
    {1920, 1080}, {1280, 720}, {960, 540}, {640, 480}, {640, 360},
};
const Pixel kPixels[] = {Pixel::NV12, Pixel::I420, Pixel::YUY2};
const int kFormatCount = (int) (ARRAYSIZE(kSizes) * ARRAYSIZE(kPixels));

Format FormatAt(int i) {
    const int s = i / (int) ARRAYSIZE(kPixels);
    const int p = i % (int) ARRAYSIZE(kPixels);
    return {kSizes[s][0], kSizes[s][1], kPixels[p]};
}

DWORD ImageSize(const Format &f) {
    return f.width * f.height * Info(f.pixel).bits / 8;
}

HINSTANCE g_module;
volatile LONG g_objects = 0;
volatile LONG g_locks = 0;

// ----- AM_MEDIA_TYPE ---------------------------------------------------------

void FreeMediaType(AM_MEDIA_TYPE &mt) {
    if (mt.cbFormat && mt.pbFormat) {
        CoTaskMemFree(mt.pbFormat);
    }
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

HRESULT CopyMediaType(AM_MEDIA_TYPE &dst, const AM_MEDIA_TYPE &src) {
    dst = src;
    if (src.cbFormat) {
        dst.pbFormat = (BYTE *) CoTaskMemAlloc(src.cbFormat);
        if (!dst.pbFormat) {
            dst.cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
    }
    if (dst.pUnk) {
        dst.pUnk->AddRef();
    }
    return S_OK;
}

HRESULT BuildMediaType(int index, AM_MEDIA_TYPE &mt) {
    ZeroMemory(&mt, sizeof(mt));
    const Format f = FormatAt(index);
    const PixelInfo info = Info(f.pixel);
    auto *vih = (VIDEOINFOHEADER *) CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!vih) {
        return E_OUTOFMEMORY;
    }
    ZeroMemory(vih, sizeof(*vih));
    vih->AvgTimePerFrame = kFrameTime;
    vih->dwBitRate = ImageSize(f) * 8 * MOBLENS_FPS;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = (LONG) f.width;
    vih->bmiHeader.biHeight = (LONG) f.height;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = info.bits;
    vih->bmiHeader.biCompression = info.fourcc;
    vih->bmiHeader.biSizeImage = ImageSize(f);

    mt.majortype = MEDIATYPE_Video;
    mt.subtype = SubtypeFromFourCC(info.fourcc);
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = ImageSize(f);
    mt.formattype = FORMAT_VideoInfo;
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = (BYTE *) vih;
    return S_OK;
}

// Índice do formato equivalente a "mt" (-1 se não suportado). Tipos parciais
// (sem formato) casam com o primeiro formato compatível a partir de "from".
int MatchFormat(const AM_MEDIA_TYPE *mt, int from = 0) {
    for (int i = from; i < kFormatCount; ++i) {
        const Format f = FormatAt(i);
        if (mt->majortype != GUID_NULL && mt->majortype != MEDIATYPE_Video) {
            return -1;
        }
        if (mt->subtype != GUID_NULL
                && mt->subtype != SubtypeFromFourCC(Info(f.pixel).fourcc)) {
            continue;
        }
        if (mt->formattype == FORMAT_VideoInfo && mt->pbFormat
                && mt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            const auto *vih = (const VIDEOINFOHEADER *) mt->pbFormat;
            if ((UINT32) vih->bmiHeader.biWidth != f.width
                    || (UINT32) abs(vih->bmiHeader.biHeight) != f.height) {
                continue;
            }
        } else if (mt->formattype != GUID_NULL
                   && mt->formattype != FORMAT_VideoInfo) {
            return -1;
        }
        return i;
    }
    return -1;
}

// ----- Leitura da memória compartilhada -------------------------------------

class SharedFrames {
public:
    ~SharedFrames() { Close(); }

    void Close() {
        if (m_header) {
            m_header->consumer_active = 0;
            UnmapViewOfFile(m_header);
            m_header = nullptr;
        }
        if (m_mapping) {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
    }

    // Quadro NV12 mais recente em "frame" (w x h); false = sem celular
    bool Read(UINT32 width, UINT32 height, std::vector<uint8_t> &frame,
              UINT32 &w, UINT32 &h) {
        if (!Open()) {
            return false;
        }
        moblens_shm_header *hd = m_header;
        hd->requested_width = (int32_t) width;
        hd->requested_height = (int32_t) height;
        hd->consumer_active = 1;

        const ULONGLONG now = GetTickCount64();
        const int64_t tick = hd->producer_tick_ms;
        if (tick <= 0 || now - (ULONGLONG) tick > MOBLENS_PRODUCER_TIMEOUT_MS) {
            return false;
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            const LONG seq1 = *(volatile LONG *) &hd->seq;
            MemoryBarrier();
            if (seq1 & 1) {
                Sleep(0);
                continue;
            }
            const UINT32 fw = hd->width;
            const UINT32 fh = hd->height;
            const UINT32 size = hd->frame_size;
            if (!fw || !fh || size != fw * fh * 3 / 2
                    || size > MOBLENS_SHM_FRAME_MAX) {
                return false;
            }
            frame.resize(size);
            memcpy(frame.data(), moblens_shm_frame(hd), size);
            MemoryBarrier();
            if (seq1 == *(volatile LONG *) &hd->seq) {
                w = fw;
                h = fh;
                return true;
            }
        }
        return false;
    }

private:
    bool Open() {
        if (m_header) {
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (m_lastAttempt && now - m_lastAttempt < 1000) {
            return false;
        }
        m_lastAttempt = now;

        const DWORD access = FILE_MAP_READ | FILE_MAP_WRITE;
        HANDLE h = OpenFileMappingW(access, FALSE, MOBLENS_SHM_NAME_GLOBAL);
        if (!h) {
            h = OpenFileMappingW(access, FALSE, MOBLENS_SHM_NAME_LOCAL);
        }
        if (!h) {
            h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, MOBLENS_SHM_TOTAL_SIZE,
                                   MOBLENS_SHM_NAME_LOCAL);
        }
        if (!h) {
            return false;
        }
        void *view = MapViewOfFile(h, access, 0, 0, MOBLENS_SHM_TOTAL_SIZE);
        if (!view) {
            CloseHandle(h);
            return false;
        }
        m_mapping = h;
        m_header = (moblens_shm_header *) view;
        if (m_header->magic != MOBLENS_SHM_MAGIC) {
            m_header->version = MOBLENS_SHM_VERSION;
            MemoryBarrier();
            m_header->magic = MOBLENS_SHM_MAGIC;
        }
        return true;
    }

    HANDLE m_mapping = nullptr;
    moblens_shm_header *m_header = nullptr;
    ULONGLONG m_lastAttempt = 0;
};

// ----- Conversão NV12 -> formato de saída -----------------------------------

// Escreve o quadro NV12 src (sw x sh) em dst no formato f, redimensionando
// pelo vizinho mais próximo quando os tamanhos diferem
void Convert(const uint8_t *src, UINT32 sw, UINT32 sh, BYTE *dst,
             const Format &f) {
    const UINT32 w = f.width;
    const UINT32 h = f.height;
    const uint8_t *srcUV = src + (size_t) sw * sh;
    const bool same = sw == w && sh == h;

    std::vector<UINT32> xs(w);
    for (UINT32 x = 0; x < w; ++x) {
        xs[x] = same ? x : (UINT32) ((uint64_t) x * sw / w);
    }
    auto srcRow = [&](UINT32 y) {
        return same ? y : (UINT32) ((uint64_t) y * sh / h);
    };

    if (f.pixel == Pixel::YUY2) {
        for (UINT32 y = 0; y < h; ++y) {
            const UINT32 sy = srcRow(y);
            const uint8_t *yRow = src + (size_t) sy * sw;
            const uint8_t *uvRow = srcUV + (size_t) (sy / 2) * sw;
            BYTE *out = dst + (size_t) y * w * 2;
            for (UINT32 x = 0; x < w; x += 2) {
                const UINT32 sx = xs[x] & ~1u;
                out[0] = yRow[xs[x]];
                out[1] = uvRow[sx];
                out[2] = yRow[xs[x + 1 < w ? x + 1 : x]];
                out[3] = uvRow[sx + 1];
                out += 4;
            }
        }
        return;
    }

    // Plano Y (igual em NV12 e I420)
    for (UINT32 y = 0; y < h; ++y) {
        const uint8_t *yRow = src + (size_t) srcRow(y) * sw;
        BYTE *out = dst + (size_t) y * w;
        if (same) {
            memcpy(out, yRow, w);
        } else {
            for (UINT32 x = 0; x < w; ++x) {
                out[x] = yRow[xs[x]];
            }
        }
    }
    BYTE *uvOut = dst + (size_t) w * h;
    for (UINT32 y = 0; y < h / 2; ++y) {
        const uint8_t *uvRow = srcUV + (size_t) (srcRow(y * 2) / 2) * sw;
        if (f.pixel == Pixel::NV12) {
            BYTE *out = uvOut + (size_t) y * w;
            if (same) {
                memcpy(out, uvRow, w);
            } else {
                for (UINT32 x = 0; x < w / 2; ++x) {
                    const UINT32 sx = xs[x * 2] & ~1u;
                    out[2 * x] = uvRow[sx];
                    out[2 * x + 1] = uvRow[sx + 1];
                }
            }
        } else {  // I420: plano U, depois plano V
            BYTE *u = uvOut + (size_t) y * (w / 2);
            BYTE *v = uvOut + (size_t) (w / 2) * (h / 2) + (size_t) y * (w / 2);
            for (UINT32 x = 0; x < w / 2; ++x) {
                const UINT32 sx = xs[x * 2] & ~1u;
                u[x] = uvRow[sx];
                v[x] = uvRow[sx + 1];
            }
        }
    }
}

// Imagem de espera (sem celular): fundo escuro com uma faixa que se move
void Placeholder(BYTE *dst, const Format &f, UINT32 tick) {
    const UINT32 w = f.width;
    const UINT32 h = f.height;
    const UINT32 stripeW = w / 8 ? w / 8 : 1;
    const UINT32 stripeX = (tick * 8) % (w + stripeW);
    const UINT32 top = h * 2 / 3;
    auto luma = [&](UINT32 x, UINT32 y) -> BYTE {
        if (y < top) {
            return 28;
        }
        return x + stripeW >= stripeX && x < stripeX ? 120 : 20;
    };
    if (f.pixel == Pixel::YUY2) {
        for (UINT32 y = 0; y < h; ++y) {
            BYTE *out = dst + (size_t) y * w * 2;
            for (UINT32 x = 0; x < w; ++x) {
                out[2 * x] = luma(x, y);
                out[2 * x + 1] = 128;
            }
        }
        return;
    }
    for (UINT32 y = 0; y < h; ++y) {
        for (UINT32 x = 0; x < w; ++x) {
            dst[(size_t) y * w + x] = luma(x, y);
        }
    }
    memset(dst + (size_t) w * h, 128, (size_t) w * h / 2);
}

// ----- Enumeradores ------------------------------------------------------------

class Filter;
class OutputPin;

class PinEnum : public IEnumPins {
public:
    PinEnum(IPin *pin, ULONG pos) : m_pin(pin), m_pos(pos) {
        m_pin->AddRef();
        InterlockedIncrement(&g_objects);
    }
    virtual ~PinEnum() {
        m_pin->Release();
        InterlockedDecrement(&g_objects);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_ref);
    }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) {
            delete this;
        }
        return r;
    }

    STDMETHODIMP Next(ULONG count, IPin **pins, ULONG *fetched) override {
        if (!pins) {
            return E_POINTER;
        }
        ULONG n = 0;
        if (count && m_pos == 0) {
            pins[0] = m_pin;
            m_pin->AddRef();
            m_pos = 1;
            n = 1;
        }
        if (fetched) {
            *fetched = n;
        }
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        m_pos += count;
        return m_pos <= 1 ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        m_pos = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins **out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = new (std::nothrow) PinEnum(m_pin, m_pos);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

private:
    volatile LONG m_ref = 1;
    IPin *m_pin;
    ULONG m_pos;
};

class TypeEnum : public IEnumMediaTypes {
public:
    explicit TypeEnum(ULONG pos) : m_pos(pos) {
        InterlockedIncrement(&g_objects);
    }
    virtual ~TypeEnum() { InterlockedDecrement(&g_objects); }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_ref);
    }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) {
            delete this;
        }
        return r;
    }

    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE **types,
                      ULONG *fetched) override {
        if (!types) {
            return E_POINTER;
        }
        ULONG n = 0;
        while (n < count && m_pos < (ULONG) kFormatCount) {
            auto *mt = (AM_MEDIA_TYPE *) CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            if (!mt || FAILED(BuildMediaType((int) m_pos, *mt))) {
                CoTaskMemFree(mt);
                break;
            }
            types[n++] = mt;
            ++m_pos;
        }
        if (fetched) {
            *fetched = n;
        }
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        m_pos += count;
        return m_pos <= (ULONG) kFormatCount ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override {
        m_pos = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes **out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = new (std::nothrow) TypeEnum(m_pos);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

private:
    volatile LONG m_ref = 1;
    ULONG m_pos;
};

// ----- Pino de saída ----------------------------------------------------------

class OutputPin : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    explicit OutputPin(Filter *filter) : m_filter(filter) {
        ZeroMemory(&m_mt, sizeof(m_mt));
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~OutputPin() {
        StopStreaming();
        ReleaseConnection();
        CloseHandle(m_stopEvent);
    }

    // IUnknown: contagem delegada ao filtro
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin *receive, const AM_MEDIA_TYPE *mt) override;
    STDMETHODIMP ReceiveConnection(IPin *, const AM_MEDIA_TYPE *) override {
        return E_UNEXPECTED;  // pino de saída
    }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin **pin) override {
        if (!pin) {
            return E_POINTER;
        }
        CsLock lock(this);
        *pin = m_connected;
        if (!m_connected) {
            return VFW_E_NOT_CONNECTED;
        }
        m_connected->AddRef();
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *mt) override {
        if (!mt) {
            return E_POINTER;
        }
        CsLock lock(this);
        if (!m_connected) {
            ZeroMemory(mt, sizeof(*mt));
            return VFW_E_NOT_CONNECTED;
        }
        return CopyMediaType(*mt, m_mt);
    }
    STDMETHODIMP QueryPinInfo(PIN_INFO *info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION *dir) override {
        if (!dir) {
            return E_POINTER;
        }
        *dir = PINDIR_OUTPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR *id) override {
        if (!id) {
            return E_POINTER;
        }
        static const wchar_t kId[] = L"Capture";
        *id = (LPWSTR) CoTaskMemAlloc(sizeof(kId));
        if (!*id) {
            return E_OUTOFMEMORY;
        }
        memcpy(*id, kId, sizeof(kId));
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *mt) override {
        return mt && MatchFormat(mt) >= 0 ? S_OK : S_FALSE;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = new (std::nothrow) TypeEnum(0);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP QueryInternalConnections(IPin **, ULONG *) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
    STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override {
        return S_OK;
    }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE *mt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE **mt) override;
    STDMETHODIMP GetNumberOfCapabilities(int *count, int *size) override {
        if (!count || !size) {
            return E_POINTER;
        }
        *count = kFormatCount;
        *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE **mt,
                               BYTE *caps) override;

    // IKsPropertySet: categoria do pino = captura
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data,
                     DWORD size, DWORD *returned) override {
        if (set != AMPROPSETID_Pin) {
            return E_PROP_SET_UNSUPPORTED;
        }
        if (id != AMPROPERTY_PIN_CATEGORY) {
            return E_PROP_ID_UNSUPPORTED;
        }
        if (!data && !returned) {
            return E_POINTER;
        }
        if (returned) {
            *returned = sizeof(GUID);
        }
        if (!data) {
            return S_OK;
        }
        if (size < sizeof(GUID)) {
            return E_UNEXPECTED;
        }
        *(GUID *) data = PIN_CATEGORY_CAPTURE;
        return S_OK;
    }
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id,
                                DWORD *support) override {
        if (set != AMPROPSETID_Pin) {
            return E_PROP_SET_UNSUPPORTED;
        }
        if (id != AMPROPERTY_PIN_CATEGORY) {
            return E_PROP_ID_UNSUPPORTED;
        }
        if (support) {
            *support = KSPROPERTY_SUPPORT_GET;
        }
        return S_OK;
    }

    // Chamados pelo filtro
    bool IsConnected() const { return m_connected != nullptr; }
    HRESULT Active();       // Parado -> Pausado: aloca buffers e inicia
    void Inactive();        // -> Parado
    void StopStreaming();

private:
    struct CsLock {
        explicit CsLock(OutputPin *pin);
        ~CsLock();
        CRITICAL_SECTION *cs;
    };

    HRESULT TryConnect(IPin *receive, int index);
    HRESULT DecideAllocator();
    void ReleaseConnection();
    static DWORD WINAPI ThreadProc(void *self);
    void StreamLoop();

    Filter *m_filter;
    IPin *m_connected = nullptr;
    IMemInputPin *m_input = nullptr;
    IMemAllocator *m_allocator = nullptr;
    AM_MEDIA_TYPE m_mt;
    Format m_format = FormatAt(0);
    int m_forced = -1;  // formato escolhido via IAMStreamConfig::SetFormat

    HANDLE m_thread = nullptr;
    HANDLE m_stopEvent = nullptr;
};

// ----- Filtro ----------------------------------------------------------------------

class Filter : public IBaseFilter, public IAMFilterMiscFlags {
public:
    Filter() : m_pin(this) {
        InitializeCriticalSection(&m_cs);
        InterlockedIncrement(&g_objects);
    }
    virtual ~Filter() {
        m_pin.StopStreaming();
        if (m_clock) {
            m_clock->Release();
        }
        DeleteCriticalSection(&m_cs);
        InterlockedDecrement(&g_objects);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IPersist
                || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
            *ppv = static_cast<IBaseFilter *>(this);
        } else if (riid == IID_IAMFilterMiscFlags) {
            *ppv = static_cast<IAMFilterMiscFlags *>(this);
        } else if (riid == IID_IAMStreamConfig) {
            // Alguns aplicativos procuram a interface no filtro
            *ppv = static_cast<IAMStreamConfig *>(&m_pin);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_ref);
    }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) {
            delete this;
        }
        return r;
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID *clsid) override {
        if (!clsid) {
            return E_POINTER;
        }
        *clsid = CLSID_MobLensDShow;
        return S_OK;
    }

    // IMediaFilter
    STDMETHODIMP Stop() override {
        EnterCriticalSection(&m_cs);
        const bool wasRunning = m_state != State_Stopped;
        m_state = State_Stopped;
        LeaveCriticalSection(&m_cs);
        if (wasRunning) {
            m_pin.Inactive();
        }
        return S_OK;
    }
    STDMETHODIMP Pause() override {
        EnterCriticalSection(&m_cs);
        const bool fromStopped = m_state == State_Stopped;
        m_state = State_Paused;
        LeaveCriticalSection(&m_cs);
        if (fromStopped && m_pin.IsConnected()) {
            return m_pin.Active();
        }
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME start) override {
        HRESULT hr = S_OK;
        if (m_state == State_Stopped) {
            hr = Pause();
            if (FAILED(hr)) {
                return hr;
            }
        }
        EnterCriticalSection(&m_cs);
        m_start = start;
        m_state = State_Running;
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE *state) override {
        if (!state) {
            return E_POINTER;
        }
        *state = m_state;
        // Fonte ao vivo: não entrega quadros pausada
        return m_state == State_Paused ? VFW_S_CANT_CUE : S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock *clock) override {
        EnterCriticalSection(&m_cs);
        if (clock) {
            clock->AddRef();
        }
        if (m_clock) {
            m_clock->Release();
        }
        m_clock = clock;
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock **clock) override {
        if (!clock) {
            return E_POINTER;
        }
        EnterCriticalSection(&m_cs);
        *clock = m_clock;
        if (m_clock) {
            m_clock->AddRef();
        }
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins **out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = new (std::nothrow) PinEnum(&m_pin, 0);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP FindPin(LPCWSTR id, IPin **pin) override {
        if (!id || !pin) {
            return E_POINTER;
        }
        if (lstrcmpW(id, L"Capture")) {
            *pin = nullptr;
            return VFW_E_NOT_FOUND;
        }
        *pin = &m_pin;
        m_pin.AddRef();
        return S_OK;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO *info) override {
        if (!info) {
            return E_POINTER;
        }
        lstrcpynW(info->achName, m_name, MAX_FILTER_NAME);
        info->pGraph = m_graph;
        if (m_graph) {
            m_graph->AddRef();
        }
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph *graph, LPCWSTR name) override {
        // Referência fraca ao grafo (regra do DirectShow)
        EnterCriticalSection(&m_cs);
        m_graph = graph;
        lstrcpynW(m_name, name ? name : L"", MAX_FILTER_NAME);
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR *) override { return E_NOTIMPL; }

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override {
        return AM_FILTER_MISC_FLAGS_IS_SOURCE;
    }

    // Tempo de mídia atual (relativo ao início), ou -1 sem relógio
    REFERENCE_TIME StreamTime() {
        EnterCriticalSection(&m_cs);
        REFERENCE_TIME t = -1;
        if (m_clock && m_state == State_Running
                && SUCCEEDED(m_clock->GetTime(&t))) {
            t -= m_start;
        } else {
            t = -1;
        }
        LeaveCriticalSection(&m_cs);
        return t;
    }

    FILTER_STATE State() const { return m_state; }
    IFilterGraph *Graph() const { return m_graph; }
    CRITICAL_SECTION *Lock() { return &m_cs; }

private:
    volatile LONG m_ref = 1;
    CRITICAL_SECTION m_cs;
    OutputPin m_pin;
    volatile FILTER_STATE m_state = State_Stopped;
    IReferenceClock *m_clock = nullptr;
    REFERENCE_TIME m_start = 0;
    IFilterGraph *m_graph = nullptr;
    wchar_t m_name[MAX_FILTER_NAME] = L"";
};

// ----- OutputPin (implementação) -----------------------------------------------

OutputPin::CsLock::CsLock(OutputPin *pin) : cs(pin->m_filter->Lock()) {
    EnterCriticalSection(cs);
}
OutputPin::CsLock::~CsLock() {
    LeaveCriticalSection(cs);
}

STDMETHODIMP OutputPin::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin *>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig *>(this);
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet *>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) OutputPin::AddRef() {
    return m_filter->AddRef();
}
STDMETHODIMP_(ULONG) OutputPin::Release() {
    return m_filter->Release();
}

STDMETHODIMP OutputPin::QueryPinInfo(PIN_INFO *info) {
    if (!info) {
        return E_POINTER;
    }
    info->pFilter = m_filter;
    m_filter->AddRef();
    info->dir = PINDIR_OUTPUT;
    lstrcpynW(info->achName, L"Capture", MAX_PIN_NAME);
    return S_OK;
}

STDMETHODIMP OutputPin::Connect(IPin *receive, const AM_MEDIA_TYPE *mt) {
    if (!receive) {
        return E_POINTER;
    }
    CsLock lock(this);
    if (m_connected) {
        return VFW_E_ALREADY_CONNECTED;
    }
    if (m_filter->State() != State_Stopped) {
        return VFW_E_NOT_STOPPED;
    }

    // Formato pedido pelo grafo, depois o escolhido via SetFormat, depois
    // a lista em ordem de preferência
    if (mt && mt->formattype == FORMAT_VideoInfo && mt->pbFormat) {
        int index = MatchFormat(mt);
        return index >= 0 ? TryConnect(receive, index)
                          : VFW_E_TYPE_NOT_ACCEPTED;
    }
    if (m_forced >= 0 && (!mt || MatchFormat(mt, m_forced) == m_forced)) {
        if (SUCCEEDED(TryConnect(receive, m_forced))) {
            return S_OK;
        }
    }
    for (int i = 0; i < kFormatCount; ++i) {
        if (mt && MatchFormat(mt, i) != i) {
            continue;
        }
        if (SUCCEEDED(TryConnect(receive, i))) {
            return S_OK;
        }
    }
    return VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT OutputPin::TryConnect(IPin *receive, int index) {
    AM_MEDIA_TYPE mt;
    HRESULT hr = BuildMediaType(index, mt);
    if (FAILED(hr)) {
        return hr;
    }
    hr = receive->ReceiveConnection(this, &mt);
    if (FAILED(hr)) {
        FreeMediaType(mt);
        return hr;
    }
    m_connected = receive;
    m_connected->AddRef();
    m_mt = mt;  // assume a posse
    m_format = FormatAt(index);

    hr = receive->QueryInterface(IID_IMemInputPin, (void **) &m_input);
    if (SUCCEEDED(hr)) {
        hr = DecideAllocator();
    }
    if (FAILED(hr)) {
        receive->Disconnect();
        ReleaseConnection();
    }
    return hr;
}

HRESULT OutputPin::DecideAllocator() {
    ALLOCATOR_PROPERTIES req = {};
    m_input->GetAllocatorRequirements(&req);
    const long size = (long) ImageSize(m_format);

    for (int attempt = 0; attempt < 2; ++attempt) {
        IMemAllocator *alloc = nullptr;
        HRESULT hr = attempt == 0
            ? m_input->GetAllocator(&alloc)
            : CoCreateInstance(CLSID_MemoryAllocator, nullptr,
                               CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                               (void **) &alloc);
        if (FAILED(hr) || !alloc) {
            continue;
        }
        ALLOCATOR_PROPERTIES props = {};
        props.cBuffers = (std::max)(req.cBuffers, 3L);
        props.cbBuffer = (std::max)(req.cbBuffer, size);
        props.cbAlign = (std::max)(req.cbAlign, 1L);
        props.cbPrefix = req.cbPrefix;
        ALLOCATOR_PROPERTIES actual = {};
        hr = alloc->SetProperties(&props, &actual);
        if (SUCCEEDED(hr) && actual.cbBuffer >= size) {
            hr = m_input->NotifyAllocator(alloc, FALSE);
            if (SUCCEEDED(hr)) {
                m_allocator = alloc;
                return S_OK;
            }
        }
        alloc->Release();
    }
    return VFW_E_NO_ALLOCATOR;
}

void OutputPin::ReleaseConnection() {
    if (m_allocator) {
        m_allocator->Release();
        m_allocator = nullptr;
    }
    if (m_input) {
        m_input->Release();
        m_input = nullptr;
    }
    if (m_connected) {
        m_connected->Release();
        m_connected = nullptr;
    }
    FreeMediaType(m_mt);
    ZeroMemory(&m_mt, sizeof(m_mt));
}

STDMETHODIMP OutputPin::Disconnect() {
    CsLock lock(this);
    if (m_filter->State() != State_Stopped) {
        return VFW_E_NOT_STOPPED;
    }
    if (!m_connected) {
        return S_FALSE;
    }
    ReleaseConnection();
    return S_OK;
}

STDMETHODIMP OutputPin::SetFormat(AM_MEDIA_TYPE *mt) {
    if (!mt) {
        m_forced = -1;  // volta ao padrão
        return S_OK;
    }
    const int index = MatchFormat(mt);
    if (index < 0) {
        return VFW_E_INVALIDMEDIATYPE;
    }
    IPin *peer = nullptr;
    IFilterGraph *graph = nullptr;
    {
        CsLock lock(this);
        if (m_filter->State() != State_Stopped) {
            return VFW_E_NOT_STOPPED;
        }
        m_forced = index;
        if (m_connected && FormatAt(index).width == m_format.width
                && FormatAt(index).height == m_format.height
                && FormatAt(index).pixel == m_format.pixel) {
            return S_OK;
        }
        if (m_connected) {
            peer = m_connected;
            peer->AddRef();
            graph = m_filter->Graph();
        }
    }
    HRESULT hr = S_OK;
    if (peer) {
        // Já conectado: reconecta com o formato novo
        AM_MEDIA_TYPE wanted;
        BuildMediaType(index, wanted);
        hr = peer->QueryAccept(&wanted) == S_OK && graph
                 ? graph->Reconnect(this)
                 : VFW_E_INVALIDMEDIATYPE;
        FreeMediaType(wanted);
        peer->Release();
    }
    return hr;
}

STDMETHODIMP OutputPin::GetFormat(AM_MEDIA_TYPE **mt) {
    if (!mt) {
        return E_POINTER;
    }
    *mt = (AM_MEDIA_TYPE *) CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!*mt) {
        return E_OUTOFMEMORY;
    }
    CsLock lock(this);
    HRESULT hr = m_connected ? CopyMediaType(**mt, m_mt)
                             : BuildMediaType(m_forced >= 0 ? m_forced : 0,
                                              **mt);
    if (FAILED(hr)) {
        CoTaskMemFree(*mt);
        *mt = nullptr;
    }
    return hr;
}

STDMETHODIMP OutputPin::GetStreamCaps(int index, AM_MEDIA_TYPE **mt,
                                      BYTE *caps) {
    if (!mt || !caps) {
        return E_POINTER;
    }
    if (index < 0 || index >= kFormatCount) {
        return S_FALSE;
    }
    *mt = (AM_MEDIA_TYPE *) CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!*mt) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = BuildMediaType(index, **mt);
    if (FAILED(hr)) {
        CoTaskMemFree(*mt);
        *mt = nullptr;
        return hr;
    }
    const Format f = FormatAt(index);
    auto *c = (VIDEO_STREAM_CONFIG_CAPS *) caps;
    ZeroMemory(c, sizeof(*c));
    const SIZE size = {(LONG) f.width, (LONG) f.height};
    c->guid = FORMAT_VideoInfo;
    c->InputSize = size;
    c->MinCroppingSize = size;
    c->MaxCroppingSize = size;
    c->CropGranularityX = 1;
    c->CropGranularityY = 1;
    c->MinOutputSize = size;
    c->MaxOutputSize = size;
    c->OutputGranularityX = 1;
    c->OutputGranularityY = 1;
    c->MinFrameInterval = kFrameTime;
    c->MaxFrameInterval = kFrameTime;
    c->MinBitsPerSecond = (LONG) (ImageSize(f) * 8 * MOBLENS_FPS);
    c->MaxBitsPerSecond = c->MinBitsPerSecond;
    return S_OK;
}

HRESULT OutputPin::Active() {
    CsLock lock(this);
    if (!m_allocator) {
        return VFW_E_NO_ALLOCATOR;
    }
    HRESULT hr = m_allocator->Commit();
    if (FAILED(hr)) {
        return hr;
    }
    if (!m_thread) {
        ResetEvent(m_stopEvent);
        m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    }
    return m_thread ? S_OK : E_FAIL;
}

void OutputPin::Inactive() {
    StopStreaming();
}

void OutputPin::StopStreaming() {
    SetEvent(m_stopEvent);
    if (m_allocator) {
        m_allocator->Decommit();  // desbloqueia GetBuffer
    }
    if (m_thread) {
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

DWORD WINAPI OutputPin::ThreadProc(void *self) {
    static_cast<OutputPin *>(self)->StreamLoop();
    return 0;
}

void OutputPin::StreamLoop() {
    SharedFrames shared;
    std::vector<uint8_t> frame;
    const Format f = m_format;
    const DWORD size = ImageSize(f);
    LARGE_INTEGER freq;
    LARGE_INTEGER next;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    const LONGLONG period = freq.QuadPart / MOBLENS_FPS;
    UINT32 tick = 0;
    LONGLONG delivered = 0;

    for (;;) {
        // Ritmo de 30 quadros por segundo
        next.QuadPart += period;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG waitMs = (next.QuadPart - now.QuadPart) * 1000
                        / freq.QuadPart;
        if (waitMs < 0) {
            if (waitMs < -200) {
                next = now;  // atrasou demais: reinicia o ritmo
            }
            waitMs = 0;
        }
        if (WaitForSingleObject(m_stopEvent, (DWORD) waitMs)
                == WAIT_OBJECT_0) {
            break;
        }
        if (m_filter->State() != State_Running) {
            continue;
        }

        IMediaSample *sample = nullptr;
        if (FAILED(m_allocator->GetBuffer(&sample, nullptr, nullptr, 0))
                || !sample) {
            if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }
        BYTE *data = nullptr;
        sample->GetPointer(&data);
        if (data && sample->GetSize() >= (long) size) {
            UINT32 fw = 0;
            UINT32 fh = 0;
            if (shared.Read(f.width, f.height, frame, fw, fh)) {
                Convert(frame.data(), fw, fh, data, f);
            } else {
                Placeholder(data, f, tick);
            }
            ++tick;
            sample->SetActualDataLength((long) size);

            REFERENCE_TIME start = m_filter->StreamTime();
            if (start < 0) {
                start = delivered * kFrameTime;
            }
            REFERENCE_TIME stop = start + kFrameTime;
            sample->SetTime(&start, &stop);
            sample->SetSyncPoint(TRUE);
            sample->SetDiscontinuity(delivered == 0);
            m_input->Receive(sample);
            ++delivered;
        }
        sample->Release();
    }
    shared.Close();
}

// ----- Fábrica e registro ------------------------------------------------------

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory *>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
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
        Filter *filter = new (std::nothrow) Filter();
        if (!filter) {
            return E_OUTOFMEMORY;
        }
        HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&g_locks);
        } else {
            InterlockedDecrement(&g_locks);
        }
        return S_OK;
    }
};

ClassFactory g_factory;

const wchar_t kClsidKey[] =
    L"SOFTWARE\\Classes\\CLSID\\" MOBLENS_DSHOW_CLSID_STR;

HRESULT SetRegString(const std::wstring &subkey, const wchar_t *name,
                     const wchar_t *value) {
    HKEY key = nullptr;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                             &key, nullptr);
    if (r != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(r);
    }
    r = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *) value,
                       (DWORD) ((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(r);
}

// Registra/remove na categoria "Video Capture Sources"
HRESULT RegisterInCategory(bool add) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFilterMapper2 *mapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IFilterMapper2,
                                  (void **) &mapper);
    if (SUCCEEDED(hr)) {
        if (add) {
            const GUID subtype = SubtypeFromFourCC(FourCC('N', 'V', '1', '2'));
            REGPINTYPES type = {&MEDIATYPE_Video, &subtype};
            REGFILTERPINS2 pin = {};
            pin.dwFlags = REG_PINFLAG_B_OUTPUT;
            pin.cInstances = 1;
            pin.nMediaTypes = 1;
            pin.lpMediaType = &type;
            pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;
            REGFILTER2 rf = {};
            rf.dwVersion = 2;
            rf.dwMerit = MERIT_DO_NOT_USE;
            rf.cPins2 = 1;
            rf.rgPins2 = &pin;
            IMoniker *moniker = nullptr;
            hr = mapper->RegisterFilter(CLSID_MobLensDShow,
                                        MOBLENS_FRIENDLY_NAME, &moniker,
                                        &CLSID_VideoInputDeviceCategory,
                                        MOBLENS_DSHOW_CLSID_STR, &rf);
            if (moniker) {
                moniker->Release();
            }
        } else {
            hr = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory,
                                          MOBLENS_DSHOW_CLSID_STR,
                                          CLSID_MobLensDShow);
        }
        mapper->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return hr;
}

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
    if (clsid != CLSID_MobLensDShow) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        return E_FAIL;
    }
    const std::wstring key = kClsidKey;
    HRESULT hr = SetRegString(key, nullptr, MOBLENS_FRIENDLY_NAME);
    if (SUCCEEDED(hr)) {
        hr = SetRegString(key + L"\\InprocServer32", nullptr, path);
    }
    if (SUCCEEDED(hr)) {
        hr = SetRegString(key + L"\\InprocServer32", L"ThreadingModel",
                          L"Both");
    }
    if (SUCCEEDED(hr)) {
        hr = RegisterInCategory(true);
    }
    return hr;
}

STDAPI DllUnregisterServer() {
    RegisterInCategory(false);
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND
               ? S_OK
               : HRESULT_FROM_WIN32(r);
}
