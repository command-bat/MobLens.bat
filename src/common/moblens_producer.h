/*
 * MobLens - lado produtor da memória compartilhada (header-only, C).
 *
 * Usado pelo cliente scrcpy (--virtual-camera) e pelo modo de teste do
 * moblens.exe. Requer linkar com advapi32.
 */
#ifndef MOBLENS_PRODUCER_H
#define MOBLENS_PRODUCER_H

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <stdbool.h>
#include <stdint.h>

#include "moblens_shm.h"

struct moblens_producer {
    HANDLE mapping;
    struct moblens_shm_header *header;
    ULONGLONG last_attempt;
};

static inline void
moblens_producer_init(struct moblens_producer *p) {
    p->mapping = NULL;
    p->header = NULL;
    p->last_attempt = 0;
}

static inline bool
moblens_producer_is_open(const struct moblens_producer *p) {
    return p->header != NULL;
}

/*
 * Tenta abrir a memória compartilhada (no máximo uma tentativa por segundo).
 *
 * Normalmente ela é criada pela câmera virtual (dentro do serviço Frame
 * Server) quando algum aplicativo abre a webcam. Se o produtor tiver
 * privilégio para criar objetos globais (ex.: executado como administrador),
 * ele mesmo a cria.
 */
static inline bool
moblens_producer_try_open(struct moblens_producer *p) {
    if (p->header) {
        return true;
    }

    ULONGLONG now = GetTickCount64();
    if (p->last_attempt && now - p->last_attempt < 1000) {
        return false;
    }
    p->last_attempt = now;

    const DWORD access = FILE_MAP_READ | FILE_MAP_WRITE;
    HANDLE h = OpenFileMappingW(access, FALSE, MOBLENS_SHM_NAME_GLOBAL);
    if (!h) {
        PSECURITY_DESCRIPTOR psd = NULL;
        SECURITY_ATTRIBUTES sa;
        ZeroMemory(&sa, sizeof(sa));
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                MOBLENS_SHM_SDDL, SDDL_REVISION_1, &psd, NULL)) {
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = psd;
        }
        h = CreateFileMappingW(INVALID_HANDLE_VALUE, psd ? &sa : NULL,
                               PAGE_READWRITE, 0, MOBLENS_SHM_TOTAL_SIZE,
                               MOBLENS_SHM_NAME_GLOBAL);
        if (psd) {
            LocalFree(psd);
        }
    }
    if (!h) {
        return false;
    }

    void *view = MapViewOfFile(h, access, 0, 0, MOBLENS_SHM_TOTAL_SIZE);
    if (!view) {
        CloseHandle(h);
        return false;
    }

    p->mapping = h;
    p->header = (struct moblens_shm_header *) view;
    if (p->header->magic != MOBLENS_SHM_MAGIC) {
        p->header->version = MOBLENS_SHM_VERSION;
        MemoryBarrier();
        p->header->magic = MOBLENS_SHM_MAGIC;
    }
    return true;
}

static inline void
moblens_producer_close(struct moblens_producer *p) {
    if (p->header) {
        p->header->producer_tick_ms = 0;
        UnmapViewOfFile(p->header);
        p->header = NULL;
    }
    if (p->mapping) {
        CloseHandle(p->mapping);
        p->mapping = NULL;
    }
}

/* Tamanho que o aplicativo selecionou na webcam (padrão: 1920x1080) */
static inline void
moblens_producer_requested_size(const struct moblens_producer *p,
                                  uint32_t *width, uint32_t *height) {
    int32_t w = p->header ? p->header->requested_width : 0;
    int32_t h = p->header ? p->header->requested_height : 0;
    if (w <= 0 || h <= 0 || (uint32_t) w > MOBLENS_MAX_WIDTH
            || (uint32_t) h > MOBLENS_MAX_HEIGHT) {
        w = 1920;
        h = 1080;
    }
    *width = (uint32_t) w & ~1u;
    *height = (uint32_t) h & ~1u;
}

static inline bool
moblens_producer_consumer_active(const struct moblens_producer *p) {
    return p->header && p->header->consumer_active;
}

/* Inicia a escrita de um quadro NV12 width x height; retorna o destino */
static inline uint8_t *
moblens_producer_begin(struct moblens_producer *p) {
    InterlockedIncrement((volatile LONG *) &p->header->seq); /* ímpar */
    MemoryBarrier();
    return moblens_shm_frame(p->header);
}

static inline void
moblens_producer_end(struct moblens_producer *p, uint32_t width,
                       uint32_t height) {
    struct moblens_shm_header *h = p->header;
    h->width = width;
    h->height = height;
    h->frame_size = width * height * 3 / 2;
    h->frame_count++;
    h->producer_tick_ms = (int64_t) GetTickCount64();
    MemoryBarrier();
    InterlockedIncrement((volatile LONG *) &h->seq); /* par */
}

/* Mantém o produtor "vivo" sem reescrever o quadro (imagem estática) */
static inline void
moblens_producer_heartbeat(struct moblens_producer *p) {
    if (p->header) {
        p->header->producer_tick_ms = (int64_t) GetTickCount64();
    }
}

#endif
