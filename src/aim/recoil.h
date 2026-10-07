#pragma once

namespace grw {

struct RecoilParams {
    float jump_rad = 0.0174533f;     // 1 degree up in one picture
    float release_rad = 0.0034907f;  // camera back within 0.2 degree
    float max_hold_ms = 400.0f;
    float follow_tau_ms = 500.0f;
};

// Tracks the camera - accumulator pitch offset. A shot (an upward camera jump that our previous correction
// does not explain) freezes the offset at its pre-shot value until the camera comes back.
class RecoilTracker {
public:
    explicit RecoilTracker(RecoilParams p = {}) : p_(p) {}
    float Update(double t_ms, float cam_pitch, float acc_pitch, float last_correction_pitch);
    bool Holding() const { return holding_; }
    void Reset() { initialized_ = false; holding_ = false; }

private:
    RecoilParams p_;
    bool initialized_ = false;
    bool holding_ = false;
    float offset_ = 0.0f;
    float held_offset_ = 0.0f;
    float prev_cam_ = 0.0f;
    double prev_t_ = 0.0;
    double hold_start_ = 0.0;
};

}  // namespace grw
