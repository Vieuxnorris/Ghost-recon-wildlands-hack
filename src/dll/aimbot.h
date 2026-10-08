#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "aim/recoil.h"
#include "aim/sway.h"
#include "aim/target.h"
#include "config/config.h"
#include "game/world.h"

namespace grw {

class Telemetry;

// Render thread, once per frame after World::BuildFrame. While the aim key is held: picks the enemy head
// closest to the crosshair inside the cone (sticky), and once per new camera picture moves the player's
// view-angle accumulator toward it with AimStep (exponential approach, dead zone, speed cap).
// The crosshair is the rendered camera, which differs from the accumulator by weapon sway and a small
// constant offset (+0.13 deg yaw measured, live/55): the error is taken from accumulator + that offset
// averaged over 0.2 s, frozen while a shot kicks the camera (the game recovers recoil itself).
// No line of sight yet (M6): every live enemy with a head is treated as visible.
class Aimbot {
public:
    struct Status {
        bool key = false;
        bool accumulator = false;
        uintptr_t target = 0;
        float error_deg = 0;
        float target_dist = 0;
        float offset_yaw_deg = 0, offset_pitch_deg = 0;  // crosshair - accumulator, smoothed
        bool recoil_hold = false;
        float flight_ms = 0;  // ballistic flight time to the target (0 = ballistics off)
        uint64_t writes = 0;
    };

    void Update(const game::EspFrame& f, const Config& c, Telemetry& tel, uint64_t frame);
    const Status& Last() const { return status_; }

private:
    TargetSelector selector_;
    std::vector<Candidate> candidates_;
    std::vector<const game::EspItem*> items_;
    Vec3 last_cam_pos_, last_cam_fwd_;
    double last_write_ms_ = 0;
    float last_step_pitch_ = 0;
    TimeWindowAverage offset_avg_{200.0};
    Angles held_offset_;
    RecoilTracker recoil_;
    // target velocity for leading, from its head position (exponential average)
    uintptr_t vel_target_ = 0;
    Vec3 vel_last_pos_, vel_;
    double vel_last_ms_ = 0;
    // kill timing oracle: shot -> dead flag gives the flight time, hence the bullet speed
    bool fire_down_ = false;
    double last_shot_ms_ = 0;
    std::unordered_map<uintptr_t, game::Category> last_cat_;
    Status status_;
    void TrackKills(const game::EspFrame& f, double now, Telemetry& tel, uint64_t frame);
};

}  // namespace grw
