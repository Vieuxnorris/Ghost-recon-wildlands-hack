#include "config/config.h"
#include <charconv>
#include <fstream>
#include <sstream>

namespace grw {

namespace {

std::string_view Trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

bool ParseInt(std::string_view v, int& out) {
    int base = 10;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v.remove_prefix(2);
        base = 16;
    }
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out, base);
    return r.ec == std::errc{} && r.ptr == v.data() + v.size();
}

bool ParseU64(std::string_view v, unsigned long long& out) {
    int base = 10;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v.remove_prefix(2);
        base = 16;
    }
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out, base);
    return r.ec == std::errc{} && r.ptr == v.data() + v.size();
}

bool ParseFloat(std::string_view v, float& out) {
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc{} && r.ptr == v.data() + v.size();
}

bool Assign(Config& c, std::string_view key, std::string_view v) {
    if (key == "menu_key") return ParseInt(v, c.menu_key);
    if (key == "panel_key") return ParseInt(v, c.panel_key);
    if (key == "unload_key") return ParseInt(v, c.unload_key);
    if (key == "aim_key") return ParseInt(v, c.aim_key);
    if (key == "aim_enabled") return ParseInt(v, c.aim_enabled);
    if (key == "aim_tau_ms") return ParseFloat(v, c.aim_tau_ms);
    if (key == "aim_cone_deg") return ParseFloat(v, c.aim_cone_deg);
    if (key == "aim_max_speed_deg_s") return ParseFloat(v, c.aim_max_speed_deg_s);
    if (key == "aim_deadzone_frac") return ParseFloat(v, c.aim_deadzone_frac);
    if (key == "aim_hidden_ms") return ParseFloat(v, c.aim_hidden_ms);
    if (key == "aim_bullet_speed") return ParseFloat(v, c.aim_bullet_speed) && c.aim_bullet_speed >= 0.0f;
    if (key == "aim_gravity") return ParseFloat(v, c.aim_gravity);
    if (key == "aim_zero_m") return ParseFloat(v, c.aim_zero_m) && c.aim_zero_m >= 0.0f;
    if (key == "aim_lead") return ParseInt(v, c.aim_lead);
    if (key == "hwbp_addr") return ParseU64(v, c.hwbp_addr);
    if (key == "hwbp_addr2") return ParseU64(v, c.hwbp_addr2);
    if (key == "hwbp_addr3") return ParseU64(v, c.hwbp_addr3);
    if (key == "hwbp_addr4") return ParseU64(v, c.hwbp_addr4);
    if (key == "hwbp_len")
        return ParseInt(v, c.hwbp_len) && (c.hwbp_len == 1 || c.hwbp_len == 2 || c.hwbp_len == 4 || c.hwbp_len == 8);
    if (key == "hwbp_max_hits") return ParseInt(v, c.hwbp_max_hits) && c.hwbp_max_hits > 0;
    if (key == "hwbp_read") return ParseInt(v, c.hwbp_read) && c.hwbp_read >= 0 && c.hwbp_read <= 2;
    if (key == "hwbp_trigger") return ParseInt(v, c.hwbp_trigger) && c.hwbp_trigger >= -1 && c.hwbp_trigger <= 3;
    if (key == "hwbp_window_ms")
        return ParseInt(v, c.hwbp_window_ms) && c.hwbp_window_ms >= 1 && c.hwbp_window_ms <= 60000;
    if (key == "bullet_probe") return ParseInt(v, c.bullet_probe);
    if (key == "los_test") return ParseInt(v, c.los_test) && (c.los_test == 0 || c.los_test == 1);
    if (key == "los_enabled") return ParseInt(v, c.los_enabled) && (c.los_enabled == 0 || c.los_enabled == 1);
    if (key == "los_interval_ms")
        return ParseInt(v, c.los_interval_ms) && c.los_interval_ms >= 5 && c.los_interval_ms <= 1000;
    if (key == "los_debug") return ParseInt(v, c.los_debug) && (c.los_debug == 0 || c.los_debug == 1);
    if (key == "magic_debug") return ParseInt(v, c.magic_debug) && (c.magic_debug == 0 || c.magic_debug == 1);
    if (key == "los_flag") return ParseInt(v, c.los_flag) && (c.los_flag == 0 || c.los_flag == 1);
    if (key == "los_mask") return ParseU64(v, c.los_mask) && c.los_mask != 0;
    if (key == "los_max_rays") return ParseInt(v, c.los_max_rays) && c.los_max_rays >= 1 && c.los_max_rays <= 256;
    if (key == "proj_track") return ParseInt(v, c.proj_track) && c.proj_track >= 0 && c.proj_track <= 2;
    if (key == "mod_spread") return ParseFloat(v, c.mod_spread) && c.mod_spread >= 0.0f && c.mod_spread <= 4.0f;
    if (key == "mod_bullet_speed")
        return ParseFloat(v, c.mod_bullet_speed) && c.mod_bullet_speed >= 0.1f && c.mod_bullet_speed <= 10.0f;
    if (key == "mod_damage") return ParseFloat(v, c.mod_damage) && c.mod_damage >= 0.0f && c.mod_damage <= 100.0f;
    if (key == "mod_recoil") return ParseFloat(v, c.mod_recoil) && c.mod_recoil >= 0.0f && c.mod_recoil <= 4.0f;
    if (key == "mod_instant_bullets")
        return ParseInt(v, c.mod_instant_bullets) && c.mod_instant_bullets >= 0 && c.mod_instant_bullets <= 1;
    if (key == "mod_bullets_per_shot")
        return ParseInt(v, c.mod_bullets_per_shot) && c.mod_bullets_per_shot >= 1 && c.mod_bullets_per_shot <= 32;
    if (key == "mod_infinite_ammo")
        return ParseInt(v, c.mod_infinite_ammo) && (c.mod_infinite_ammo == 0 || c.mod_infinite_ammo == 1);
    if (key == "mod_straight_bullets")
        return ParseInt(v, c.mod_straight_bullets) && c.mod_straight_bullets >= 0 && c.mod_straight_bullets <= 1;
    if (key == "mod_teleport_bullets")
        return ParseInt(v, c.mod_teleport_bullets) && c.mod_teleport_bullets >= 0 && c.mod_teleport_bullets <= 1;
    if (key == "mod_magic_bullets")
        return ParseInt(v, c.mod_magic_bullets) && c.mod_magic_bullets >= 0 && c.mod_magic_bullets <= 1;
    if (key == "magic_target") return ParseInt(v, c.magic_target) && (c.magic_target == 0 || c.magic_target == 1);
    if (key == "magic_spread") return ParseInt(v, c.magic_spread) && (c.magic_spread == 0 || c.magic_spread == 1);
    if (key == "magic_fov_deg")
        return ParseFloat(v, c.magic_fov_deg) && c.magic_fov_deg >= 1.0f && c.magic_fov_deg <= 180.0f;
    if (key == "magic_max_m") return ParseFloat(v, c.magic_max_m) && c.magic_max_m >= 10.0f && c.magic_max_m <= 5000.0f;
    if (key == "magic_walls") return ParseInt(v, c.magic_walls) && (c.magic_walls == 0 || c.magic_walls == 1);
    if (key == "magic_start_m")
        return ParseFloat(v, c.magic_start_m) && c.magic_start_m >= 2.0f && c.magic_start_m <= 40.0f;
    if (key == "magic_vertical_est")
        return ParseInt(v, c.magic_vertical_est) && (c.magic_vertical_est == 0 || c.magic_vertical_est == 1);
    if (key == "magic_pierce") return ParseInt(v, c.magic_pierce) && (c.magic_pierce == 0 || c.magic_pierce == 1);
    if (key == "esp_max_distance_m") return ParseFloat(v, c.esp_max_distance_m);
    if (key == "esp_enabled") return ParseInt(v, c.esp_enabled);
    if (key == "esp_key") return ParseInt(v, c.esp_key);
    if (key == "tp_key") return ParseInt(v, c.tp_key) && c.tp_key > 0 && c.tp_key <= 0xFF;
    if (key == "invisible") return ParseInt(v, c.invisible) && (c.invisible == 0 || c.invisible == 1);
    if (key == "invisible_key")
        return ParseInt(v, c.invisible_key) && c.invisible_key > 0 && c.invisible_key <= 0xFF;
    if (key == "esp_civilians") return ParseInt(v, c.esp_civilians);
    if (key == "esp_dead") return ParseInt(v, c.esp_dead);
    if (key == "esp_list_interval_ms")
        return ParseInt(v, c.esp_list_interval_ms) && c.esp_list_interval_ms >= 20 && c.esp_list_interval_ms <= 5000;
    if (key == "esp_scan_interval_ms") return ParseInt(v, c.esp_scan_interval_ms) && c.esp_scan_interval_ms >= 250;
    if (key == "overlay_mode") return ParseInt(v, c.overlay_mode) && c.overlay_mode >= 0 && c.overlay_mode <= 2;
    if (key == "hook_method") return ParseInt(v, c.hook_method) && c.hook_method >= 0 && c.hook_method <= 3;
    return false;
}

}  // namespace

