// Vídeo exibido na webcam quando o MobLens está aberto mas o celular não
// está transmitindo: logo do app com uma onda pulsando e uma mensagem.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

const COLORREF kBgTop = RGB(24, 26, 33);
const COLORREF kBgBottom = RGB(10, 11, 15);
const COLORREF kAccent = RGB(76, 160, 255);
const COLORREF kTitle = RGB(240, 242, 246);
const COLORREF kText = RGB(196, 200, 210);
const COLORREF kHint = RGB(128, 134, 148);

COLORREF Mix(COLORREF a, COLORREF b, double t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return RGB((int) (GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int) (GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int) (GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

HFONT MakeFont(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

} // namespace

IdleFrameRenderer::~IdleFrameRenderer() {
    Release();
}

void IdleFrameRenderer::Release() {
    if (m_dc) {
        DeleteDC(m_dc);
        m_dc = nullptr;
    }
    if (m_bitmap) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    if (m_icon) {
        DestroyIcon(m_icon);
        m_icon = nullptr;
    }
    for (HFONT &f : m_fonts) {
        if (f) {
            DeleteObject(f);
            f = nullptr;
        }
    }
    m_bits = nullptr;
    m_width = m_height = 0;
}

bool IdleFrameRenderer::Prepare(uint32_t w, uint32_t h) {
    if (m_dc && w == m_width && h == m_height) {
        return true;
    }
    Release();
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG) w;
    bi.bmiHeader.biHeight = -(LONG) h;  // de cima para baixo
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    m_dc = CreateCompatibleDC(nullptr);
    m_bitmap = CreateDIBSection(m_dc, &bi, DIB_RGB_COLORS, &m_bits, nullptr,
                                0);
    if (!m_dc || !m_bitmap || !m_bits) {
        Release();
        return false;
    }
    SelectObject(m_dc, m_bitmap);
    m_width = w;
    m_height = h;

    m_logoSize = (int) (h * 0.26);
    m_icon = (HICON) LoadImageW(GetModuleHandleW(nullptr),
                                MAKEINTRESOURCEW(1), IMAGE_ICON, m_logoSize,
                                m_logoSize, LR_DEFAULTCOLOR);
    m_fonts[0] = MakeFont((int) (h / 13.0), FW_SEMIBOLD);  // "MobLens"
    m_fonts[1] = MakeFont((int) (h / 26.0), FW_NORMAL);    // mensagem
    m_fonts[2] = MakeFont((int) (h / 38.0), FW_NORMAL);    // dica
    return true;
}

void IdleFrameRenderer::Render(uint8_t *nv12, uint32_t w, uint32_t h,
                               const std::wstring &message,
                               const std::wstring &hint, bool busy,
                               double seconds) {
    if (!Prepare(w, h)) {
        memset(nv12, 16, (size_t) w * h);
        memset(nv12 + (size_t) w * h, 128, (size_t) w * h / 2);
        return;
    }
    HDC dc = m_dc;

    // Fundo em degradê vertical
    const int bands = 64;
    for (int i = 0; i < bands; ++i) {
        RECT r = {0, (int) (h * i / bands), (int) w,
                  (int) (h * (i + 1) / bands)};
        HBRUSH b = CreateSolidBrush(Mix(kBgTop, kBgBottom,
                                        (double) i / (bands - 1)));
        FillRect(dc, &r, b);
        DeleteObject(b);
    }

    // Logo com ondas saindo dele (efeito "procurando")
    const int cx = (int) w / 2;
    const int cy = (int) (h * 0.36);
    const int r0 = m_logoSize / 2;
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (int k = 0; k < 2; ++k) {
        double phase = fmod(seconds / 2.4 + k * 0.5, 1.0);
        int radius = r0 + (int) (phase * r0 * 0.9);
        COLORREF color = Mix(kAccent, kBgTop, phase);
        HPEN pen = CreatePen(PS_SOLID, (std::max)(2, (int) (h / 270)), color);
        HGDIOBJ old = SelectObject(dc, pen);
        Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    if (m_icon) {
        DrawIconEx(dc, cx - r0, cy - r0, m_icon, m_logoSize, m_logoSize, 0,
                   nullptr, DI_NORMAL);
    }

    // Textos
    SetBkMode(dc, TRANSPARENT);
    int y = cy + r0 + (int) (h * 0.13);
    auto line = [&](HFONT font, COLORREF color, const std::wstring &text) {
        SelectObject(dc, font);
        SetTextColor(dc, color);
        RECT r = {(int) (w * 0.06), y, (int) (w * 0.94), (int) h};
        DrawTextW(dc, text.c_str(), -1, &r,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS
                  | DT_NOPREFIX);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        y += tm.tmHeight + (int) (h * 0.015);
    };
    line(m_fonts[0], kTitle, L"MobLens");
    std::wstring msg = message;
    if (busy) {
        const int dots = (int) (seconds * 2.5) % 4;
        msg += std::wstring((size_t) dots, L'.')
             + std::wstring((size_t) (3 - dots), L' ');
    }
    line(m_fonts[1], kText, msg);
    if (!hint.empty()) {
        line(m_fonts[2], kHint, hint);
    }
    GdiFlush();

    // BGRA -> NV12 (BT.601, faixa limitada)
    const uint8_t *px = (const uint8_t *) m_bits;
    uint8_t *yPlane = nv12;
    uint8_t *uvPlane = nv12 + (size_t) w * h;
    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t *src = px + (size_t) row * w * 4;
        uint8_t *dst = yPlane + (size_t) row * w;
        for (uint32_t x = 0; x < w; ++x) {
            const int b = src[4 * x], g = src[4 * x + 1], r = src[4 * x + 2];
            dst[x] = (uint8_t) (((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (uint32_t row = 0; row < h / 2; ++row) {
        const uint8_t *s0 = px + (size_t) (row * 2) * w * 4;
        const uint8_t *s1 = s0 + (size_t) w * 4;
        uint8_t *dst = uvPlane + (size_t) row * w;
        for (uint32_t x = 0; x < w / 2; ++x) {
            const uint8_t *a = s0 + x * 8;
            const uint8_t *b2 = s1 + x * 8;
            const int b = (a[0] + a[4] + b2[0] + b2[4]) / 4;
            const int g = (a[1] + a[5] + b2[1] + b2[5]) / 4;
            const int r = (a[2] + a[6] + b2[2] + b2[6]) / 4;
            dst[2 * x] = (uint8_t) (((-38 * r - 74 * g + 112 * b + 128) >> 8)
                                    + 128);
            dst[2 * x + 1] = (uint8_t) (((112 * r - 94 * g - 18 * b + 128)
                                         >> 8) + 128);
        }
    }
}
