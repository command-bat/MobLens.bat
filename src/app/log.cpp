// Log de diagnóstico: %LOCALAPPDATA%\MobLens\moblens.log (UTF-8)
#include "app.h"

#include <cstdarg>
#include <cstdio>

namespace {

std::mutex g_logMutex;

std::wstring LogPath() {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    std::wstring path = (n && n < MAX_PATH) ? dir : L"C:\\ProgramData";
    path += L"\\MobLens";
    CreateDirectoryW(path.c_str(), nullptr);
    return path + L"\\moblens.log";
}

std::string ToUtf8(const std::wstring &s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int) s.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out((size_t) n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int) s.size(), &out[0], n,
                        nullptr, nullptr);
    return out;
}

void WriteLine(const std::string &text) {
    static const std::wstring path = LogPath();
    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%02u:%02u:%02u.%03u [%lu] ",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             GetCurrentProcessId());
    std::string line = prefix + text + "\r\n";
    DWORD written;
    WriteFile(f, line.data(), (DWORD) line.size(), &written, nullptr);
    CloseHandle(f);
}

} // namespace

void LogInit() {
    // Mantém o arquivo pequeno: recomeça se passar de 2 MB
    std::wstring path = LogPath();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)
            && fa.nFileSizeLow > 2 * 1024 * 1024) {
        DeleteFileW(path.c_str());
    }
}

void Log(const wchar_t *fmt, ...) {
    wchar_t buf[2048];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 2048, fmt, args);
    va_end(args);
    WriteLine(ToUtf8(buf));
}

void LogRaw(const std::string &prefix, const std::string &text) {
    // Uma linha de log por linha do texto
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            WriteLine(prefix + line);
        }
        start = end + 1;
    }
}
