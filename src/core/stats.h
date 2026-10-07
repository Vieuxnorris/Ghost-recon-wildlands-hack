#pragma once
#include <cstddef>
#include <cstdint>

namespace grw {

// Chain: the call into the original Present (other overlays' detours, then dxgi).
// Interval: wall time between two frames seen by the hook (cpu is 0).
enum class Stage : uint8_t { Camera, Entities, Bones, Aim, Draw, Total, Chain, Interval, Count };
inline constexpr size_t kStageCount = size_t(Stage::Count);
const char* StageName(Stage s);

struct Percentiles {
    float p50_cpu = 0, p99_cpu = 0, p50_wall = 0, p99_wall = 0;
    uint32_t n = 0;
    uint32_t dropped = 0;
};

// Fixed-capacity sample window per stage; Flush() sorts in place (no allocation) and resets.
class StageStats {
public:
    static constexpr uint32_t kCapacity = 4096;
    void Add(Stage s, float cpu_ms, float wall_ms);
    void Flush(Percentiles (&out)[kStageCount]);

private:
    struct Series {
        float cpu[kCapacity];
        float wall[kCapacity];
        uint32_t n = 0;
        uint32_t dropped = 0;
    };
    Series series_[kStageCount];
};

struct StageSpan {
    double cpu0, wall0;
    StageSpan();
    void End(StageStats& stats, Stage s) const;
};

}  // namespace grw
