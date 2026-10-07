#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "math/angles.h"
#include "math/project.h"

using namespace grw;
using Catch::Matchers::WithinAbs;

static Mat4 Identity() {
    Mat4 m;
    for (int i = 0; i < 4; ++i) m.m[i][i] = 1.0f;
    return m;
}

static Mat4 PerspectiveWEqualsZ() {
    Mat4 m = Identity();
    m.m[3][2] = 1.0f;  // w = z
    m.m[3][3] = 0.0f;
    return m;
}

TEST_CASE("WorldToScreen maps NDC to pixels") {
    const Viewport vp{1280.0f, 720.0f};
    Vec2 s;
    REQUIRE(WorldToScreen(Identity(), {0, 0, 0}, vp, s));
    CHECK_THAT(s.x, WithinAbs(640.0, 1e-3));
    CHECK_THAT(s.y, WithinAbs(360.0, 1e-3));
    REQUIRE(WorldToScreen(Identity(), {1, 1, 0}, vp, s));
    CHECK_THAT(s.x, WithinAbs(1280.0, 1e-3));
    CHECK_THAT(s.y, WithinAbs(0.0, 1e-3));  // NDC +y is the top of the screen
}

TEST_CASE("WorldToScreen divides by w and rejects points behind the camera") {
    const Viewport vp{1280.0f, 720.0f};
    Vec2 s;
    REQUIRE(WorldToScreen(PerspectiveWEqualsZ(), {0.5f, 0, 2}, vp, s));
    CHECK_THAT(s.x, WithinAbs(800.0, 1e-3));
    CHECK_FALSE(WorldToScreen(PerspectiveWEqualsZ(), {0, 0, -1}, vp, s));
    CHECK_FALSE(WorldToScreen(PerspectiveWEqualsZ(), {0, 0, 0}, vp, s));
}

TEST_CASE("OnScreen honours the margin") {
    const Viewport vp{100.0f, 100.0f};
    CHECK(OnScreen({50, 50}, vp, 0));
    CHECK_FALSE(OnScreen({-5, 50}, vp, 0));
    CHECK(OnScreen({-5, 50}, vp, 10));
}

TEST_CASE("Mat4::Transposed swaps rows and columns") {
    Mat4 m;
    m.m[0][3] = 7.0f;
    CHECK(m.Transposed().m[3][0] == 7.0f);
}

TEST_CASE("DirectionToAngles uses Z up and yaw = atan2(y, x)") {
    Angles a = DirectionToAngles({1, 0, 0});
    CHECK_THAT(a.yaw, WithinAbs(0.0, 1e-6));
    CHECK_THAT(a.pitch, WithinAbs(0.0, 1e-6));
    a = DirectionToAngles({0, 2, 0});
    CHECK_THAT(a.yaw, WithinAbs(kPi / 2, 1e-6));
    a = DirectionToAngles({1, 0, 1});
    CHECK_THAT(a.pitch, WithinAbs(kPi / 4, 1e-6));
}

TEST_CASE("AnglesToDirection round-trips") {
    const Angles in{DegToRad(-123.0f), DegToRad(17.0f)};
    const Angles out = DirectionToAngles(AnglesToDirection(in));
    CHECK_THAT(out.yaw, WithinAbs(in.yaw, 1e-5));
    CHECK_THAT(out.pitch, WithinAbs(in.pitch, 1e-5));
}

TEST_CASE("WrapPi and AngleDelta take the short way round") {
    CHECK_THAT(WrapPi(1.5f * kPi), WithinAbs(-0.5 * kPi, 1e-5));
    CHECK_THAT(WrapPi(-1.5f * kPi), WithinAbs(0.5 * kPi, 1e-5));
    const Angles d = AngleDelta({DegToRad(170), 0}, {DegToRad(-170), DegToRad(5)});
    CHECK_THAT(RadToDeg(d.yaw), WithinAbs(20.0, 1e-3));
    CHECK_THAT(RadToDeg(d.pitch), WithinAbs(5.0, 1e-3));
}

TEST_CASE("AngularDistance is the angle between the two directions") {
    CHECK_THAT(RadToDeg(AngularDistance({0, 0}, {0, DegToRad(10)})), WithinAbs(10.0, 1e-3));
    CHECK_THAT(RadToDeg(AngularDistance({0, 0}, {0, 0})), WithinAbs(0.0, 1e-2));
}
