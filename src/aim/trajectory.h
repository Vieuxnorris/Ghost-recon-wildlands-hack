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

struct TableAim {
    Vec3 point;            // where to put the crosshair
    float flight_s = 0;    // time of flight to the (led) target
    float drop_m = 0;      // drop compensated
    bool extrapolated = false;  // beyond the learned range: drop grown with the square of the distance
};
// Where to aim from origin to hit target moving at velocity (world Z up), with a learned table: lead by the
// table's flight time, then raise by its drop, perpendicular to the line of sight in its vertical plane.
// False when the table is empty.
bool TableAimPoint(const Vec3& origin, const Vec3& target, const Vec3& velocity, const TrajectoryTable& tab,
                   TableAim& out);

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
    bool Dirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }

    std::string Serialize() const;
    bool Parse(const std::string& text);  // all or nothing

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
    bool dirty_ = false;
};

}  // namespace grw
