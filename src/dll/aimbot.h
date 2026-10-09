#pragma once
#include "aim/trajectory.h"
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
// Targets: live enemies with a head whose line-of-sight verdict (M6, EspItem::los) is "visible" and fresh.
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
        float lag_ms = 0;     // the controller's lag, led along the target's relative motion
        Vec3 target_head, target_vel;  // the locked target's head and its velocity (m/s), valid with target
        float drop_m = 0;     // drop compensated (learned table)
        int trajectory = 0;   // 0 = physics fallback, 1 = learned table, 2 = table extrapolated, 3 = straight (mod)
        bool modelled = false;  // flight time from the game's speed law (trajectory.h, FlightTime)
        float launch_len = 1;   // the launch vector's length predicted for this shot (LaunchLength)
        float cruise_mps = 0;   // the bullet's speed after its first step, for that launch (0 = unknown)
        float table_max_m = 0;
        BulletKey bullet;     // the setup of the table in use
        uint64_t writes = 0;
    };

    // tab: the learned path of the weapon in hand (nullptr = none yet), curve: the bullets' speed curve; both
    // valid during the call.
    void Update(const game::EspFrame& f, const Config& c, Telemetry& tel, uint64_t frame,
                const TrajectoryTable* tab = nullptr, const BulletKey& key = {}, const SpeedCurve* curve = nullptr);
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
    // the player's velocity (its head, else the camera): the view turns with the motion relative to the player
    Vec3 pvel_last_pos_, pvel_;
    double pvel_last_ms_ = 0;
    // kill timing oracle: shot -> dead flag gives the flight time, hence the bullet speed
    bool fire_down_ = false;
    double last_shot_ms_ = 0;
    uintptr_t shot_target_ = 0;  // the locked target and the flight time predicted when the shot was fired
    float shot_flight_ms_ = 0;
    std::unordered_map<uintptr_t, game::Category> last_cat_;
    Status status_;
    void TrackKills(const game::EspFrame& f, double now, Telemetry& tel, uint64_t frame);
};

}  // namespace grw
