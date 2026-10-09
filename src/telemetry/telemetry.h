#pragma once
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include "telemetry/json_line.h"
#include "telemetry/spsc_ring.h"

namespace grw {

// One producer thread per channel. Writer = the on_tick callback (it runs on the writer thread).
enum class Channel : uint8_t { Render, Worker, Control, Writer, Probe, Los, Count };
inline constexpr size_t kChannelCount = size_t(Channel::Count);

struct TelemetryOptions {
    std::wstring dir;
    std::wstring pipe_name = L"\\\\.\\pipe\\grw-telemetry";
    DWORD interval_ms = 50;
    std::function<void()> on_tick;
};

// Rings -> writer thread -> <dir>/telemetry_<date>_<pid>.jsonl + named pipe. ~2 MB: allocate on the heap.
class Telemetry {
public:
    Telemetry() = default;
    ~Telemetry() { Stop(); }
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;

    bool Start(const TelemetryOptions& opt);
    void Stop();

    bool Push(Channel ch, const Line& line) { return rings_[size_t(ch)].Push(line); }
    double SinceStartMs() const;
    uint64_t Dropped(Channel ch) const { return rings_[size_t(ch)].Dropped(); }
    bool PipeConnected() const { return pipe_connected_.load(std::memory_order_relaxed); }
    const std::wstring& FilePath() const { return path_; }

private:
    void Run();
    void Drain();
    void ServicePipe();
    void Flush();

    static constexpr size_t kRingSize = 1024;
    SpscRing<Line, kRingSize> rings_[kChannelCount];
    TelemetryOptions opt_;
    std::wstring path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::atomic<bool> pipe_connected_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    double start_ms_ = 0;
    char buf_[64 * 1024];
    size_t buf_len_ = 0;
};

}  // namespace grw
