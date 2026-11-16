// MobLens.exe - transforma a câmera do celular (via USB) na webcam nativa
// "MobLens". Janela com prévia e configurações + ícone na bandeja.
//
//   MobLens.exe              abre a janela
//   MobLens.exe --tray       inicia escondido na bandeja (início com o Windows)
//   MobLens.exe --install    instala a câmera virtual (administrador)
//   MobLens.exe --uninstall  remove a câmera virtual (administrador)
#include "app.h"
#include "theme.h"

#include <commctrl.h>
#include <mfapi.h>
#include <shellapi.h>

#include <cstdio>

namespace {

const wchar_t kMainClass[] = L"MobLensMain";
const wchar_t kPreviewClass[] = L"MobLensPreview";
const wchar_t kAppName[] = L"MobLens";
const wchar_t kSettingsKey[] = L"Software\\MobLens";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

const UINT WM_TRAY = WM_APP + 1;
const UINT WM_STATUS_CHANGED = WM_APP + 2;
const UINT WM_CAMERAS_CHANGED = WM_APP + 3;
const UINT WM_PREVIEW_READY = WM_APP + 4;

enum ControlId : UINT {
    IDC_CAMERA = 200,
    IDC_RESOLUTION,
    IDC_FPS,
    IDC_QUALITY,
    IDC_ROTATION,
    IDC_ZOOM,
    IDC_ZOOM_LABEL,
    IDC_MIRROR,
    IDC_TORCH,
    IDC_AUTOSTART,
    IDC_CLOSE_ACTION,
    IDC_UNINSTALL,
    IDC_LABEL,
    ID_TRAY_OPEN = 300,
    ID_TRAY_EXIT,
};

const UINT32 kBitRates[] = {2, 4, 8, 16, 24};
const wchar_t *const kBitRateNames[] = {
    L"Baixa (2 Mbps)", L"Média (4 Mbps)", L"Alta (8 Mbps)",
    L"Muito alta (16 Mbps)", L"Máxima (24 Mbps)",
};
const CameraSize kDefaultSizes[] = {
    {3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}, {640, 480},
};

// ----- Layout (unidades de 96 dpi) -----------------------------------------

const int kMargin = 20;
const int kPad = 16;          // margem interna dos cartões
const int kPreviewY = 68;
const int kRightX = kMargin + PREVIEW_WIDTH + kMargin;
const int kRightW = 300;
const int kClientW = kRightX + kRightW + kMargin;
const int kClientH = 668;

struct CardDef {
    int x, y, w, h;
    const wchar_t *title;
};
const CardDef kCards[] = {
    {kRightX, 16, kRightW, 214, L"Câmera"},
    {kRightX, 242, kRightW, 222, L"Imagem"},
    {kRightX, 476, kRightW, 176, L"Aplicativo"},
    {kMargin, 444, PREVIEW_WIDTH, 208, L"Status"},
};

// ----- Estado global da interface ------------------------------------------

HINSTANCE g_instance;
HWND g_main;
HWND g_preview;
HFONT g_font;
HFONT g_fontBold;
HFONT g_fontTitle;
double g_scale = 1.0;
UINT g_taskbarCreatedMsg;
NOTIFYICONDATAW g_nid;
HICON g_icons[3];  // 0 = inativo, 1 = pronto, 2 = transmitindo
bool g_hiddenHintShown = false;

Streamer *g_streamer;
VideoSettings g_settings;
DWORD g_closeAction = 0;  // 0 = esconder na bandeja, 1 = sair

std::mutex g_sharedMutex;            // dados vindos da thread do streamer
StreamStatus g_status;
std::vector<CameraInfo> g_pendingCameras;
bool g_camerasPending = false;
std::atomic<bool> g_previewPosted{false};

std::vector<CameraInfo> g_cameras;   // somente na thread da interface
std::vector<uint32_t> g_previewPixels;
bool g_hasPreview = false;

int S(int v) {
    return (int) (v * g_scale + 0.5);
}

HWND Ctl(UINT id) {
    return GetDlgItem(g_main, id);
}

// ----- Configurações (HKCU) -------------------------------------------------

DWORD ReadDword(const wchar_t *name, DWORD def) {
    DWORD value = def;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS) {
        return def;
    }
    return value;
}

void WriteDword(const wchar_t *name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value,
                    sizeof(value));
}

std::wstring ReadString(const wchar_t *name) {
    wchar_t buf[128] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ,
                     nullptr, buf, &size) != ERROR_SUCCESS) {
        return L"";
    }
    return buf;
}

void WriteString(const wchar_t *name, const std::wstring &value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_SZ,
                    value.c_str(),
                    (DWORD) ((value.size() + 1) * sizeof(wchar_t)));
}

void LoadSettings() {
    g_settings.cameraId = ReadString(L"CameraId");
    g_settings.width = ReadDword(L"Width", 1920);
    g_settings.height = ReadDword(L"Height", 1080);
    g_settings.fps = ReadDword(L"Fps", 30);
    g_settings.highSpeed = false;  // alta velocidade desativada (máx. 30 fps)
    g_settings.fps = std::min<UINT32>(g_settings.fps, 30);
    g_settings.bitRateMbps = ReadDword(L"BitRate", 8);
    g_settings.zoomTenths = ReadDword(L"Zoom", 10);
    g_settings.rotation = ReadDword(L"Rotation", 0);
    g_settings.torch = ReadDword(L"Torch", 0) != 0;
    g_settings.mirror = ReadDword(L"Mirror", 0) != 0;
    g_closeAction = ReadDword(L"CloseAction", 0);
}

