#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>
#include "aim/recoil.h"
#include "aim/target.h"
#include "math/angles.h"

using namespace grw;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kDt = 1000.0 / 90.0;
float D(float deg) { return DegToRad(deg); }
}  // namespace

TEST_CASE("RecoilTracker follows a steady stance offset without holding") {
    RecoilTracker r;
    float off = 0;
    for (int i = 0; i < 200; ++i) off = r.Update(i * kDt, D(-11.0f), 0.0f, 0.0f);
    CHECK_THAT(RadToDeg(off), WithinAbs(-11.0, 1e-3));
    CHECK_FALSE(r.Holding());
}

TEST_CASE("RecoilTracker ignores a stance offset moving 0.6 degrees per picture") {
    RecoilTracker r;
    bool held = false;
    for (int i = 0; i < 200; ++i) {
        const int phase = i % 8;
        const float tri = float(phase < 4 ? phase : 8 - phase);  // steps of exactly 0.6 per picture
        const float cam = D(-11.0f - 1.2f + 0.6f * tri);
        r.Update(i * kDt, cam, 0.0f, 0.0f);
        held = held || r.Holding();
    }
    CHECK_FALSE(held);
}

TEST_CASE("RecoilTracker holds the pre-shot offset during a kick, then releases") {
    RecoilTracker r;
    int i = 0;
    for (; i < 50; ++i) r.Update(i * kDt, D(-2.0f), 0.0f, 0.0f);
    // Shot: camera pitch jumps up 3 degrees in one picture, then returns over ~0.3 s.
    float kick = 3.0f;
    float off = r.Update(i * kDt, D(-2.0f + kick), 0.0f, 0.0f);
    ++i;
    CHECK(r.Holding());
    CHECK_THAT(RadToDeg(off), WithinAbs(-2.0, 1e-3));
    int frames_held = 0;
    while (r.Holding() && frames_held < 100) {
        kick *= 0.8f;
        off = r.Update(i * kDt, D(-2.0f + kick), 0.0f, 0.0f);
        ++i;
        ++frames_held;
        if (r.Holding()) CHECK_THAT(RadToDeg(off), WithinAbs(-2.0, 1e-3));
    }
    CHECK_FALSE(r.Holding());
    CHECK(frames_held < 36);  // released when the camera is back, before the 0.4 s cap
}

TEST_CASE("RecoilTracker does not mistake our own correction for a kick") {
    RecoilTracker r;
    for (int i = 0; i < 10; ++i) r.Update(i * kDt, 0.0f, 0.0f, 0.0f);
    r.Update(10 * kDt, D(2.0f), D(2.0f), D(2.0f));
    CHECK_FALSE(r.Holding());
}

TEST_CASE("RecoilTracker gives up holding after 0.4 s and follows again") {
    RecoilTracker r;
    int i = 0;
    for (; i < 20; ++i) r.Update(i * kDt, 0.0f, 0.0f, 0.0f);
    r.Update(i * kDt, D(3.0f), 0.0f, 0.0f);
    REQUIRE(r.Holding());
    const double start = i * kDt;
    while (r.Holding() && i < 200) {
        ++i;
        r.Update(i * kDt, D(3.0f), 0.0f, 0.0f);
    }
    CHECK(i * kDt - start <= 400.0 + kDt);
    float off = 0;
    for (int k = 0; k < 300; ++k) off = r.Update((++i) * kDt, D(3.0f), 0.0f, 0.0f);
    CHECK_THAT(RadToDeg(off), WithinAbs(3.0, 0.05));
}

namespace {
const TargetParams kTp{DegToRad(8.0f), 150.0f, 150.0f};
Candidate Enemy(uint32_t id, float angle_deg, bool visible = true) {
    return {id, true, true, visible, 10.0, DegToRad(angle_deg)};
}
}  // namespace

TEST_CASE("TargetSelector picks the closest visible enemy inside the cone") {
    TargetSelector s;
    std::vector<Candidate> c = {
        {1, false, true, true, 10.0, D(1.0f)},  // civilian
        {2, true, false, true, 10.0, D(1.0f)},  // dead
        Enemy(3, 1.5f, false),                  // hidden
        {4, true, true, true, 400.0, D(1.5f)},  // stale line-of-sight verdict
        Enemy(5, 12.0f),                        // outside the cone
        Enemy(6, 5.0f),
        Enemy(7, 3.0f),
    };
    CHECK(s.Update(0, true, c.data(), c.size(), kTp) == 7);
    CHECK(s.Update(0, true, nullptr, 0, kTp) == 0);
}

TEST_CASE("TargetSelector clears on key release") {
    TargetSelector s;
    std::vector<Candidate> c = {Enemy(7, 3.0f)};
    REQUIRE(s.Update(0, true, c.data(), c.size(), kTp) == 7);
    CHECK(s.Update(10, false, c.data(), c.size(), kTp) == 0);
    CHECK(s.Current() == 0);
}

TEST_CASE("TargetSelector is sticky, tolerates short occlusion and switches on death") {
    TargetSelector s;
    std::vector<Candidate> c = {Enemy(1, 4.0f), Enemy(2, 6.0f)};
    REQUIRE(s.Update(0, true, c.data(), c.size(), kTp) == 1);

    c[1].angle_rad = D(0.5f);  // another enemy is now closer: keep the current one
    CHECK(s.Update(10, true, c.data(), c.size(), kTp) == 1);

    c[0].los_visible = false;  // hidden for 100 ms: keep
    CHECK(s.Update(110, true, c.data(), c.size(), kTp) == 1);
    CHECK(s.Update(170, true, c.data(), c.size(), kTp) == 2);  // hidden 160 ms: drop, pick the next

    c[1].alive = false;
    c[0].los_visible = true;
    CHECK(s.Update(180, true, c.data(), c.size(), kTp) == 1);
}
