// MobLens.exe - declarações internas do aplicativo.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/moblens_ids.h"

// Versão do scrcpy-server distribuído em third_party/scrcpy (deve coincidir
// exatamente, o servidor recusa versões diferentes)
#define SCRCPY_SERVER_VERSION "5.0.1"
#define SCRCPY_SERVER_VERSION_W L"5.0.1"

// Tamanho da prévia exibida na janela
#define PREVIEW_WIDTH 640
#define PREVIEW_HEIGHT 360

std::wstring AppDir();

// Log de diagnóstico em %LOCALAPPDATA%\MobLens\moblens.log (log.cpp)
void LogInit();
void Log(const wchar_t *fmt, ...);
void LogRaw(const std::string &prefix, const std::string &text);

// ---------------------------------------------------------------------------
// Instalação da câmera virtual (installer.cpp)
// ---------------------------------------------------------------------------
enum class CameraInstallState { Missing, Outdated, Ok };
CameraInstallState GetCameraInstallState();
bool InstallCamera(std::wstring &error);    // requer administrador
bool UninstallCamera(std::wstring &error);  // requer administrador

// ---------------------------------------------------------------------------
// adb (adb.cpp)
// ---------------------------------------------------------------------------
class Adb {
public:
    explicit Adb(std::wstring exe) : m_exe(std::move(exe)) {}

    // Executa "adb <args>" e devolve o código de saída (-1 em falha/timeout)
    int Run(const std::wstring &args, std::string *output = nullptr,
            DWORD timeoutMs = 30000) const;

    // Inicia "adb <args>" em segundo plano (ex.: o servidor no celular)
    HANDLE Spawn(const std::wstring &args) const;

    // Primeiro aparelho autorizado ("device"); vazio se nenhum
    std::wstring FirstDevice(bool *unauthorized = nullptr) const;

private:
    std::wstring m_exe;
};

// ---------------------------------------------------------------------------
// Câmeras do celular e configurações de captura
// ---------------------------------------------------------------------------
struct CameraSize {
    UINT32 width;
    UINT32 height;
    bool operator==(const CameraSize &o) const {
        return width == o.width && height == o.height;
    }
};

struct HighSpeedMode {           // captura de alta velocidade (120+ fps)
    CameraSize size;
    std::vector<UINT32> fps;
};

struct CameraInfo {
    std::wstring id;              // ex.: "0"
    std::wstring facing;          // "back", "front", "external"
    std::vector<CameraSize> sizes;
    std::vector<UINT32> fps;
    std::vector<HighSpeedMode> highSpeed;
};

// Interpreta a saída de "list_camera_sizes=true" do scrcpy-server
std::vector<CameraInfo> ParseCameraList(const std::string &output);

struct VideoSettings {
    std::wstring cameraId;        // vazio = câmera traseira padrão
    UINT32 width = 1920;
    UINT32 height = 1080;
    UINT32 fps = 30;
    bool highSpeed = false;       // captura de alta velocidade (120 fps)
    UINT32 bitRateMbps = 8;
    UINT32 rotation = 0;          // 0, 90, 180, 270

    // Alterados com a câmera ligada (canal de controle, sem reiniciar)
    UINT32 zoomTenths = 10;       // 10 = 1,0x
    bool torch = false;

    bool mirror = false;          // aplicado no PC (sem reiniciar)

    // Configurações que exigem reiniciar a câmera no celular
    bool SameCapture(const VideoSettings &o) const {
        return cameraId == o.cameraId && width == o.width
            && height == o.height && fps == o.fps
            && highSpeed == o.highSpeed && bitRateMbps == o.bitRateMbps
            && rotation == o.rotation;
    }
};

// ---------------------------------------------------------------------------
// Decodificador H.264 nativo do Windows (h264_decoder.cpp)
// ---------------------------------------------------------------------------
struct Nv12Frame {
    const uint8_t *y;
    const uint8_t *uv;
    LONG pitch;
    UINT32 width;   // área visível
    UINT32 height;
};

class H264Decoder {
public:
    using FrameCallback = std::function<void(const Nv12Frame &)>;

