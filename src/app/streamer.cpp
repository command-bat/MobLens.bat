// Transmissão: celular (scrcpy-server, câmera) -> decodificador -> câmera
// virtual MobLens (+ prévia na janela).
//
// Protocolo do scrcpy-server (somente vídeo, túnel "adb reverse"):
//   1. 64 bytes: nome do aparelho
//   2. 4 bytes:  codec (big-endian; "h264")
//   3. pacotes com cabeçalho de 12 bytes:
//        - bit mais alto do byte 0 = 1: cabeçalho de sessão
//          (bytes 4..7 largura, 8..11 altura)
//        - senão: 8 bytes pts+flags (bit 62 = config, bit 61 = keyframe),
//          4 bytes tamanho, seguidos do conteúdo
#include "app.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "../common/moblens_producer.h"

namespace {

// Caminho do servidor no celular (ele se apaga ao iniciar)
const wchar_t kDeviceServerPath[] =
    L"/data/local/tmp/moblens-server-" SCRCPY_SERVER_VERSION_W L".jar";

const uint64_t kPacketFlagConfig = 1ull << 62;
const uint64_t kPacketFlagKeyFrame = 1ull << 61;
const uint64_t kPacketPtsMask = kPacketFlagKeyFrame - 1;
const uint32_t kCodecH264 = 0x68323634;  // "h264"

// Sem webcam em uso nem prévia aberta por este tempo: desliga a câmera
const ULONGLONG kIdleTimeoutMs = 3000;
const ULONGLONG kPreviewIntervalMs = 33;

uint32_t ReadBE32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
         | ((uint32_t) p[2] << 8) | p[3];
}

uint64_t ReadBE64(const uint8_t *p) {
    return ((uint64_t) ReadBE32(p) << 32) | ReadBE32(p + 4);
}

bool RecvAll(SOCKET s, void *buf, size_t len) {
    char *p = static_cast<char *>(buf);
    while (len) {
        int r = recv(s, p, (int) std::min<size_t>(len, 1 << 20), 0);
        if (r <= 0) {
            return false;
        }
        p += r;
        len -= (size_t) r;
    }
    return true;
}

std::wstring Hex8(uint32_t v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%08x", v);
    return buf;
}

std::wstring Widen(const std::string &s) {
    return std::wstring(s.begin(), s.end());
}

std::string Trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r");
    size_t e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

uint32_t RandomScid() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint32_t v = (uint32_t) t.QuadPart ^ (GetCurrentProcessId() << 16)
               ^ (uint32_t) GetTickCount64();
    return v & 0x7fffffff;  // positivo (int do Java)
}

} // namespace

