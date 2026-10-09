#include "dll/aimbot.h"
#include <windows.h>
#include <cmath>
#include "aim/ballistics.h"
#include "aim/controller.h"
#include "core/clock.h"
#include "core/mem.h"
#include "math/angles.h"
#include "telemetry/json_line.h"
#include "telemetry/telemetry.h"

namespace grw {

namespace {

constexpr float kHeadRadiusM = 0.11f;
constexpr double kStalePictureMs = 50.0;  // correct anyway if the camera has not changed for this long

bool IsEnemy(game::Category c) { return c == game::Category::SantaBlanca || c == game::Category::Unidad; }

bool SameVec(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

}  // namespace

// A character that turns Dead shortly after our last shot: the delay is the bullet's flight time (plus up to
// a frame or two), so distance / delay estimates the bullet speed of the weapon in hand.
void Aimbot::TrackKills(const game::EspFrame& f, double now, Telemetry& tel, uint64_t frame) {
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !fire_down_) {
        last_shot_ms_ = now;
        shot_target_ = status_.target;
        shot_flight_ms_ = status_.flight_ms;
    }
    fire_down_ = down;
    for (const game::EspItem& it : f.items) {
        auto [slot, fresh] = last_cat_.try_emplace(it.addr, it.cat);
        if (!fresh && slot->second != game::Category::Dead && it.cat == game::Category::Dead) {
            const double dt = now - last_shot_ms_;
            if (last_shot_ms_ > 0 && dt > 0 && dt < 2000) {
                Line l;
                JsonLine(l, "kill", tel.SinceStartMs(), frame)
                    .Str("was", game::CategoryName(slot->second))
                    .Num("dist", it.dist)
                    .Num("shot_to_dead_ms", dt)
                    .Num("pred_ms", it.addr == shot_target_ ? double(shot_flight_ms_) : -1.0)  // the aim's, at the shot
                    .Num("speed_est", it.dist / (dt / 1000.0))
                    .Num("fov", f.fov_deg)
                    .Bool("locked", it.addr == status_.target)
                    .Num("weapon_speed", f.weapon_speed)
                    .Num("weapon_damage", f.weapon_damage)
                    .Num("bullet_V", f.bullet_V)
                    .Num("spread", f.spread)
                    .Finish();
                tel.Push(Channel::Render, l);
            }
        }
        slot->second = it.cat;
    }
    if (last_cat_.size() > 4096) last_cat_.clear();
}

void Aimbot::Update(const game::EspFrame& f, const Config& c, Telemetry& tel, uint64_t frame,
                    const TrajectoryTable* tab, const BulletKey& key, const SpeedCurve* curve) {
    status_.table_max_m = tab ? tab->MaxAlong() : 0;
    status_.bullet = tab ? key : BulletKey{};
    const double now = Clock::NowMs();
    TrackKills(f, now, tel, frame);
    status_.key = c.aim_enabled && (GetAsyncKeyState(c.aim_key) & 0x8000) != 0;
    status_.accumulator = f.aim.ok;
    if (!f.camera_ok || !f.aim.ok) {
        selector_.Update(now, false, nullptr, 0, {});
        status_.target = 0;
        return;
    }

    // Current view from the accumulator: it already includes our previous write.
    const Angles view{f.aim.yaw + game::kAimYawToHeading, f.aim.pitch};
    const Vec3& fw = f.camera.forward;
    const Angles cam{std::atan2(fw.y, fw.x), std::asin(std::fmax(-1.0f, std::fmin(1.0f, fw.z)))};

    // Crosshair - accumulator offset, once per new picture; frozen while a shot kicks the camera.
    const bool new_picture = !SameVec(f.camera.pos, last_cam_pos_) || !SameVec(fw, last_cam_fwd_);
    last_cam_pos_ = f.camera.pos;
    last_cam_fwd_ = fw;
    if (new_picture) {
        recoil_.Update(now, cam.pitch, view.pitch, last_step_pitch_);
        last_step_pitch_ = 0;
        if (!recoil_.Holding()) {
            offset_avg_.Add(now, AngleDelta(view, cam));
            held_offset_ = offset_avg_.Mean(now);
        }
    }
    const Angles offset = recoil_.Holding() ? held_offset_ : offset_avg_.Mean(now);
    const Angles aim_view{view.yaw + offset.yaw, view.pitch + offset.pitch};
    status_.offset_yaw_deg = RadToDeg(offset.yaw);
    status_.offset_pitch_deg = RadToDeg(offset.pitch);
    status_.recoil_hold = recoil_.Holding();

    candidates_.clear();
    items_.clear();
    for (const game::EspItem& it : f.items) {
        if (it.head_radius <= 0 || !IsEnemy(it.cat)) continue;
        Candidate k;
        k.id = uint32_t(it.addr >> 4);
        k.enemy = true;
        k.alive = true;  // dead characters are Category::Dead
        // M6: the line-of-sight worker's verdict; off -> every enemy counts as visible
        k.los_visible = !c.los_enabled || it.los == game::kLosVisible;
        k.los_age_ms = c.los_enabled ? it.los_age_ms : 0.0;
        k.angle_rad = AngularDistance(aim_view, DirectionToAngles(it.head_world - f.camera.pos));
        candidates_.push_back(k);
        items_.push_back(&it);
    }
    TargetParams tp;
    tp.cone_rad = DegToRad(c.aim_cone_deg);
    tp.hidden_ms = c.aim_hidden_ms;
    const uint32_t id = selector_.Update(now, status_.key, candidates_.data(), candidates_.size(), tp);
    const game::EspItem* target = nullptr;
    for (size_t i = 0; i < candidates_.size(); ++i)
        if (candidates_[i].id == id) target = items_[i];
    status_.target = target ? target->addr : 0;
    if (!target) return;
    status_.target_head = target->head_world;
    status_.target_vel = target->addr == vel_target_ ? vel_ : Vec3{};

    // One correction per new camera picture: the camera shows a write one frame later, and correcting the
    // same picture twice overshoots (FC6 lesson).
    const double since = now - last_write_ms_;
    if (!new_picture && since < kStalePictureMs) return;

    // Target velocity from its head, for leading (reset on a new target).
    if (target->addr != vel_target_) {
        vel_target_ = target->addr;
        vel_ = {};
        vel_last_ms_ = 0;
    }
    if (vel_last_ms_ > 0 && now - vel_last_ms_ > 1.0) {
        const float dts = float((now - vel_last_ms_) / 1000.0);
        const Vec3 inst = (target->head_world - vel_last_pos_) * (1.0f / dts);
        const float k = 1.0f - std::exp(-dts / 0.15f);
        // ignore teleports (vehicles reach 50+ m/s: a lower cap left them without lead)
        if (inst.Length() < 60.0f) vel_ = vel_ + (inst - vel_) * k;
    }
    if (vel_last_ms_ == 0 || now - vel_last_ms_ > 1.0) {
        vel_last_pos_ = target->head_world;
        vel_last_ms_ = now;
    }
    const Vec3 player = f.player_head_ok ? f.player_head : f.camera.pos;
    if (pvel_last_ms_ > 0 && now - pvel_last_ms_ > 1000.0) pvel_ = {}, pvel_last_ms_ = 0;  // a gap: start again
    if (pvel_last_ms_ > 0 && now - pvel_last_ms_ > 1.0) {
        const float dts = float((now - pvel_last_ms_) / 1000.0);
        const Vec3 inst = (player - pvel_last_pos_) * (1.0f / dts);
        if (inst.Length() < 60.0f) pvel_ = pvel_ + (inst - pvel_) * (1.0f - std::exp(-dts / 0.15f));
    }
    if (pvel_last_ms_ == 0 || now - pvel_last_ms_ > 1.0) {
        pvel_last_pos_ = player;
        pvel_last_ms_ = now;
    }

    Ballistics bal;
    bal.speed = c.aim_bullet_speed > 0 ? c.aim_bullet_speed : f.weapon_speed;  // the game's value by default
    bal.gravity = c.aim_gravity;
    bal.zero_m = c.aim_zero_m;
    float flight_s = 0;
    Vec3 aim_point;
    // The bullets start at the player's head in third person (at the camera through a scope): distances and the
    // drop's plane are measured from there; the crosshair is still turned from the camera.
    const Vec3 origin = f.player_head_ok ? f.player_head : f.camera.pos;
    // The game's speed law (trajectory.h): V from the weapon's ballistics block, the launch vector's length from
    // its spread and the distance to the aimed point.
    FlightModel fm;
    if (f.bullet_ok && curve) {
        fm.V = f.bullet_V;
        fm.curve = curve;
        // the spread the next shot will use: the live attribute (the component holds the last shot's), with
        // the spread mod the shot will get
        const float spread = f.weapon.spread_live >= 0 ? f.weapon.spread_live : f.spread_ok ? f.spread : 0.0f;
        fm.spread = spread * f.weapon.spread_factor;
        if (f.spread_ok) fm.spread_range = f.spread_range;
    }
    // The bullet path learned from the player's own shots (the game's real one); physics with the configured
    // speed as a fallback, or when aim_bullet_speed forces it.
    TableAim ta;
    const Vec3 lead = c.aim_lead ? vel_ : Vec3{};
    status_.modelled = false;
    status_.launch_len = 1;
    if (f.bullet_straight && fm.Valid()) {
        // straight bullets (the mod): no drop, straight at the head, led by the flight at the speed after the
        // first step
        status_.launch_len = LaunchLength(fm.spread, fm.spread_range, (target->head_world - origin).Length());
        bal.speed = fm.V * curve->Lookup(std::acos(std::fmin(1.0f, status_.launch_len)) * 57.29578f);
        bal.gravity = 0;
        bal.zero_m = 0;
        status_.cruise_mps = bal.speed;
        aim_point = BallisticAimPoint(origin, target->head_world, lead, bal, &flight_s);
        if (f.bullet_teleport) {
            // a wait for the first step, the first segment, then one hop per step
            const float d = (target->head_world - origin).Length();
            const float hops = std::ceil(std::fmax(0.0f, d - f.bullet_first_segment) / game::kTeleportChunkM);
            flight_s = (2.0f + hops) * game::kSimStepS;
            aim_point = target->head_world + lead * flight_s;
        }
        status_.drop_m = 0;
        status_.trajectory = 3;
    } else if (c.aim_bullet_speed <= 0 && tab && TableAimPoint(origin, target->head_world, lead, *tab, ta, &fm)) {
        aim_point = ta.point;
        flight_s = ta.flight_s;
        status_.drop_m = ta.drop_m;
        status_.trajectory = ta.extrapolated ? 2 : 1;
        status_.modelled = ta.modelled;
        status_.launch_len = ta.launch_len;
        status_.cruise_mps = fm.Valid() ? fm.V * curve->Lookup(std::acos(std::fmin(1.0f, ta.launch_len)) * 57.29578f) : 0;
    } else {
        // no path learned for these bullets yet: their speed after the first step, straight (gravity beyond)
        if (c.aim_bullet_speed <= 0 && fm.Valid()) {
            status_.launch_len = LaunchLength(fm.spread, fm.spread_range, (target->head_world - origin).Length());
            bal.speed = fm.V * curve->Lookup(std::acos(std::fmin(1.0f, status_.launch_len)) * 57.29578f);
        }
        status_.cruise_mps = bal.speed;
        aim_point = BallisticAimPoint(origin, target->head_world, lead, bal, &flight_s);
        status_.drop_m = 0;
        status_.trajectory = 0;
    }
    status_.flight_ms = flight_s * 1000.0f;
    // The controller closes the error by 1 - exp(-dt/tau) per picture: across the view, a moving target stays about
    // dt / (1 - exp(-dt/tau)) behind (~80 ms by default: 0.15 deg, 1.3 m, behind a car at 500 m, measured). Aim that
    // much further along its motion relative to the player.
    const float dt = float(since > 100.0 || last_write_ms_ == 0 ? 16.0 : since);
    const float lag_s = c.aim_tau_ms > 0 ? dt / 1000.0f / (1.0f - std::exp(-dt / c.aim_tau_ms)) : 0.0f;
    if (c.aim_lead) aim_point = aim_point + (vel_ - pvel_) * lag_s;
    status_.lag_ms = lag_s * 1000.0f;
    const Angles want = DirectionToAngles(aim_point - f.camera.pos);
    const Angles err = AngleDelta(aim_view, want);
    status_.error_deg = RadToDeg(std::sqrt(err.yaw * err.yaw + err.pitch * err.pitch));
    status_.target_dist = target->dist;

    AimParams ap;
    ap.tau_ms = c.aim_tau_ms;
    ap.deadzone_rad = HeadDeadzoneRad(kHeadRadiusM, target->dist, c.aim_deadzone_frac);
    ap.max_speed_rad_s = DegToRad(c.aim_max_speed_deg_s);
    const Angles step = AimStep(err, dt, ap);

    float yaw = f.aim.yaw + step.yaw;
    float pitch = f.aim.pitch + step.pitch;
    pitch = std::fmin(std::fmax(pitch, f.aim.pitch_min), f.aim.pitch_max);
    bool wrote = false;
    if (step.yaw != 0.0f || step.pitch != 0.0f) {
        wrote = mem::Write(f.aim.comp + game::kAimYawOff, yaw) && mem::Write(f.aim.comp + game::kAimPitchOff, pitch);
        if (wrote) {
            ++status_.writes;
            last_step_pitch_ = pitch - f.aim.pitch;
        }
    }
    last_write_ms_ = now;

    Line l;
    JsonLine(l, "aim_tick", tel.SinceStartMs(), frame)
        .Int("target", int64_t(target->addr & 0xFFFFFFFF))
        .Str("cat", game::CategoryName(target->cat))
        .Num("dist", target->dist)
        .Num("err_yaw", RadToDeg(err.yaw))
        .Num("err_pitch", RadToDeg(err.pitch))
        .Num("step_yaw", RadToDeg(step.yaw))
        .Num("step_pitch", RadToDeg(step.pitch))
        .Num("acc_yaw", RadToDeg(view.yaw))
        .Num("acc_pitch", RadToDeg(view.pitch))
        .Num("cam_yaw", RadToDeg(cam.yaw))
        .Num("cam_pitch", RadToDeg(cam.pitch))
        .Num("off_yaw", RadToDeg(offset.yaw))
        .Num("off_pitch", RadToDeg(offset.pitch))
        .Bool("recoil_hold", recoil_.Holding())
        .Num("flight_ms", flight_s * 1000.0f)
        .Num("bullet_speed", bal.speed)
        .Int("traj", status_.trajectory)
        .Num("drop_m", status_.drop_m)
        .Num("table_max", status_.table_max_m)
        .Bool("model", status_.modelled)
        .Num("launch_len", status_.launch_len)
        .Num("bullet_V", f.bullet_V)
        .Num("spread", f.spread)
        .Bool("origin_head", f.player_head_ok)
        .Num("lead_speed", vel_.Length())
        .Num("player_speed", pvel_.Length())
        .Num("lag_ms", status_.lag_ms)
        .Num("fov", f.fov_deg)
        .Num("dt", dt)
        .Bool("new_picture", new_picture)
        .Bool("wrote", wrote)
        .Finish();
    tel.Push(Channel::Render, l);
}

}  // namespace grw
