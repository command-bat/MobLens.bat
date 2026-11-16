// MobLens - tema visual e controles personalizados.
#include "theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

Theme g_theme;

namespace {

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                 L"Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                 &size);
    return value == 0;
}

} // namespace

void ThemeLoad() {
    Theme &t = g_theme;
    t.dark = SystemUsesDarkTheme();
    if (t.dark) {
        t.bg = RGB(32, 32, 32);
        t.card = RGB(43, 43, 43);
        t.cardBorder = RGB(58, 58, 58);
        t.text = RGB(255, 255, 255);
        t.subtext = RGB(170, 170, 170);
        t.accent = RGB(76, 194, 255);
        t.previewBg = RGB(24, 24, 24);
        t.danger = RGB(255, 153, 164);
    } else {
        t.bg = RGB(243, 243, 243);
        t.card = RGB(255, 255, 255);
        t.cardBorder = RGB(229, 229, 229);
        t.text = RGB(27, 27, 27);
        t.subtext = RGB(96, 96, 96);
        t.accent = RGB(0, 95, 184);
        t.previewBg = RGB(32, 32, 32);
        t.danger = RGB(196, 43, 28);
    }
    if (t.bgBrush) {
        DeleteObject(t.bgBrush);
    }
    if (t.cardBrush) {
        DeleteObject(t.cardBrush);
    }
    t.bgBrush = CreateSolidBrush(t.bg);
    t.cardBrush = CreateSolidBrush(t.card);
}

void ThemeApplyToWindow(HWND hwnd) {
    // Atributos do DWM (Windows 11): modo escuro e cores da barra de título
    const DWORD kImmersiveDarkMode = 20;
    const DWORD kCaptionColor = 35;
    const DWORD kTextColor = 36;
    const DWORD kBorderColor = 34;
    BOOL dark = g_theme.dark;
    DwmSetWindowAttribute(hwnd, kImmersiveDarkMode, &dark, sizeof(dark));
    COLORREF caption = g_theme.bg;
    COLORREF text = g_theme.text;
    COLORREF border = g_theme.cardBorder;
    DwmSetWindowAttribute(hwnd, kCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, kTextColor, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, kBorderColor, &border, sizeof(border));
}

void ThemeApplyToControl(HWND ctl) {
    wchar_t cls[32] = {};
    GetClassNameW(ctl, cls, 32);
    if (!lstrcmpiW(cls, WC_COMBOBOXW)) {
        SetWindowTheme(ctl, g_theme.dark ? L"DarkMode_CFD" : L"Explorer",
                       nullptr);
    } else if (!lstrcmpiW(cls, WC_SCROLLBARW)
               || !lstrcmpiW(cls, L"msctls_trackbar32")) {
        SetWindowTheme(ctl, g_theme.dark ? L"DarkMode_Explorer" : L"Explorer",
                       nullptr);
    }
}

void DrawRoundRect(HDC dc, const RECT &rc, int radius, COLORREF fill,
                   COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

// ----- Interruptor ----------------------------------------------------------

namespace {

void NotifyParent(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    UINT id = (UINT) GetDlgCtrlID(hwnd);
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                 (LPARAM) hwnd);
}

void Flip(HWND hwnd) {
    ToggleSet(hwnd, !ToggleGet(hwnd));
    NotifyParent(hwnd);
}

void PaintToggle(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    // Desenho em memória (sem cintilar)
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRect(mem, &rc, g_theme.cardBrush);

    const bool on = ToggleGet(hwnd);
    const bool focused = GetFocus() == hwnd;
    const int h = rc.bottom;
    const int sw = h * 40 / 22;   // proporção do interruptor do Windows 11
    const int sh = h * 20 / 28;
    RECT sr = {rc.right - sw - 2, (h - sh) / 2, rc.right - 2,
               (h - sh) / 2 + sh};

    // Texto
    wchar_t text[128];
    GetWindowTextW(hwnd, text, 128);
    HGDIOBJ oldFont = SelectObject(mem, (HFONT) SendMessageW(hwnd, WM_GETFONT,
                                                            0, 0));
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, g_theme.text);
    RECT tr = {0, 0, sr.left - 8, h};
    DrawTextW(mem, text, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(mem, oldFont);

    // Trilho
    COLORREF track = on ? g_theme.accent : g_theme.card;
    COLORREF outline = on ? g_theme.accent : g_theme.subtext;
    if (focused) {
        RECT fr = sr;
        InflateRect(&fr, 2, 2);
        DrawRoundRect(mem, fr, fr.bottom - fr.top, g_theme.card, g_theme.text);
    }
    DrawRoundRect(mem, sr, sh, track, outline);

    // Botão
    const int knob = sh - sh / 3;
    const int ky = sr.top + (sh - knob) / 2;
    const int kx = on ? sr.right - knob - (sh - knob) / 2
                      : sr.left + (sh - knob) / 2;
    RECT kr = {kx, ky, kx + knob, ky + knob};
    COLORREF knobColor = on ? (g_theme.dark ? RGB(0, 0, 0) : RGB(255, 255, 255))
                            : g_theme.subtext;
    DrawRoundRect(mem, kr, knob, knobColor, knobColor);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK ToggleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintToggle(hwnd);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) {
                ReleaseCapture();
                POINT pt = {(short) LOWORD(lp), (short) HIWORD(lp)};
                RECT rc;
                GetClientRect(hwnd, &rc);
                if (PtInRect(&rc, pt)) {
                    Flip(hwnd);
                }
            }
            return 0;
        case BM_CLICK:  // barra de espaço (IsDialogMessage)
            Flip(hwnd);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_BUTTON;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_SETTEXT:
        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_SETFONT:
            SetPropW(hwnd, L"font", (HANDLE) wp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETFONT:
            return (LRESULT) GetPropW(hwnd, L"font");
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void RegisterToggleClass(HINSTANCE instance) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ToggleProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = WC_MOBLENS_TOGGLE;
    RegisterClassW(&wc);
}

bool ToggleGet(HWND toggle) {
    return GetWindowLongPtrW(toggle, GWLP_USERDATA) != 0;
}

void ToggleSet(HWND toggle, bool on) {
    SetWindowLongPtrW(toggle, GWLP_USERDATA, on ? 1 : 0);
    InvalidateRect(toggle, nullptr, FALSE);
}