// Exemplo da saída do servidor:
//   --camera-id=0    (back, 4000x3000, fps={15, 20, 30}, zoom-range=[1, 8])
//       - 4000x3000
//       - 1920x1080
//       High speed capture (--camera-high-speed):
//           - 1920x1080 (fps={120, 240})
std::vector<CameraInfo> ParseCameraList(const std::string &output) {
    std::vector<CameraInfo> cameras;
    bool highSpeed = false;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        size_t pos = line.find("--camera-id=");
        if (pos != std::string::npos) {
            CameraInfo cam;
            size_t start = pos + 12;
            size_t end = line.find_first_of(" \t\r", start);
            cam.id = Widen(line.substr(start, end - start));

            size_t open = line.find('(', start);
            if (open != std::string::npos) {
                size_t close = line.find_first_of(",)", open + 1);
                cam.facing = Widen(Trim(line.substr(open + 1,
                                                    close - open - 1)));
            }

            size_t fps = line.find("fps=");
            if (fps != std::string::npos && fps + 5 < line.size()) {
                // "fps={15, 20, 30}" (ou "fps=[...]")
                size_t close = line.find_first_of("}]", fps);
                std::string list = line.substr(fps + 5, close - fps - 5);
                std::istringstream items(list);
                std::string item;
                while (std::getline(items, item, ',')) {
                    unsigned v = 0;
                    if (sscanf(item.c_str(), "%u", &v) == 1 && v) {
                        cam.fps.push_back(v);
                    }
                }
            }
            cameras.push_back(cam);
            highSpeed = false;
            continue;
        }

        if (line.find("igh speed") != std::string::npos) {
            highSpeed = true;  // seção de alta velocidade
            continue;
        }

        std::string t = Trim(line);
        unsigned w = 0;
        unsigned h = 0;
        if (cameras.empty() || t.size() < 3 || t[0] != '-'
                || sscanf(t.c_str() + 1, " %ux%u", &w, &h) != 2 || !w || !h) {
            continue;
        }
        CameraSize size = {w, h};
        if (!highSpeed) {
            auto &sizes = cameras.back().sizes;
            if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) {
                sizes.push_back(size);
            }
            continue;
        }

        // "- 1920x1080 (fps={120, 240})"
        HighSpeedMode mode;
        mode.size = size;
        size_t fpsPos = t.find("fps=");
        if (fpsPos != std::string::npos) {
            std::string list = t.substr(fpsPos + 5,
                                        t.find_first_of("}]", fpsPos)
                                            - fpsPos - 5);
            std::istringstream items(list);
            std::string item;
            while (std::getline(items, item, ',')) {
                unsigned v = 0;
                if (sscanf(item.c_str(), "%u", &v) == 1 && v) {
                    mode.fps.push_back(v);
                }
            }
        }
        if (!mode.fps.empty()) {
            cameras.back().highSpeed.push_back(mode);
        }
    }

    for (auto &cam : cameras) {
        std::sort(cam.sizes.begin(), cam.sizes.end(),
                  [](const CameraSize &a, const CameraSize &b) {
                      return (uint64_t) a.width * a.height
                           > (uint64_t) b.width * b.height;
                  });
        if (cam.fps.empty()) {
            cam.fps = {30};
        }
    }
    return cameras;
}

// Seções "== Camera HAL device .../<id> (v3.5) static information: =="
// contêm "android.lens.info.availableFocalLengths" e
// "android.sensor.info.physicalSize", com os valores na linha seguinte
void ParseCameraOptics(const std::string &dump,
                       std::vector<CameraInfo> &cameras) {
    std::istringstream lines(dump);
    std::string line;
    CameraInfo *current = nullptr;
    while (std::getline(lines, line)) {
        if (line.find("static information") != std::string::npos
                && line.find("==") != std::string::npos) {
            current = nullptr;
            size_t paren = line.find(" (");
            size_t slash = line.rfind('/', paren);
            if (paren == std::string::npos || slash == std::string::npos) {
                continue;
            }
            std::wstring id = Widen(line.substr(slash + 1,
                                                paren - slash - 1));
            for (auto &cam : cameras) {
                if (cam.id == id) {
                    current = &cam;
                }
            }
            continue;
        }
        if (!current) {
            continue;
        }
        const bool focal =
            line.find("lens.info.availableFocalLengths (") != std::string::npos;
        const bool sensor =
            line.find("sensor.info.physicalSize (") != std::string::npos;
        if (!focal && !sensor) {
            continue;
        }
        std::string values;
        if (!std::getline(lines, values)) {
            break;
        }
        float a = 0;
        float b = 0;
        size_t open = values.find('[');
        if (open == std::string::npos) {
            continue;
        }
        const char *p = values.c_str() + open + 1;
        if (focal && sscanf(p, "%f", &a) == 1) {
            current->focalLength = a;
        } else if (sensor && sscanf(p, "%f %f", &a, &b) == 2) {
            current->sensorDiagonal = sqrtf(a * a + b * b);
        }
    }
}

Streamer::Streamer(Callbacks cbs)
        : m_cbs(std::move(cbs)), m_adb(AppDir() + L"\\adb.exe"),
          m_serverPath(AppDir() + L"\\scrcpy-server") {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_producer = new moblens_producer;
    moblens_producer_init(m_producer);
    m_preview.assign((size_t) PREVIEW_WIDTH * PREVIEW_HEIGHT, 0xff000000u);
}

