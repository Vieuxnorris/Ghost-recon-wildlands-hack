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
