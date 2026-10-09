#include "telemetry/telemetry.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "core/clock.h"

namespace grw {

bool Telemetry::Start(const TelemetryOptions& opt) {
    if (thread_.joinable()) return false;
    opt_ = opt;
    std::error_code ec;
    std::filesystem::create_directories(opt_.dir, ec);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[96];
    std::swprintf(name, 96, L"telemetry_%04u%02u%02u-%02u%02u%02u_%lu.jsonl", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    path_ = (std::filesystem::path(opt_.dir) / name).wstring();
    file_ = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;

    // Non-blocking byte pipe, one client. Failure (name taken) only disables the pipe.
    pipe_ = CreateNamedPipeW(opt_.pipe_name.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_NOWAIT, 1,
                             1 << 20, 0, 0, nullptr);

    start_ms_ = Clock::NowMs();
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
    return true;
}

void Telemetry::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    if (pipe_ != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(pipe_);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    pipe_connected_ = false;
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
}

double Telemetry::SinceStartMs() const { return Clock::NowMs() - start_ms_; }

void Telemetry::Run() {
    while (!stop_.load()) {
        ServicePipe();
        if (opt_.on_tick) opt_.on_tick();
        Drain();
        Sleep(opt_.interval_ms);
    }
    Drain();
}

void Telemetry::Drain() {
    Line line;
    for (auto& ring : rings_) {
        while (ring.Pop(line)) {
            if (buf_len_ + line.len > sizeof buf_) Flush();
            std::memcpy(buf_ + buf_len_, line.text, line.len);
            buf_len_ += line.len;
        }
    }
    Flush();
}

void Telemetry::ServicePipe() {
    if (pipe_ == INVALID_HANDLE_VALUE || pipe_connected_) return;
    if (!ConnectNamedPipe(pipe_, nullptr)) {
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) pipe_connected_ = true;
        else if (err == ERROR_NO_DATA) DisconnectNamedPipe(pipe_);  // a client came and left
    }
}

void Telemetry::Flush() {
    if (buf_len_ == 0) return;
    DWORD written = 0;
    WriteFile(file_, buf_, DWORD(buf_len_), &written, nullptr);
    if (pipe_connected_) {
        // A client that can't take the whole chunk is too slow: drop it rather than split lines.
        if (!WriteFile(pipe_, buf_, DWORD(buf_len_), &written, nullptr) || written != buf_len_) {
            DisconnectNamedPipe(pipe_);
            pipe_connected_ = false;
        }
    }
    buf_len_ = 0;
}

}  // namespace grw
