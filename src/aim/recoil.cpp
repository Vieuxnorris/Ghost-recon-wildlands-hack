#include "aim/recoil.h"
#include <cmath>

namespace grw {

float RecoilTracker::Update(double t_ms, float cam_pitch, float acc_pitch, float last_correction_pitch) {
    const float raw = cam_pitch - acc_pitch;
    if (!initialized_) {
        initialized_ = true;
        offset_ = raw;
        prev_cam_ = cam_pitch;
        prev_t_ = t_ms;
        return offset_;
    }

    const float jump = (cam_pitch - prev_cam_) - last_correction_pitch;
    const double dt = t_ms - prev_t_;
    prev_cam_ = cam_pitch;
    prev_t_ = t_ms;

    if (!holding_ && jump > p_.jump_rad) {
        holding_ = true;
        hold_start_ = t_ms;
        held_offset_ = offset_;
    }
    if (holding_) {
        const bool back = std::fabs(raw - held_offset_) < p_.release_rad;
        const bool expired = t_ms - hold_start_ > p_.max_hold_ms;
        if (!back && !expired) return held_offset_;
        holding_ = false;
        offset_ = held_offset_;
    }

    const float k = dt > 0.0 ? 1.0f - std::exp(-float(dt) / p_.follow_tau_ms) : 0.0f;
    offset_ += (raw - offset_) * k;
    return offset_;
}

}  // namespace grw
