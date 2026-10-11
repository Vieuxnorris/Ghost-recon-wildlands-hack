#pragma once
#include "math/vec.h"

// The player's teleport to the map waypoint (MODLOG 2026-10-10, TP perso). Writing the entity's position moves the
// player; on loaded ground the character controller puts them on it within a frame. Far away the ground is not loaded
// yet (and the waypoint's z is then the player's own): the player is held above the destination until a ray cast
// down finds the ground.
namespace grw::game {

// One downward search at (x, y), from top_z to bottom_z, in rays of at most chunk_m (a single long ray misses beyond
// ~300-450 m). Hits within [skip_lo, skip_hi] are skipped (the player's own body while held there): the search goes
// on below them.
struct GroundQuery {
    float x = 0, y = 0;
    float top_z = 6000.0f, bottom_z = -500.0f;
    float chunk_m = 200.0f;
    float skip_lo = 1.0f, skip_hi = 0.0f;  // empty band
};
// cast: one ray from -> to, true with the hit's z on a hit.
using GroundCast = bool (*)(void* ctx, const Vec3& from, const Vec3& to, float& hit_z);
bool FindGround(const GroundQuery& q, GroundCast cast, void* ctx, float& ground_z);

// A waypoint position read from the game worth going to: finite, within the map, not the origin.
bool PlausibleWaypoint(const Vec3& p);

// What F5 / the menu does (MODLOG 2026-10-10): with the tactical map open, go under its cursor. The game is paused
// while the map is open, so that teleport starts when the map closes. With the map closed, go to the waypoint at once.
enum class TpAct { None, Queued, Waypoint, Cursor };
class TpQueue {
public:
    // Once per frame while no teleport runs. request: F5 or the menu since the last call; map_open/cursor: the map's
    // cursor, map_open false when the map is closed or the cursor unreadable. A cursor kept wins over a request made
    // in the frame the map closes.
    TpAct Update(bool request, bool map_open, const Vec3& cursor);
    bool Pending() const { return pending_; }
    const Vec3& Target() const { return target_; }  // the cursor kept (Queued, Cursor)

private:
    bool pending_ = false;
    Vec3 target_;
};

struct TeleportParams {
    float above_m = 1.0f;          // placed this far above the ground found
    float hold_above_m = 300.0f;   // held this far above max(start z, waypoint z) while the ground loads
    double timeout_ms = 15000.0;   // then back to the start
    double probe_interval_ms = 100.0;
    // The final position is written this long, and at least place_frames frames: the game's physics follows a
    // position written over many frames (the hold), not always one written 3 frames (MODLOG 2026-10-10).
    double place_ms = 250.0;
    int place_frames = 3;
};

enum class TpPhase { Idle, Holding, Placing, Returning, Done, Failed };

struct TpStep {
    bool write = false;  // write the player's position this frame
    Vec3 pos;
};

// One teleport, driven once per frame by the render thread: Update says where the player must be this frame.
class Teleporter {
public:
    void Begin(const Vec3& start, const Vec3& waypoint, double now_ms, const TeleportParams& p);
    // ground_ok/ground_z: the result of the ground search done this frame (the caller searches when ProbeDue).
    TpStep Update(double now_ms, bool ground_ok, float ground_z);
    bool ProbeDue(double now_ms) const { return Phase() == TpPhase::Holding && now_ms >= next_probe_ms_; }
    bool Active() const { return phase_ == TpPhase::Holding || phase_ == TpPhase::Placing || phase_ == TpPhase::Returning; }
    TpPhase Phase() const { return phase_; }
    float HoldZ() const { return hold_z_; }
    double BeginMs() const { return begin_ms_; }
    double GroundMs() const { return ground_ms_; }  // when the ground was found, -1 = not yet
    const Vec3& Start() const { return start_; }
    const Vec3& Waypoint() const { return waypoint_; }
    const Vec3& Target() const { return target_; }  // the waypoint, then the position written

private:
    TpStep Write(const Vec3& p);
    TeleportParams p_;
    TpPhase phase_ = TpPhase::Idle;
    Vec3 start_, waypoint_, target_;
    float hold_z_ = 0;
    double begin_ms_ = 0, next_probe_ms_ = 0, ground_ms_ = -1;
    double place_until_ms_ = 0;  // the final position is written until then (Placing, Returning)
    int frames_left_ = 0;
};

}  // namespace grw::game
