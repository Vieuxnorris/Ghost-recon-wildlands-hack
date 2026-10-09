#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include "config/config.h"

using namespace grw;

TEST_CASE("ParseConfig reads keys, floats, hex and comments") {
    Config c;
    const auto r = ParseConfig("# comment\n\n aim_key = 0x05 \naim_tau_ms=120.5\nmenu_key = 45 # trailing\n", c);
    REQUIRE(r.ok);
    CHECK(c.aim_key == 5);
    CHECK(c.aim_tau_ms == 120.5f);
    CHECK(c.menu_key == 45);
    CHECK(c.aim_cone_deg == Config{}.aim_cone_deg);  // absent key keeps its value
}

TEST_CASE("ParseConfig reads overlay_mode and rejects values outside 0..2") {
    Config c;
    CHECK(c.overlay_mode == 2);
    REQUIRE(ParseConfig("overlay_mode = 0\n", c).ok);
    CHECK(c.overlay_mode == 0);
    CHECK_FALSE(ParseConfig("overlay_mode = 3\n", c).ok);
    CHECK(c.overlay_mode == 0);
}

TEST_CASE("ParseConfig reads the hwbp trigger window and rejects out-of-range values") {
    Config c;
    CHECK(c.hwbp_trigger == -1);
    REQUIRE(ParseConfig("hwbp_trigger = 1\nhwbp_window_ms = 2000\n", c).ok);
    CHECK(c.hwbp_trigger == 1);
    CHECK(c.hwbp_window_ms == 2000);
    CHECK_FALSE(ParseConfig("hwbp_trigger = 4\n", c).ok);
    CHECK_FALSE(ParseConfig("hwbp_window_ms = 0\n", c).ok);
}

TEST_CASE("ParseConfig reads hook_method and rejects values outside 0..3") {
    Config c;
    CHECK(c.hook_method == 0);
    REQUIRE(ParseConfig("hook_method = 3\n", c).ok);
    CHECK(c.hook_method == 3);
    CHECK_FALSE(ParseConfig("hook_method = 4\n", c).ok);
    CHECK(c.hook_method == 3);
}

TEST_CASE("ParseConfig rejects the whole file on an unknown key") {
    Config c;
    c.aim_tau_ms = 99.0f;
    const auto r = ParseConfig("aim_tau_ms = 50\nbogus = 1\n", c);
    CHECK_FALSE(r.ok);
    CHECK(r.line == 2);
    CHECK(c.aim_tau_ms == 99.0f);
}

TEST_CASE("ParseConfig rejects malformed and truncated values") {
    Config c;
    CHECK_FALSE(ParseConfig("aim_tau_ms = abc\n", c).ok);
    CHECK_FALSE(ParseConfig("aim_tau_ms = \n", c).ok);
    CHECK_FALSE(ParseConfig("aim_tau_ms\n", c).ok);
    CHECK_FALSE(ParseConfig("aim_key = 0xZZ\n", c).ok);
    CHECK(c.aim_tau_ms == Config{}.aim_tau_ms);
}

TEST_CASE("DefaultConfigText parses back to the defaults") {
    Config c;
    c.aim_tau_ms = 1.0f;
    REQUIRE(ParseConfig(DefaultConfigText(), c).ok);
    CHECK(c.aim_tau_ms == Config{}.aim_tau_ms);
}

