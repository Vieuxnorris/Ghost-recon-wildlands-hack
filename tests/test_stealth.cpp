#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "game/model.h"
#include "game/stealth.h"

using namespace grw::game;

namespace {

// Zero-filled blocks of fake game memory (as in test_resources.cpp), writable.
struct FakeMem {
    std::map<uintptr_t, std::vector<uint8_t>> blocks;
    int writes = 0;
    bool fail_writes = false;
    void Alloc(uintptr_t addr, size_t size) { blocks[addr].assign(size, 0); }
    template <class T> void Set(uintptr_t addr, const T& v) {
        auto it = std::prev(blocks.upper_bound(addr));
        std::memcpy(it->second.data() + (addr - it->first), &v, sizeof v);
    }
    template <class T> T Get(uintptr_t addr) const {
        T v{};
        Read(addr, &v, sizeof v, const_cast<FakeMem*>(this));
        return v;
    }
    static bool Read(uintptr_t addr, void* dst, size_t size, void* ctx) {
        auto& m = static_cast<FakeMem*>(ctx)->blocks;
        auto it = m.upper_bound(addr);
        if (it == m.begin()) return false;
        --it;
        if (addr + size > it->first + it->second.size()) return false;
        std::memcpy(dst, it->second.data() + (addr - it->first), size);
        return true;
    }
    static bool Write(uintptr_t addr, const void* src, size_t size, void* ctx) {
        auto* self = static_cast<FakeMem*>(ctx);
        if (self->fail_writes) return false;
        auto it = self->blocks.upper_bound(addr);
        if (it == self->blocks.begin()) return false;
        --it;
        if (addr + size > it->first + it->second.size()) return false;
        ++self->writes;
        std::memcpy(it->second.data() + (addr - it->first), src, size);
        return true;
    }
};

constexpr uintptr_t kBase = 0x140000000;
constexpr uintptr_t kPlayer = 0x20000000, kV = 0x20001000, kV2 = 0x20002000;

// A visibility component as the game had it (MODLOG: +0x30 = 20, the three multipliers at 1).
void AddComponent(FakeMem& m, uintptr_t v, uintptr_t owner = kPlayer, uintptr_t vt = kBase + kVisibilityVtableRva) {
    m.Alloc(v, 0x200);
    m.Set<uint64_t>(v, vt);
    m.Set<uint64_t>(v + kVisibilityOwnerOff, owner);
    m.Set<float>(v + 0x30, 20.0f);
    for (size_t k = 0; k < kVisibilityMultCount; ++k) m.Set<float>(v + kVisibilityMultOff + 4 * k, 1.0f);
}

float Mult(const FakeMem& m, uintptr_t v, size_t k) { return m.Get<float>(v + kVisibilityMultOff + 4 * k); }

StealthEvent Step(Stealth& s, FakeMem& m, bool on, uintptr_t v) {
    return s.Update(on, v, &FakeMem::Read, &FakeMem::Write, &m);
}

}  // namespace

TEST_CASE("IsPlayerVisibility checks the vtable and the owner", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    AddComponent(m, kV2, kPlayer + 0x100);
    CHECK(IsPlayerVisibility(kV, kBase, kPlayer, &FakeMem::Read, &m));
    CHECK_FALSE(IsPlayerVisibility(kV2, kBase, kPlayer, &FakeMem::Read, &m));  // another entity's
    CHECK_FALSE(IsPlayerVisibility(kV, kBase, 0, &FakeMem::Read, &m));         // no player known
    CHECK_FALSE(IsPlayerVisibility(0x30000000, kBase, kPlayer, &FakeMem::Read, &m));  // unreadable
    m.Set<uint64_t>(kV, kBase + kVisibilityVtableRva + 8);
    CHECK_FALSE(IsPlayerVisibility(kV, kBase, kPlayer, &FakeMem::Read, &m));  // another class
}

TEST_CASE("Stealth sets the three multipliers to 0 and keeps the game's values", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    Stealth s;
    CHECK(Step(s, m, true, kV) == StealthEvent::Applied);
    CHECK(s.Applied() == kV);
    for (size_t k = 0; k < kVisibilityMultCount; ++k) {
        CHECK(Mult(m, kV, k) == 0.0f);
        CHECK(s.GameValue(k) == 1.0f);
    }
    CHECK(m.Get<float>(kV + 0x30) == 20.0f);  // the noise radius is the game's
    const int writes = m.writes;
    CHECK(Step(s, m, true, kV) == StealthEvent::None);  // nothing to do
    CHECK(m.writes == writes);
}

