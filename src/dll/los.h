#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "math/vec.h"

namespace grw {
class Telemetry;

// One character whose head should be checked.
struct LosRequest {
    uintptr_t key = 0;  // character object (EspItem::addr)
    Vec3 point;         // head, world
    Vec3 feet;          // root, world: with the head, the target's capsule
    bool vehicle = false;
    bool enemy = false;
    float angle = 0;  // radians from the view axis (priority)
};

struct LosVerdict {
    uint8_t state = 0;    // game::kLosUnknown / kLosVisible / kLosHidden
    double t_ms = 0;      // Clock::NowMs() of the ray
    float hit_dist = 0;   // where a blocked ray stopped (m from the origin)
    // diagnostics: distance to the target and of each hit along the way (skipped ones included)
    float target_dist = 0;
    float hits[4] = {-1, -1, -1, -1};
    Vec3 last_hit;
    uint8_t raw[4][0x50] = {};  // the collector's result block of each hit
};

struct LosParams {
    // The bullets' layers without layer 25: the characters' sensor spheres (2 m and 10 m) and proxy capsules,
    // which the bullets' rays also report (camera-findings.md section 9).
    uint64_t mask = 0x3115fd67full;
    // The filter context's flag. The bullets use 1 (context flags 4), which leaves out the vehicles' bodies
    // (layer 12, handled apart by the game like the characters): 0 so that a car hides what is behind it.
    bool flag = false;
    bool debug = false;  // los_detail / los_hit lines (target by target, once a second)
    int interval_ms = 33;
    int max_rays = 40;          // per pass
    // Without layer 25 nothing on the mask belongs to a character: any hit short of the head is an obstacle
    // (a wall or car the target leans on included). Visible when the ray stops within head_tol_m of the head;
    // a target seated in a vehicle: within vehicle_tol_m (its glass and body, Far Cry 6 lesson).
    float head_tol_m = 0.3f;
    float hitbox_m = 0.6f;  // a hitbox hit (material 8) this close to the target's axis is the target
    float vehicle_tol_m = 1.6f;
    float self_capsule_m = 0.8f;  // the player's own capsule: passed through
    float self_skip_m = 1.0f;     // hits this close to the origin (the player's weapon or scope) too
};

// Line-of-sight worker: its own thread, registered with Havok like the game's threads (memory router in the
// thread's TLS, camera-findings.md section 9), so it can call the game's ray queries (raycast.h). Rays go
// from the submitted origin (the player's head: where the bullets start) to each requested head, with the
// bullets' layers minus the characters' sensor spheres, and the vehicles (LosParams).
// Telemetry channel Los: "los" every second, "ray_test" with SetTest.
class LineOfSight {
public:
    void Start(Telemetry* tel);
    void Stop();
    // Render thread.
    void SetCamera(const Vec3& pos, const Vec3& forward);
    // swaps reqs out; player_feet: the player's capsule, if known
    void Submit(const Vec3& origin, const Vec3* player_feet, std::vector<LosRequest>& reqs);
    void Collect(std::unordered_map<uintptr_t, LosVerdict>& verdicts);  // merges the verdicts since the last call
    // Writer thread (config).
    void SetTest(bool on) { test_.store(on, std::memory_order_relaxed); }
    void SetParams(const LosParams& p);

private:
    void Run();
    void Pass();
    void RayTest();
    LosVerdict Check(const Vec3& origin, const Vec3* player_feet, const LosRequest& r, const LosParams& p);

    Telemetry* tel_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> test_{false};
    std::atomic<float> cam_[6] = {};  // position, forward

    std::mutex params_mtx_;
    LosParams params_;

    std::mutex req_mtx_;  // render -> worker
    Vec3 req_origin_;
    bool req_player_ok_ = false;
    Vec3 req_player_feet_;
    std::vector<LosRequest> reqs_;
    uint64_t req_gen_ = 0;

    std::mutex out_mtx_;  // worker -> render
    std::vector<std::pair<uintptr_t, LosVerdict>> out_;

    // worker thread
    std::vector<LosRequest> work_;
    struct Stats {
        uint64_t passes = 0, rays = 0, faults = 0, self_skips = 0, visible = 0, hidden = 0;
        double ray_us = 0, pass_ms = 0, max_pass_ms = 0;
    } stats_;
    double next_stats_ms_ = 0;
};

}  // namespace grw