Streamer::~Streamer() {
    Stop();
    moblens_producer_close(m_producer);
    delete m_producer;
    CloseHandle(m_wakeEvent);
    CloseHandle(m_stopEvent);
}

void Streamer::Start() {
    if (m_thread.joinable()) {
        return;
    }
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&Streamer::Run, this);
    m_idleThread = std::thread(&Streamer::IdleLoop, this);
}

// Enquanto algum aplicativo usa a webcam e o celular não está transmitindo,
// envia um vídeo com o logo do MobLens e o que falta fazer (~15 fps).
// Durante um reinício da câmera (troca de resolução, lente...) a webcam
// mantém a última imagem real, como antes.
void Streamer::IdleLoop() {
    moblens_producer producer;
    moblens_producer_init(&producer);
    IdleFrameRenderer renderer;
    const ULONGLONG start = GetTickCount64();

    while (WaitForSingleObject(m_stopEvent, 66) == WAIT_TIMEOUT) {
        const auto state = (StreamState) m_state.load();
        if (state == StreamState::Streaming
                || (state == StreamState::Starting && m_realFrames)) {
            continue;
        }
        if (!moblens_producer_try_open(&producer)
                || !moblens_producer_consumer_active(&producer)) {
            continue;
        }

        std::wstring detail;
        {
            std::lock_guard<std::mutex> lock(m_detailMutex);
            detail = m_detail;
        }
        std::wstring message;
        std::wstring hint;
        bool busy = false;
        switch (state) {
            case StreamState::NoDevice:
                message = L"Celular não conectado";
                hint = L"Conecte o celular pelo cabo USB, com a depuração "
                       L"USB ativada";
                break;
            case StreamState::Unauthorized:
                message = L"Autorize este computador no celular";
                hint = L"Toque em \"Permitir\" no aviso de depuração USB";
                break;
            case StreamState::Error:
                message = L"Não foi possível ligar a câmera do celular";
                hint = detail;
                break;
            default:  // pronto / iniciando
                message = L"Ligando a câmera do celular";
                busy = true;
                break;
        }

        uint32_t w;
        uint32_t h;
        moblens_producer_requested_size(&producer, &w, &h);
        const double seconds = (GetTickCount64() - start) / 1000.0;
        std::lock_guard<std::mutex> lock(m_shmWriteMutex);
        // Confere de novo: o vídeo real pode ter começado agora
        const auto now = (StreamState) m_state.load();
        if (now == StreamState::Streaming || m_realFrames) {
            continue;
        }
        uint8_t *dst = moblens_producer_begin(&producer);
        renderer.Render(dst, w, h, message, hint, busy, seconds);
        moblens_producer_end(&producer, w, h);
    }
    moblens_producer_close(&producer);
}

void Streamer::Stop() {
    SetEvent(m_stopEvent);
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        if (m_socket != INVALID_SOCKET) {
            shutdown(m_socket, SD_BOTH);
        }
        if (m_serverProcess) {
            TerminateProcess(m_serverProcess, 0);
        }
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_idleThread.joinable()) {
        m_idleThread.join();
    }
}

void Streamer::InterruptSession() {
    std::lock_guard<std::mutex> lock(m_sessionMutex);
    if (m_socket != INVALID_SOCKET) {
        m_restart = true;
        shutdown(m_socket, SD_BOTH);
    }
}

void Streamer::SetSettings(const VideoSettings &settings) {
    bool restart;
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        restart = !m_settings.SameCapture(settings);
        m_settings = settings;
        if (restart) {
            m_autoSize = false;
        }
    }
    if (restart) {
        InterruptSession();
    } else {
        ApplyLiveControls();
    }
}

namespace {
// Zoom do scrcpy-server: passos de 1 + 1/16 (CameraCapture.ZOOM_FACTOR)
int ZoomSteps(UINT32 zoomTenths) {
    return (int) std::lround(std::log(zoomTenths / 10.0)
                             / std::log(1.0 + 1.0 / 16.0));
}

// Mensagens de controle do scrcpy (control_msg.h)
const uint8_t kMsgCameraSetTorch = 18;
const uint8_t kMsgCameraZoomIn = 19;
const uint8_t kMsgCameraZoomOut = 20;
} // namespace

