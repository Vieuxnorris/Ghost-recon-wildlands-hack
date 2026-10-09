#include <catch2/catch_test_macros.hpp>
#include <cmath>
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

TEST_CASE("SpeedCurve starts from the measured curve, learns consistent steps and rejects stray ones") {
    SpeedCurve c;
    CHECK_THAT(c.Lookup(0.0f), WithinAbs(1.0, 1e-6));
    CHECK_THAT(c.Lookup(2.0f), WithinAbs(0.533, 1e-6));
    CHECK_THAT(c.Lookup(0.5f), WithinAbs((0.878 + 0.819) / 2, 1e-4));  // between the seeded 0.4 and 0.6
    CHECK_THAT(c.Lookup(12.0f), WithinAbs(0.4, 1e-6));
    CHECK_THAT(c.Lookup(45.0f), WithinAbs(0.4, 1e-6));  // past the last bin
    REQUIRE(c.Add(0.58f, 0.8267f));                     // a step measured at 0.58 deg
    CHECK(c.Lookup(0.58f) > 0.82f);
    CHECK_FALSE(c.Add(7.9f, 0.99f));  // a speed paired with the wrong step: far from the curve
    CHECK(c.Samples() == 1);
    SpeedCurve d;
    REQUIRE(d.Parse(c.Serialize()));
    CHECK_THAT(d.Lookup(0.58f), WithinAbs(c.Lookup(0.58f), 1e-4));
    CHECK_FALSE(d.Parse("0.5 0.8"));  // truncated: untouched
    CHECK_THAT(d.Lookup(0.58f), WithinAbs(c.Lookup(0.58f), 1e-4));
}

TEST_CASE("LaunchLength follows the spread and the distance to the aimed point") {
    CHECK(LaunchLength(0.0f, 100, 10) == 1.0f);   // no spread: the direction is left alone
    CHECK(LaunchLength(5e-6f, 100, 10) == 1.0f);  // below the game's 1e-5
    CHECK_THAT(LaunchLength(0.03f, 100, 7.6f), WithinAbs(0.991469, 2e-5));  // a shot at 7.6 m (live, 2026-10-09)
    CHECK_THAT(LaunchLength(0.03f, 100, 250), WithinAbs(0.99995, 1e-6));    // capped at 100 m
}

TEST_CASE("FlightTime flies one step at V, then at the curve's speed for the launch vector and the drop") {
    SpeedCurve curve;
    TrajectoryTable flat;
    for (int m = 0; m <= 300; m += 3) flat.Add(float(m), 0.0f, 0.0f);
    FlightModel m{300, 0, 100, &curve};
    // a unit launch vector on a path that does not drop: V all along
    CHECK_THAT(FlightTime(flat, m, 1.0f, 0.0f, 150), WithinAbs(0.5, 1e-4));
    // the 100 m cap of a spread shot: 0.573 deg -> about 0.827 V after the first step
    const float len = LaunchLength(0.03f, 100, 150);
    const float v = 300 * curve.Lookup(std::acos(len) * 57.29578f);
    CHECK_THAT(FlightTime(flat, m, len, 0.0f, 150), WithinAbs(kFirstStepS + (150 - 300 * kFirstStepS) / v, 1e-3));
    // a path that drops: slower than the same launch on the flat one
    TrajectoryTable falling;
    for (int x = 0; x <= 300; x += 3) falling.Add(float(x), x > 40 ? 1e-4f * (x - 40) * (x - 40) : 0.0f, 0.0f);
    CHECK(FlightTime(falling, m, 1.0f, 0.0f, 250) > FlightTime(flat, m, 1.0f, 0.0f, 250) + 0.01f);
    CHECK(FlightTime(flat, FlightModel{}, 1.0f, 0.0f, 100) == 0.0f);  // no model
}

TEST_CASE("TableAimPoint leads by the speed law when given a model") {
    SpeedCurve curve;
    TrajectoryTable tab;
    for (int m = 0; m <= 200; m += 2) tab.Add(float(m), 1e-4f * m * m, 9.0f);  // the stored times are ignored
    const FlightModel fm{300, 0, 100, &curve};
    TableAim a;
    REQUIRE(TableAimPoint({0, 0, 0}, {100, 0, 0}, {0, 0, 0}, tab, a, &fm));
    CHECK(a.modelled);
    CHECK(a.launch_len == 1.0f);
    CHECK_THAT(a.flight_s, WithinAbs(FlightTime(tab, fm, 1.0f, 0.0f, 100), 1e-5));
    CHECK(a.flight_s > 0.36f);  // slower than V as the direction drops
    CHECK_THAT(a.point.z, WithinAbs(1.0, 1e-3));  // the drop still comes from the table
    REQUIRE(TableAimPoint({0, 0, 0}, {100, 0, 0}, {0, 0, 0}, tab, a));  // without: the table's own time
    CHECK_FALSE(a.modelled);
    CHECK_THAT(a.flight_s, WithinAbs(9.0, 1e-3));
}

TEST_CASE("TrajectoryBook finds a table by the weapon's V and first segment, and keeps the curve") {
    const BulletKey r4{300, 308.35f, 41.74f}, longbarrel{353.3f, 358.86f, 50.49f};
    TrajectoryBook book;
    book.Add(r4, 100, 0.47f, 0.6f);
    book.Add(longbarrel, 100, 0.3f, 0.4f);
    BulletKey k;
    REQUIRE(book.FindSetup(353.3f, 50.49f, &k));
    CHECK(k == longbarrel);
    CHECK(book.FindSetup(353.3f, 41.74f) == nullptr);
    book.AddCurve(0.58f, 0.8267f);
    TrajectoryBook copy;
    REQUIRE(copy.Parse(book.Serialize()));
    CHECK(copy.Curve().Samples() == 1);
    REQUIRE(copy.FindSetup(300, 41.74f, &k));
    CHECK(k == r4);
    TrajectoryBook old;  // a book saved before the curve: the seeded one
    REQUIRE(old.Parse("table 300 308.35 41.74\n100 0.47 0.6\nend\n"));
    CHECK_THAT(old.Curve().Lookup(2.0f), WithinAbs(0.533, 1e-6));
}