void SaveSettings() {
    WriteString(L"CameraId", g_settings.cameraId);
    WriteDword(L"Width", g_settings.width);
    WriteDword(L"Height", g_settings.height);
    WriteDword(L"Fps", g_settings.fps);
    WriteDword(L"HighSpeed", g_settings.highSpeed);
    WriteDword(L"BitRate", g_settings.bitRateMbps);
    WriteDword(L"Zoom", g_settings.zoomTenths);
    WriteDword(L"Rotation", g_settings.rotation);
    WriteDword(L"Torch", g_settings.torch);
    WriteDword(L"Mirror", g_settings.mirror);
    WriteDword(L"CloseAction", g_closeAction);
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ,
                        nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool enable) {
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --tray";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName, REG_SZ,
                        cmd.c_str(),
                        (DWORD) ((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName);
    }
}

// ----- Execução elevada (instalar/remover a câmera) ------------------------

bool RunElevated(const wchar_t *args) {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        return false;  // UAC cancelado
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

// ----- Ícone (desenhado em tempo de execução) -------------------------------

HICON MakeCameraIcon(COLORREF color) {
    const int n = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00ff0000;
    bi.bV5GreenMask = 0x0000ff00;
    bi.bV5BlueMask = 0x000000ff;
    bi.bV5AlphaMask = 0xff000000;

    uint32_t *pixels = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color32 = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&bi),
                                       DIB_RGB_COLORS,
                                       reinterpret_cast<void **>(&pixels),
                                       nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color32) {
        return LoadIcon(nullptr, IDI_APPLICATION);
    }

    const uint32_t body = 0xff000000u | ((uint32_t) GetRValue(color) << 16)
                        | ((uint32_t) GetGValue(color) << 8) | GetBValue(color);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            uint32_t px = 0;
            // lente externa (anel), lente interna e brilho
            int dx = x - 16;
            int dy = y - 16;
            int d2 = dx * dx + dy * dy;
            if (d2 <= 15 * 15) {
                px = body;
            }
            if (d2 <= 10 * 10) {
                px = 0xff1e1e1e;
            }
            if (d2 <= 6 * 6) {
                px = body;
            }
            int hx = x - 13;
            int hy = y - 13;
            if (hx * hx + hy * hy <= 4) {
                px = 0xffffffff;
            }
            pixels[y * n + x] = px;
        }
    }

    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color32;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color32);
    DeleteObject(mask);
    return icon;
}

// ----- Status ---------------------------------------------------------------

StreamStatus CurrentStatus() {
    std::lock_guard<std::mutex> lock(g_sharedMutex);
    return g_status;
}

std::wstring StatusText(const StreamStatus &s) {
    switch (s.state) {
        case StreamState::NoDevice:
            return L"Conecte o celular via USB (depuração USB ativada)";
        case StreamState::Unauthorized:
            return L"Autorize a depuração USB na tela do celular";
        case StreamState::Idle:
            return L"Pronto: escolha a câmera \"MobLens\" no aplicativo";
        case StreamState::Starting:
            return L"Aguarde: " + s.detail + L"...";
        case StreamState::Streaming:
            return L"Transmitindo: " + s.detail;
        case StreamState::Error:
        default:
            return L"Erro: " + s.detail;
    }
}

HICON StatusIcon(StreamState state) {
    switch (state) {
        case StreamState::Streaming:
            return g_icons[2];
        case StreamState::Idle:
        case StreamState::Starting:
            return g_icons[1];
        default:
            return g_icons[0];
    }
}