// Aplica zoom e lanterna à câmera ligada, sem reiniciá-la
void Streamer::ApplyLiveControls() {
    UINT32 zoomTenths;
    bool torch;
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        zoomTenths = m_settings.zoomTenths;
        torch = m_settings.torch;
    }

    std::lock_guard<std::mutex> lock(m_sessionMutex);
    if (m_control == INVALID_SOCKET) {
        return;  // sem câmera ligada: vale na próxima sessão
    }
    if (torch != m_torchOn) {
        uint8_t msg[2] = {kMsgCameraSetTorch, (uint8_t) (torch ? 1 : 0)};
        if (send(m_control, (const char *) msg, 2, 0) == 2) {
            m_torchOn = torch;
            Log(L"lanterna: %ls", torch ? L"ligada" : L"desligada");
        }
    }
    int target = ZoomSteps(zoomTenths);
    while (m_zoomSteps != target) {
        uint8_t msg = m_zoomSteps < target ? kMsgCameraZoomIn
                                           : kMsgCameraZoomOut;
        if (send(m_control, (const char *) &msg, 1, 0) != 1) {
            break;
        }
        m_zoomSteps += m_zoomSteps < target ? 1 : -1;
    }
}

void Streamer::SetPreviewEnabled(bool enabled) {
    m_previewEnabled = enabled;
    if (enabled) {
        SetEvent(m_wakeEvent);
    }
}

bool Streamer::GetPreview(std::vector<uint32_t> &out) {
    std::lock_guard<std::mutex> lock(m_previewMutex);
    if (!m_previewFresh) {
        return false;
    }
    out = m_preview;
    m_previewFresh = false;
    return true;
}

void Streamer::SetStatus(StreamState state, const std::wstring &detail) {
    static const wchar_t *const names[] = {L"sem celular", L"não autorizado",
        L"pronto", L"iniciando", L"transmitindo", L"erro"};
    Log(L"estado: %ls %ls", names[(int) state], detail.c_str());
    {
        std::lock_guard<std::mutex> lock(m_detailMutex);
        m_detail = detail;
    }
    if (state != StreamState::Starting && state != StreamState::Streaming) {
        m_realFrames = false;  // sem vídeo do celular: volta o vídeo de espera
    }
    m_state = (int) state;
    StreamStatus status;
    status.state = state;
    status.detail = detail;
    m_cbs.status(status);
}

bool Streamer::Sleep(DWORD ms) {
    HANDLE handles[2] = {m_stopEvent, m_wakeEvent};
    return WaitForMultipleObjects(2, handles, FALSE, ms) != WAIT_OBJECT_0;
}

bool Streamer::ConsumerActive() {
    return moblens_producer_try_open(m_producer)
        && moblens_producer_consumer_active(m_producer);
}

bool Streamer::Wanted() {
    return m_previewEnabled || ConsumerActive();
}

void Streamer::Run() {
    while (WaitForSingleObject(m_stopEvent, 0) == WAIT_TIMEOUT) {
        bool unauthorized = false;
        std::wstring serial = m_adb.FirstDevice(&unauthorized);
        if (serial.empty()) {
            m_preparedSerial.clear();
            SetStatus(unauthorized ? StreamState::Unauthorized
                                   : StreamState::NoDevice);
            if (!Sleep(2000)) {
                break;
            }
            continue;
        }
        if (m_preparedSerial != serial && !PrepareDevice(serial)) {
            if (!Sleep(3000)) {
                break;
            }
            continue;
        }
        RunSession(serial);
    }
}

// O scrcpy-server apaga o próprio arquivo ao iniciar, então ele precisa ser
// enviado antes de cada execução (como o scrcpy faz)
bool Streamer::PushServer(const std::wstring &serial) {
    if (m_adb.Run(L"-s " + serial + L" push \"" + m_serverPath + L"\" "
                  + kDeviceServerPath) != 0) {
        SetStatus(StreamState::Error, L"falha ao enviar o servidor");
        return false;
    }
    return true;
}

