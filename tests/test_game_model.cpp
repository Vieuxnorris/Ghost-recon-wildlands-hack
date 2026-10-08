#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include "game/model.h"

using namespace grw;
using namespace grw::game;
using Catch::Matchers::WithinAbs;

TEST_CASE("Classify matches the in-game labels") {
    CHECK(Classify(0xd09) == Category::Player);
    CHECK(Classify(0xd0a) == Category::Squad);
    CHECK(Classify(0x40d0a) == Category::Squad);
    CHECK(Classify(0x60d0a) == Category::Squad);  // in a vehicle
    CHECK(Classify(0x912) == Category::Civilian);
    CHECK(Classify(0x20912) == Category::Civilian);
    CHECK(Classify(0x111a) == Category::SantaBlanca);
    CHECK(Classify(0x2111a) == Category::SantaBlanca);
    CHECK(Classify(0x191a) == Category::Unidad);
    CHECK(Classify(0x1d1a) == Category::Ally);
    CHECK(Classify(0x511a) == Category::Dead);
    CHECK(Classify(0x4912) == Category::Dead);
}

static GameMatrix Facing(float yaw_rad, Vec3 pos) {
    const float c = std::cos(yaw_rad), s = std::sin(yaw_rad);
    GameMatrix g{};
    const float rows[4][4] = {{s, -c, 0, 0}, {c, s, 0, 0}, {0, 0, 1, 0}, {pos.x, pos.y, pos.z, 1}};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) g.m[r][k] = rows[r][k];
    return g;
}

TEST_CASE("GameMatrix maps model space with +y forward and +z up") {
    const GameMatrix g = Facing(0.0f, {10, 20, 30});  // forward = world +x, right = world -y
    REQUIRE(g.Plausible());
    const Vec3 w = g.ToWorld({0.5f, 2.0f, 1.6f});
    CHECK_THAT(w.x, WithinAbs(12.0, 1e-5));
    CHECK_THAT(w.y, WithinAbs(19.5, 1e-5));
    CHECK_THAT(w.z, WithinAbs(31.6, 1e-5));
    GameMatrix bad = g;
    bad.m[0][0] = 2.0f;
    CHECK_FALSE(bad.Plausible());
}

static void SetBone(std::vector<float>& b, size_t i, Vec3 p) {
    b[i * kBoneFloats + 0] = p.x;
    b[i * kBoneFloats + 1] = p.y;
    b[i * kBoneFloats + 2] = p.z;
    b[i * kBoneFloats + 7] = 1.0f;  // quaternion w
}

TEST_CASE("HeadFromBones averages the head cluster and ignores a raised hand") {
    std::vector<float> b(20 * kBoneFloats, 0.0f);
    const float spine[5] = {0.96f, 0.96f, 1.03f, 1.20f, 1.38f};
    for (size_t i = 0; i < 5; ++i) SetBone(b, i, {0, 0, spine[i]});
    SetBone(b, 5, {0.0f, 0.10f, 1.60f});   // face
    SetBone(b, 6, {0.0f, 0.10f, 1.70f});   // face
    SetBone(b, 7, {0.0f, 0.00f, 1.78f});   // crown
    SetBone(b, 8, {0.35f, 0.30f, 1.95f});  // hand above the head: 0.69 m from the neck
    SetBone(b, 9, {0.20f, 0.00f, 1.10f});  // elbow, below the neck
    Vec3 head, crown;
    REQUIRE(HeadFromBones(b.data(), 20, head, crown));
    CHECK_THAT(head.z, WithinAbs((1.60 + 1.70 + 1.78) / 3.0, 1e-4));
    CHECK_THAT(head.y, WithinAbs(0.2 / 3.0, 1e-4));
    CHECK_THAT(crown.z, WithinAbs(1.78, 1e-5));
}

TEST_CASE("HeadFromBones fails without head bones") {
    std::vector<float> b(8 * kBoneFloats, 0.0f);
    Vec3 head, crown;
    CHECK_FALSE(HeadFromBones(b.data(), 8, head, crown));
    CHECK_FALSE(HeadFromBones(b.data(), 3, head, crown));
}

TEST_CASE("Project matches the screen scale measured in game") {
    // In game (1920x1080, FOV 46.70 deg): 1 m across at 8 m spans ~156 px; a point straight ahead is centred.
    CameraView cam{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 0.814994f};
    Vec2 s;
    float depth = 0;
    REQUIRE(Project(cam, {0, 10, 0}, 1920, 1080, s, depth));
    CHECK_THAT(s.x, WithinAbs(960.0, 1e-3));
    CHECK_THAT(s.y, WithinAbs(540.0, 1e-3));
    CHECK_THAT(depth, WithinAbs(10.0, 1e-5));
    Vec2 a, b;
    REQUIRE(Project(cam, {0, 8, 0}, 1920, 1080, a, depth));
    REQUIRE(Project(cam, {1, 8, 0}, 1920, 1080, b, depth));
    CHECK_THAT(b.x - a.x, WithinAbs(156.4, 0.5));
    REQUIRE(Project(cam, {0, 8, 1}, 1920, 1080, b, depth));
    CHECK(b.y < a.y);  // world up is screen up
    CHECK_FALSE(Project(cam, {0, -5, 0}, 1920, 1080, s, depth));
}