    ~H264Decoder();
    bool Init(UINT32 width, UINT32 height);
    bool Decode(const uint8_t *data, size_t size, int64_t ptsUs,
                const FrameCallback &onFrame);
    void Close();

private:
    bool ConfigureOutput();
    bool Drain(const FrameCallback &onFrame);

    struct IMFTransform *m_mft = nullptr;
    UINT32 m_codedWidth = 0;
    UINT32 m_codedHeight = 0;
    UINT32 m_visibleWidth = 0;
    UINT32 m_visibleHeight = 0;
    LONG m_defaultStride = 0;
};

// ---------------------------------------------------------------------------
// Conversões de imagem (nv12.cpp)
// ---------------------------------------------------------------------------
// NV12 -> NV12 dstWidth x dstHeight, preservando a proporção (barras pretas)
void ScaleNv12(const Nv12Frame &src, uint8_t *dst, UINT32 dstWidth,
               UINT32 dstHeight, bool mirror, std::vector<UINT32> &xmap);

// NV12 -> BGRA (prévia), preservando a proporção
void ScaleNv12ToBgra(const Nv12Frame &src, uint32_t *dst, UINT32 dstWidth,
                     UINT32 dstHeight, bool mirror);

// ---------------------------------------------------------------------------
// Transmissão celular -> câmera virtual (streamer.cpp)
// ---------------------------------------------------------------------------
enum class StreamState {
    NoDevice,      // nenhum celular conectado
    Unauthorized,  // depuração USB não autorizada no celular
    Idle,          // celular pronto; ninguém usando a câmera
    Starting,      // iniciando a câmera do celular
    Streaming,     // transmitindo
    Error,
};

struct StreamStatus {
    StreamState state = StreamState::NoDevice;
    std::wstring detail;
};

class Streamer {
public:
    struct Callbacks {
        std::function<void(const StreamStatus &)> status;
        std::function<void(const std::vector<CameraInfo> &)> cameras;
        std::function<void()> preview;  // nova imagem de prévia disponível
    };

    explicit Streamer(Callbacks cbs);
    ~Streamer();

    void Start();
    void Stop();

    // Aplica novas configurações (reinicia a câmera do celular se preciso)
    void SetSettings(const VideoSettings &settings);

    // Com a prévia ativa, a câmera do celular fica ligada mesmo sem nenhum
    // aplicativo usando a webcam
    void SetPreviewEnabled(bool enabled);

    // Copia a última prévia (PREVIEW_WIDTH x PREVIEW_HEIGHT, BGRA)
    bool GetPreview(std::vector<uint32_t> &out);

private:
    void Run();
    void RunSession(const std::wstring &serial);
    bool PrepareDevice(const std::wstring &serial);
    bool PushServer(const std::wstring &serial);
    void SetStatus(StreamState state, const std::wstring &detail = L"");
    bool Sleep(DWORD ms);  // false se parando
    bool ConsumerActive();
    bool Wanted();         // algum app usando a webcam ou prévia aberta
    void InterruptSession();
    void ApplyLiveControls();  // zoom e lanterna pelo canal de controle
    std::wstring ServerArgs(uint32_t scid);

    Callbacks m_cbs;
    Adb m_adb;
    std::wstring m_serverPath;
    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_wakeEvent = nullptr;

    std::mutex m_settingsMutex;
    VideoSettings m_settings;
    bool m_autoSize = false;  // resolução escolhida falhou: usa automática

    std::atomic<bool> m_previewEnabled{false};
    std::atomic<bool> m_restart{false};

    std::mutex m_previewMutex;
    std::vector<uint32_t> m_preview;
    bool m_previewFresh = false;

    std::mutex m_sessionMutex;
    SOCKET m_socket = INVALID_SOCKET;   // conexão ativa (para interromper)
    SOCKET m_control = INVALID_SOCKET;  // canal de controle (zoom, lanterna)
    int m_zoomSteps = 0;                // zoom aplicado, em passos de 1,0625x
    bool m_torchOn = false;             // lanterna aplicada
    HANDLE m_serverProcess = nullptr;
    struct moblens_producer *m_producer = nullptr;
    std::wstring m_preparedSerial;
};
