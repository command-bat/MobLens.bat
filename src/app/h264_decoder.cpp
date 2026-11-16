// Decodificação H.264 com o decodificador nativo do Windows (Media Foundation
// MFT), que entrega NV12 - o mesmo formato da câmera virtual.
#include "app.h"

#include <initguid.h>
#include <cguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstring>

// MF_LOW_LATENCY (mfapi.h do Windows SDK): decodifica sem reter quadros
DEFINE_GUID(MCAM_MF_LOW_LATENCY,
    0x9c27891a, 0xed7a, 0x40e1, 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee);

template <class T> static void SafeRelease(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

H264Decoder::~H264Decoder() {
    Close();
}

void H264Decoder::Close() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        SafeRelease(m_mft);
    }
}

bool H264Decoder::Init(UINT32 width, UINT32 height) {
    Close();

    // Procura um decodificador H.264 síncrono (o decodificador da Microsoft)
    MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT
                               | MFT_ENUM_FLAG_SORTANDFILTER,
                           &input, nullptr, &activates, &count);
    if (FAILED(hr) || !count) {
        CoTaskMemFree(activates);
        return false;
    }
    hr = activates[0]->ActivateObject(IID_IMFTransform,
                                      reinterpret_cast<void **>(&m_mft));
    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    if (FAILED(hr)) {
        return false;
    }

    IMFAttributes *attrs = nullptr;
    if (SUCCEEDED(m_mft->GetAttributes(&attrs)) && attrs) {
        attrs->SetUINT32(MCAM_MF_LOW_LATENCY, TRUE);
        attrs->Release();
    }

    IMFMediaType *type = nullptr;
    hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) {
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
        hr = m_mft->SetInputType(0, type, 0);
        type->Release();
    }
    if (FAILED(hr) || !ConfigureOutput()) {
        Close();
        return false;
    }

    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::ConfigureOutput() {
    for (DWORD i = 0;; ++i) {
        IMFMediaType *type = nullptr;
        if (FAILED(m_mft->GetOutputAvailableType(0, i, &type))) {
            return false;
        }
        GUID subtype = GUID_NULL;
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype != MFVideoFormat_NV12) {
            type->Release();
            continue;
        }

        HRESULT hr = m_mft->SetOutputType(0, type, 0);
        if (SUCCEEDED(hr)) {
            MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &m_codedWidth,
                               &m_codedHeight);
            m_visibleWidth = m_codedWidth;
            m_visibleHeight = m_codedHeight;

            // A altura codificada costuma ser alinhada (ex.: 1088); a área
            // visível vem da abertura mínima de exibição
            MFVideoArea area = {};
            if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                        reinterpret_cast<UINT8 *>(&area),
                                        sizeof(area), nullptr))
                    && area.Area.cx > 0 && area.Area.cy > 0) {
                m_visibleWidth = (UINT32) area.Area.cx;
                m_visibleHeight = (UINT32) area.Area.cy;
            }

            UINT32 stride = 0;
            m_defaultStride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                                        &stride))
                            ? (LONG) stride : (LONG) m_codedWidth;
        }
        type->Release();
        return SUCCEEDED(hr);
    }
}

bool H264Decoder::Decode(const uint8_t *data, size_t size, int64_t ptsUs,
                         const FrameCallback &onFrame) {
    if (!m_mft || !size) {
        return false;
    }

    IMFMediaBuffer *buffer = nullptr;
    IMFSample *sample = nullptr;
    HRESULT hr = MFCreateMemoryBuffer((DWORD) size, &buffer);
    if (SUCCEEDED(hr)) {
        BYTE *p = nullptr;
        hr = buffer->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(p, data, size);
            buffer->Unlock();
            buffer->SetCurrentLength((DWORD) size);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = MFCreateSample(&sample);
    }
    if (SUCCEEDED(hr)) {
        sample->AddBuffer(buffer);
        sample->SetSampleTime(ptsUs * 10);
        hr = m_mft->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            // Esvazia a saída e tenta de novo
            if (Drain(onFrame)) {
                hr = m_mft->ProcessInput(0, sample, 0);
            }
        }
    }
    SafeRelease(sample);
    SafeRelease(buffer);

    if (FAILED(hr)) {
        return false;
    }
    return Drain(onFrame);
}

bool H264Decoder::Drain(const FrameCallback &onFrame) {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info = {};
        if (FAILED(m_mft->GetOutputStreamInfo(0, &info))) {
            return false;
        }

        IMFSample *outSample = nullptr;
        bool mftProvides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES
                                           | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        if (!mftProvides) {
            IMFMediaBuffer *buf = nullptr;
            if (FAILED(MFCreateMemoryBuffer(info.cbSize, &buf))) {
                return false;
            }
            MFCreateSample(&outSample);
            outSample->AddBuffer(buf);
            buf->Release();
        }

        MFT_OUTPUT_DATA_BUFFER out = {};
        out.dwStreamID = 0;
        out.pSample = outSample;
        DWORD status = 0;
        HRESULT hr = m_mft->ProcessOutput(0, 1, &out, &status);
        SafeRelease(out.pEvents);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            SafeRelease(outSample);
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            SafeRelease(outSample);
            if (!ConfigureOutput()) {
                return false;
            }
            continue;
        }
        if (FAILED(hr)) {
            if (out.pSample != outSample) {
                SafeRelease(out.pSample);
            }
            SafeRelease(outSample);
            return false;
        }

        IMFSample *result = out.pSample;
        IMFMediaBuffer *buffer = nullptr;
        if (result && SUCCEEDED(result->ConvertToContiguousBuffer(&buffer))) {
            IMF2DBuffer *buffer2d = nullptr;
            BYTE *base = nullptr;
            LONG pitch = 0;
            bool locked2d = false;
            if (SUCCEEDED(buffer->QueryInterface(
                    IID_IMF2DBuffer, reinterpret_cast<void **>(&buffer2d)))
                    && SUCCEEDED(buffer2d->Lock2D(&base, &pitch))) {
                locked2d = true;
            } else if (SUCCEEDED(buffer->Lock(&base, nullptr, nullptr))) {
                pitch = m_defaultStride;
            } else {
                base = nullptr;
            }

            if (base && pitch > 0) {
                Nv12Frame frame;
                frame.y = base;
                frame.uv = base + (size_t) pitch * m_codedHeight;
                frame.pitch = pitch;
                frame.width = m_visibleWidth;
                frame.height = m_visibleHeight;
                onFrame(frame);
            }

            if (locked2d) {
                buffer2d->Unlock2D();
            } else if (base) {
                buffer->Unlock();
            }
            SafeRelease(buffer2d);
            buffer->Release();
        }

        if (result != outSample) {
            SafeRelease(result);
        }
        SafeRelease(outSample);
    }
}
