#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <windows.h>
#include "core/clock.h"
#include "core/stats.h"

using namespace grw;

TEST_CASE("Clock calibrates a plausible TSC rate") {
    Clock::Calibrate();
    CHECK(Clock::TscPerMs() > 500'000.0);     // > 0.5 GHz
    CHECK(Clock::TscPerMs() < 10'000'000.0);  // < 10 GHz
}

TEST_CASE("ThreadCpuMs counts busy time but not sleep") {
    Clock::Calibrate();
    const double cpu0 = Clock::ThreadCpuMs();
    const double wall0 = Clock::NowMs();
    while (Clock::NowMs() - wall0 < 50.0) {}
    const double busy = Clock::ThreadCpuMs() - cpu0;
    CHECK(busy > 35.0);
    CHECK(busy < 70.0);

    const double cpu1 = Clock::ThreadCpuMs();
    Sleep(50);
    CHECK(Clock::ThreadCpuMs() - cpu1 < 5.0);
}

TEST_CASE("StageStats computes nearest-rank percentiles and resets") {
    static StageStats stats;  // large: keep it off the stack
    for (int i = 1; i <= 100; ++i) stats.Add(Stage::Total, float(i), float(i) * 2.0f);
    Percentiles p[kStageCount];
    stats.Flush(p);
    const auto& t = p[size_t(Stage::Total)];
    CHECK(t.n == 100);
    CHECK(t.p50_cpu == 50.0f);
    CHECK(t.p99_cpu == 99.0f);
    CHECK(t.p50_wall == 100.0f);
    CHECK(p[size_t(Stage::Aim)].n == 0);
    CHECK(p[size_t(Stage::Aim)].p99_cpu == 0.0f);

    stats.Flush(p);
    CHECK(p[size_t(Stage::Total)].n == 0);
}

TEST_CASE("StageStats counts samples beyond capacity as dropped") {
    static StageStats stats;
    for (uint32_t i = 0; i < StageStats::kCapacity + 10; ++i) stats.Add(Stage::Draw, 1.0f, 1.0f);
    Percentiles p[kStageCount];
    stats.Flush(p);
    CHECK(p[size_t(Stage::Draw)].n == StageStats::kCapacity);
    CHECK(p[size_t(Stage::Draw)].dropped == 10);
}

TEST_CASE("StageName covers every stage") {
    CHECK(std::string_view(StageName(Stage::Camera)) == "camera");
    CHECK(std::string_view(StageName(Stage::Total)) == "total");
}
