/*
 * MobLens - protocolo de memória compartilhada entre o produtor (cliente
 * scrcpy, que recebe e decodifica o vídeo do celular) e o consumidor (DLL da
 * câmera virtual, carregada pelo Windows Camera Frame Server).
 *
 * Cabeçalho C puro, usado pelos dois lados.
 *
 * Layout da memória:
 *   [0 .. MOBLENS_SHM_HEADER_SIZE)      struct moblens_shm_header
 *   [MOBLENS_SHM_HEADER_SIZE .. fim)    um quadro NV12 (Y + UV intercalado)
 *
 * Sincronização (seqlock, um único produtor):
 *   produtor:   seq++ (ímpar) -> escreve quadro -> seq++ (par)
 *   consumidor: lê seq (par) -> copia quadro -> relê seq; se mudou, descarta
 *
 * O consumidor publica em requested_width/requested_height o tamanho que o
 * aplicativo escolheu para a webcam; o produtor escala (preservando a
 * proporção, com barras pretas) cada quadro para esse tamanho.
 */
#ifndef MOBLENS_SHM_H
#define MOBLENS_SHM_H

#include <stdint.h>

#define MOBLENS_SHM_NAME_GLOBAL L"Global\\MobLensFrameBuffer"

#define MOBLENS_SHM_MAGIC   0x4D41434Du /* "MCAM" */
#define MOBLENS_SHM_VERSION 1u

#define MOBLENS_MAX_WIDTH  3840u
#define MOBLENS_MAX_HEIGHT 2160u

#define MOBLENS_SHM_HEADER_SIZE 4096u
#define MOBLENS_SHM_FRAME_MAX \
    (MOBLENS_MAX_WIDTH * MOBLENS_MAX_HEIGHT * 3u / 2u)
#define MOBLENS_SHM_TOTAL_SIZE \
    (MOBLENS_SHM_HEADER_SIZE + MOBLENS_SHM_FRAME_MAX)

/* Security descriptor (SDDL) da memória compartilhada:
 * SYSTEM, LocalService, NetworkService e Administradores: acesso total;
 * usuários interativos (o cliente scrcpy): leitura/escrita.
 * Rótulo de integridade média explícito, para que o scrcpy (integridade média)
 * consiga escrever em um objeto criado por um serviço. */
#define MOBLENS_SHM_SDDL \
    L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;NS)(A;;GA;;;BA)(A;;GRGW;;;IU)" \
    L"S:(ML;;NW;;;ME)"

/* Considera-se que o produtor parou se não houver quadro novo neste prazo */
#define MOBLENS_PRODUCER_TIMEOUT_MS 1500u

struct moblens_shm_header {
    uint32_t magic;
    uint32_t version;

    /* Escrito pelo consumidor (câmera virtual) */
    volatile int32_t requested_width;
    volatile int32_t requested_height;
    volatile int32_t consumer_active;   /* 1 enquanto algum app usa a câmera */
    uint32_t reserved0;

    /* Escrito pelo produtor (scrcpy) */
    volatile int32_t seq;               /* seqlock: ímpar = escrevendo */
    uint32_t width;                     /* dimensões do quadro no buffer */
    uint32_t height;
    uint32_t frame_size;                /* width * height * 3 / 2 */
    volatile int64_t producer_tick_ms;  /* GetTickCount64() da última escrita */
    volatile int64_t frame_count;
};

static inline uint8_t *
moblens_shm_frame(void *base) {
    return (uint8_t *) base + MOBLENS_SHM_HEADER_SIZE;
}

#endif
