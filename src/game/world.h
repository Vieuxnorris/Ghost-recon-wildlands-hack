#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#include "core/stats.h"
#include "game/model.h"
#include "math/vec.h"

namespace grw {
class Telemetry;
}

namespace grw::game {

struct EspItem {
    uintptr_t addr = 0;     // character object: stable identity while it lives
    Vec2 box_min, box_max;  // screen pixels, feet to crown
    Vec2 head;
    float head_radius = 0;  // pixels, 0 = no pose (no head marker)
    Vec3 head_world;        // valid when head_radius > 0
    float dist = 0;         // metres from the camera
    uint32_t flags = 0;
    Category cat = Category::Unknown;
    bool vehicle = false;
};

struct EspOptions {
    float width = 0, height = 0;
    float max_dist = 300;
    bool civilians = true;
    bool dead = true;
};

// The player's view-angle accumulator, read every frame.
struct AimState {
    bool ok = false;
    uintptr_t comp = 0;  // aim component address
    float yaw = 0, pitch = 0, pitch_min = 0, pitch_max = 0;  // radians, as stored
};

struct EspFrame {
    bool camera_ok = false;
    float fov_deg = 0;
    Vec3 camera_pos;
    CameraView camera;
    AimState aim;
    uint32_t known = 0;     // character objects from the last scan
    uint32_t active = 0;    // valid, not at the origin, within range
    uint32_t on_screen = 0;
    uint32_t heads = 0;     // pose read and head found
    std::vector<EspItem> items;
};

// Finds the camera and the character objects by their vtables in this process's writable memory, on its
// own thread, every interval_ms (and at once after the camera is lost). BuildFrame reads them on the render
// thread: entity flags for every character in range, bones only for those that survive screen culling.
class World {
public:
    void Start(Telemetry* tel, DWORD interval_ms);
    void Stop();
    void SetInterval(DWORD ms) { interval_ms_.store(ms, std::memory_order_relaxed); }
    void BuildFrame(const EspOptions& opt, EspFrame& out, StageStats& stats);

private:
    void Run();
    void ReadAim(uintptr_t entity, AimState& aim) const;

    Telemetry* tel_ = nullptr;
    uintptr_t base_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> rescan_{false};
    std::atomic<DWORD> interval_ms_{1000};
    HANDLE wake_ = nullptr;

    SRWLOCK lock_ = SRWLOCK_INIT;  // guards the published scan result
    uintptr_t camera_ = 0;
    std::vector<uintptr_t> chars_;
    uint64_t generation_ = 0;

    // render thread copy
    uint64_t seen_generation_ = ~0ull;
    uintptr_t r_camera_ = 0;
    std::vector<uintptr_t> r_chars_;
    std::vector<float> bones_;
};

}  // namespace grw::game
