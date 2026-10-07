#include "math/angles.h"

namespace grw {

float WrapPi(float a) {
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a <= 0.0f) a += 2.0f * kPi;
    return a - kPi;
}

Angles DirectionToAngles(const Vec3& d) {
    return {std::atan2(d.y, d.x), std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y))};
}

Vec3 AnglesToDirection(const Angles& a) {
    const float cp = std::cos(a.pitch);
    return {cp * std::cos(a.yaw), cp * std::sin(a.yaw), std::sin(a.pitch)};
}

Angles AngleDelta(const Angles& from, const Angles& to) {
    return {WrapPi(to.yaw - from.yaw), to.pitch - from.pitch};
}

// atan2(|a x b|, a . b) stays accurate for tiny angles, unlike acos(dot).
float AngularDistance(const Angles& a, const Angles& b) {
    const Vec3 u = AnglesToDirection(a);
    const Vec3 v = AnglesToDirection(b);
    const Vec3 cross{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return std::atan2(cross.Length(), u.Dot(v));
}

}  // namespace grw
