#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <windows.h>
#include "scan/regions.h"

using namespace grw::scan;

namespace {
uintptr_t U(const void* p) { return reinterpret_cast<uintptr_t>(p); }
// A rigid D3D-style matrix: rotation about Z by a, translation (x, y, z).
void PlantRigid(uint8_t* at, float a, float x, float y, float z) {
    const float m[16] = {std::cos(a), std::sin(a), 0, 0, -std::sin(a), std::cos(a), 0, 0, 0, 0, 1, 0, x, y, z, 1};
    std::memcpy(at, m, sizeof m);
}
void PlantPersp(uint8_t* at) {
    const float ys = 1.7320508f;  // 60 deg
    const float m[16] = {ys / 1.7777778f, 0, 0, 0, 0, ys, 0, 0, 0, 0, 1.0001f, 1, 0, 0, -0.10001f, 0};
    std::memcpy(at, m, sizeof m);
}
bool Has(const CandidateSet& s, uintptr_t addr) {
    for (size_t i = 0; i < s.Size(); ++i)
        if (s.At(i).addr == addr) return true;
    return false;
}
size_t Count(const CandidateSet& s, uintptr_t addr) {
    size_t n = 0;
    for (size_t i = 0; i < s.Size(); ++i) n += s.At(i).addr == addr;
    return n;
}
}  // namespace

TEST_CASE("Subtract removes excluded ranges, splitting regions") {
    const std::vector<Region> in = {{0x10000, 0x40000}};
    const MemRange ex[] = {{0x20000, 0x1000}, {0x48000, 0x10000}};
    const std::vector<Region> out = Subtract(in, ex, 2);
    REQUIRE(out.size() == 2);
    CHECK(out[0].base == 0x10000);
    CHECK(out[0].size == 0x10000);
    CHECK(out[1].base == 0x21000);
    CHECK(out[1].size == 0x27000);
    const MemRange all[] = {{0x0, 0x100000}};
    CHECK(Subtract(in, all, 1).empty());
}

TEST_CASE("ReadableRegions lists committed readable memory, not no-access pages nor excluded ranges") {
    auto* rw = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* na = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    auto* ex = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const MemRange exclude[] = {{U(ex), 0x10000}};
    const std::vector<Region> regs = ReadableRegions(true, exclude, 1);
    const auto covers = [&](uintptr_t a) {
        for (const Region& r : regs)
            if (a >= r.base && a < r.base + r.size) return true;
        return false;
    };
    CHECK(covers(U(rw) + 0x100));
    CHECK_FALSE(covers(U(na) + 0x100));
    CHECK_FALSE(covers(U(ex) + 0x100));
    VirtualFree(rw, 0, MEM_RELEASE);
    VirtualFree(na, 0, MEM_RELEASE);
    VirtualFree(ex, 0, MEM_RELEASE);
}

TEST_CASE("ScanMatrices finds planted rigid matrices once, including across a block boundary") {
    const size_t size = 3 * ScanBuffer::kBlock;
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    PlantRigid(p + 0x100, 0.5f, 100, 200, 30);
    PlantRigid(p + ScanBuffer::kBlock - 32, 1.0f, -50, 60, 7);  // crosses the first block boundary
    PlantRigid(p + 2 * ScanBuffer::kBlock + 4, 2.0f, 1, 2, 3);
    ScanBuffer buf;
    CandidateSet set;
    const ScanStats st = ScanMatrices({{U(p), size}}, MatrixKind::Rigid, buf, set, &ProcessReader, nullptr, nullptr);
    CHECK(st.found == 3);
    CHECK(Count(set, U(p) + 0x100) == 1);
    CHECK(Count(set, U(p) + ScanBuffer::kBlock - 32) == 1);
    CHECK(Has(set, U(p) + 2 * ScanBuffer::kBlock + 4));
    CHECK(set.At(0).kind == kFloats16);
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("ScanMatrices Perspective finds a projection and not the rigid ones") {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    PlantRigid(p + 0x40, 0.5f, 1, 2, 3);
    PlantPersp(p + 0x800);
    ScanBuffer buf;
    CandidateSet set;
    ScanMatrices({{U(p), 0x10000}}, MatrixKind::Perspective, buf, set, &ProcessReader, nullptr, nullptr);
    REQUIRE(set.Size() == 1);
    CHECK(set.At(0).addr == U(p) + 0x800);
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("ScanFloats keeps floats matching the predicate") {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const float yaw = 1.2345f;
    std::memcpy(p + 0x204, &yaw, 4);
    ScanBuffer buf;
    CandidateSet set;
    const auto near_yaw = [](float v, void*) { return std::fabs(v - 1.2345f) < 1e-4f; };
    ScanFloats({{U(p), 0x10000}}, near_yaw, nullptr, buf, set, &ProcessReader, nullptr, nullptr);
    REQUIRE(set.Size() == 1);
    CHECK(set.At(0).addr == U(p) + 0x204);
    CHECK(set.At(0).kind == kFloats1);
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("A region freed before the scan is counted unreadable, without crashing") {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const Region r{U(p), 0x10000};
    uint32_t tmp = 0;
    REQUIRE(ProcessReader(U(p), &tmp, 4, nullptr));  // caches the region in mem's thread cache
    ScanBuffer buf;  // allocated first: once p is freed, VirtualAlloc could hand the same address back
    VirtualFree(p, 0, MEM_RELEASE);
    CandidateSet set;
    const ScanStats st = ScanMatrices({r}, MatrixKind::Rigid, buf, set, &ProcessReader, nullptr, nullptr);
    CHECK(st.unreadable_blocks == 1);
    CHECK(set.Size() == 0);
}

TEST_CASE("A set cancel flag stops the scan") {
    auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    std::atomic<bool> cancel{true};
    ScanBuffer buf;
    CandidateSet set;
    const ScanStats st = ScanMatrices({{U(p), 0x10000}}, MatrixKind::Rigid, buf, set, &ProcessReader, nullptr, &cancel);
    CHECK(st.cancelled);
    VirtualFree(p, 0, MEM_RELEASE);
}
