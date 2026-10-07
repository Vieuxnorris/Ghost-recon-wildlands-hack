#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include "core/clock.h"
#include "telemetry/json_line.h"
#include "telemetry/spsc_ring.h"
#include "telemetry/telemetry.h"

using namespace grw;

namespace {
std::string Text(const Line& l) { return std::string(l.text, l.len); }
}  // namespace

TEST_CASE("SpscRing keeps order and counts drops when full") {
    static SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) REQUIRE(ring.Push(i));
    CHECK_FALSE(ring.Push(99));
    CHECK(ring.Dropped() == 1);
    int v = -1;
    for (int i = 0; i < 4; ++i) {
        REQUIRE(ring.Pop(v));
        CHECK(v == i);
    }
    CHECK_FALSE(ring.Pop(v));
}

TEST_CASE("SpscRing survives a producer/consumer stress run") {
    static SpscRing<uint64_t, 1024> ring;
    constexpr uint64_t kCount = 1'000'000;
    uint64_t received = 0, last = 0;
    bool ordered = true;
    std::thread producer([] { for (uint64_t i = 1; i <= kCount; ++i) ring.Push(i); });
    std::thread consumer([&] {
        uint64_t v = 0;
        while (received + ring.Dropped() < kCount) {
            if (ring.Pop(v)) {
                ordered = ordered && v > last;
                last = v;
                ++received;
            }
        }
    });
    producer.join();
    consumer.join();
    CHECK(ordered);
    CHECK(received + ring.Dropped() == kCount);
}

TEST_CASE("JsonLine writes one JSON object per line") {
    Line l;
    JsonLine(l, "aim_tick", 1.5, 7).Num("err", 0.25).Int("id", -3).Bool("ok", true).Str("name", "a\"b\\c").Finish();
    CHECK(Text(l) == "{\"type\":\"aim_tick\",\"t\":1.5,\"frame\":7,\"err\":0.25,\"id\":-3,\"ok\":true,"
                     "\"name\":\"a\\\"b\\\\c\"}\n");
}

TEST_CASE("JsonLine writes non-finite numbers as null") {
    Line l;
    JsonLine(l, "x", 0, 0).Num("nan", std::nan("")).Num("inf", std::numeric_limits<double>::infinity()).Finish();
    CHECK(Text(l) == "{\"type\":\"x\",\"t\":0,\"frame\":0,\"nan\":null,\"inf\":null}\n");
}

TEST_CASE("JsonLine stays valid JSON when fields overflow the line") {
    Line l;
    JsonLine line(l, "status", 0, 0);
    const std::string big(200, 'x');
    for (int i = 0; i < 5; ++i) line.Str("msg", big.c_str());
    line.Finish();
    const std::string s = Text(l);
    CHECK(s.size() <= sizeof(l.text));
    CHECK(s.ends_with(",\"truncated\":true}\n"));
}
namespace {
std::filesystem::path FreshDir(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

std::vector<std::string> ReadLines(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    std::vector<std::string> lines;
    for (std::string s; std::getline(in, s);) lines.push_back(s);
    return lines;
}

Line Make(const char* type, int64_t i) {
    Line l;
    JsonLine(l, type, 0, 0).Int("i", i).Finish();
    return l;
}

std::wstring TestPipeName() {
    return L"\\\\.\\pipe\\grw-test-" + std::to_wstring(GetCurrentProcessId());
}
}  // namespace

TEST_CASE("Telemetry writes every channel to the JSONL file") {
    auto t = std::make_unique<Telemetry>();
    TelemetryOptions opt;
    opt.dir = FreshDir("grw_tel_file").wstring();
    opt.pipe_name = TestPipeName();
    REQUIRE(t->Start(opt));
    REQUIRE(t->Push(Channel::Render, Make("a", 1)));
    REQUIRE(t->Push(Channel::Control, Make("b", 2)));
    REQUIRE(t->Push(Channel::Worker, Make("c", 3)));
    const std::wstring path = t->FilePath();
    t->Stop();
    const auto lines = ReadLines(path);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].find("\"type\":\"a\"") != std::string::npos);
}

TEST_CASE("Telemetry calls on_tick from the writer thread") {
    auto t = std::make_unique<Telemetry>();
    std::atomic<int> ticks{0};
    TelemetryOptions opt;
    opt.dir = FreshDir("grw_tel_tick").wstring();
    opt.pipe_name = TestPipeName();
    opt.interval_ms = 10;
    opt.on_tick = [&] { ++ticks; };
    REQUIRE(t->Start(opt));
    Sleep(100);
    t->Stop();
    CHECK(ticks.load() >= 3);
}

TEST_CASE("Telemetry streams to a pipe client and survives its disconnection") {
    auto t = std::make_unique<Telemetry>();
    TelemetryOptions opt;
    opt.dir = FreshDir("grw_tel_pipe").wstring();
    opt.pipe_name = TestPipeName();
    opt.interval_ms = 10;
    REQUIRE(t->Start(opt));

    HANDLE client = CreateFileW(opt.pipe_name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(client != INVALID_HANDLE_VALUE);
    for (int i = 0; i < 100 && !t->PipeConnected(); ++i) Sleep(10);
    REQUIRE(t->PipeConnected());

    REQUIRE(t->Push(Channel::Render, Make("pipe_test", 1)));
    char buf[512] = {};
    DWORD got = 0;
    REQUIRE(ReadFile(client, buf, sizeof buf - 1, &got, nullptr));
    CHECK(std::string(buf, got).find("pipe_test") != std::string::npos);

    CloseHandle(client);
    for (int i = 0; i < 5; ++i) REQUIRE(t->Push(Channel::Render, Make("after", i)));
    Sleep(100);
    CHECK_FALSE(t->PipeConnected());
    const std::wstring path = t->FilePath();
    t->Stop();
    CHECK(ReadLines(path).size() == 6);
}

TEST_CASE("Telemetry::Push never blocks, even when nobody keeps up") {
    auto t = std::make_unique<Telemetry>();
    TelemetryOptions opt;
    opt.dir = FreshDir("grw_tel_burst").wstring();
    opt.pipe_name = TestPipeName();
    opt.interval_ms = 50;
    REQUIRE(t->Start(opt));
    const Line l = Make("burst", 0);
    const double t0 = Clock::NowMs();
    for (int i = 0; i < 10'000; ++i) t->Push(Channel::Render, l);
    CHECK(Clock::NowMs() - t0 < 50.0);
    const uint64_t dropped = t->Dropped(Channel::Render);
    const std::wstring path = t->FilePath();
    t->Stop();
    CHECK(ReadLines(path).size() + dropped == 10'000);
}
