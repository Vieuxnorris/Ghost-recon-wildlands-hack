#include "aim/ballistics.h"

namespace grw {

Vec3 BallisticAimPoint(const Vec3& origin, const Vec3& target, const Vec3& velocity, const Ballistics& b,
                       float* flight_s) {
    if (flight_s) *flight_s = 0.0f;
    if (b.speed <= 0.0f) return target;
    // Lead: the flight time depends on where the target will be; two fixed-point iterations are plenty for
    // target speeds far below the bullet's.
    Vec3 p = target;
    float t = (p - origin).Length() / b.speed;
    for (int i = 0; i < 2; ++i) {
        p = target + velocity * t;
        t = (p - origin).Length() / b.speed;
    }
    const float d = (p - origin).Length();
    const float drop = 0.5f * b.gravity * t * t;
    // A sight zeroed at Z tilts the barrel up by g*Z/(2 v^2): the bullet rises that much per metre.
    const float zero_rise = b.zero_m > 0.0f ? b.gravity * b.zero_m / (2.0f * b.speed * b.speed) * d : 0.0f;
    if (flight_s) *flight_s = t;
    return {p.x, p.y, p.z + drop - zero_rise};
}

}  // namespace grw
