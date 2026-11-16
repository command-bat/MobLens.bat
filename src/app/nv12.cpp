// Conversões de imagem: redimensionamento NV12 -> NV12 (webcam) e
// NV12 -> BGRA (prévia), por vizinho mais próximo, preservando a proporção.
#include "app.h"

#include <algorithm>
#include <cstring>

namespace {

struct FitRect {
    UINT32 x, y, w, h;
};

// Maior retângulo com a proporção da origem centralizado no destino
FitRect Fit(UINT32 sw, UINT32 sh, UINT32 dw, UINT32 dh) {
    FitRect r;
    if ((uint64_t) sw * dh > (uint64_t) dw * sh) {
        r.w = dw;
        r.h = (UINT32) ((uint64_t) sh * dw / sw);
    } else {
        r.h = dh;
        r.w = (UINT32) ((uint64_t) sw * dh / sh);
    }
    r.w &= ~1u;
    r.h &= ~1u;
    r.x = ((dw - r.w) / 2) & ~1u;
    r.y = ((dh - r.h) / 2) & ~1u;
    return r;
}

void BuildMap(std::vector<UINT32> &map, UINT32 outCount, UINT32 inCount,
              bool mirror) {
    if (map.size() < outCount) {
        map.resize(outCount);
    }
    for (UINT32 i = 0; i < outCount; ++i) {
        UINT32 v = (UINT32) ((uint64_t) i * inCount / outCount);
        map[i] = mirror ? inCount - 1 - v : v;
    }
}

inline uint8_t Clamp(int v) {
    return (uint8_t) std::min(255, std::max(0, v));
}

} // namespace

void ScaleNv12(const Nv12Frame &src, uint8_t *dst, UINT32 dw, UINT32 dh,
               bool mirror, std::vector<UINT32> &xmap) {
    const UINT32 sw = src.width;
    const UINT32 sh = src.height;
    if (!sw || !sh || !dw || !dh) {
        return;
    }
    const FitRect r = Fit(sw, sh, dw, dh);
    if (!r.w || !r.h) {
        return;
    }

    uint8_t *dstY = dst;
    uint8_t *dstUV = dst + (size_t) dw * dh;
    if (r.w != dw || r.h != dh) {
        memset(dstY, 16, (size_t) dw * dh);       // preto
        memset(dstUV, 128, (size_t) dw * dh / 2);
    }

    // Luma
    const bool copyRows = r.w == sw && !mirror;
    if (!copyRows) {
        BuildMap(xmap, r.w, sw, mirror);
    }
    for (UINT32 y = 0; y < r.h; ++y) {
        UINT32 sy = (UINT32) ((uint64_t) y * sh / r.h);
        const uint8_t *in = src.y + (size_t) sy * src.pitch;
        uint8_t *out = dstY + (size_t) (r.y + y) * dw + r.x;
        if (copyRows) {
            memcpy(out, in, r.w);
        } else {
            for (UINT32 x = 0; x < r.w; ++x) {
                out[x] = in[xmap[x]];
            }
        }
    }

    // Croma (pares U/V intercalados)
    const UINT32 scw = sw / 2;
    const UINT32 sch = sh / 2;
    const UINT32 ocw = r.w / 2;
    const UINT32 och = r.h / 2;
    const bool copyChroma = ocw == scw && !mirror;
    if (!copyChroma) {
        BuildMap(xmap, ocw, scw, mirror);
    }
    for (UINT32 y = 0; y < och; ++y) {
        UINT32 sy = (UINT32) ((uint64_t) y * sch / och);
        const uint8_t *in = src.uv + (size_t) sy * src.pitch;
        uint8_t *out = dstUV + (size_t) (r.y / 2 + y) * dw + r.x;
        if (copyChroma) {
            memcpy(out, in, (size_t) ocw * 2);
        } else {
            for (UINT32 x = 0; x < ocw; ++x) {
                UINT32 sx = xmap[x];
                out[2 * x] = in[2 * sx];
                out[2 * x + 1] = in[2 * sx + 1];
            }
        }
    }
}

void ScaleNv12ToBgra(const Nv12Frame &src, uint32_t *dst, UINT32 dw,
                     UINT32 dh, bool mirror) {
    const UINT32 sw = src.width;
    const UINT32 sh = src.height;
    std::fill(dst, dst + (size_t) dw * dh, 0xff000000u);
    if (!sw || !sh) {
        return;
    }
    const FitRect r = Fit(sw, sh, dw, dh);
    std::vector<UINT32> xmap;
    BuildMap(xmap, r.w, sw, mirror);

    for (UINT32 y = 0; y < r.h; ++y) {
        UINT32 sy = (UINT32) ((uint64_t) y * sh / r.h);
        const uint8_t *rowY = src.y + (size_t) sy * src.pitch;
        const uint8_t *rowUV = src.uv + (size_t) (sy / 2) * src.pitch;
        uint32_t *out = dst + (size_t) (r.y + y) * dw + r.x;
        for (UINT32 x = 0; x < r.w; ++x) {
            UINT32 sx = xmap[x];
            // BT.601, faixa limitada
            int c = 298 * (rowY[sx] - 16);
            int d = rowUV[sx & ~1u] - 128;
            int e = rowUV[(sx & ~1u) + 1] - 128;
            uint8_t rr = Clamp((c + 409 * e + 128) >> 8);
            uint8_t gg = Clamp((c - 100 * d - 208 * e + 128) >> 8);
            uint8_t bb = Clamp((c + 516 * d + 128) >> 8);
            out[x] = 0xff000000u | ((uint32_t) rr << 16)
                   | ((uint32_t) gg << 8) | bb;
        }
    }
}