// Lê a lista de câmeras (uma vez por conexão do aparelho)
bool Streamer::PrepareDevice(const std::wstring &serial) {
    SetStatus(StreamState::Starting, L"preparando o celular");
    const std::wstring s = L"-s " + serial + L" ";
    if (!PushServer(serial)) {
        return false;
    }

    std::string output;
    m_adb.Run(s + L"shell CLASSPATH=" + kDeviceServerPath
                  + L" app_process / com.genymobile.scrcpy.Server "
                  + L"" SCRCPY_SERVER_VERSION
                  + L" log_level=info list_camera_sizes=true",
              &output, 20000);
    std::vector<CameraInfo> cameras = ParseCameraList(output);
    // Distância focal / sensor para nomear as lentes (opcional)
    std::string dump;
    if (!cameras.empty()
            && m_adb.Run(s + L"shell dumpsys media.camera", &dump, 15000)
                   == 0) {
        ParseCameraOptics(dump, cameras);
    }
    if (m_cbs.cameras) {
        m_cbs.cameras(cameras);
    }

    m_preparedSerial = serial;
    return true;
}

std::wstring Streamer::ServerArgs(uint32_t scid) {
    VideoSettings st;
    bool autoSize;
    {
        std::lock_guard<std::mutex> lock(m_settingsMutex);
        st = m_settings;
        autoSize = m_autoSize;
    }

    std::wstring a = L"shell CLASSPATH=";
    a += kDeviceServerPath;
    a += L" app_process / com.genymobile.scrcpy.Server " SCRCPY_SERVER_VERSION_W;
    a += L" scid=" + Hex8(scid);
    // control=true: canal de controle para zoom e lanterna em tempo real
    a += L" log_level=info audio=false control=true clipboard_autosync=false";
    a += L" video_source=camera video_codec=h264";
    a += L" video_bit_rate=" + std::to_wstring(st.bitRateMbps * 1000000u);
    if (!st.cameraId.empty()) {
        a += L" camera_id=" + st.cameraId;
    } else {
        a += L" camera_facing=back";
    }
    if (autoSize) {
        a += L" max_size=" + std::to_wstring(std::max(st.width, st.height));
    } else {
        a += L" camera_size=" + std::to_wstring(st.width) + L"x"
           + std::to_wstring(st.height);
    }
    a += L" camera_fps=" + std::to_wstring(st.fps);
    if (st.highSpeed) {
        a += L" camera_high_speed=true";
    }
    int steps = ZoomSteps(st.zoomTenths);
    if (steps != 0) {
        // Valor exato do passo, para o zoom ao vivo partir do mesmo ponto
        wchar_t zoom[32];
        swprintf(zoom, 32, L"%.5f", std::pow(1.0 + 1.0 / 16.0, steps));
        a += L" camera_zoom=";
        a += zoom;
    }
    if (st.torch) {
        a += L" camera_torch=true";
    }
    if (st.rotation) {
        a += L" capture_orientation=" + std::to_wstring(st.rotation);
    }
    return a;
}

