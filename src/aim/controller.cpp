#include "aim/controller.h"
#include <cmath>

namespace grw {

Angles AimStep(const Angles& error, float dt_ms, const AimParams& p) {
    const float mag = std::sqrt(error.yaw * error.yaw + error.pitch * error.pitch);
    if (dt_ms <= 0.0f || mag == 0.0f || mag <= p.deadzone_rad) return {};
    const float k = 1.0f - std::exp(-dt_ms / p.tau_ms);
    float step = mag * k;
    const float cap = p.max_speed_rad_s * dt_ms / 1000.0f;
    if (step > cap) step = cap;
    const float scale = step / mag;
    return {error.yaw * scale, error.pitch * scale};
}

float HeadDeadzoneRad(float head_radius_m, float distance_m, float frac) {
    if (distance_m <= 0.0f) return 0.0f;
    return frac * std::atan(head_radius_m / distance_m);
}

}  // namespace grw