ParseResult ParseConfig(std::string_view text, Config& inout) {
    Config next = inout;
    int line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) return {false, line_no, "missing '='"};
        const auto key = Trim(line.substr(0, eq));
        const auto value = Trim(line.substr(eq + 1));
        if (value.empty() || !Assign(next, key, value))
            return {false, line_no, "bad key or value: " + std::string(key)};
    }
    inout = next;
    return {};
}

std::string ConfigText(const Config& d) {
    std::ostringstream s;
    s << "# grw.ini - reloaded automatically when saved. Keys are Windows virtual-key codes.\n"
      << "menu_key = 0x" << std::hex << d.menu_key << "\n"
      << "panel_key = 0x" << d.panel_key << "\n"
      << "unload_key = 0x" << d.unload_key << "\n"
      << "aim_key = 0x" << d.aim_key << std::dec << "\n"
      << "aim_enabled = " << d.aim_enabled << "\n"
      << "aim_tau_ms = " << d.aim_tau_ms << "\n"
      << "aim_cone_deg = " << d.aim_cone_deg << "\n"
      << "aim_max_speed_deg_s = " << d.aim_max_speed_deg_s << "\n"
      << "aim_deadzone_frac = " << d.aim_deadzone_frac << "\n"
      << "aim_hidden_ms = " << d.aim_hidden_ms << "\n"
      << "# ballistics: bullet speed m/s (0 = the active weapon's value read from the game), gravity m/s2,\n"
      << "# sight zero distance m, lead moving targets\n"
      << "aim_bullet_speed = " << d.aim_bullet_speed << "\n"
      << "aim_gravity = " << d.aim_gravity << "\n"
      << "aim_zero_m = " << d.aim_zero_m << "\n"
      << "aim_lead = " << d.aim_lead << "\n"
      << "esp_max_distance_m = " << d.esp_max_distance_m << "\n"
      << "esp_enabled = " << d.esp_enabled << "\n"
      << "esp_key = 0x" << std::hex << d.esp_key << std::dec << "\n"
      << "# teleport to the map waypoint (on foot)\n"
      << "tp_key = 0x" << std::hex << d.tp_key << std::dec << "\n"
      << "# invisibility: no enemy sees the player (1 = on at injection), and its key\n"
      << "invisible = " << d.invisible << "\n"
      << "invisible_key = 0x" << std::hex << d.invisible_key << std::dec << "\n"
      << "esp_civilians = " << d.esp_civilians << "\n"
      << "esp_dead = " << d.esp_dead << "\n"
      << "# how often the game's entity list is read (ms, 20-5000): a new character appears within this\n"
      << "esp_list_interval_ms = " << d.esp_list_interval_ms << "\n"
      << "# fallback when the list can't be read: hot-region rescans (ms, >= 250), full memory scans every 30 s\n"
      << "esp_scan_interval_ms = " << d.esp_scan_interval_ms << "\n"
      << "# line of sight through the game's bullet rays (camera -> heads): 0 = off (every enemy visible)\n"
      << "los_enabled = " << d.los_enabled << "\n"
      << "los_interval_ms = " << d.los_interval_ms << "\n"
      << "los_max_rays = " << d.los_max_rays << "\n"
      << "# collision layers of the rays: the bullets' without 25 (the characters' sensor spheres)\n"
      << "los_mask = 0x" << std::hex << d.los_mask << std::dec << "\n"
      << "# filter flag: 1 like the bullets (cars then hide nothing), 0 = vehicles block\n"
      << "los_flag = " << d.los_flag << "\n"
      << "# learn bullet paths from the player's shots: 0 off, 1 always, 2 only while useful\n"
      << "proj_track = " << d.proj_track << "\n"
      << "# weapon mods, factors of the game's values (1 = unchanged): spread at each shot (0 = none, the bullets\n"
      << "# then also fly at full speed), bullet speed, damage per bullet. Also adjustable in the menu.\n"
      << "mod_spread = " << d.mod_spread << "\n"
      << "mod_bullet_speed = " << d.mod_bullet_speed << "\n"
      << "mod_recoil = " << d.mod_recoil << "\n"
      << "# 1 = the bullets reach their target almost at once (30000 m/s), whatever mod_bullet_speed says\n"
      << "mod_instant_bullets = " << d.mod_instant_bullets << "\n"
      << "# 1 = firing consumes no ammo and reloads are free (the game's own flag: every weapon, the NPCs' too)\n"
      << "mod_infinite_ammo = " << d.mod_infinite_ammo << "\n"
      << "# 1..32 bullets per shot of the player's weapon (with magic bullets and spread 1: one per enemy)\n"
      << "mod_bullets_per_shot = " << d.mod_bullets_per_shot << "\n"
      << "# 1 = no drop: the bullets fly in a straight line (with instant bullets: a laser)\n"
      << "mod_straight_bullets = " << d.mod_straight_bullets << "\n"
      << "# 1 = the bullets hit at once along that straight line, to the weapon's range (a hitscan)\n"
      << "mod_teleport_bullets = " << d.mod_teleport_bullets << "\n"
      << "# 1 = while the aim holds a target, the bullets jump to its head through vehicles and walls\n"
      << "mod_magic_bullets = " << d.mod_magic_bullets << "\n"
      << "# magic bullets: target 0 = the aim's locked target, 1 = any enemy; spread 1 = one bullet per enemy;\n"
      << "# fov (deg) and max distance (m) of the targets; walls 1 = look for a clear way to each head;\n"
      << "# start_m = length of the last test before the head; vertical_est 1 = a head not read: from above;\n"
      << "# pierce 1 = through anything on the way (vehicles' bodies, walls)\n"
      << "magic_target = " << d.magic_target << "\n"
      << "magic_spread = " << d.magic_spread << "\n"
      << "magic_fov_deg = " << d.magic_fov_deg << "\n"
      << "magic_max_m = " << d.magic_max_m << "\n"
      << "magic_walls = " << d.magic_walls << "\n"
      << "magic_start_m = " << d.magic_start_m << "\n"
      << "magic_vertical_est = " << d.magic_vertical_est << "\n"
      << "magic_pierce = " << d.magic_pierce << "\n"
      << "mod_damage = " << d.mod_damage << "\n"
      << "# 0 = hooks only, 1 = ImGui frame without rendering, 2 = full overlay\n"
      << "overlay_mode = " << d.overlay_mode << "\n"
      << "# read at injection: 0 = inline patch (MinHook), 1 = dxgi vtable slots,\n"
      << "# diagnostics: 2 = throwaway swap chain only, no hooks; 3 = nothing (threads only)\n"
      << "hook_method = " << d.hook_method << "\n"
      << "# research (0 = off): hardware breakpoints, bullet probe, ray tests\n"
      << "hwbp_addr = 0x" << std::hex << d.hwbp_addr << "\n"
      << "hwbp_addr2 = 0x" << d.hwbp_addr2 << "\n"
      << "hwbp_addr3 = 0x" << d.hwbp_addr3 << "\n"
      << "hwbp_addr4 = 0x" << d.hwbp_addr4 << std::dec << "\n"
      << "hwbp_read = " << d.hwbp_read << "\n"
      << "hwbp_len = " << d.hwbp_len << "\n"
      << "hwbp_max_hits = " << d.hwbp_max_hits << "\n"
      << "hwbp_trigger = " << d.hwbp_trigger << "\n"
      << "hwbp_window_ms = " << d.hwbp_window_ms << "\n"
      << "bullet_probe = " << d.bullet_probe << "\n"
      << "los_test = " << d.los_test << "\n"
      << "los_debug = " << d.los_debug << "\n"
      << "magic_debug = " << d.magic_debug << "\n";
    return s.str();
}

