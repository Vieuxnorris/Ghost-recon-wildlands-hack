#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "math/vec.h"

namespace grw {

// The game's bullet path, measured (live/72-74): the bullet flies straight along the launch direction for a
// first segment (R4 41.7 m), then in straight segments whose direction drops a little more at each boundary,
// at speed V x curve(angle from the launch direction). Rather than re-implementing that model, the player's
// own bullets are sampled at every simulation step and binned here, per metre along the launch direction:
// the drop below the launch line (in its vertical plane) and the flight time. The shape does not depend on
// the launch elevation for near-horizontal shots.
class TrajectoryTable {
public:
    static constexpr int kMaxMetres = 2000;

    // along: metres along the launch direction; drop: metres below the launch line; t: seconds of flight.
    void Add(float along, float drop, float t);
    // Linear interpolation between the measured metres; false beyond the measured range.
    bool Lookup(float along, float& drop, float& t) const;
    float MaxAlong() const { return max_along_; }
    int Samples() const { return samples_; }
    void Clear();

    std::string Serialize() const;           // "metre drop t" lines
    bool Parse(const std::string& text);     // all or nothing

private:
    struct Bin {
        float drop = 0, t = 0;
        bool valid = false;
    };
    std::vector<Bin> bins_ = std::vector<Bin>(kMaxMetres + 1);
    float max_along_ = 0;
    int samples_ = 0;
};

// The game's bullet speed law (camera-findings.md section 7b), at every simulation step:
//   v = max(20, V * curve(angle)) + 4.905 t^2 max(-dir_z, 0),  angle = acos(|d| cos(Delta)) in degrees,
// Delta = how far the direction has dropped since the launch, |d| = the launch vector's length (LaunchLength).
// curve = v/V against that angle, the same for every weapon: learned from the traced steps, seeded with the
// values measured on 2026-10-09 (364 flights, V 300 to 660): 1.0 at 0 deg, 0.83 at 0.57, 0.53 at 2, 0.40 past 5.2.
class SpeedCurve {
public:
    static constexpr float kBinDeg = 0.02f;
    static constexpr int kBins = 1000;  // 0 .. 20 deg; beyond: the last bin
    SpeedCurve();                       // seeded
    // One step: rejected when it is more than 0.08 away from the curve (a step paired with another's speed).
    bool Add(float angle_deg, float ratio);
    float Lookup(float angle_deg) const;  // linear between the bins that hold data
    int Samples() const { return samples_; }
    std::string Serialize() const;        // "angle mean count" lines
    bool Parse(const std::string& text);  // all or nothing

private:
    struct Bin {
        double sum = 0;
        uint32_t n = 0;
    };
    void Rebuild();
    std::vector<Bin> bins_ = std::vector<Bin>(kBins);
    std::vector<float> dense_;  // every bin, interpolated between those with data
    int samples_ = 0;
};

// The launch vector's length: 1 when the shooter's spread is (near) zero, else L / sqrt(L^2 + 1) with L the
// distance to the aimed point capped at the spread's range (the game normalises P - origin over 4 lanes, P.w = 1).
float LaunchLength(float spread, float spread_range, float aim_dist);

// What the time of flight needs besides the path's drop.
struct FlightModel {
    float V = 0;                 // the weapon's bullet speed (its ballistics block)
    float spread = 0, spread_range = 100;
    const SpeedCurve* curve = nullptr;
    bool Valid() const { return V > 0 && curve; }
};
// Seconds to fly `along` metres on tab's path from a launch whose direction has this sine of elevation and length:
// one simulation step at V (8 ms), then the speed law with Delta from the slope of the learned drop (the last
// slope beyond the learned range).
float FlightTime(const TrajectoryTable& tab, const FlightModel& m, float launch_len, float sin_elevation, float along);
inline constexpr float kFirstStepS = 0.008f;

struct TableAim {
    Vec3 point;            // where to put the crosshair
    float flight_s = 0;    // time of flight to the (led) target
    float drop_m = 0;      // drop compensated
    bool extrapolated = false;  // beyond the learned range: drop grown with the square of the distance
    bool modelled = false;      // flight time from the speed law (else the table's own times)
    float launch_len = 1;
};
// Where to aim from origin to hit target moving at velocity (world Z up), with a learned table: lead by the
// flight time, then raise by its drop, perpendicular to the line of sight in its vertical plane. The flight time
// comes from the speed law when model is given and valid, else from the table. False when the table is empty.
bool TableAimPoint(const Vec3& origin, const Vec3& target, const Vec3& velocity, const TrajectoryTable& tab,
                   TableAim& out, const FlightModel* model = nullptr);

// The constants every bullet of one weapon setup carries (projectile +0x1b4, +0x1bc, +0x168): a different
// set (another weapon, a suppressor fitted) is a different trajectory.
struct BulletKey {
    float V = 0, v0 = 0, first_segment = 0;
    bool operator==(const BulletKey& o) const;
    bool Valid() const { return V > 0; }
};

// The weapon in the player's hands, by its design data (+0x208, +0x398): stable across sessions.
struct WeaponSig {
    float speed = 0, damage = 0;
    bool operator==(const WeaponSig& o) const;
};

// Learned tables of every weapon setup, and the setup last fired with each weapon. Not thread-safe.
class TrajectoryBook {
public:
    // Bullets of setup k were just fired with weapon w (a fitted suppressor changes the setup: latest wins).
    void Link(const WeaponSig& w, const BulletKey& k);
    // One simulation step of a bullet of setup k.
    void Add(const BulletKey& k, float along, float drop, float t);
    // The table of the bullets last fired with w, if any (and their key).
    const TrajectoryTable* Find(const WeaponSig& w, BulletKey* key = nullptr) const;
    // The table of the bullets with this V and first segment, read from the weapon before any shot (the most
    // sampled one if several v0 match).
    const TrajectoryTable* FindSetup(float V, float first_segment, BulletKey* key = nullptr) const;
    // One step of the speed curve (SpeedCurve::Add).
    void AddCurve(float angle_deg, float ratio);
    const SpeedCurve& Curve() const { return curve_; }
    bool Dirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }

    std::string Serialize() const;
    bool Parse(const std::string& text);  // all or nothing; a book without a curve keeps the seeded one

private:
    struct Entry {
        BulletKey key;
        TrajectoryTable table;
    };
    struct WeaponLink {
        WeaponSig weapon;
        BulletKey key;
    };
    TrajectoryTable* Table(const BulletKey& k);
    std::vector<Entry> tables_;
    std::vector<WeaponLink> links_;
    SpeedCurve curve_;
    int curve_unsaved_ = 0;
    bool dirty_ = false;
};

}  // namespace grw