void Streamer::RunSession(const std::wstring &serial) {
    // 1. Espera um aplicativo abrir a webcam (ou a prévia), poupando a
    //    bateria do celular
    if (!m_restart) {
        SetStatus(StreamState::Idle);
        ULONGLONG lastDeviceCheck = GetTickCount64();
        while (!Wanted()) {
            if (!Sleep(300)) {
                return;
            }
            if (GetTickCount64() - lastDeviceCheck > 3000) {
                lastDeviceCheck = GetTickCount64();
                if (m_adb.FirstDevice() != serial) {
                    return;  // celular desconectado
                }
            }
        }
    }
    m_restart = false;
    SetStatus(StreamState::Starting, L"ligando a câmera do celular");
    moblens_producer_heartbeat(m_producer);

    const std::wstring s = L"-s " + serial + L" ";

    // 2. Socket local + túnel reverso (o celular conecta no PC)
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int addrLen = sizeof(addr);
    if (listener == INVALID_SOCKET
            || bind(listener, (sockaddr *) &addr, sizeof(addr)) != 0
            || listen(listener, 2) != 0
            || getsockname(listener, (sockaddr *) &addr, &addrLen) != 0) {
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
        }
        SetStatus(StreamState::Error, L"falha ao abrir porta local");
        Sleep(2000);
        return;
    }
    const unsigned port = ntohs(addr.sin_port);

    const uint32_t scid = RandomScid();
    const std::wstring socketName = L"localabstract:scrcpy_" + Hex8(scid);
    if (m_adb.Run(s + L"reverse " + socketName + L" tcp:"
                  + std::to_wstring(port)) != 0) {
        closesocket(listener);
        m_preparedSerial.clear();
        SetStatus(StreamState::Error, L"falha ao criar o túnel adb");
        Sleep(2000);
        return;
    }

    // 3. Inicia o servidor no celular
    if (!PushServer(serial)) {
        closesocket(listener);
        m_adb.Run(s + L"reverse --remove " + socketName, nullptr, 5000);
        Sleep(2000);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        m_serverProcess = m_adb.Spawn(s + ServerArgs(scid));
    }

    // 4. Aguarda as conexões do celular: vídeo e depois controle (até 15 s)
    auto acceptOne = [&]() -> SOCKET {
        for (int i = 0; i < 150 && m_serverProcess; ++i) {
            // Mantém a última imagem na webcam enquanto a câmera reinicia
            moblens_producer_heartbeat(m_producer);
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener, &set);
            timeval tv = {0, 100000};
            if (select(0, &set, nullptr, nullptr, &tv) > 0) {
                return accept(listener, nullptr, nullptr);
            }
            if (WaitForSingleObject(m_stopEvent, 0) != WAIT_TIMEOUT
                    || WaitForSingleObject(m_serverProcess, 0)
                       == WAIT_OBJECT_0) {
                break;  // parando, ou o servidor terminou com erro
            }
        }
        return INVALID_SOCKET;
    };
    SOCKET sock = acceptOne();
    SOCKET control = sock != INVALID_SOCKET ? acceptOne() : INVALID_SOCKET;
    if (control == INVALID_SOCKET && sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    if (control != INVALID_SOCKET) {
        BOOL noDelay = TRUE;
        setsockopt(control, IPPROTO_TCP, TCP_NODELAY, (const char *) &noDelay,
                   sizeof(noDelay));
    }
    closesocket(listener);
    m_adb.Run(s + L"reverse --remove " + socketName, nullptr, 5000);

    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        m_socket = sock;
        m_control = control;
        std::lock_guard<std::mutex> settingsLock(m_settingsMutex);
        m_zoomSteps = ZoomSteps(m_settings.zoomTenths);
        m_torchOn = m_settings.torch;
    }

    // 5. Recebe e decodifica o vídeo
    bool streamed = false;
    if (sock != INVALID_SOCKET) {
        uint8_t name[64];
        uint8_t codec[4];
        uint8_t header[12];
        if (RecvAll(sock, name, sizeof(name)) && RecvAll(sock, codec, 4)
                && ReadBE32(codec) == kCodecH264
                && RecvAll(sock, header, sizeof(header))
                && (header[0] & 0x80)) {
            name[63] = 0;
            const std::wstring deviceName = Widen((char *) name);

            H264Decoder decoder;
            if (decoder.Init(ReadBE32(header + 4), ReadBE32(header + 8))) {
                std::vector<uint8_t> config;
                std::vector<uint8_t> packet;
                std::vector<UINT32> xmap;
                std::vector<uint32_t> previewTmp(
                    (size_t) PREVIEW_WIDTH * PREVIEW_HEIGHT);
                ULONGLONG lastWantedTick = GetTickCount64();
                ULONGLONG lastPreviewTick = 0;

                auto onFrame = [&](const Nv12Frame &frame) {
                    if (!streamed) {
                        streamed = true;
                        wchar_t size[32];
                        swprintf(size, 32, L" (%ux%u)", frame.width,
                                 frame.height);
                        SetStatus(StreamState::Streaming, deviceName + size);
                    }
                    bool mirror;
                    {
                        std::lock_guard<std::mutex> lock(m_settingsMutex);
                        mirror = m_settings.mirror;
                    }

                    // Webcam virtual
                    if (moblens_producer_consumer_active(m_producer)) {
                        uint32_t w;
                        uint32_t h;
                        moblens_producer_requested_size(m_producer, &w, &h);
                        std::lock_guard<std::mutex> lock(m_shmWriteMutex);
                        uint8_t *dst = moblens_producer_begin(m_producer);
                        ScaleNv12(frame, dst, w, h, mirror, xmap);
                        moblens_producer_end(m_producer, w, h);
                        m_realFrames = true;
                    } else {
                        moblens_producer_heartbeat(m_producer);
                    }

                    // Prévia na janela (~30 fps)
                    ULONGLONG now = GetTickCount64();
                    if (m_previewEnabled
                            && now - lastPreviewTick >= kPreviewIntervalMs) {
                        lastPreviewTick = now;
                        ScaleNv12ToBgra(frame, previewTmp.data(),
                                        PREVIEW_WIDTH, PREVIEW_HEIGHT, mirror);
                        {
                            std::lock_guard<std::mutex> lock(m_previewMutex);
                            m_preview.swap(previewTmp);
                            m_previewFresh = true;
                        }
                        if (m_cbs.preview) {
                            m_cbs.preview();
                        }
                    }
                };

                while (RecvAll(sock, header, sizeof(header))) {
                    if (header[0] & 0x80) {
                        continue;  // nova sessão: o decodificador se adapta
                    }
                    uint64_t ptsFlags = ReadBE64(header);
                    uint32_t len = ReadBE32(header + 8);
                    if (!len || len > (64u << 20)) {
                        break;
                    }
                    packet.resize(len);
                    if (!RecvAll(sock, packet.data(), len)) {
                        break;
                    }

                    if (ptsFlags & kPacketFlagConfig) {
                        // SPS/PPS: enviado junto com o próximo quadro
                        config = packet;
                        continue;
                    }
                    if (!config.empty()) {
                        packet.insert(packet.begin(), config.begin(),
                                      config.end());
                        config.clear();
                    }
                    decoder.Decode(packet.data(), packet.size(),
                                   (int64_t) (ptsFlags & kPacketPtsMask),
                                   onFrame);

                    // Desliga a câmera do celular quando ninguém está usando
                    if (Wanted()) {
                        lastWantedTick = GetTickCount64();
                    } else if (GetTickCount64() - lastWantedTick
                               > kIdleTimeoutMs) {
                        break;
                    }
                }
            }
        }
    }

    // 6. Encerramento
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        if (m_control != INVALID_SOCKET) {
            closesocket(m_control);
            m_control = INVALID_SOCKET;
        }
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
        if (m_serverProcess) {
            TerminateProcess(m_serverProcess, 0);
            WaitForSingleObject(m_serverProcess, 3000);
            CloseHandle(m_serverProcess);
            m_serverProcess = nullptr;
        }
    }

    if (!streamed && !m_restart
            && WaitForSingleObject(m_stopEvent, 0) == WAIT_TIMEOUT) {
        bool retryAuto = false;
        {
            std::lock_guard<std::mutex> lock(m_settingsMutex);
            if (!m_autoSize) {
                m_autoSize = true;  // tenta de novo com resolução automática
                retryAuto = true;
            }
        }
        if (retryAuto) {
            SetStatus(StreamState::Error, L"configuração não suportada pela "
                      L"câmera; tentando resolução automática");
            Sleep(500);
        } else {
            SetStatus(StreamState::Error, L"o celular não iniciou a câmera");
            m_preparedSerial.clear();
            Sleep(3000);
        }
    }
}
