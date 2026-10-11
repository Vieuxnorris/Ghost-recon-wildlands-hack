#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include "game/teleport.h"

using namespace grw;
using namespace grw::game;
using Catch::Matchers::WithinAbs;

namespace {

// A world of horizontal surfaces at the given heights; records every ray.
struct FakeWorld {
    std::vector<float> surfaces;
    std::vector<std::pair<float, float>> rays;  // from z, to z
    static bool Cast(void* ctx, const Vec3& from, const Vec3& to, float& hit_z) {
        auto* w = static_cast<FakeWorld*>(ctx);
        w->rays.push_back({from.z, to.z});
        float best = -1e9f;
        bool hit = false;
        for (float s : w->surfaces)
            if (s <= from.z && s >= to.z && s > best) best = s, hit = true;
        if (hit) hit_z = best;
        return hit;
    }
};

}  // namespace

TEST_CASE("FindGround casts down in chunks and returns the highest surface") {
    FakeWorld w{{1234.5f, 800.0f}};
    GroundQuery q{10, 20, 6000, -500, 200};
    float z = 0;
    REQUIRE(FindGround(q, &FakeWorld::Cast, &w, z));
    CHECK_THAT(z, WithinAbs(1234.5, 1e-3));
    for (const auto& r : w.rays) CHECK(r.first - r.second <= 200.0f + 1e-3f);  // no ray longer than a chunk
    CHECK(w.rays.front().first == 6000.0f);
}

TEST_CASE("FindGround skips hits inside the excluded band (the player's own body) and goes on below") {
    FakeWorld w{{1302.0f, 1150.0f}};  // the player's body at 1300-1302, the ground below
    GroundQuery q{0, 0, 6000, -500, 200};
    q.skip_lo = 1299.5f;
    q.skip_hi = 1302.5f;
    float z = 0;
    REQUIRE(FindGround(q, &FakeWorld::Cast, &w, z));
    CHECK_THAT(z, WithinAbs(1150.0, 1e-3));
}

TEST_CASE("FindGround fails when nothing is loaded below") {
    FakeWorld w{};
    float z = 0;
    CHECK_FALSE(FindGround(GroundQuery{0, 0, 6000, -500, 200}, &FakeWorld::Cast, &w, z));
    CHECK(w.rays.size() == 33);  // 6500 m in chunks of 200 m
}

TEST_CASE("PlausibleWaypoint rejects garbage and the origin") {
    CHECK(PlausibleWaypoint({-3084.5f, -1832.8f, 1013.4f}));
    CHECK_FALSE(PlausibleWaypoint({0.0f, 0.0f, 0.0f}));
    CHECK_FALSE(PlausibleWaypoint({NAN, 1.0f, 1.0f}));
    CHECK_FALSE(PlausibleWaypoint({1e7f, 1.0f, 1.0f}));
}

TEST_CASE("Teleporter places the player at once when the ground is already known") {
    Teleporter t;
    TeleportParams p;
    t.Begin({0, 0, 1000}, {500, 0, 1100}, 0.0, p);
    const TpStep s = t.Update(10.0, true, 1100.0f);
    CHECK(s.write);
    CHECK_THAT(s.pos.x, WithinAbs(500.0, 1e-3));
    CHECK_THAT(s.pos.z, WithinAbs(1100.0 + p.above_m, 1e-3));
    CHECK(t.Phase() == TpPhase::Placing);
    // written place_ms long, then released
    int writes = 1;
    double last_write = 10.0;
    for (int i = 0; i < 100 && t.Active(); ++i) {
        const double now = 20.0 + 10.0 * i;
        if (t.Update(now, true, 1100.0f).write) ++writes, last_write = now;
    }
    CHECK(writes >= p.place_frames);
    CHECK(last_write >= 10.0 + p.place_ms - 10.0);
    CHECK(last_write < 10.0 + p.place_ms + 10.0);
    CHECK(t.Phase() == TpPhase::Done);
    CHECK_FALSE(t.Active());
}

TEST_CASE("Teleporter writes the final position long enough for the game's physics to take it") {
    // MODLOG 2026-10-10: written 3 frames (~30 ms) after a hold, the player was carried back up to the hold
    // position within 1.6 s, once in 4 short holds; the hold itself, written every frame, was always followed.
    Teleporter t;
    TeleportParams p;
    t.Begin({0, 0, 1000}, {2600, 0, 1000}, 0.0, p);
    t.Update(0.0, false, 0.0f);
    t.Update(107.0, true, 1180.0f);  // the ground at the second probe
    REQUIRE(t.Phase() == TpPhase::Placing);
    double last_write = 107.0;
    for (double now = 107.0 + 16.7; now < 2000.0 && t.Active(); now += 16.7) {  // 60 fps
        const TpStep s = t.Update(now, true, 1180.0f);
        if (!s.write) continue;
        CHECK_THAT(s.pos.z, WithinAbs(1180.0 + p.above_m, 1e-3));
        last_write = now;
    }
    CHECK(last_write >= 107.0 + p.place_ms - 16.7);
    CHECK(last_write < 107.0 + p.place_ms + 16.7);
    CHECK(t.Phase() == TpPhase::Done);
}

