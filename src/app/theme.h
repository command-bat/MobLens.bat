// MobLens - tema visual (claro/escuro, seguindo o Windows) e controles
// personalizados (interruptor, cartões arredondados).
#pragma once

#include "app.h"

struct Theme {
    bool dark = false;
    COLORREF bg;          // fundo da janela
    COLORREF card;        // fundo dos cartões
    COLORREF cardBorder;
    COLORREF text;
    COLORREF subtext;
    COLORREF accent;      // cor de destaque (azul do Windows 11)
    COLORREF previewBg;
    COLORREF danger;
    HBRUSH bgBrush = nullptr;
    HBRUSH cardBrush = nullptr;
};

extern Theme g_theme;

// Lê o tema do Windows (claro/escuro) e recria pincéis
void ThemeLoad();

// Barra de título escura/clara com a cor do fundo
void ThemeApplyToWindow(HWND hwnd);

// Ajusta um controle comum (combo, trackbar) ao tema
void ThemeApplyToControl(HWND ctl);

// Retângulo arredondado preenchido com borda
void DrawRoundRect(HDC dc, const RECT &rc, int radius, COLORREF fill,
                   COLORREF border);

// ----- Interruptor (toggle) -------------------------------------------------
// Envia WM_COMMAND (BN_CLICKED) ao pai quando muda.
#define WC_MOBLENS_TOGGLE L"MobLensToggle"
void RegisterToggleClass(HINSTANCE instance);
bool ToggleGet(HWND toggle);
void ToggleSet(HWND toggle, bool on);
