#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "aim/trajectory.h"

using namespace grw;
using Catch::Matchers::WithinAbs;

TEST_CASE("TrajectoryTable interpolates between measured metres and starts at the muzzle") {
    TrajectoryTable tab;
    // a few steps of the R4 (live/74): straight first segment, then the drop grows
    tab.Add(3.1f, 0.0f, 0.010f);
    tab.Add(41.7f, 0.0f, 0.173f);
    tab.Add(44.0f, 0.01f, 0.182f);
    tab.Add(46.8f, 0.03f, 0.193f);
    float drop = -1, t = -1;
    REQUIRE(tab.Lookup(1.5f, drop, t));  // between the muzzle (0, 0) and 3 m
    CHECK_THAT(drop, WithinAbs(0.0, 1e-6));
    CHECK_THAT(t, WithinAbs(0.005, 1e-4));
    REQUIRE(tab.Lookup(45.0f, drop, t));  // between 44 and 47
    CHECK_THAT(drop, WithinAbs(0.01 + (0.03 - 0.01) * (1.0 / 3.0), 1e-5));
    CHECK_FALSE(tab.Lookup(60.0f, drop, t));  // beyond the measured range
    CHECK_FALSE(tab.Lookup(20.0f, drop, t));  // a hole wider than the steps
}

TEST_CASE("TrajectoryTable round-trips through text") {
    TrajectoryTable a;
    a.Add(10, 0.0f, 0.04f);
    a.Add(100, 0.5f, 0.42f);
    TrajectoryTable b;
    REQUIRE(b.Parse(a.Serialize()));
    float drop, t;
    REQUIRE(b.Lookup(100, drop, t));
    CHECK_THAT(drop, WithinAbs(0.5, 1e-4));
    CHECK_THAT(t, WithinAbs(0.42, 1e-4));
    CHECK(b.MaxAlong() == 100.0f);
    CHECK_FALSE(b.Parse("10 0.1"));  // truncated: untouched
    CHECK(b.MaxAlong() == 100.0f);
}

TEST_CASE("BulletKey tells weapon setups apart") {
    const BulletKey r4{300, 308.35f, 41.74f}, scoped{600, 593.37f, 71.4f};
    CHECK(r4 == BulletKey{300, 308.35f, 41.74f});
    CHECK_FALSE(r4 == scoped);
    CHECK_FALSE(BulletKey{}.Valid());
}

TEST_CASE("TrajectoryBook links each weapon to the table of its latest bullets and persists") {
    const WeaponSig r4{889, 77}, pistol{200.66f, 40};
    const BulletKey suppressed{300, 308.35f, 41.74f}, bare{533, 511.22f, 97.5f};
    TrajectoryBook book;
    book.Link(r4, suppressed);
    book.Add(suppressed, 100, 0.5f, 0.45f);
    book.Link(pistol, BulletKey{300, 321.47f, 20});
    book.Add(BulletKey{300, 321.47f, 20}, 100, 1.0f, 0.5f);
    BulletKey k;
    const TrajectoryTable* t = book.Find(r4, &k);
    REQUIRE(t);
    CHECK(k == suppressed);
    CHECK(book.Find(WeaponSig{1124, 108}) == nullptr);
    book.Link(r4, bare);  // the suppressor came off: the R4 now uses the other table
    book.Add(bare, 50, 0.1f, 0.1f);
    REQUIRE(book.Find(r4, &k));
    CHECK(k == bare);
    TrajectoryBook copy;
    REQUIRE(copy.Parse(book.Serialize()));
    float drop, tt;
    REQUIRE(copy.Find(pistol)->Lookup(100, drop, tt));
    CHECK_THAT(drop, WithinAbs(1.0, 1e-4));
    REQUIRE(copy.Find(r4, &k));
    CHECK(k == bare);
    CHECK_FALSE(copy.Parse("weapon 1 2"));
}

TEST_CASE("TableAimPoint raises by the learned drop and leads by the learned time") {
    TrajectoryTable tab;
    for (int m = 0; m <= 200; m += 2) tab.Add(float(m), 1e-4f * m * m, m / 250.0f);  // 1 m at 100 m, 0.4 s
    TableAim a;
    REQUIRE(TableAimPoint({0, 0, 0}, {100, 0, 0}, {0, 0, 0}, tab, a));
    CHECK_THAT(a.point.z, WithinAbs(1.0, 1e-3));
    CHECK_THAT(a.flight_s, WithinAbs(0.4, 1e-3));
    CHECK_FALSE(a.extrapolated);
    REQUIRE(TableAimPoint({0, 0, 0}, {100, 0, 0}, {0, 5, 0}, tab, a));  // moving sideways at 5 m/s
    CHECK_THAT(a.point.y, WithinAbs(5 * a.flight_s, 1e-3));
    CHECK(a.point.y > 1.9f);
    REQUIRE(TableAimPoint({0, 0, 0}, {400, 0, 0}, {0, 0, 0}, tab, a));  // twice the learned range
    CHECK(a.extrapolated);
    CHECK_THAT(a.drop_m, WithinAbs(4 * 1e-4 * 200 * 200, 1e-2));
    CHECK_FALSE(TableAimPoint({0, 0, 0}, {100, 0, 0}, {0, 0, 0}, TrajectoryTable{}, a));
}
