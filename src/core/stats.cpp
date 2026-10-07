#include "core/stats.h"
#include <algorithm>
#include <cmath>
#include "core/clock.h"

namespace grw {

const char* StageName(Stage s) {
    static constexpr const char* kNames[kStageCount] = {"camera", "entities", "bones", "aim", "draw", "total"};
    return size_t(s) < kStageCount ? kNames[size_t(s)] : "?";
}

void StageStats::Add(Stage s, float cpu_ms, float wall_ms) {
    Series& series = series_[size_t(s)];
    if (series.n >= kCapacity) {
        ++series.dropped;
        return;
    }
    series.cpu[series.n] = cpu_ms;
    series.wall[series.n] = wall_ms;
    ++series.n;
}

namespace {
// Nearest-rank percentile: the smallest value with at least p*n samples at or below it.
float Rank(float* v, uint32_t n, float p) {
    if (n == 0) return 0.0f;
    uint32_t k = uint32_t(std::ceil(p * float(n)));
    k = std::clamp<uint32_t>(k, 1, n) - 1;
    std::nth_element(v, v + k, v + n);
    return v[k];
}
}  // namespace

void StageStats::Flush(Percentiles (&out)[kStageCount]) {
    for (size_t i = 0; i < kStageCount; ++i) {
        Series& s = series_[i];
        out[i] = {Rank(s.cpu, s.n, 0.50f), Rank(s.cpu, s.n, 0.99f),
                  Rank(s.wall, s.n, 0.50f), Rank(s.wall, s.n, 0.99f), s.n, s.dropped};
        s.n = 0;
        s.dropped = 0;
    }
}

StageSpan::StageSpan() : cpu0(Clock::ThreadCpuMs()), wall0(Clock::NowMs()) {}

void StageSpan::End(StageStats& stats, Stage s) const {
    stats.Add(s, float(Clock::ThreadCpuMs() - cpu0), float(Clock::NowMs() - wall0));
}

}  // namespace grw