void UpdateTray(bool add) {
    StreamStatus status = CurrentStatus();
    std::wstring tip = std::wstring(kAppName) + L" - " + StatusText(status);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.hIcon = StatusIcon(status.state);
    lstrcpynW(g_nid.szTip, tip.c_str(), ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
}

void ShowBalloon(const wchar_t *text) {
    g_nid.uFlags = NIF_INFO;
    lstrcpynW(g_nid.szInfoTitle, kAppName, ARRAYSIZE(g_nid.szInfoTitle));
    lstrcpynW(g_nid.szInfo, text, ARRAYSIZE(g_nid.szInfo));
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.szInfo[0] = L'\0';
}

void OnStatusChanged() {
    StreamStatus status = CurrentStatus();
    UpdateTray(false);
    InvalidateRect(g_main, nullptr, FALSE);  // cabeçalho e status
    // Durante um reinício da câmera (mudança de resolução, fps...) a prévia
    // mantém a última imagem
    if (status.state == StreamState::NoDevice
            || status.state == StreamState::Unauthorized
            || status.state == StreamState::Idle) {
        g_hasPreview = false;
    }
    InvalidateRect(g_preview, nullptr, FALSE);
}

// ----- Prévia ---------------------------------------------------------------

void UpdatePreviewEnabled() {
    bool visible = IsWindowVisible(g_main) && !IsIconic(g_main);
    g_streamer->SetPreviewEnabled(visible);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (g_hasPreview && !g_previewPixels.empty()) {
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = PREVIEW_WIDTH;
                bi.bmiHeader.biHeight = -PREVIEW_HEIGHT;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0,
                              PREVIEW_WIDTH, PREVIEW_HEIGHT,
                              g_previewPixels.data(), &bi, DIB_RGB_COLORS,
                              SRCCOPY);
            } else {
                HBRUSH bg = CreateSolidBrush(g_theme.previewBg);
                FillRect(dc, &rc, bg);
                DeleteObject(bg);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(170, 170, 170));
                HGDIOBJ old = SelectObject(dc, g_font);
                std::wstring text = StatusText(CurrentStatus());
                DrawTextW(dc, text.c_str(), -1, &rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, old);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ----- Controles de configuração -------------------------------------------

const CameraInfo *SelectedCamera() {
    int sel = (int) SendMessageW(Ctl(IDC_CAMERA), CB_GETCURSEL, 0, 0);
    if (sel < 0 || g_cameras.empty()) {
        return nullptr;
    }
    LRESULT idx = SendMessageW(Ctl(IDC_CAMERA), CB_GETITEMDATA, sel, 0);
    return idx >= 0 && (size_t) idx < g_cameras.size() ? &g_cameras[idx]
                                                       : nullptr;
}

std::wstring FacingName(const std::wstring &facing) {
    if (facing == L"back") return L"traseira";
    if (facing == L"front") return L"frontal";
    if (facing == L"external") return L"externa";
    return facing;
}

// Itens do combo de fps: valor | 0x10000 se for alta velocidade
const LPARAM kHighSpeedFlag = 0x10000;

void FillFps() {
    HWND fps = Ctl(IDC_FPS);
    SendMessageW(fps, CB_RESETCONTENT, 0, 0);

    const CameraInfo *cam = SelectedCamera();
    std::vector<UINT32> rates = cam ? cam->fps : std::vector<UINT32>{30};

    int selected = -1;
    int fallback = -1;
    int index = 0;
    auto add = [&](UINT32 value, bool highSpeed) {
        wchar_t text[48];
        swprintf(text, 48, highSpeed ? L"%u fps (alta velocidade)" : L"%u fps",
                 value);
        SendMessageW(fps, CB_ADDSTRING, 0, (LPARAM) text);
        SendMessageW(fps, CB_SETITEMDATA, index,
                     (LPARAM) value | (highSpeed ? kHighSpeedFlag : 0));
        if (value == g_settings.fps && highSpeed == g_settings.highSpeed) {
            selected = index;
        }
        if (value == 30 && !highSpeed) {
            fallback = index;
        }
        ++index;
    };
    for (UINT32 r : rates) {
        if (r <= 30) {  // máximo de 30 fps
            add(r, false);
        }
    }
    if (selected < 0) {
        selected = fallback >= 0 ? fallback : index - 1;
    }
    SendMessageW(fps, CB_SETCURSEL, selected, 0);
}

void FillResolutionAndFps() {
    HWND res = Ctl(IDC_RESOLUTION);
    SendMessageW(res, CB_RESETCONTENT, 0, 0);

    const CameraInfo *cam = SelectedCamera();
    std::vector<CameraSize> sizes;
    if (cam && !cam->sizes.empty()) {
        sizes = cam->sizes;
    } else {
        sizes.assign(std::begin(kDefaultSizes), std::end(kDefaultSizes));
    }

    // Resolução: a salva, senão a maior com largura <= 1920
    int selected = -1;
    int fallback = -1;
    for (size_t i = 0; i < sizes.size(); ++i) {
        wchar_t text[32];
        swprintf(text, 32, L"%u x %u", sizes[i].width, sizes[i].height);
        SendMessageW(res, CB_ADDSTRING, 0, (LPARAM) text);
        SendMessageW(res, CB_SETITEMDATA, i,
                     MAKELPARAM(sizes[i].width, sizes[i].height));
        if (sizes[i].width == g_settings.width
                && sizes[i].height == g_settings.height) {
            selected = (int) i;
        }
        if (fallback < 0 && sizes[i].width <= 1920) {
            fallback = (int) i;
        }
    }
    if (selected < 0) {
        selected = fallback >= 0 ? fallback : 0;
    }
    SendMessageW(res, CB_SETCURSEL, selected, 0);
    FillFps();
}

// Nome amigável: "Traseira 1", "Frontal 2"...
std::wstring CameraLabel(size_t index) {
    const CameraInfo &cam = g_cameras[index];
    int number = 1;
    for (size_t i = 0; i < index; ++i) {
        if (g_cameras[i].facing == cam.facing) {
            ++number;
        }
    }
    std::wstring facing = FacingName(cam.facing);
    if (!facing.empty()) {
        facing[0] = towupper(facing[0]);
    }
    std::wstring text = facing + L" " + std::to_wstring(number);
    return text;
}

void FillCameras() {
    HWND combo = Ctl(IDC_CAMERA);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (g_cameras.empty()) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM) L"Traseira (padrão)");
        SendMessageW(combo, CB_SETITEMDATA, 0, (LPARAM) -1);
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        return;
    }
    int selected = -1;
    int firstBack = -1;
    for (size_t i = 0; i < g_cameras.size(); ++i) {
        const CameraInfo &cam = g_cameras[i];
        SendMessageW(combo, CB_ADDSTRING, 0,
                     (LPARAM) CameraLabel(i).c_str());
        SendMessageW(combo, CB_SETITEMDATA, i, (LPARAM) i);
        if (cam.id == g_settings.cameraId) {
            selected = (int) i;
        }
        if (firstBack < 0 && cam.facing == L"back") {
            firstBack = (int) i;
        }
    }
    if (selected < 0) {
        selected = firstBack >= 0 ? firstBack : 0;
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void UpdateZoomLabel() {
    UINT32 z = (UINT32) SendMessageW(Ctl(IDC_ZOOM), TBM_GETPOS, 0, 0);
    wchar_t text[32];
    swprintf(text, 32, L"%u,%ux", z / 10, z % 10);
    SetWindowTextW(Ctl(IDC_ZOOM_LABEL), text);
}

// Lê a interface -> g_settings, salva e aplica
void ApplyFromUi() {
    const CameraInfo *cam = SelectedCamera();
    g_settings.cameraId = cam ? cam->id : L"";

    int sel = (int) SendMessageW(Ctl(IDC_RESOLUTION), CB_GETCURSEL, 0, 0);
    if (sel >= 0) {
        LRESULT d = SendMessageW(Ctl(IDC_RESOLUTION), CB_GETITEMDATA, sel, 0);
        g_settings.width = LOWORD(d);
        g_settings.height = HIWORD(d);
    }
    sel = (int) SendMessageW(Ctl(IDC_FPS), CB_GETCURSEL, 0, 0);
    if (sel >= 0) {
        LRESULT d = SendMessageW(Ctl(IDC_FPS), CB_GETITEMDATA, sel, 0);
        g_settings.fps = (UINT32) (d & 0xffff);
        g_settings.highSpeed = (d & kHighSpeedFlag) != 0;
    }
    sel = (int) SendMessageW(Ctl(IDC_QUALITY), CB_GETCURSEL, 0, 0);
    if (sel >= 0) {
        g_settings.bitRateMbps = kBitRates[sel];
    }
    sel = (int) SendMessageW(Ctl(IDC_ROTATION), CB_GETCURSEL, 0, 0);
    if (sel >= 0) {
        g_settings.rotation = (UINT32) sel * 90;
    }
    g_settings.zoomTenths = (UINT32) SendMessageW(Ctl(IDC_ZOOM), TBM_GETPOS,
                                                  0, 0);
    g_settings.mirror = ToggleGet(Ctl(IDC_MIRROR));
    g_settings.torch = ToggleGet(Ctl(IDC_TORCH));
    g_closeAction = (DWORD) SendMessageW(Ctl(IDC_CLOSE_ACTION), CB_GETCURSEL,
                                         0, 0);

    SaveSettings();
    g_streamer->SetSettings(g_settings);
}

HWND AddControl(const wchar_t *cls, const wchar_t *text, DWORD style, int x,
                int y, int w, int h, UINT id, HFONT font = nullptr) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                S(x), S(y), S(w), S(h), g_main,
                                (HMENU) (UINT_PTR) id, g_instance, nullptr);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM) (font ? font : g_font), TRUE);
    ThemeApplyToControl(hwnd);
    return hwnd;
}

