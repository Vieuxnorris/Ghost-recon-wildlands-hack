#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include "aim/controller.h"
#include "aim/sway.h"

using namespace grw;
using Catch::Matchers::WithinAbs;

namespace {
const AimParams kParams{80.0f, 0.0f, DegToRad(10'000.0f)};
float Mag(const Angles& a) { return std::sqrt(a.yaw * a.yaw + a.pitch * a.pitch); }

// Simulates a static target: the view moves by each step, for duration_ms at the given fps.
float RemainingAfter(float fps, float duration_ms, const AimParams& p) {
    Angles err{DegToRad(4.0f), DegToRad(-2.0f)};
    const float dt = 1000.0f / fps;
    for (float t = 0; t + dt <= duration_ms + 1e-3f; t += dt) {
        const Angles s = AimStep(err, dt, p);
        err.yaw -= s.yaw;
        err.pitch -= s.pitch;
    }
    return Mag(err);
}
}  // namespace

TEST_CASE("AimStep follows 1 - exp(-dt/tau)") {
    const Angles s = AimStep({0, DegToRad(1.0f)}, 80.0f, kParams);
    CHECK_THAT(RadToDeg(s.pitch), WithinAbs(1.0 - std::exp(-1.0), 1e-4));
    CHECK(s.yaw == 0.0f);
}

TEST_CASE("AimStep converges the same way at 60 and 144 fps") {
    const float at60 = RemainingAfter(60.0f, 250.0f, kParams);
    const float at144 = RemainingAfter(144.0f, 250.0f, kParams);
    CHECK_THAT(RadToDeg(at60), WithinAbs(RadToDeg(at144), 0.02));
}

TEST_CASE("AimStep does nothing inside the dead zone, or with no time") {
    AimParams p = kParams;
    p.deadzone_rad = DegToRad(0.5f);
    CHECK(Mag(AimStep({DegToRad(0.3f), DegToRad(0.3f)}, 16.0f, p)) == 0.0f);
    CHECK(Mag(AimStep({DegToRad(0.5f), DegToRad(0.5f)}, 16.0f, p)) > 0.0f);
    CHECK(Mag(AimStep({DegToRad(5.0f), 0}, 0.0f, kParams)) == 0.0f);
}

TEST_CASE("AimStep caps the angular speed and keeps the direction") {
    AimParams p = kParams;
    p.max_speed_rad_s = DegToRad(360.0f);
    const Angles s = AimStep({DegToRad(60.0f), DegToRad(-80.0f)}, 1000.0f / 60.0f, p);
    CHECK_THAT(RadToDeg(Mag(s)), WithinAbs(6.0, 1e-3));
    CHECK_THAT(s.pitch / s.yaw, WithinAbs(-80.0 / 60.0, 1e-4));
}

TEST_CASE("AimStep never overshoots, even on a long frame") {
    const Angles err{DegToRad(2.0f), DegToRad(1.0f)};
    const Angles s = AimStep(err, 1000.0f, kParams);
    CHECK(Mag(s) <= Mag(err));
}

TEST_CASE("HeadDeadzoneRad is a fraction of the head's angular radius") {
    CHECK_THAT(HeadDeadzoneRad(0.12f, 50.0f, 0.3f), WithinAbs(0.3 * std::atan(0.12 / 50.0), 1e-7));
    CHECK(HeadDeadzoneRad(0.12f, 0.0f, 0.3f) == 0.0f);
}

TEST_CASE("TimeWindowAverage averages only the last window") {
    TimeWindowAverage avg(200.0);
    CHECK(avg.Mean(0.0).yaw == 0.0f);
    for (int i = 0; i < 20; ++i) avg.Add(i * 10.0, {i % 2 ? 0.1f : -0.1f, 0.5f});
    const Angles m = avg.Mean(190.0);
    CHECK_THAT(m.yaw, WithinAbs(0.0, 1e-6));
    CHECK_THAT(m.pitch, WithinAbs(0.5, 1e-6));
    avg.Add(400.0, {1.0f, 1.0f});
    CHECK_THAT(avg.Mean(400.0).yaw, WithinAbs(1.0, 1e-6));  // everything older than 200 ms is ignored
}

TEST_CASE("TimeWindowAverage keeps the newest samples when full") {
    TimeWindowAverage avg(1e9);
    for (int i = 0; i < 1000; ++i) avg.Add(i, {float(i), 0});
    CHECK_THAT(avg.Mean(1000.0).yaw, WithinAbs((744.0 + 999.0) / 2.0, 1e-3));  // last 256 samples
    avg.Reset();
    CHECK(avg.Mean(1000.0).yaw == 0.0f);
}
