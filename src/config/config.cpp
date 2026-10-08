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

bool ParseFloat(std::string_view v, float& out) {
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc{} && r.ptr == v.data() + v.size();
}

bool Assign(Config& c, std::string_view key, std::string_view v) {
    if (key == "menu_key") return ParseInt(v, c.menu_key);
    if (key == "panel_key") return ParseInt(v, c.panel_key);
    if (key == "unload_key") return ParseInt(v, c.unload_key);
    if (key == "aim_key") return ParseInt(v, c.aim_key);
    if (key == "aim_tau_ms") return ParseFloat(v, c.aim_tau_ms);
    if (key == "aim_cone_deg") return ParseFloat(v, c.aim_cone_deg);
    if (key == "aim_max_speed_deg_s") return ParseFloat(v, c.aim_max_speed_deg_s);
    if (key == "aim_deadzone_frac") return ParseFloat(v, c.aim_deadzone_frac);
    if (key == "aim_hidden_ms") return ParseFloat(v, c.aim_hidden_ms);
    if (key == "esp_max_distance_m") return ParseFloat(v, c.esp_max_distance_m);
    if (key == "esp_enabled") return ParseInt(v, c.esp_enabled);
    if (key == "esp_key") return ParseInt(v, c.esp_key);
    if (key == "esp_civilians") return ParseInt(v, c.esp_civilians);
    if (key == "esp_dead") return ParseInt(v, c.esp_dead);
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

std::string DefaultConfigText() {
    const Config d;
    std::ostringstream s;
    s << "# grw.ini - reloaded automatically when saved. Keys are Windows virtual-key codes.\n"
      << "menu_key = 0x" << std::hex << d.menu_key << "\n"
      << "panel_key = 0x" << d.panel_key << "\n"
      << "unload_key = 0x" << d.unload_key << "\n"
      << "aim_key = 0x" << d.aim_key << std::dec << "\n"
      << "aim_tau_ms = " << d.aim_tau_ms << "\n"
      << "aim_cone_deg = " << d.aim_cone_deg << "\n"
      << "aim_max_speed_deg_s = " << d.aim_max_speed_deg_s << "\n"
      << "aim_deadzone_frac = " << d.aim_deadzone_frac << "\n"
      << "aim_hidden_ms = " << d.aim_hidden_ms << "\n"
      << "esp_max_distance_m = " << d.esp_max_distance_m << "\n"
      << "esp_enabled = " << d.esp_enabled << "\n"
      << "esp_key = 0x" << std::hex << d.esp_key << std::dec << "\n"
      << "esp_civilians = " << d.esp_civilians << "\n"
      << "esp_dead = " << d.esp_dead << "\n"
      << "# how often the character list is refreshed (ms, >= 250); a full memory scan runs every 30 s\n"
      << "esp_scan_interval_ms = " << d.esp_scan_interval_ms << "\n"
      << "# 0 = hooks only, 1 = ImGui frame without rendering, 2 = full overlay\n"
      << "overlay_mode = " << d.overlay_mode << "\n"
      << "# read at injection: 0 = inline patch (MinHook), 1 = dxgi vtable slots,\n"
      << "# diagnostics: 2 = throwaway swap chain only, no hooks; 3 = nothing (threads only)\n"
      << "hook_method = " << d.hook_method << "\n";
    return s.str();
}

ConfigStore::ConfigStore(std::wstring path) : path_(std::move(path)) {}

ReloadResult ConfigStore::Poll() {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &attr)) {
        std::ofstream(path_, std::ios::binary) << DefaultConfigText();
        if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &attr))
            return {ReloadResult::Error, {false, 0, "cannot create config file"}};
    }
    if (loaded_once_ && CompareFileTime(&attr.ftLastWriteTime, &last_write_) == 0) return {};
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

Config ConfigStore::Snapshot() const {
    AcquireSRWLockShared(&lock_);
    const Config c = current_;
    ReleaseSRWLockShared(&lock_);
    return c;
}

}  // namespace grw