TEST_CASE("Stealth takes a value the game put back as its own and zeroes it again", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    Stealth s;
    Step(s, m, true, kV);
    m.Set<float>(kV + kVisibilityMultOff + 4, 0.8f);
    CHECK(Step(s, m, true, kV) == StealthEvent::Rewritten);
    CHECK(Mult(m, kV, 1) == 0.0f);
    CHECK(s.GameValue(1) == 0.8f);
    CHECK(Step(s, m, false, kV) == StealthEvent::Restored);
    CHECK(Mult(m, kV, 0) == 1.0f);
    CHECK(Mult(m, kV, 1) == 0.8f);
    CHECK(Mult(m, kV, 2) == 1.0f);
    CHECK(s.Applied() == 0);
    CHECK(Step(s, m, false, kV) == StealthEvent::None);
}

TEST_CASE("Stealth restores 1.0 over zeros left by an earlier session", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    for (size_t k = 0; k < kVisibilityMultCount; ++k) m.Set<float>(kV + kVisibilityMultOff + 4 * k, 0.0f);
    Stealth s;
    CHECK(Step(s, m, true, kV) == StealthEvent::Applied);
    CHECK(s.GameValue(0) == kVisibilityMultDefault);
    CHECK(Step(s, m, false, kV) == StealthEvent::Restored);
    for (size_t k = 0; k < kVisibilityMultCount; ++k) CHECK(Mult(m, kV, k) == kVisibilityMultDefault);
}

TEST_CASE("Stealth forgets a component that is gone and writes nothing to it", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    AddComponent(m, kV2);
    Stealth s;
    Step(s, m, true, kV);
    const int writes = m.writes;
    CHECK(Step(s, m, true, 0) == StealthEvent::Lost);
    CHECK(s.Applied() == 0);
    CHECK(m.writes == writes);
    CHECK(Step(s, m, false, 0) == StealthEvent::None);

    // turned off once the game made a new one: the old one is not written, the new one was never changed
    Step(s, m, true, kV);
    const int writes2 = m.writes;
    CHECK(Step(s, m, false, kV2) == StealthEvent::Lost);
    CHECK(m.writes == writes2);
    CHECK(Mult(m, kV2, 0) == 1.0f);
}

TEST_CASE("Stealth moves to the game's new component", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    AddComponent(m, kV2);
    Stealth s;
    Step(s, m, true, kV);
    CHECK(Step(s, m, true, kV2) == StealthEvent::Applied);
    CHECK(s.Applied() == kV2);
    CHECK(Mult(m, kV2, 2) == 0.0f);
}

TEST_CASE("Stealth reports a failed write and tries again at the next step", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    Stealth s;
    m.fail_writes = true;
    CHECK(Step(s, m, true, kV) == StealthEvent::Failed);
    CHECK(s.Applied() == 0);
    m.fail_writes = false;
    CHECK(Step(s, m, true, kV) == StealthEvent::Applied);
    CHECK(Step(s, m, true, 0x30000000) == StealthEvent::Failed);  // unreadable
}

TEST_CASE("SilenceNoise zeroes the noise radius and its copy, and says what it was", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    m.Set<float>(kV + kVisibilityNoiseCopyOff, 20.0f);
    float before = -1;
    CHECK(SilenceNoise(kV, kBase, kPlayer, &FakeMem::Read, &FakeMem::Write, &m, &before));
    CHECK(before == 20.0f);
    CHECK(m.Get<float>(kV + kVisibilityNoiseOff) == 0.0f);
    CHECK(m.Get<float>(kV + kVisibilityNoiseCopyOff) == 0.0f);
    CHECK(Mult(m, kV, 0) == 1.0f);  // the multipliers are not its business
    CHECK(SilenceNoise(kV, kBase, kPlayer, &FakeMem::Read, &FakeMem::Write, &m, nullptr));
}

TEST_CASE("SilenceNoise writes nothing to an object that is not the player's component any more", "[stealth]") {
    FakeMem m;
    AddComponent(m, kV);
    AddComponent(m, kV2, kPlayer + 0x100);
    CHECK_FALSE(SilenceNoise(kV2, kBase, kPlayer, &FakeMem::Read, &FakeMem::Write, &m, nullptr));  // another owner
    m.Set<uint64_t>(kV, 0x3F7FD09B40C7CCDBull);  // freed and reused (as seen in game)
    CHECK_FALSE(SilenceNoise(kV, kBase, kPlayer, &FakeMem::Read, &FakeMem::Write, &m, nullptr));
    CHECK(m.writes == 0);
    CHECK(m.Get<float>(kV + kVisibilityNoiseOff) == 20.0f);
}

TEST_CASE("StealthEventName names every event", "[stealth]") {
    CHECK(std::string(StealthEventName(StealthEvent::Applied)) == "applied");
    CHECK(std::string(StealthEventName(StealthEvent::Rewritten)) == "rewritten");
    CHECK(std::string(StealthEventName(StealthEvent::Restored)) == "restored");
    CHECK(std::string(StealthEventName(StealthEvent::Lost)) == "lost");
    CHECK(std::string(StealthEventName(StealthEvent::Failed)) == "failed");
    CHECK(std::string(StealthEventName(StealthEvent::None)) == "none");
}
