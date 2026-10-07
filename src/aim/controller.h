#pragma once
#include "math/angles.h"

namespace grw {

struct AimParams {
    float tau_ms = 80.0f;
    float deadzone_rad = 0.0f;
    float max_speed_rad_s = 6.28f;
};

// Correction for one new picture: error * (1 - exp(-dt/tau)), zero inside the dead zone, capped in speed.
// The result never exceeds the error, so it cannot overshoot on its own.
Angles AimStep(const Angles& error, float dt_ms, const AimParams& p);

// frac of the head's angular radius seen from distance_m; 0 when the distance is not positive.
float HeadDeadzoneRad(float head_radius_m, float distance_m, float frac);

}  // namespace grw