namespace {
void WriteFileText(const std::filesystem::path& p, const std::string& s, int bump_seconds) {
    { std::ofstream(p, std::ios::binary | std::ios::trunc) << s; }
    // Force a distinct write time even when two writes land in the same tick.
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
    u.QuadPart += ULONGLONG(bump_seconds) * 10'000'000ull;
    ft = {u.LowPart, u.HighPart};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
}  // namespace

TEST_CASE("ConfigStore creates, reloads and survives a broken file") {
    const auto dir = std::filesystem::temp_directory_path() / "grw_cfg_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "grw.ini";

    ConfigStore store(path.wstring());
    CHECK(store.Poll().kind == ReloadResult::Applied);  // file was missing: defaults written and applied
    CHECK(std::filesystem::exists(path));
    CHECK(store.Poll().kind == ReloadResult::Unchanged);

    WriteFileText(path, "aim_tau_ms = 42\n", 10);
    CHECK(store.Poll().kind == ReloadResult::Applied);
    CHECK(store.Snapshot().aim_tau_ms == 42.0f);

    WriteFileText(path, "aim_tau_ms = 7\naim_cone_deg = oops\n", 20);
    const auto r = store.Poll();
    CHECK(r.kind == ReloadResult::Error);
    CHECK(r.parse.line == 2);
    CHECK(store.Snapshot().aim_tau_ms == 42.0f);
}

TEST_CASE("ConfigText writes every value back") {
    Config c;
    c.aim_tau_ms = 55.5f;
    c.aim_key = 0x05;
    c.mod_damage = 5.69f;
    c.mod_magic_bullets = 1;
    c.magic_target = 0;
    c.magic_fov_deg = 30.0f;
    c.magic_start_m = 8.0f;
    c.magic_pierce = 0;
    c.mod_infinite_ammo = 1;
    c.mod_bullets_per_shot = 12;
    c.magic_debug = 1;
    c.los_mask = 0x3115fd67full;
    c.hwbp_addr = 0x7ff6746af9d8ull;
    Config back;
    REQUIRE(ParseConfig(ConfigText(c), back).ok);
    CHECK(back.magic_pierce == 0);
    CHECK(back.mod_infinite_ammo == 1);
    CHECK(back.mod_bullets_per_shot == 12);
    CHECK(back.magic_debug == 1);
    CHECK(Config{}.magic_debug == 0);
    CHECK(Config{}.mod_bullets_per_shot == 1);
    CHECK(Config{}.mod_infinite_ammo == 0);
    CHECK(Config{}.magic_pierce == 1);
    CHECK(back.aim_tau_ms == 55.5f);
    CHECK(back.aim_key == 0x05);
    CHECK(back.mod_damage == 5.69f);
    CHECK(back.mod_magic_bullets == 1);
    CHECK(back.magic_target == 0);
    CHECK(back.magic_fov_deg == 30.0f);
    CHECK(back.magic_start_m == 8.0f);
    CHECK(back.los_mask == 0x3115fd67full);
    CHECK(back.hwbp_addr == 0x7ff6746af9d8ull);
    CHECK_FALSE(ParseConfig("magic_start_m = 1\n", back).ok);
    CHECK_FALSE(ParseConfig("magic_fov_deg = 200\n", back).ok);
    CHECK_FALSE(ParseConfig("magic_pierce = 2\n", back).ok);
    CHECK_FALSE(ParseConfig("mod_bullets_per_shot = 0\n", back).ok);
    CHECK_FALSE(ParseConfig("mod_bullets_per_shot = 33\n", back).ok);
}

TEST_CASE("ParseConfig reads the weapon mods and rejects out-of-range factors") {
    Config c;
    CHECK(c.mod_spread == 1.0f);
    CHECK(c.mod_bullet_speed == 1.0f);
    CHECK(c.mod_damage == 1.0f);
    REQUIRE(ParseConfig("mod_spread = 0\nmod_bullet_speed = 2.5\nmod_damage = 10\n", c).ok);
    CHECK(c.mod_spread == 0.0f);
    CHECK(c.mod_bullet_speed == 2.5f);
    CHECK(c.mod_damage == 10.0f);
    CHECK_FALSE(ParseConfig("mod_spread = -1\n", c).ok);
    CHECK_FALSE(ParseConfig("mod_bullet_speed = 0\n", c).ok);
    CHECK_FALSE(ParseConfig("mod_damage = 1000\n", c).ok);
    REQUIRE(ParseConfig("mod_recoil = 0\n", c).ok);
    CHECK(c.mod_recoil == 0.0f);
    CHECK_FALSE(ParseConfig("mod_recoil = 5\n", c).ok);
    CHECK_FALSE(ParseConfig("mod_fire_rate = 4\n", c).ok);  // removed: not the cadence
    CHECK(c.mod_instant_bullets == 0);
    REQUIRE(ParseConfig("mod_instant_bullets = 1\n", c).ok);
    CHECK(c.mod_instant_bullets == 1);
    CHECK_FALSE(ParseConfig("mod_instant_bullets = 2\n", c).ok);
    CHECK(c.mod_straight_bullets == 0);
    REQUIRE(ParseConfig("mod_straight_bullets = 1\n", c).ok);
    CHECK(c.mod_straight_bullets == 1);
    CHECK_FALSE(ParseConfig("mod_straight_bullets = -1\n", c).ok);
    CHECK(c.mod_teleport_bullets == 0);
    REQUIRE(ParseConfig("mod_teleport_bullets = 1\n", c).ok);
    CHECK(c.mod_teleport_bullets == 1);
    CHECK_FALSE(ParseConfig("mod_teleport_bullets = 3\n", c).ok);
    REQUIRE(ParseConfig("mod_magic_bullets = 1\n", c).ok);
    CHECK(c.mod_magic_bullets == 1);
    CHECK_FALSE(ParseConfig("mod_magic_bullets = 2\n", c).ok);
}
