#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <string_view>

namespace grw {

struct Config {
    int menu_key = 0x2D;    // VK_INSERT
    int panel_key = 0x77;   // VK_F8
    int unload_key = 0x23;  // VK_END
    int aim_key = 0x06;     // VK_XBUTTON2
    int aim_enabled = 1;
    float aim_tau_ms = 80.0f;
    float aim_cone_deg = 8.0f;
    float aim_max_speed_deg_s = 360.0f;
    float aim_deadzone_frac = 0.3f;
    float aim_hidden_ms = 150.0f;
    // Ballistics (0 = off): bullet speed in m/s, gravity in m/s2, sight zero distance in m, lead moving targets.
    float aim_bullet_speed = 0.0f;
    float aim_gravity = 9.81f;
    float aim_zero_m = 0.0f;
    int aim_lead = 1;
    // Research: hardware write watchpoint (debug registers) on a game address; 0 = off.
    unsigned long long hwbp_addr = 0, hwbp_addr2 = 0, hwbp_addr3 = 0, hwbp_addr4 = 0;  // DR0-DR3
    int hwbp_len = 4;
    int hwbp_max_hits = 8;
    int hwbp_read = 0;  // 0 write, 1 read or write, 2 execute (hwbp_addr = an instruction)
    // Research: a hit of this slot (0-3 = DR0-DR3) opens a window of hwbp_window_ms during which the other
    // slots are recorded; outside it they are ignored and not counted against hwbp_max_hits. -1 = record all.
    int hwbp_trigger = -1;
    int hwbp_window_ms = 1500;
    // Research: find each fired bullet in memory and record its flight (needs the execute breakpoint on the
    // fire function, base + 0x1471F8E0).
    int bullet_probe = 0;
    // Research: every 500 ms, one ray through the game's physics (bullet filter) from the camera along its
    // axis, 1000 m, logged as "ray_test".
    int los_test = 0;
    // Line of sight (M6): rays through the game's physics with the bullets' filter, camera -> heads, on their
    // own thread every los_interval_ms, at most los_max_rays per pass (enemies near the crosshair first).
    // 0 = off: every enemy counts as visible, as before M6.
    int los_enabled = 1;
    int los_interval_ms = 33;
    int los_max_rays = 40;
    // the bullets' collision layers without 25 (the characters' sensor spheres), camera-findings.md section 9
    unsigned long long los_mask = 0x3115fd67full;
    int los_flag = 0;  // the filter context's flag: 1 like the bullets, which then miss the vehicles
    int los_debug = 0;  // research: los_detail / los_hit telemetry
    int magic_debug = 0;  // research: 1 = magic_seg / magic_hit / magic_kept lines (each magic bullet's collision test)
    // Learn the bullet paths from the player's projectiles (instruction breakpoint on DR1, one exception per
    // simulation step of every projectile in the game): 0 = off, 1 = always, 2 = only while learning is useful
    // (weapon in hand not fired yet since injection, no learned path, or aiming beyond the learned range).
    int proj_track = 2;
    // Weapon mods (offline, the player's weapon only), as factors of the game's values: 1 = unchanged.
    // mod_spread scales the spread at each shot (instruction breakpoint): 0 = none at all, and then the bullets
    // also fly at full speed (camera-findings.md section 7b). The other two rewrite the weapon's ballistics block.
    float mod_spread = 1.0f;
    float mod_bullet_speed = 1.0f;
    float mod_damage = 1.0f;
    float mod_recoil = 1.0f;  // the camera's recoil, scaled every frame (instruction breakpoint): 0 = none
    int mod_instant_bullets = 0;  // 1 = the bullets fly at kInstantBulletSpeed (overrides mod_bullet_speed)
    int mod_infinite_ammo = 0;    // 1 = firing consumes no ammo, reloads are free (the game's own flag: every weapon)
    int mod_bullets_per_shot = 1;  // 1..32: each shot of the player's weapon fires this many bullets (a shotgun's way)
    int mod_straight_bullets = 0;  // 1 = no drop: the player's bullets fly in a straight line (tracker breakpoint)
    int mod_teleport_bullets = 0;  // 1 = they reach the end of that line at their first step (a hitscan)
    int mod_magic_bullets = 0;  // 1 = the bullets jump to their targets' heads (the magic_* options)
    // Magic bullets: 0 = the aim's locked target only (aim key held), 1 = any enemy (no aiming needed).
    int magic_target = 1;
    int magic_spread = 1;           // 1 = one bullet per enemy in turn, 0 = every bullet on the best target
    float magic_fov_deg = 180.0f;   // only targets this close to the crosshair (180 = all around)
    float magic_max_m = 1200.0f;    // and this near (within the ESP's range: esp_max_distance_m)
    int magic_walls = 1;            // 1 = look for a clear way to each head (from above, the sides): through walls
    float magic_start_m = 12.0f;    // the last collision test starts this far from the head (outside its 10 m sphere)
    int magic_vertical_est = 1;     // 1 = a head not read (far, off the screen) is tested from above, down its body
    int magic_pierce = 1;           // 1 = they go through anything on the way (vehicles' bodies, walls), damage kept
    float esp_max_distance_m = 600.0f;
    int esp_enabled = 1;
    int esp_key = 0x75;  // VK_F6 toggles the ESP
    int tp_key = 0x74;   // VK_F5: teleport to the map waypoint
    int invisible = 0;       // 1 = no enemy sees the player (its visibility multipliers at 0)
    int invisible_key = 0x76;  // VK_F7 toggles it
    int esp_civilians = 1;
    int esp_dead = 1;
    int esp_list_interval_ms = 100;   // reads of the game's entity list (new characters appear within this)
    int esp_scan_interval_ms = 1000;  // fallback only: hot-region rescans; full scans every 30 s
    int overlay_mode = 2;  // 0 = hooks only, 1 = ImGui frame without rendering, 2 = full overlay
    // Read at injection: 0 = inline patch (MinHook), 1 = dxgi vtable slots.
    // Diagnostics: 2 = throwaway swap chain only, no hooks; 3 = nothing at all (threads only).
    int hook_method = 0;
};

struct ParseResult {
    bool ok = true;
    int line = 0;
    std::string error;
};

// All or nothing: on error, inout is left untouched.
ParseResult ParseConfig(std::string_view text, Config& inout);
std::string DefaultConfigText();
// The text of grw.ini for these values (every key, with its comment).
std::string ConfigText(const Config& c);

struct ReloadResult {
    enum Kind { Unchanged, Applied, Error } kind = Unchanged;
    ParseResult parse;
};

// Poll() from one thread (the telemetry writer); Snapshot() from any thread.
class ConfigStore {
public:
    explicit ConfigStore(std::wstring path);
    ReloadResult Poll();
    Config Snapshot() const;
    // Any thread: the values in use from now (the menu's edits); Save() writes them to the file.
    void Set(const Config& c);
    bool Save();
    void Invalidate() { force_.store(true); }  // any thread: the next Poll reads the file even if unchanged

private:
    std::wstring path_;
    FILETIME last_write_{};
    bool loaded_once_ = false;
    std::atomic<bool> force_{false};
    Config current_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
};

}  // namespace grw
