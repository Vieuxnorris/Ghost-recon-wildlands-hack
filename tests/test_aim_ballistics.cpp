#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "aim/ballistics.h"

using namespace grw;
using Catch::Matchers::WithinAbs;

TEST_CASE("BallisticAimPoint is the identity when ballistics are off") {
    const Vec3 t{100, 50, 3};
    const Vec3 p = BallisticAimPoint({0, 0, 0}, t, {5, 0, 0}, Ballistics{});
    CHECK(p.x == t.x);
    CHECK(p.y == t.y);
    CHECK(p.z == t.z);
}

TEST_CASE("BallisticAimPoint raises by the gravity drop over the flight time") {
    Ballistics b;
    b.speed = 800.0f;
    float t = 0;
    const Vec3 p = BallisticAimPoint({0, 0, 0}, {200, 0, 0}, {0, 0, 0}, b, &t);
    CHECK_THAT(t, WithinAbs(0.25, 1e-5));
    CHECK_THAT(p.z, WithinAbs(0.5 * 9.81 * 0.25 * 0.25, 1e-4));  // 0.307 m
    CHECK_THAT(p.x, WithinAbs(200.0, 1e-4));
}

TEST_CASE("At the zero distance no correction is needed; closer, aim slightly low") {
    Ballistics b;
    b.speed = 800.0f;
    b.zero_m = 200.0f;
    const Vec3 at_zero = BallisticAimPoint({0, 0, 0}, {200, 0, 0}, {0, 0, 0}, b);
    CHECK_THAT(at_zero.z, WithinAbs(0.0, 1e-4));
    const Vec3 closer = BallisticAimPoint({0, 0, 0}, {100, 0, 0}, {0, 0, 0}, b);
    CHECK(closer.z < 0.0f);  // the bullet is still above the line of sight at 100 m
    const Vec3 farther = BallisticAimPoint({0, 0, 0}, {400, 0, 0}, {0, 0, 0}, b);
    CHECK(farther.z > 0.0f);
}

TEST_CASE("BallisticAimPoint leads a moving target by the flight time") {
    Ballistics b;
    b.speed = 500.0f;
    b.gravity = 0.0f;
    float t = 0;
    const Vec3 p = BallisticAimPoint({0, 0, 0}, {100, 0, 0}, {0, 5, 0}, b, &t);
    CHECK_THAT(t, WithinAbs(0.2, 1e-3));
    CHECK_THAT(p.y, WithinAbs(5.0 * t, 1e-4));
}
