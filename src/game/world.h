#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#include "core/stats.h"
#include "game/model.h"
#include "game/registry.h"
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
    Vec3 feet;              // world position of the character's root (its feet)
    uint32_t flags = 0;
    Category cat = Category::Unknown;
    bool vehicle = false;
    // Line of sight from the camera to the head through the game's bullet ray filter (M6, dll/los).
    uint8_t los = 0;            // kLosUnknown / kLosVisible / kLosHidden
    float los_age_ms = 1e9f;    // age of that verdict
};
inline constexpr uint8_t kLosUnknown = 0, kLosVisible = 1, kLosHidden = 2;

struct EspOptions {
    float width = 0, height = 0;
    float max_dist = 300;
    float heads_within_m = 200;  // bones read for every character this near...
    float head_cone_deg = 15;    // ... and, farther, within this angle of the view axis
    bool enemy_heads = false;    // every enemy's, off the screen too (the magic bullets aim at them)
    bool civilians = true;
    bool dead = true;
};

// The player's view-angle accumulator, read every frame.
struct AimState {
    bool ok = false;
    uintptr_t comp = 0;  // aim component address
    float yaw = 0, pitch = 0, pitch_min = 0, pitch_max = 0;  // radians, as stored
};

// The weapon in hand as the game's menu describes it (model.h, kWeaponPartNamesOff and after).
struct WeaponView {
    bool parts_ok = false;
    uint32_t parts = 0;
    char slot[kMaxParts][24] = {};   // UTF-8, as the menu shows it
    char part[kMaxParts][48] = {};
    bool stats_ok = false;
    float base[kStatCount] = {}, add[kStatCount] = {}, mul[kStatCount] = {};  // menu value = base * mul + add
    float damage = 0;    // per bullet ([M + 0x78]), 0 = unknown
    int magazine = 0;    // rounds, 0 = unknown
    float spread_live = -1;  // the shooter's spread attribute now (what the next shot will use), -1 = unknown
    float spread_factor = 1;  // the spread mod applied at each shot (set by the runtime)
    uintptr_t spread_comp = 0, ballistics = 0, request = 0;  // for the weapon mods
    uintptr_t logic = 0;  // W, the weapon logic object (the fire handler's rsi)
    uintptr_t shooter = 0;  // S = [W + 0x110] resolved (model.h)
    float Stat(size_t i) const { return base[i] * mul[i] + add[i]; }
};

struct EspFrame {
    bool camera_ok = false;
    float fov_deg = 0;
    Vec3 camera_pos;
    CameraView camera;
    AimState aim;
    float weapon_speed = 0;   // m/s from the active weapon's data, 0 = unknown
    float weapon_damage = 0;
    uint64_t weapon_id = 0;   // the weapon objects in hand (changes on a switch), 0 = unknown
    uintptr_t weapon_state = 0;  // the player's weapon state component (model.h)
    // its bullets (model.h, kWeaponBallisticsOff): V and the first segment, the parts included
    bool bullet_ok = false;
    float bullet_V = 0, bullet_first_segment = 0;  // what the bullets use (the shooter's request, else M)
    float bullet_V_table = 0;  // the game's own V (without the speed mod): the learned paths are keyed by it
    float bullet_V_block = 0;  // the ballistics block's copy of V (the mods leave it alone), 0 = unknown
    bool bullet_straight = false;  // the straight-bullets mod holds (set by the runtime): no drop at all
    bool bullet_teleport = false;  // and the teleport mod: no flight time either
    // the shooter's spread (kShooterSpreadOff): the launch vector's length depends on it
    bool spread_ok = false;
    float spread = 0, spread_range = 0;  // the spread of the last shot (component +0x28) and its distance cap
    WeaponView weapon;
    bool from_list = false;   // characters from the game's entity list (else from the fallback scan)
    bool player_ok = false;   // the player's character was among them
    Vec3 player_feet;
    bool player_head_ok = false;
    Vec3 player_head;         // from its bones: the bullets' origin in third person
    uint32_t known = 0;     // character objects from the list (or the last scan)
    uint32_t active = 0;    // valid, not at the origin, within range
    uint32_t on_screen = 0;
    uint32_t heads = 0;     // pose read and head found
    std::vector<EspItem> items;
    // characters in range but off the screen: no box, no pose read (the magic bullets target them by their root)
    struct OffscreenChar {
        uintptr_t addr;
        Vec3 feet;
        Category cat;
        bool vehicle;
        bool head_ok;  // read from its bones (EspOptions::enemy_heads)
        Vec3 head;
    };
    std::vector<OffscreenChar> offscreen;
};

// Finds the characters through the game's entity list (EntityRegistry) on its own thread, every
// list_interval_ms; the camera through the image's pointer (render thread) and the weapon state through the
// player's entity. Memory scans for their vtables are only the fallback while one of these chains fails
// (another game build, loading): hot regions every scan_interval_ms, all writable memory every 30 s.
// BuildFrame reads them on the render thread: entity flags for every character in range, bones only for
// those that survive screen culling.
class World {
public:
    void Start(Telemetry* tel, DWORD scan_interval_ms, DWORD list_interval_ms);
    void Stop();
    void SetIntervals(DWORD scan_ms, DWORD list_ms) {
        interval_ms_.store(scan_ms, std::memory_order_relaxed);
        list_interval_ms_.store(list_ms, std::memory_order_relaxed);
    }
    void BuildFrame(const EspOptions& opt, EspFrame& out, StageStats& stats);

private:
    struct ScanState;
    void Run();
    void Scan(ScanState& s, bool camera_missing);
    void ReadAim(uintptr_t entity, AimState& aim) const;
    void ReadWeapon(EspFrame& out) const;
    void ReadBullets(uintptr_t weapon, EspFrame& out) const;
    void ReadWeaponView(uintptr_t weapon, uintptr_t data, EspFrame& out) const;
    // the parts' names, refreshed every 500 ms (render thread)
    mutable uintptr_t names_weapon_ = 0;
    mutable double names_ms_ = 0;
    mutable WeaponView names_;

    Telemetry* tel_ = nullptr;
    uintptr_t base_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> camera_missing_{false};  // render thread: neither the image's pointer nor the scan's camera reads
    std::atomic<DWORD> interval_ms_{1000};
    std::atomic<DWORD> list_interval_ms_{100};
    HANDLE wake_ = nullptr;
    EntityRegistry registry_;  // world thread

    SRWLOCK lock_ = SRWLOCK_INIT;  // guards the published result
    uintptr_t camera_ = 0;         // from the fallback scan
    uintptr_t weapon_state_ = 0;
    std::vector<uintptr_t> chars_;
    bool from_list_ = false;
    uint64_t generation_ = 0;

    // render thread copy
    uint64_t seen_generation_ = ~0ull;
    uintptr_t r_camera_ = 0;
    uintptr_t r_weapon_state_ = 0;
    bool r_from_list_ = false;
    std::vector<uintptr_t> r_chars_;
    std::vector<float> bones_;
};

}  // namespace grw::game
