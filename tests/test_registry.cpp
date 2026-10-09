#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include <vector>
#include "game/model.h"
#include "game/registry.h"

using namespace grw::game;

namespace {

// Zero-filled blocks of fake game memory.
struct FakeMem {
    std::map<uintptr_t, std::vector<uint8_t>> blocks;
    void Alloc(uintptr_t addr, size_t size) { blocks[addr].assign(size, 0); }
    template <class T> void Set(uintptr_t addr, const T& v) {
        auto it = std::prev(blocks.upper_bound(addr));
        std::memcpy(it->second.data() + (addr - it->first), &v, sizeof v);
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
};

constexpr uintptr_t kBase = 0x140000000;
constexpr uintptr_t kWorld = 0x10000000, kList = 0x10001000, kData = 0x10002000;

void AddEntity(FakeMem& m, uintptr_t o, uint32_t flags, uintptr_t comps) {
    m.Alloc(o, 0x100);
    m.Set(o, uint64_t(kBase + kEntityVtableRva));
    m.Set(o + kEntityComponentsOff, comps);
    m.Set(o + kEntityFlagsOff, flags);
    m.Alloc(comps, 0x200);
}

void AddChar(FakeMem& m, uintptr_t a, uintptr_t o, uintptr_t comps, size_t slot_off) {
    m.Alloc(a, 0x280);
    m.Set(a, uint64_t(kBase + kCharVtableRva));
    m.Set(a + kCharOwnerOff, o);
    m.Set(comps + slot_off, a);
}

// Player O1 (with its weapon), a Santa Blanca O2, an entity without a character O3, a slot of another kind.
FakeMem World() {
    FakeMem m;
    m.Alloc(kBase + kWorldRootRva, 8);
    m.Set(kBase + kWorldRootRva, kWorld);
    m.Alloc(kWorld, 0x100);
    m.Set(kWorld, uint64_t(kBase + kWorldVtableRva));
    m.Set(kWorld + kWorldEntityListOff, kList);
    m.Alloc(kList, 0x100);
    m.Set(kList, uint64_t(kBase + kEntityListVtableRva));
    m.Set(kList + kEntityListDataOff, kData);
    m.Set(kList + kEntityListCountOff, uint16_t(6));
    m.Alloc(kData, 6 * kEntityListStride);
    const uintptr_t slots[6] = {0x20000000, 0x20000000, 0x21000000, 0x23000000, 0, 0x22000000};
    for (int i = 0; i < 6; ++i) m.Set(kData + i * kEntityListStride, slots[i]);
    AddEntity(m, 0x20000000, 0xd09, 0x20100000);
    AddChar(m, 0x20200000, 0x20000000, 0x20100000, 0xB8);
    m.Set(0x20100000 + kComponentsWeaponHolderOff, uintptr_t(0x20300000));
    m.Alloc(0x20300000, 0x500);
    m.Set(0x20300000, uint64_t(kBase + kWeaponHolderVtableRva));
    m.Set(0x20300000 + kWeaponHolderStateOff, uintptr_t(0x20400000));
    m.Alloc(0x20400000, 0x60);
    m.Set(0x20400000, uint64_t(kBase + kWeaponStateVtableRva));
    AddEntity(m, 0x21000000, 0x111a, 0x21100000);
    m.Set(0x21100000 + 0x40, uintptr_t(0x20000000));  // another pointer before the character
    AddChar(m, 0x21200000, 0x21000000, 0x21100000, 0x120);
    AddEntity(m, 0x22000000, 0x912, 0x22100000);
    m.Alloc(0x23000000, 0x100);
    m.Set(0x23000000, uint64_t(kBase + 0x1234));
    return m;
}

}  // namespace

TEST_CASE("EntityRegistry lists each live character once, and the player's weapon state") {
    FakeMem m = World();
    EntityRegistry reg;
    std::vector<uintptr_t> chars;
    const RegistryPass r = reg.Read(kBase, &FakeMem::Read, &m, chars);
    REQUIRE(r.ok);
    CHECK(r.slots == 6);
    CHECK(r.entities == 3);
    CHECK(r.searched == 3);
    CHECK(chars == std::vector<uintptr_t>{0x20200000, 0x21200000});
    CHECK(r.player_entity == 0x20000000);
    CHECK(r.weapon_state == 0x20400000);
}

TEST_CASE("EntityRegistry searches components only for new or changed entities") {
    FakeMem m = World();
    EntityRegistry reg;
    std::vector<uintptr_t> chars;
    reg.Read(kBase, &FakeMem::Read, &m, chars);
    RegistryPass r = reg.Read(kBase, &FakeMem::Read, &m, chars);
    CHECK(r.searched == 0);  // known characters are only checked; O3 waits for its retry
    CHECK(chars.size() == 2);

    // O2's character object is replaced
    m.Set(0x21200000 + kCharOwnerOff, uintptr_t(0));
    AddChar(m, 0x21400000, 0x21000000, 0x21100000, 0x120);
    r = reg.Read(kBase, &FakeMem::Read, &m, chars);
    CHECK(r.searched == 1);
    CHECK(chars == std::vector<uintptr_t>{0x20200000, 0x21400000});

    // O3 gets its character: found at its retry (5 passes after the first search)
    AddChar(m, 0x22200000, 0x22000000, 0x22100000, 0xB0);
    for (int i = 0; i < 2; ++i) reg.Read(kBase, &FakeMem::Read, &m, chars);
    CHECK(chars.size() == 2);
    r = reg.Read(kBase, &FakeMem::Read, &m, chars);
    CHECK(r.searched == 1);
    CHECK(chars.size() == 3);
}

TEST_CASE("EntityRegistry prefers the character update job's array") {
    FakeMem m = World();
    constexpr uintptr_t kHolder = 0x30000000, kJob = 0x30001000, kArr = 0x30002000;
    m.Alloc(kBase + kCharJobHolderRva, 8);
    m.Set(kBase + kCharJobHolderRva, kHolder);
    m.Alloc(kHolder, 0x200);
    m.Set(kHolder, uint64_t(kBase + kCharJobHolderVtableRva));
    m.Set(kHolder + kHolderCharJobOff, kJob);
    m.Alloc(kJob, 0x200);
    m.Set(kJob, uint64_t(kBase + kJobVtableRva));
    m.Set(kJob + kJobFnOff, uint64_t(kBase + kCharUpdateFnRva));
    m.Set(kJob + kJobCharsOff, kArr);
    m.Set(kJob + kJobCharsSizeOff, uint32_t(3));
    m.Alloc(kArr, 3 * 8);
    // the player's and O2's characters, and a stale pointer
    const uintptr_t arr[3] = {0x20200000, 0x21200000, 0x23000000};
    for (int i = 0; i < 3; ++i) m.Set(kArr + i * 8, arr[i]);

    EntityRegistry reg;
    std::vector<uintptr_t> chars;
    RegistryPass r = reg.Read(kBase, &FakeMem::Read, &m, chars);
    REQUIRE(r.ok);
    CHECK(r.source == 1);
    CHECK(r.slots == 3);
    CHECK(r.entities == 2);
    CHECK(r.searched == 0);
    CHECK(chars == std::vector<uintptr_t>{0x20200000, 0x21200000});
    CHECK(r.player_entity == 0x20000000);
    CHECK(r.weapon_state == 0x20400000);

    // another job's function: not ours, back to the entity list
    m.Set(kJob + kJobFnOff, uint64_t(kBase + 0x13562D8));
    r = reg.Read(kBase, &FakeMem::Read, &m, chars);
    REQUIRE(r.ok);
    CHECK(r.source == 2);
}

TEST_CASE("EntityRegistry rejects a list object that holds something else than slots") {
    FakeMem m = World();
    // as seen after a zone reload: floats where the slot pointer was, FLT_MAX's low half as the count
    m.Set(kList + kEntityListDataOff, uint64_t(0x42B65108435745FEull));
    m.Set(kList + kEntityListCountOff, uint16_t(0xFFFF));
    EntityRegistry reg;
    std::vector<uintptr_t> chars{1};
    CHECK_FALSE(reg.Read(kBase, &FakeMem::Read, &m, chars).ok);
    CHECK(chars.empty());
}

TEST_CASE("EntityRegistry fails cleanly when the chain does not resolve") {
    FakeMem m = World();
    m.Set(kList, uint64_t(kBase + 0x999));
    EntityRegistry reg;
    std::vector<uintptr_t> chars{1, 2};
    const RegistryPass r = reg.Read(kBase, &FakeMem::Read, &m, chars);
    CHECK_FALSE(r.ok);
    CHECK(chars.empty());

    FakeMem none;
    CHECK_FALSE(reg.Read(kBase, &FakeMem::Read, &none, chars).ok);
}