std::string DefaultConfigText() { return ConfigText(Config{}); }

ConfigStore::ConfigStore(std::wstring path) : path_(std::move(path)) {}

ReloadResult ConfigStore::Poll() {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &attr)) {
        std::ofstream(path_, std::ios::binary) << DefaultConfigText();
        if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &attr))
            return {ReloadResult::Error, {false, 0, "cannot create config file"}};
    }
    if (loaded_once_ && CompareFileTime(&attr.ftLastWriteTime, &last_write_) == 0 && !force_.exchange(false)) return {};
    last_write_ = attr.ftLastWriteTime;
    loaded_once_ = true;

    std::ifstream in(path_, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Config next = Snapshot();
    ParseResult r = ParseConfig(text, next);
    if (!r.ok) return {ReloadResult::Error, r};
    AcquireSRWLockExclusive(&lock_);
    current_ = next;
    ReleaseSRWLockExclusive(&lock_);
    return {ReloadResult::Applied, r};
}

void ConfigStore::Set(const Config& c) {
    AcquireSRWLockExclusive(&lock_);
    current_ = c;
    ReleaseSRWLockExclusive(&lock_);
}

bool ConfigStore::Save() {
    const std::string text = ConfigText(Snapshot());
    const std::wstring tmp = path_ + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

Config ConfigStore::Snapshot() const {
    AcquireSRWLockShared(&lock_);
    const Config c = current_;
    ReleaseSRWLockShared(&lock_);
    return c;
}

}  // namespace grw
