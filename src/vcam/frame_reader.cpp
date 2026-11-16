// Leitura dos quadros publicados pelo scrcpy na memória compartilhada.
#include "vcam.h"

#include <sddl.h>
#include <cstring>

FrameReader::FrameReader() = default;

FrameReader::~FrameReader() {
    Close();
}

void FrameReader::Close() {
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

void FrameReader::SetActive(bool active) {
    if (m_header) {
        m_header->consumer_active = active ? 1 : 0;
    }
}

static HANDLE CreateSharedMapping(const wchar_t *name, bool withSecurity) {
    SECURITY_ATTRIBUTES sa = {};
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (withSecurity &&
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            MOBLENS_SHM_SDDL, SDDL_REVISION_1, &psd, nullptr)) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = psd;
        sa.bInheritHandle = FALSE;
    }
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, psd ? &sa : nullptr,
                                  PAGE_READWRITE, 0, MOBLENS_SHM_TOTAL_SIZE,
                                  name);
    if (psd) {
        LocalFree(psd);
    }
    return h;
}

bool FrameReader::TryOpen() {
    if (m_header) {
        return true;
    }

    ULONGLONG now = GetTickCount64();
    if (m_lastOpenAttempt && now - m_lastOpenAttempt < 1000) {
        return false;
    }
    m_lastOpenAttempt = now;

    const DWORD access = FILE_MAP_READ | FILE_MAP_WRITE;
    HANDLE h = OpenFileMappingW(access, FALSE, MOBLENS_SHM_NAME_GLOBAL);
    if (!h) {
        // O Frame Server roda como serviço e tem SeCreateGlobalPrivilege
        h = CreateSharedMapping(MOBLENS_SHM_NAME_GLOBAL, true);
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
    m_header = static_cast<struct moblens_shm_header *>(view);
    if (m_header->magic != MOBLENS_SHM_MAGIC) {
        // Memória recém-criada (zerada): inicializa o cabeçalho
        m_header->version = MOBLENS_SHM_VERSION;
        MemoryBarrier();
        m_header->magic = MOBLENS_SHM_MAGIC;
    }
    return true;
}

bool FrameReader::ReadShared(UINT32 width, UINT32 height) {
    if (!TryOpen()) {
        return false;
    }

    struct moblens_shm_header *h = m_header;
    h->requested_width = (int32_t) width;
    h->requested_height = (int32_t) height;
    h->consumer_active = 1;

    if (h->magic != MOBLENS_SHM_MAGIC) {
        return false;
    }

    ULONGLONG now = GetTickCount64();
    int64_t producerTick = h->producer_tick_ms;
    if (producerTick <= 0
            || now - (ULONGLONG) producerTick > MOBLENS_PRODUCER_TIMEOUT_MS) {
        return false;  // nenhum produtor ativo
    }

    const uint8_t *src = moblens_shm_frame(h);
    for (int attempt = 0; attempt < 4; ++attempt) {
        LONG seq1 = *(volatile LONG *) &h->seq;
        MemoryBarrier();
        if (seq1 & 1) {
            Sleep(0);  // produtor escrevendo
            continue;
        }

        UINT32 w = h->width;
        UINT32 hh = h->height;
        UINT32 size = h->frame_size;
        if (w != width || hh != height || size != w * hh * 3 / 2
                || size > MOBLENS_SHM_FRAME_MAX) {
            // O produtor ainda não se adaptou ao tamanho solicitado
            return false;
        }

        m_frame.resize(size);
        memcpy(m_frame.data(), src, size);
        MemoryBarrier();

        LONG seq2 = *(volatile LONG *) &h->seq;
        if (seq1 == seq2) {
            m_frameWidth = w;
            m_frameHeight = hh;
            m_lastGoodTick = now;
            return true;
        }
    }
    return false;
}

void FrameReader::Fill(uint8_t *dst, UINT32 width, UINT32 height) {
    ++m_tick;
    bool fresh = ReadShared(width, height);
    bool recent = m_frameWidth == width && m_frameHeight == height
            && GetTickCount64() - m_lastGoodTick < MOBLENS_PRODUCER_TIMEOUT_MS;
    if (fresh || recent) {
        memcpy(dst, m_frame.data(), (size_t) width * height * 3 / 2);
    } else {
        DrawPlaceholder(dst, width, height);
    }
}

// Imagem exibida enquanto o celular não está transmitindo: barras de cor
// escurecidas e uma faixa que se move (indica que a câmera está viva).
void FrameReader::DrawPlaceholder(uint8_t *dst, UINT32 width, UINT32 height) {
    // Cores BT.601 (faixa limitada): branco, amarelo, ciano, verde, magenta,
    // vermelho, azul
    static const uint8_t bars[7][3] = {
        {180, 128, 128}, {162, 44, 142}, {131, 156, 44}, {112, 72, 58},
        {84, 184, 198}, {65, 100, 212}, {35, 212, 114},
    };

    uint8_t *yPlane = dst;
    uint8_t *uvPlane = dst + (size_t) width * height;
    UINT32 barsHeight = height * 2 / 3;
    UINT32 stripeWidth = width / 8 ? width / 8 : 1;
    UINT32 stripeX = (m_tick * 8) % (width + stripeWidth);

    for (UINT32 y = 0; y < height; ++y) {
        uint8_t *row = yPlane + (size_t) y * width;
        if (y < barsHeight) {
            for (UINT32 x = 0; x < width; ++x) {
                const uint8_t *c = bars[x * 7 / width];
                // escurece para ~50%
                row[x] = (uint8_t) (16 + (c[0] - 16) / 2);
            }
        } else {
            for (UINT32 x = 0; x < width; ++x) {
                bool inStripe = x + stripeWidth >= stripeX && x < stripeX;
                row[x] = inStripe ? 120 : 24;
            }
        }
    }

    for (UINT32 y = 0; y < height / 2; ++y) {
        uint8_t *row = uvPlane + (size_t) y * width;
        bool inBars = y * 2 < barsHeight;
        for (UINT32 x = 0; x < width / 2; ++x) {
            if (inBars) {
                const uint8_t *c = bars[(x * 2) * 7 / width];
                row[2 * x] = (uint8_t) (128 + (c[1] - 128) / 2);
                row[2 * x + 1] = (uint8_t) (128 + (c[2] - 128) / 2);
            } else {
                row[2 * x] = 128;
                row[2 * x + 1] = 128;
            }
        }
    }
}
