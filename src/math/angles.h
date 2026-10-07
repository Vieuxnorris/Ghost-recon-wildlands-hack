#pragma once
#include "math/vec.h"

namespace grw {

inline constexpr float kPi = 3.14159265358979f;

// Radians. World is Z up; yaw = atan2(y, x); pitch is positive upward.
struct Angles { float yaw = 0, pitch = 0; };

inline float DegToRad(float d) { return d * (kPi / 180.0f); }
inline float RadToDeg(float r) { return r * (180.0f / kPi); }

float WrapPi(float a);
Angles DirectionToAngles(const Vec3& dir);
Vec3 AnglesToDirection(const Angles& a);
Angles AngleDelta(const Angles& from, const Angles& to);
float AngularDistance(const Angles& a, const Angles& b);

}  // namespace grw
