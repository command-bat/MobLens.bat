// Execução do adb sem janelas de console.
#include "app.h"

#include <sstream>

static std::wstring BuildCommandLine(const std::wstring &exe,
                                     const std::wstring &args) {
    return L"\"" + exe + L"\" " + args;
}

int Adb::Run(const std::wstring &args, std::string *output,
             DWORD timeoutMs) const {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return -1;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi = {};

    std::wstring cmd = BuildCommandLine(m_exe, args);
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return -1;
    }

    // Lê a saída numa thread para não travar o processo com o pipe cheio
    std::string out;
    std::thread reader([&] {
        char buf[4096];
        DWORD n;
        while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n) {
            out.append(buf, n);
        }
    });

    int code = -1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        code = (int) exitCode;
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    reader.join();
    CloseHandle(readPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // "devices" roda a cada 2 s: não polui o log
    if (args != L"devices") {
        Log(L"adb %ls -> %d", args.c_str(), code);
        LogRaw("    ", out.size() > 4000 ? out.substr(0, 4000) : out);
    }

    if (output) {
        *output = std::move(out);
    }
    return code;
}

HANDLE Adb::Spawn(const std::wstring &args) const {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return nullptr;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = BuildCommandLine(m_exe, args);
    Log(L"adb (servidor) %ls", args.c_str());
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return nullptr;
    }
    CloseHandle(pi.hThread);

    // Registra a saída do servidor do celular no log
    std::thread([readPipe] {
        std::string pending;
        char buf[1024];
        DWORD n;
        while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n) {
            pending.append(buf, n);
            size_t nl = pending.rfind('\n');
            if (nl != std::string::npos) {
                LogRaw("    [celular] ", pending.substr(0, nl));
                pending.erase(0, nl + 1);
            }
        }
        LogRaw("    [celular] ", pending);
        CloseHandle(readPipe);
    }).detach();

    return pi.hProcess;
}

std::wstring Adb::FirstDevice(bool *unauthorized) const {
    if (unauthorized) {
        *unauthorized = false;
    }
    std::string out;
    if (Run(L"devices", &out, 15000) != 0) {
        return L"";
    }

    // Formato: "List of devices attached\n<serial>\t<estado>\n..."
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        std::string serial = line.substr(0, tab);
        std::string state = line.substr(tab + 1);
        if (state == "device") {
            return std::wstring(serial.begin(), serial.end());
        }
        if (state == "unauthorized" && unauthorized) {
            *unauthorized = true;
        }
    }
    return L"";
}
