#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "scan/ray_filter.h"

using namespace grw;
using Catch::Matchers::WithinAbs;

TEST_CASE("RayFilter locates points along, right of and below a horizontal shot") {
    const auto f = scan::RayFilter::Make({100, 200, 50}, {0, 1, 0}, 5, 1000, 3, -50, 5);
    // heading +Y, right is +X, up is +Z
    const auto w = f.Locate({101, 300, 48});
    CHECK_THAT(w.along, WithinAbs(100, 1e-3));
    CHECK_THAT(w.lateral, WithinAbs(1, 1e-3));
    CHECK_THAT(w.vertical, WithinAbs(-2, 1e-3));
    scan::RayFilter::Where out;
    const float below[3] = {100, 400, 40};   // 10 m drop at 200 m: accepted
    const float behind[3] = {100, 190, 50};  // behind the shooter
    const float aside[3] = {110, 400, 50};   // 10 m to the right
    const float above[3] = {100, 400, 60};   // 10 m above the line
    CHECK(f.Test(below, out));
    CHECK_THAT(out.vertical, WithinAbs(-10, 1e-3));
    CHECK_FALSE(f.Test(behind, out));
    CHECK_FALSE(f.Test(aside, out));
    CHECK_FALSE(f.Test(above, out));
}

TEST_CASE("RayFilter handles a shot into the sky and normalises the direction") {
    const auto f = scan::RayFilter::Make({0, 0, 0}, {0, 3, 4}, 5, 1000, 3, -100, 5);  // 53 deg up
    CHECK_THAT(f.dir.Length(), WithinAbs(1, 1e-5));
    scan::RayFilter::Where out;
    const float on_line[3] = {0, 300, 400};  // 500 m along
    CHECK(f.Test(on_line, out));
    CHECK_THAT(out.along, WithinAbs(500, 1e-2));
    CHECK_THAT(out.vertical, WithinAbs(0, 1e-2));
    const auto v = scan::RayFilter::Make({0, 0, 0}, {0, 0, 1}, 5, 1000, 3, -100, 5);  // straight up
    CHECK(v.right.Length() > 0.99f);
    const float up[3] = {0, 0, 100};
    CHECK(v.Test(up, out));
}
