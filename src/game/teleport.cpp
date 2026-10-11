#include "game/teleport.h"
#include <cmath>

namespace grw::game {

bool FindGround(const GroundQuery& q, GroundCast cast, void* ctx, float& ground_z) {
    float from = q.top_z;
    while (from > q.bottom_z) {
        const float to = std::fmax(from - q.chunk_m, q.bottom_z);
        float z = 0;
        if (cast(ctx, {q.x, q.y, from}, {q.x, q.y, to}, z)) {
            if (z >= q.skip_lo && z <= q.skip_hi) {
                from = std::fmin(z, q.skip_lo) - 0.05f;  // under the skipped band
                continue;
            }
            ground_z = z;
            return true;
        }
        from = to;
    }
    return false;
}

bool PlausibleWaypoint(const Vec3& p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    if (std::fabs(p.x) > 20000.0f || std::fabs(p.y) > 20000.0f || p.z < -1000.0f || p.z > 10000.0f) return false;
    return std::fabs(p.x) + std::fabs(p.y) > 1.0f;
}

TpAct TpQueue::Update(bool request, bool map_open, const Vec3& cursor) {
    if (pending_ && !map_open) {
        pending_ = false;
        return TpAct::Cursor;
    }
    if (!request) return TpAct::None;
    if (!map_open) return TpAct::Waypoint;
    pending_ = true;
    target_ = cursor;
    return TpAct::Queued;
}

void Teleporter::Begin(const Vec3& start, const Vec3& waypoint, double now_ms, const TeleportParams& p) {
    p_ = p;
    start_ = start;
    waypoint_ = target_ = waypoint;
    hold_z_ = std::fmax(start.z, waypoint.z) + p.hold_above_m;
    begin_ms_ = next_probe_ms_ = now_ms;
    ground_ms_ = -1;
    frames_left_ = 0;
    phase_ = TpPhase::Holding;
}

TpStep Teleporter::Write(const Vec3& p) {
    target_ = p;
    return {true, p};
}

TpStep Teleporter::Update(double now_ms, bool ground_ok, float ground_z) {
    switch (phase_) {
        case TpPhase::Holding:
            if (now_ms >= next_probe_ms_) next_probe_ms_ = now_ms + p_.probe_interval_ms;
            if (ground_ok) {
                ground_ms_ = now_ms;
                phase_ = TpPhase::Placing;
                frames_left_ = p_.place_frames - 1;
                place_until_ms_ = now_ms + p_.place_ms;
                return Write({target_.x, target_.y, ground_z + p_.above_m});
            }
            if (now_ms - begin_ms_ >= p_.timeout_ms) {
                phase_ = TpPhase::Returning;
                frames_left_ = p_.place_frames - 1;
                place_until_ms_ = now_ms + p_.place_ms;
                return Write({start_.x, start_.y, start_.z + p_.above_m});
            }
            return {true, {target_.x, target_.y, hold_z_}};
        case TpPhase::Placing:
        case TpPhase::Returning:
            if (frames_left_ > 0 || now_ms < place_until_ms_) {
                if (frames_left_ > 0) --frames_left_;
                return {true, target_};
            }
            phase_ = phase_ == TpPhase::Placing ? TpPhase::Done : TpPhase::Failed;
            return {};
        default:
            return {};
    }
}

}  // namespace grw::game
