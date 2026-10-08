#pragma once
#include <windows.h>
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
    // Research: find each fired bullet in memory and record its flight (needs the execute breakpoint on the
    // fire function, base + 0x1471F8E0).
    int bullet_probe = 0;
    // Learn the bullet paths from the player's projectiles (instruction breakpoint on DR1, one exception per
    // simulation step of every projectile in the game): 0 = off, 1 = always, 2 = only while learning is useful
    // (weapon in hand not fired yet since injection, no learned path, or aiming beyond the learned range).
    int proj_track = 2;
    float esp_max_distance_m = 600.0f;
    int esp_enabled = 1;
    int esp_key = 0x75;  // VK_F6 toggles the ESP
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

private:
    std::wstring path_;
    FILETIME last_write_{};
    bool loaded_once_ = false;
    Config current_;
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
};

}  // namespace grw
