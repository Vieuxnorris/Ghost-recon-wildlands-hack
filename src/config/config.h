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
    float aim_tau_ms = 80.0f;
    float aim_cone_deg = 8.0f;
    float aim_max_speed_deg_s = 360.0f;
    float aim_deadzone_frac = 0.3f;
    float aim_hidden_ms = 150.0f;
    float esp_max_distance_m = 300.0f;
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