HWND AddCombo(const wchar_t *label, int x, int y, int w, UINT id) {
    AddControl(WC_STATICW, label, 0, x, y, w, 18, IDC_LABEL);
    return AddControl(WC_COMBOBOXW, L"",
                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y + 20,
                      w, 300, id);
}

void CreateControls() {
    g_preview = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE,
                                S(kMargin), S(kPreviewY), S(PREVIEW_WIDTH),
                                S(PREVIEW_HEIGHT), g_main, nullptr,
                                g_instance, nullptr);
    // Cantos arredondados
    SetWindowRgn(g_preview,
                 CreateRoundRectRgn(0, 0, S(PREVIEW_WIDTH) + 1,
                                    S(PREVIEW_HEIGHT) + 1, S(14), S(14)),
                 FALSE);

    // Cartão "Câmera"
    const CardDef &c1 = kCards[0];
    const int x = c1.x + kPad;
    const int w = c1.w - 2 * kPad;
    const int half = (w - 12) / 2;
    AddCombo(L"Câmera", x, c1.y + 44, w, IDC_CAMERA);
    AddCombo(L"Resolução", x, c1.y + 96, half, IDC_RESOLUTION);
    AddCombo(L"Taxa de quadros", x + half + 12, c1.y + 96, half, IDC_FPS);
    HWND quality = AddCombo(L"Qualidade do vídeo", x, c1.y + 148, w,
                            IDC_QUALITY);

    // Cartão "Imagem"
    const CardDef &c2 = kCards[1];
    HWND rotation = AddCombo(L"Rotação", x, c2.y + 44, w, IDC_ROTATION);
    AddControl(WC_STATICW, L"Zoom", 0, x, c2.y + 98, w / 2, 18, IDC_LABEL);
    AddControl(WC_STATICW, L"", SS_RIGHT, x + w / 2, c2.y + 98, w - w / 2, 18,
               IDC_ZOOM_LABEL, g_fontBold);
    HWND zoom = AddControl(TRACKBAR_CLASSW, L"",
                           TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x - 6,
                           c2.y + 118, w + 12, 28, IDC_ZOOM);
    AddControl(WC_MOBLENS_TOGGLE, L"Espelhar imagem", WS_TABSTOP, x,
               c2.y + 150, w, 28, IDC_MIRROR);
    AddControl(WC_MOBLENS_TOGGLE, L"Lanterna (câmeras com flash)", WS_TABSTOP,
               x, c2.y + 182, w, 28, IDC_TORCH);

    // Cartão "Aplicativo"
    const CardDef &c3 = kCards[2];
    AddControl(WC_MOBLENS_TOGGLE, L"Iniciar com o Windows", WS_TABSTOP, x,
               c3.y + 40, w, 28, IDC_AUTOSTART);
    HWND closeAction = AddCombo(L"Ao fechar a janela", x, c3.y + 74, w,
                                IDC_CLOSE_ACTION);
    AddControl(WC_BUTTONW, L"Desinstalar webcam virtual",
               BS_OWNERDRAW | WS_TABSTOP, x, c3.y + 130, w, 32,
               IDC_UNINSTALL);

    // Valores iniciais
    for (size_t i = 0; i < ARRAYSIZE(kBitRateNames); ++i) {
        SendMessageW(quality, CB_ADDSTRING, 0, (LPARAM) kBitRateNames[i]);
        if (kBitRates[i] == g_settings.bitRateMbps) {
            SendMessageW(quality, CB_SETCURSEL, i, 0);
        }
    }
    if (SendMessageW(quality, CB_GETCURSEL, 0, 0) < 0) {
        SendMessageW(quality, CB_SETCURSEL, 2, 0);
    }
    const wchar_t *rotations[] = {L"0°", L"90°", L"180°", L"270°"};
    for (const wchar_t *r : rotations) {
        SendMessageW(rotation, CB_ADDSTRING, 0, (LPARAM) r);
    }
    SendMessageW(rotation, CB_SETCURSEL, (g_settings.rotation / 90) % 4, 0);

    SendMessageW(zoom, TBM_SETRANGE, TRUE, MAKELPARAM(10, 80));
    SendMessageW(zoom, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(zoom, TBM_SETPOS, TRUE, g_settings.zoomTenths);
    UpdateZoomLabel();

    ToggleSet(Ctl(IDC_MIRROR), g_settings.mirror);
    ToggleSet(Ctl(IDC_TORCH), g_settings.torch);
    ToggleSet(Ctl(IDC_AUTOSTART), AutostartEnabled());

    SendMessageW(closeAction, CB_ADDSTRING, 0,
                 (LPARAM) L"Esconder na bandeja do sistema");
    SendMessageW(closeAction, CB_ADDSTRING, 0, (LPARAM) L"Fechar o MobLens");
    SendMessageW(closeAction, CB_SETCURSEL, g_closeAction ? 1 : 0, 0);

    FillCameras();
    FillResolutionAndFps();
}

