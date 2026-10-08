#pragma once
#include "math/vec.h"

namespace grw {

struct Ballistics {
    float speed = 0.0f;     // m/s; 0 = no ballistic correction
    float gravity = 9.81f;  // m/s2
    float zero_m = 0.0f;    // the sight's zero distance (the bullet crosses the line of sight there); 0 = none
};

// Where to aim from `origin` to hit `target` moving at `velocity` (world Z up): lead by the flight time,
// then raise by the bullet's drop below the line of sight (gravity drop minus the sight's zero angle).
// Returns `target` unchanged when b.speed <= 0. flight_s, when given, receives the flight time.
Vec3 BallisticAimPoint(const Vec3& origin, const Vec3& target, const Vec3& velocity, const Ballistics& b,
                       float* flight_s = nullptr);

}  // namespace grw
