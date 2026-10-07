#pragma once
#include <cstddef>
#include <cstdint>

namespace grw {

struct Candidate {
    uint32_t id = 0;  // 0 means "no target"
    bool enemy = false;
    bool alive = false;
    bool los_visible = false;
    double los_age_ms = 0.0;
    float angle_rad = 0.0f;  // angular distance from the crosshair
};

struct TargetParams {
    float cone_rad = 0.14f;
    float hidden_ms = 150.0f;
    float max_los_age_ms = 150.0f;
};

// Sticky choice: keep the current target until it dies, stays hidden longer than hidden_ms,
// or the key is released; otherwise take the visible enemy closest to the crosshair inside the cone.
class TargetSelector {
public:
    uint32_t Update(double now_ms, bool key_held, const Candidate* c, size_t n, const TargetParams& p);
    uint32_t Current() const { return current_; }

private:
    uint32_t current_ = 0;
    double last_visible_ms_ = 0.0;
};

}  // namespace grw
