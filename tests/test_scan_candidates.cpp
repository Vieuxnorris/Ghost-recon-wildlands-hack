#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include "scan/candidates.h"

using namespace grw::scan;

namespace {
// Fake address space: addr -> 16 floats.
struct Fake {
    std::map<uintptr_t, std::array<float, 16>> mem;
};
bool FakeRead(uintptr_t addr, void* dst, size_t size, void* ctx) {
    auto& f = *static_cast<Fake*>(ctx);
    const auto it = f.mem.find(addr);
    if (it == f.mem.end() || size > sizeof(float) * 16) return false;
    std::memcpy(dst, it->second.data(), size);
    return true;
}
Candidate Make(uintptr_t addr, float first, uint32_t kind = kFloats16) {
    Candidate c{addr, kind, {}};
    c.v[0] = first;
    return c;
}
}  // namespace

TEST_CASE("Refine Changed keeps only candidates whose values moved, and stores the new values") {
    Fake f;
    CandidateSet s;
    for (uintptr_t a = 0x1000; a < 0x1000 + 4 * 64; a += 64) {
        s.Add(Make(a, 1.0f));
        f.mem[a] = {1.0f};
    }
    f.mem[0x1040][0] = 2.0f;  // moved
    f.mem[0x10c0][5] = 0.5f;  // moved (another float)
    const RefineResult r = s.Refine(RefineMode::Changed, 1e-6f, &FakeRead, &f);
    CHECK(r.applied);
    CHECK(r.before == 4);
    CHECK(r.kept == 2);
    REQUIRE(s.Size() == 2);
    CHECK(s.At(0).addr == 0x1040);
    CHECK(s.At(0).v[0] == 2.0f);
}

TEST_CASE("Refine Unchanged keeps the stable ones, within eps") {
    Fake f;
    CandidateSet s;
    s.Add(Make(0x2000, 1.0f));
    s.Add(Make(0x2040, 1.0f));
    f.mem[0x2000] = {1.0000001f};
    f.mem[0x2040] = {1.5f};
    const RefineResult r = s.Refine(RefineMode::Unchanged, 1e-5f, &FakeRead, &f);
    CHECK(r.kept == 1);
    CHECK(s.At(0).addr == 0x2000);
}

TEST_CASE("A refine that would keep nothing is not applied") {
    Fake f;
    CandidateSet s;
    s.Add(Make(0x3000, 1.0f));
    f.mem[0x3000] = {1.0f};
    const RefineResult r = s.Refine(RefineMode::Changed, 1e-6f, &FakeRead, &f);  // game paused: nothing moved
    CHECK_FALSE(r.applied);
    CHECK(r.kept == 0);
    CHECK(s.Size() == 1);
}

TEST_CASE("Unreadable candidates are dropped even when the refine is not applied otherwise") {
    Fake f;
    CandidateSet s;
    s.Add(Make(0x4000, 1.0f));
    s.Add(Make(0x4040, 1.0f));  // freed: absent from the fake memory
    f.mem[0x4000] = {1.0f};
    const RefineResult r = s.Refine(RefineMode::Unchanged, 1e-6f, &FakeRead, &f);
    CHECK(r.unreadable == 1);
    CHECK(r.kept == 1);
    CHECK(s.Size() == 1);
}

TEST_CASE("Single-float candidates compare only their first float") {
    Fake f;
    CandidateSet s;
    s.Add(Make(0x5000, 0.25f, kFloats1));
    f.mem[0x5000] = {0.25f, 99.0f};  // the float after it changed, it does not count
    CHECK(s.Refine(RefineMode::Unchanged, 1e-6f, &FakeRead, &f).kept == 1);
}

TEST_CASE("KeepMatching filters with a predicate on fresh values") {
    Fake f;
    CandidateSet s;
    s.Add(Make(0x6000, 0.0f));
    s.Add(Make(0x6040, 0.0f));
    f.mem[0x6000] = {3.0f};
    f.mem[0x6040] = {-3.0f};
    const auto positive = [](const float* v, void*) { return v[0] > 0.0f; };
    const RefineResult r = s.KeepMatching(positive, nullptr, &FakeRead, &f);
    CHECK(r.kept == 1);
    CHECK(s.At(0).addr == 0x6000);
}

TEST_CASE("Storage never moves, across adds and refines") {
    Fake f;
    CandidateSet s;
    const MemRange before = s.Storage();
    CHECK(before.size == CandidateSet::kMax * sizeof(Candidate));
    for (uintptr_t a = 0x7000; a < 0x7000 + 1000 * 64; a += 64) {
        s.Add(Make(a, 1.0f));
        f.mem[a] = {a % 128 == 0 ? 2.0f : 1.0f};
    }
    s.Refine(RefineMode::Changed, 1e-6f, &FakeRead, &f);
    CHECK(s.Size() == 500);
    CHECK(s.Storage().base == before.base);
    CHECK(reinterpret_cast<uintptr_t>(&s.At(0)) == before.base);
}

TEST_CASE("Add refuses beyond kMax") {
    CandidateSet s;
    for (size_t i = 0; i < CandidateSet::kMax; ++i) REQUIRE(s.Add(Make(0x10000 + i * 4, 0.0f, kFloats1)));
    CHECK_FALSE(s.Add(Make(0x1, 0.0f)));
    CHECK(s.Size() == CandidateSet::kMax);
}