TEST_CASE("Teleporter writes the final position at least place_frames frames, even after a long frame") {
    Teleporter t;
    TeleportParams p;
    t.Begin({0, 0, 1000}, {500, 0, 1100}, 0.0, p);
    t.Update(0.0, true, 1100.0f);
    int writes = 1;
    for (int i = 0; i < 10 && t.Active(); ++i) writes += t.Update(1000.0 * (i + 1), true, 1100.0f).write;
    CHECK(writes == p.place_frames);
}

TEST_CASE("Teleporter holds above the destination until the ground loads, then places") {
    Teleporter t;
    TeleportParams p;
    t.Begin({0, 0, 1000}, {2600, 0, 1000}, 0.0, p);
    TpStep s = t.Update(10.0, false, 0.0f);
    CHECK(s.write);
    CHECK(t.Phase() == TpPhase::Holding);
    CHECK_THAT(s.pos.x, WithinAbs(2600.0, 1e-3));
    CHECK_THAT(s.pos.z, WithinAbs(1000.0 + p.hold_above_m, 1e-3));  // above max(start z, waypoint z)
    CHECK_THAT(t.HoldZ(), WithinAbs(1000.0 + p.hold_above_m, 1e-3));
    s = t.Update(2000.0, false, 0.0f);
    CHECK(s.write);
    CHECK(t.Phase() == TpPhase::Holding);
    s = t.Update(2500.0, true, 1180.0f);
    CHECK(t.Phase() == TpPhase::Placing);
    CHECK_THAT(s.pos.z, WithinAbs(1180.0 + p.above_m, 1e-3));
    CHECK(t.GroundMs() - t.BeginMs() == 2500.0);
    CHECK_THAT(t.Waypoint().z, WithinAbs(1000.0, 1e-3));  // the waypoint as read, not the position written
    CHECK_THAT(t.Target().z, WithinAbs(1180.0 + p.above_m, 1e-3));
}

TEST_CASE("Teleporter goes back to the start when the ground never loads") {
    Teleporter t;
    TeleportParams p;
    t.Begin({10, 20, 1000}, {2600, 0, 1000}, 0.0, p);
    t.Update(10.0, false, 0.0f);
    const TpStep s = t.Update(p.timeout_ms + 1.0, false, 0.0f);
    CHECK(s.write);
    CHECK(t.Phase() == TpPhase::Returning);
    CHECK_THAT(s.pos.x, WithinAbs(10.0, 1e-3));
    CHECK_THAT(s.pos.y, WithinAbs(20.0, 1e-3));
    CHECK_THAT(s.pos.z, WithinAbs(1000.0 + p.above_m, 1e-3));
    for (int i = 0; i < 100 && t.Active(); ++i) t.Update(p.timeout_ms + 20.0 + 10.0 * i, false, 0.0f);
    CHECK(t.Phase() == TpPhase::Failed);
}

TEST_CASE("Teleporter asks for a ground probe at most every probe interval while holding") {
    Teleporter t;
    TeleportParams p;
    t.Begin({0, 0, 1000}, {2600, 0, 1000}, 0.0, p);
    CHECK(t.ProbeDue(0.0));
    t.Update(0.0, false, 0.0f);
    CHECK_FALSE(t.ProbeDue(p.probe_interval_ms * 0.5));
    CHECK(t.ProbeDue(p.probe_interval_ms + 1.0));
}

TEST_CASE("TpQueue goes to the waypoint at once when the map is closed") {
    TpQueue q;
    CHECK(q.Update(true, false, {}) == TpAct::Waypoint);
    CHECK_FALSE(q.Pending());
}

TEST_CASE("TpQueue keeps the map cursor while the map is open and starts when the map closes") {
    TpQueue q;
    CHECK(q.Update(true, true, {-982.9f, -2936.7f, 1225.2f}) == TpAct::Queued);
    CHECK(q.Pending());
    // the game is paused while the map is open: nothing starts, and moving the cursor after F5 changes nothing
    CHECK(q.Update(false, true, {-546.9f, -2821.8f, 0.0f}) == TpAct::None);
    CHECK(q.Update(false, false, {}) == TpAct::Cursor);
    CHECK_THAT(q.Target().x, WithinAbs(-982.9, 1e-3));
    CHECK_THAT(q.Target().y, WithinAbs(-2936.7, 1e-3));
    CHECK_THAT(q.Target().z, WithinAbs(1225.2, 1e-3));
    CHECK_FALSE(q.Pending());
    CHECK(q.Update(false, false, {}) == TpAct::None);
}

TEST_CASE("TpQueue: a later F5 on the map replaces the cursor kept") {
    TpQueue q;
    CHECK(q.Update(true, true, {100.0f, 200.0f, 0.0f}) == TpAct::Queued);
    CHECK(q.Update(true, true, {300.0f, 400.0f, 0.0f}) == TpAct::Queued);
    CHECK(q.Update(false, false, {}) == TpAct::Cursor);
    CHECK_THAT(q.Target().x, WithinAbs(300.0, 1e-3));
    CHECK_THAT(q.Target().y, WithinAbs(400.0, 1e-3));
}

TEST_CASE("TpQueue does nothing without a request, map open or not") {
    TpQueue q;
    CHECK(q.Update(false, true, {100.0f, 200.0f, 0.0f}) == TpAct::None);
    CHECK(q.Update(false, false, {}) == TpAct::None);
    CHECK_FALSE(q.Pending());
}