// ----- Pintura da janela principal -----------------------------------------

RECT ScaledRect(int x, int y, int w, int h) {
    return {S(x), S(y), S(x + w), S(y + h)};
}

COLORREF StatusColor(StreamState state) {
    switch (state) {
        case StreamState::Streaming:
            return RGB(46, 160, 67);
        case StreamState::Idle:
        case StreamState::Starting:
            return g_theme.accent;
        case StreamState::Error:
            return RGB(229, 83, 75);
        case StreamState::Unauthorized:
            return RGB(210, 153, 34);
        default:
            return g_theme.subtext;
    }
}

const wchar_t *StatusShort(StreamState state) {
    switch (state) {
        case StreamState::Streaming:
            return L"Ao vivo";
        case StreamState::Idle:
            return L"Pronto";
        case StreamState::Starting:
            return L"Iniciando";
        case StreamState::Error:
            return L"Erro";
        case StreamState::Unauthorized:
            return L"Autorizar no celular";
        default:
            return L"Sem celular";
    }
}

void PaintMain(HDC dc) {
    RECT client;
    GetClientRect(g_main, &client);
    FillRect(dc, &client, g_theme.bgBrush);
    SetBkMode(dc, TRANSPARENT);

    // Cabeçalho: título + subtítulo
    HGDIOBJ oldFont = SelectObject(dc, g_fontTitle);
    SetTextColor(dc, g_theme.text);
    RECT title = ScaledRect(kMargin, 12, 300, 40);
    DrawTextW(dc, L"MobLens", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SIZE titleSize;
    GetTextExtentPoint32W(dc, L"MobLens", 7, &titleSize);
    SelectObject(dc, g_font);
    SetTextColor(dc, g_theme.subtext);
    RECT sub = {title.left + titleSize.cx + S(12), title.top + S(4),
                S(kMargin + PREVIEW_WIDTH), title.bottom};
    DrawTextW(dc, L"Câmera do celular como webcam", -1, &sub,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Indicador de status (pílula à direita do cabeçalho)
    StreamStatus status = CurrentStatus();
    const wchar_t *label = StatusShort(status.state);
    SelectObject(dc, g_fontBold);
    SIZE ls;
    GetTextExtentPoint32W(dc, label, lstrlenW(label), &ls);
    const int pillH = S(30);
    const int pillW = ls.cx + S(40);
    RECT pill = {S(kMargin + PREVIEW_WIDTH) - pillW, S(17),
                 S(kMargin + PREVIEW_WIDTH), S(17) + pillH};
    DrawRoundRect(dc, pill, pillH, g_theme.card, g_theme.cardBorder);
    const COLORREF dotColor = StatusColor(status.state);
    const int dot = S(10);
    RECT dr = {pill.left + S(14), pill.top + (pillH - dot) / 2,
               pill.left + S(14) + dot, pill.top + (pillH - dot) / 2 + dot};
    DrawRoundRect(dc, dr, dot, dotColor, dotColor);
    SetTextColor(dc, g_theme.text);
    RECT lr = {dr.right + S(8), pill.top, pill.right, pill.bottom};
    DrawTextW(dc, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Cartões
    for (const CardDef &card : kCards) {
        RECT rc = ScaledRect(card.x, card.y, card.w, card.h);
        DrawRoundRect(dc, rc, S(16), g_theme.card, g_theme.cardBorder);
        SelectObject(dc, g_fontBold);
        SetTextColor(dc, g_theme.text);
        RECT hr = ScaledRect(card.x + kPad, card.y + 12, card.w - 2 * kPad,
                             22);
        DrawTextW(dc, card.title, -1, &hr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // Cartão de status e ajuda (abaixo da prévia)
    const CardDef &help = kCards[3];
    SelectObject(dc, g_font);
    SetTextColor(dc, g_theme.text);
    RECT st = ScaledRect(help.x + kPad, help.y + 40, help.w - 2 * kPad, 20);
    std::wstring line = StatusText(status);
    DrawTextW(dc, line.c_str(), -1, &st,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SetTextColor(dc, g_theme.subtext);
    const wchar_t *tips[] = {
        L"1.  Conecte o celular pelo cabo USB, com a depuração USB ativada.",
        L"2.  No Meet, Teams, Zoom ou Discord, escolha a câmera \"MobLens\".",
        L"3.  A câmera do celular liga sozinha e desliga quando ninguém usa.",
        L"4.  Zoom e lanterna mudam na hora; as outras opções reiniciam a "
        L"câmera.",
    };
    int ty = help.y + 72;
    for (const wchar_t *tip : tips) {
        RECT tr = ScaledRect(help.x + kPad, ty, help.w - 2 * kPad, 20);
        DrawTextW(dc, tip, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ty += 24;
    }

    SelectObject(dc, oldFont);
}

void DrawUninstallButton(const DRAWITEMSTRUCT *di) {
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    FillRect(dc, &rc, g_theme.cardBrush);
    const bool pressed = di->itemState & ODS_SELECTED;
    const bool focused = di->itemState & ODS_FOCUS;
    COLORREF fill = pressed ? g_theme.cardBorder : g_theme.card;
    DrawRoundRect(dc, rc, S(8), fill,
                  focused ? g_theme.danger : g_theme.cardBorder);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_theme.danger);
    HGDIOBJ old = SelectObject(dc, g_font);
    wchar_t text[64];
    GetWindowTextW(di->hwndItem, text, 64);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

// Reaplica cores após mudança de tema do Windows
void OnThemeChanged() {
    ThemeLoad();
    ThemeApplyToWindow(g_main);
    SetClassLongPtrW(g_main, GCLP_HBRBACKGROUND, (LONG_PTR) g_theme.bgBrush);
    for (HWND child = GetWindow(g_main, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        ThemeApplyToControl(child);
    }
    RedrawWindow(g_main, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// ----- Janela principal -----------------------------------------------------

void ShowMainWindow() {
    ShowWindow(g_main, IsIconic(g_main) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_main);
    UpdatePreviewEnabled();
}

void HideMainWindow() {
    ShowWindow(g_main, SW_HIDE);
    UpdatePreviewEnabled();
    if (!g_hiddenHintShown) {
        g_hiddenHintShown = true;
        ShowBalloon(L"O MobLens continua funcionando na bandeja do sistema.");
    }
}

// Opções do aplicativo no menu da bandeja (espelham os controles da janela)
const UINT kTrayComboBase = 1000;  // + combo * 100 + item
const UINT kTrayZoomBase = 1900;   // + índice do zoom
const UINT kTrayToggleBase = 1950; // + índice do interruptor
const UINT kTrayCombos[] = {IDC_CAMERA, IDC_RESOLUTION, IDC_FPS, IDC_QUALITY,
                            IDC_ROTATION, IDC_CLOSE_ACTION};
const wchar_t *const kTrayComboNames[] = {
    L"Câmera", L"Resolução", L"Taxa de quadros", L"Qualidade do vídeo",
    L"Rotação", L"Ao fechar a janela"};
const UINT kTrayZooms[] = {10, 15, 20, 30, 40, 60, 80};
const UINT kTrayToggles[] = {IDC_MIRROR, IDC_TORCH, IDC_AUTOSTART};
const wchar_t *const kTrayToggleNames[] = {
    L"Espelhar imagem", L"Lanterna", L"Iniciar com o Windows"};

HMENU TrayComboMenu(size_t combo) {
    HMENU sub = CreatePopupMenu();
    HWND ctl = Ctl(kTrayCombos[combo]);
    int count = (int) SendMessageW(ctl, CB_GETCOUNT, 0, 0);
    int sel = (int) SendMessageW(ctl, CB_GETCURSEL, 0, 0);
    for (int i = 0; i < count && i < 100; ++i) {
        wchar_t text[128] = {};
        if (SendMessageW(ctl, CB_GETLBTEXTLEN, i, 0) < 127) {
            SendMessageW(ctl, CB_GETLBTEXT, i, (LPARAM) text);
        }
        UINT id = kTrayComboBase + (UINT) combo * 100 + i;
        AppendMenuW(sub, MF_STRING, id, text);
        if (i == sel) {
            CheckMenuRadioItem(sub, id, id, id, MF_BYCOMMAND);
        }
    }
    return sub;
}

void AppendTrayOptions(HMENU menu) {
    for (size_t i = 0; i < 5; ++i) {  // câmera e imagem
        AppendMenuW(menu, MF_POPUP, (UINT_PTR) TrayComboMenu(i),
                    kTrayComboNames[i]);
    }
    HMENU zoom = CreatePopupMenu();
    UINT current = (UINT) SendMessageW(Ctl(IDC_ZOOM), TBM_GETPOS, 0, 0);
    for (size_t i = 0; i < ARRAYSIZE(kTrayZooms); ++i) {
        wchar_t text[16];
        swprintf(text, 16, L"%u,%ux", kTrayZooms[i] / 10, kTrayZooms[i] % 10);
        UINT id = kTrayZoomBase + (UINT) i;
        AppendMenuW(zoom, MF_STRING, id, text);
        if (kTrayZooms[i] == current) {
            CheckMenuRadioItem(zoom, id, id, id, MF_BYCOMMAND);
        }
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR) zoom, L"Zoom");
    for (size_t i = 0; i < 2; ++i) {
        AppendMenuW(menu, MF_STRING | (ToggleGet(Ctl(kTrayToggles[i]))
                                           ? MF_CHECKED : 0),
                    kTrayToggleBase + i, kTrayToggleNames[i]);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (ToggleGet(Ctl(IDC_AUTOSTART)) ? MF_CHECKED
                                                                 : 0),
                kTrayToggleBase + 2, kTrayToggleNames[2]);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR) TrayComboMenu(5),
                kTrayComboNames[5]);
}

void OnCommand(UINT id, UINT code);

// Retorna true se o comando era uma opção do menu da bandeja
bool HandleTrayOption(UINT id) {
    if (id >= kTrayComboBase && id < kTrayComboBase + 600) {
        UINT combo = (id - kTrayComboBase) / 100;
        SendMessageW(Ctl(kTrayCombos[combo]), CB_SETCURSEL,
                     (id - kTrayComboBase) % 100, 0);
        OnCommand(kTrayCombos[combo], CBN_SELCHANGE);
        return true;
    }
    if (id >= kTrayZoomBase && id < kTrayZoomBase + ARRAYSIZE(kTrayZooms)) {
        SendMessageW(Ctl(IDC_ZOOM), TBM_SETPOS, TRUE,
                     kTrayZooms[id - kTrayZoomBase]);
        UpdateZoomLabel();
        ApplyFromUi();
        return true;
    }
    if (id >= kTrayToggleBase && id < kTrayToggleBase + 3) {
        HWND t = Ctl(kTrayToggles[id - kTrayToggleBase]);
        ToggleSet(t, !ToggleGet(t));
        OnCommand(kTrayToggles[id - kTrayToggleBase], BN_CLICKED);
        return true;
    }
    return false;
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                StatusText(CurrentStatus()).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Abrir o MobLens");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendTrayOptions(menu);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Sair");
    SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0,
                   g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void Uninstall() {
    if (MessageBoxW(g_main,
                    L"Remover a webcam virtual MobLens do Windows e fechar o "
                    L"aplicativo?",
                    kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    Log(L"desinstalação solicitada pelo usuário");
    g_streamer->Stop();
    if (RunElevated(L"--uninstall")) {
        SetAutostart(false);
        DestroyWindow(g_main);
    } else {
        g_streamer->Start();
    }
}

void OnCommand(UINT id, UINT code) {
    switch (id) {
        case IDC_CAMERA:
            if (code == CBN_SELCHANGE) {
                FillResolutionAndFps();
                ApplyFromUi();
            }
            break;
        case IDC_RESOLUTION:
            if (code == CBN_SELCHANGE) {
                FillFps();  // a alta velocidade depende da resolução
                ApplyFromUi();
            }
            break;
        case IDC_FPS:
        case IDC_QUALITY:
        case IDC_ROTATION:
        case IDC_CLOSE_ACTION:
            if (code == CBN_SELCHANGE) {
                ApplyFromUi();
            }
            break;
        case IDC_MIRROR:
        case IDC_TORCH:
            ApplyFromUi();
            break;
        case IDC_AUTOSTART:
            SetAutostart(ToggleGet(Ctl(IDC_AUTOSTART)));
            break;
        case IDC_UNINSTALL:
            Uninstall();
            break;
        case ID_TRAY_OPEN:
            ShowMainWindow();
            break;
        case ID_TRAY_EXIT:
            DestroyWindow(g_main);
            break;
        default:
            HandleTrayOption(id);
            break;
    }
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        UpdateTray(true);  // o Explorer reiniciou
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                    ShowMainWindow();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayMenu();
                    break;
            }
            return 0;
        case WM_STATUS_CHANGED:
            OnStatusChanged();
            return 0;
        case WM_CAMERAS_CHANGED: {
            {
                std::lock_guard<std::mutex> lock(g_sharedMutex);
                if (!g_camerasPending) {
                    return 0;
                }
                g_cameras = g_pendingCameras;
                g_camerasPending = false;
            }
            FillCameras();
            FillResolutionAndFps();
            ApplyFromUi();
            return 0;
        }
        case WM_PREVIEW_READY:
            g_previewPosted = false;
            if (g_streamer->GetPreview(g_previewPixels)) {
                g_hasPreview = true;
                InvalidateRect(g_preview, nullptr, FALSE);
            }
            return 0;
        case WM_HSCROLL:
            if ((HWND) lp == Ctl(IDC_ZOOM)) {
                UpdateZoomLabel();
                // Zoom ao vivo (canal de controle, sem reiniciar a câmera)
                ApplyFromUi();
            }
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            // Desenho em memória (sem cintilar)
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            PaintMain(mem);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            // Rótulos e trackbar ficam sobre os cartões
            HDC dc = (HDC) wp;
            bool value = (HWND) lp == Ctl(IDC_ZOOM_LABEL);
            SetBkColor(dc, g_theme.card);
            SetTextColor(dc, value ? g_theme.text : g_theme.subtext);
            return (LRESULT) g_theme.cardBrush;
        }
        case WM_CTLCOLORLISTBOX: {
            // Lista aberta dos combos
            HDC dc = (HDC) wp;
            SetBkColor(dc, g_theme.card);
            SetTextColor(dc, g_theme.text);
            return (LRESULT) g_theme.cardBrush;
        }
        case WM_DRAWITEM:
            if (wp == IDC_UNINSTALL) {
                DrawUninstallButton((const DRAWITEMSTRUCT *) lp);
                return TRUE;
            }
            break;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((const wchar_t *) lp, L"ImmersiveColorSet")) {
                OnThemeChanged();
            }
            break;
        case WM_SIZE:
            UpdatePreviewEnabled();
            break;
        case WM_CLOSE:
            if (g_closeAction == 0) {
                HideMainWindow();
            } else {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Garante que a câmera virtual esteja instalada e atualizada
bool EnsureCameraInstalled() {
    CameraInstallState state = GetCameraInstallState();
    Log(L"webcam virtual: %ls", state == CameraInstallState::Ok ? L"instalada"
        : state == CameraInstallState::Missing ? L"não instalada" : L"desatualizada");
    if (state == CameraInstallState::Ok) {
        return true;
    }
    const wchar_t *msg = state == CameraInstallState::Missing
        ? L"A webcam virtual \"MobLens\" ainda não está instalada no "
          L"Windows.\n\nInstalar agora? (será pedida permissão de "
          L"administrador uma única vez)"
        : L"Há uma versão nova da webcam virtual \"MobLens\".\n\n"
          L"Atualizar agora? (requer permissão de administrador)";
    if (MessageBoxW(nullptr, msg, kAppName, MB_YESNO | MB_ICONQUESTION)
            == IDYES && RunElevated(L"--install")) {
        return true;
    }
    // Uma versão antiga instalada ainda funciona
    return state == CameraInstallState::Outdated;
}

int RunAdminCommand(bool install) {
    std::wstring error;
    bool ok = install ? InstallCamera(error) : UninstallCamera(error);
    Log(L"%ls: %ls %ls", install ? L"instalação" : L"desinstalação",
        ok ? L"ok" : L"FALHOU", error.c_str());
    if (!ok) {
        MessageBoxW(nullptr, error.c_str(), kAppName, MB_OK | MB_ICONERROR);
    }
    return ok ? 0 : 1;
}

} // namespace

std::wstring AppDir() {
    std::wstring path = ExePath();
    return path.substr(0, path.find_last_of(L"\\/"));
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    std::wstring args = cmdLine ? cmdLine : L"";
    LogInit();
    Log(L"MobLens iniciado (args: '%ls')", args.c_str());
    if (args.find(L"--install") != std::wstring::npos) {
        return RunAdminCommand(true);
    }
    if (args.find(L"--uninstall") != std::wstring::npos) {
        return RunAdminCommand(false);
    }
    const bool startHidden = args.find(L"--tray") != std::wstring::npos;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MobLensApp");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Já em execução: traz a janela existente para a frente
        HWND existing = FindWindowW(kMainClass, nullptr);
        if (existing) {
            PostMessageW(existing, WM_TRAY, 0, WM_LBUTTONUP);
        }
        return 0;
    }

    if (!EnsureCameraInstalled()) {
        return 1;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    INITCOMMONCONTROLSEX icc = {sizeof(icc),
                                ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    g_scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, screen);

    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&lf);
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 2;
    lf.lfWeight = FW_SEMIBOLD;
    g_fontTitle = CreateFontIndirectW(&lf);

    ThemeLoad();
    RegisterToggleClass(instance);

    LoadSettings();

    g_icons[0] = MakeCameraIcon(RGB(140, 140, 140));
    g_icons[1] = MakeCameraIcon(RGB(52, 152, 219));
    g_icons[2] = MakeCameraIcon(RGB(39, 174, 96));

    WNDCLASSW wc = {};
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kPreviewClass;
    RegisterClassW(&wc);

    wc.lpfnWndProc = MainProc;
    wc.hbrBackground = g_theme.bgBrush;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kMainClass;
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU
                      | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rc = {0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRect(&rc, style, FALSE);
    g_main = CreateWindowW(kMainClass, kAppName, style, CW_USEDEFAULT,
                           CW_USEDEFAULT, rc.right - rc.left,
                           rc.bottom - rc.top, nullptr, nullptr, instance,
                           nullptr);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    ThemeApplyToWindow(g_main);

    Streamer::Callbacks cbs;
    cbs.status = [](const StreamStatus &status) {
        {
            std::lock_guard<std::mutex> lock(g_sharedMutex);
            if (g_status.state == status.state
                    && g_status.detail == status.detail) {
                return;
            }
            g_status = status;
        }
        PostMessageW(g_main, WM_STATUS_CHANGED, 0, 0);
    };
    cbs.cameras = [](const std::vector<CameraInfo> &cameras) {
        {
            std::lock_guard<std::mutex> lock(g_sharedMutex);
            g_pendingCameras = cameras;
            g_camerasPending = true;
        }
        PostMessageW(g_main, WM_CAMERAS_CHANGED, 0, 0);
    };
    cbs.preview = [] {
        if (!g_previewPosted.exchange(true)) {
            PostMessageW(g_main, WM_PREVIEW_READY, 0, 0);
        }
    };
    Streamer streamer(cbs);
    g_streamer = &streamer;

    CreateControls();
    streamer.SetSettings(g_settings);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    UpdateTray(true);
    OnStatusChanged();

    if (startHidden) {
        g_hiddenHintShown = true;
    } else {
        ShowWindow(g_main, SW_SHOW);
        UpdateWindow(g_main);
    }
    UpdatePreviewEnabled();
    streamer.Start();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    streamer.Stop();
    g_streamer = nullptr;
    for (HICON icon : g_icons) {
        DestroyIcon(icon);
    }
    DeleteObject(g_font);
    DeleteObject(g_fontBold);
    DeleteObject(g_fontTitle);
    WSACleanup();
    MFShutdown();
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
