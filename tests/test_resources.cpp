#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include <vector>
#include "game/model.h"
#include "game/resources.h"

using namespace grw::game;

namespace {

// Zero-filled blocks of fake game memory (as in test_registry.cpp), writable.
struct FakeMem {
    std::map<uintptr_t, std::vector<uint8_t>> blocks;
    int writes = 0;            // writes that reached memory
    int calls = 0;             // write calls
    int fail_at = -1;          // the write call (from 0) that fails
    bool drop_writes = false;  // the writes report success but change nothing
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
        if (self->calls++ == self->fail_at) return false;
        auto it = self->blocks.upper_bound(addr);
        if (it == self->blocks.begin()) return false;
        --it;
        if (addr + size > it->first + it->second.size()) return false;
        ++self->writes;
        if (!self->drop_writes) std::memcpy(it->second.data() + (addr - it->first), src, size);
        return true;
    }
};

constexpr uintptr_t kBase = 0x140000000;
constexpr uintptr_t kOwner = 0x10000000, kStore = 0x10001000, kList = 0x10002000, kKeys = 0x10003000,
                    kElems = 0x10004000, kPool = 0x10005000, kPoints = 0x10006000;

struct Amount {
    uint32_t id;
    uint8_t bytes[4];
    uint32_t key;
};

// The store as read in game (MODLOG 2026-10-10): 8 entries, the four resources among unused ones, in this order.
const Amount kGameAmounts[8] = {
    {0x40005133, {0x6a, 0x18, 0xba, 0xb0}, 0xc1de7050u},  // 0
    {0x40005184, {0x7b, 0x09, 0xab, 0xb0}, 0xc1d97057u},  // 0
    {0x40005185, {0x7b, 0x18, 0xab, 0xa1}, 0xc1d3705du},  // 0
    {kFoodId, {0x4d, 0x3d, 0xbc, 0xb7}, 0xc1ee7060u},     // 4075
    {0x40005815, {0x48, 0x2b, 0xab, 0xb0}, 0xc1e87066u},  // 0
    {kFuelId, {0x40, 0x3d, 0xba, 0xa0}, 0xc1e67068u},     // 4650
    {kMedicineId, {0x58, 0x3b, 0xa9, 0xa0}, 0xc1e3706du}, // 75
    {kCommsId, {0x5d, 0x26, 0xaa, 0xa6}, 0xc1e1706fu},    // 11150
};

// The owner, its store and a list of n amounts with tag 1; the game's slot table says where the amounts start.
FakeMem Store(const Amount* amounts, int n, uint8_t first_slot, int32_t skill_points = 92) {
    FakeMem m;
    m.Alloc(kBase + kResourceOwnerRva, 8);
    m.Set(kBase + kResourceOwnerRva, kOwner);
    m.Alloc(kOwner, 0x40);
    m.Set(kOwner, uint64_t(kBase + kResourceOwnerVtableRva));
    m.Set(kOwner + kResourceStoreOff, kStore);
    m.Alloc(kStore, 0x40);
    m.Set(kStore, uint64_t(kBase + kResourceStoreVtableRva));
    m.Set(kStore + kResourceListOff, kList | 1);
    m.Alloc(kBase + kResourceFirstSlotRva, 64);
    m.Set(kBase + kResourceFirstSlotRva + (n - 1), first_slot);
    m.Alloc(kList, 8 * (first_slot + n));
    m.Set(kList, uint64_t(n - 1));
    m.Alloc(kKeys, 0x40 * n);
    m.Alloc(kElems, 0x28 * n);
    m.Alloc(kPool, 0x20 * n);
    for (int i = 0; i < n; ++i) {
        const uintptr_t key = kKeys + 0x40 * i, elem = kElems + 0x28 * i;
        m.Set(key, uint64_t(kBase + kResourceKeyVtableRva));
        m.Set(key + kResourceKeyIdOff, amounts[i].id);
        m.Set(kList + 8 + 8 * i, key);
        m.Set(kList + 8 * (first_slot + i), elem);
        for (int k = 0; k < 4; ++k) {
            const uintptr_t b = kPool + 0x20 * i + 8 * k;
            m.Set(elem + 8 * k, b);
            m.Set(b, amounts[i].bytes[k]);
        }
        m.Set(elem + kProtectedKeyOff, amounts[i].key);
    }
    m.Alloc(kBase + kSkillPointsRootRva, 8);
    m.Set(kBase + kSkillPointsRootRva, kPoints);
    m.Alloc(kPoints, 0x30);
    m.Set(kPoints, uint64_t(kBase + kSkillPointsVtableRva));
    m.Set(kPoints + kSkillPointsOff, skill_points);
    return m;
}

FakeMem GameStore() { return Store(kGameAmounts, 8, 12); }

}  // namespace

TEST_CASE("ReadResources reads the four amounts and the skill points as the game keeps them") {
    FakeMem m = GameStore();
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s.error == nullptr);
    REQUIRE(s[Resource::SkillPoints].ok);
    CHECK(s[Resource::SkillPoints].value == 92);
    REQUIRE(s[Resource::Fuel].ok);
    CHECK(s[Resource::Fuel].value == 4650);
    REQUIRE(s[Resource::Medicine].ok);
    CHECK(s[Resource::Medicine].value == 75);
    REQUIRE(s[Resource::Comms].ok);
    CHECK(s[Resource::Comms].value == 11150);
    REQUIRE(s[Resource::Food].ok);
    CHECK(s[Resource::Food].value == 4075);
}

TEST_CASE("ReadResources finds each amount by its id where the slot table puts the amounts") {
    // another size (5 entries, the amounts from slot 8), the resources in another order
    const Amount five[5] = {kGameAmounts[7], kGameAmounts[0], kGameAmounts[5], kGameAmounts[3], kGameAmounts[6]};
    FakeMem m = Store(five, 5, 8);
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s.error == nullptr);
    CHECK(s[Resource::Comms].value == 11150);
    CHECK(s[Resource::Fuel].value == 4650);
    CHECK(s[Resource::Food].value == 4075);
    CHECK(s[Resource::Medicine].value == 75);
}

TEST_CASE("ReadResources refuses a list in the compact layout it cannot read") {
    FakeMem m = GameStore();
    m.Set(kStore + kResourceListOff, kList | 3);
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s.error != nullptr);
    CHECK_FALSE(s[Resource::Fuel].ok);
    CHECK_FALSE(s[Resource::Comms].ok);
    CHECK(s[Resource::SkillPoints].ok);  // read on its own
}

TEST_CASE("ReadResources refuses an owner or a store of another class") {
    FakeMem m = GameStore();
    m.Set(kOwner, uint64_t(kBase + 0x1234));
    CHECK_FALSE(ReadResources(kBase, &FakeMem::Read, &m)[Resource::Fuel].ok);
    m = GameStore();
    m.Set(kStore, uint64_t(kBase + 0x1234));
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s.error != nullptr);
    CHECK_FALSE(s[Resource::Fuel].ok);
}

TEST_CASE("ReadResources refuses a list longer than any the game keeps") {
    FakeMem m = GameStore();
    m.Set(kList, uint64_t(5000));
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s.error != nullptr);
    CHECK_FALSE(s[Resource::Food].ok);
}

TEST_CASE("ReadResources refuses a list whose keys are of another class") {
    FakeMem m = GameStore();
    m.Set(kKeys + 0x40 * 2, uint64_t(kBase + 0x1234));  // an unused entry's key
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s.error != nullptr);
    CHECK_FALSE(s[Resource::Fuel].ok);
}

TEST_CASE("ReadResources refuses a list holding a resource twice") {
    Amount twice[8];
    std::memcpy(twice, kGameAmounts, sizeof twice);
    twice[1].id = kFuelId;
    FakeMem m = Store(twice, 8, 12);
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s.error != nullptr);
    CHECK_FALSE(s[Resource::Fuel].ok);
}

TEST_CASE("ReadResources keeps the resources when the skill points' object is not the game's") {
    FakeMem m = GameStore();
    m.Set(kPoints, uint64_t(kBase + 0x1234));
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s.error != nullptr);
    CHECK_FALSE(s[Resource::SkillPoints].ok);
    CHECK(s[Resource::Fuel].ok);
    CHECK(s[Resource::Fuel].value == 4650);
}

// Bytes and keys read from the game (MODLOG 2026-10-10), with the amounts its skills menu showed.
TEST_CASE("DecodeProtected gives the amounts the game showed for its bytes") {
    struct Case {
        uint8_t bytes[4];
        uint32_t key;
        int32_t want;
    };
    const Case cases[] = {
        {{0x40, 0x3d, 0xba, 0xa0}, 0xc1e67068u, 4650},   // fuel
        {{0x4d, 0x3d, 0xbc, 0xb7}, 0xc1ee7060u, 4075},   // food
        {{0x58, 0x3b, 0xa9, 0xa0}, 0xc1e3706du, 75},     // medicine
        {{0x5d, 0x26, 0xaa, 0xa6}, 0xc1e1706fu, 11150},  // comms
        {{0x6a, 0x18, 0xba, 0xb0}, 0xc1de7050u, 0},      // an unused entry of the store
    };
    for (const Case& c : cases) {
        const uint8_t(&b)[4] = c.bytes;
        CHECK(DecodeProtected(b, c.key) == c.want);
    }
}

TEST_CASE("EncodeProtected writes the bytes the game decoded and showed") {
    uint8_t b[4] = {0xff, 0xff, 0xff, 0xff};
    EncodeProtected(5650, 0xc1e67068u, b);  // written in game: the menu showed 5650
    CHECK(b[0] == 0x42);
    CHECK(b[1] == 0x3f);
    CHECK(b[2] == 0xbe);
    CHECK(b[3] == 0xa1);
    EncodeProtected(4650, 0xc1e67068u, b);  // the game's own bytes for 4650
    CHECK(b[0] == 0x40);
    CHECK(b[1] == 0x3d);
    CHECK(b[2] == 0xba);
    CHECK(b[3] == 0xa0);
}

TEST_CASE("WriteResource encodes the new amount into the entry's bytes with its own key") {
    FakeMem m = GameStore();
    const ResourceWrite w = WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::Fuel, 5650);
    CHECK(w.ok);
    CHECK(w.old_value == 4650);
    const uintptr_t fuel_bytes = kPool + 0x20 * 5;  // entry 5 of the game's list
    CHECK(m.Get<uint8_t>(fuel_bytes + 0) == 0x42);   // the bytes the game showed as 5650
    CHECK(m.Get<uint8_t>(fuel_bytes + 8) == 0x3f);
    CHECK(m.Get<uint8_t>(fuel_bytes + 16) == 0xbe);
    CHECK(m.Get<uint8_t>(fuel_bytes + 24) == 0xa1);
    CHECK(m.Get<uint32_t>(kElems + 0x28 * 5 + kProtectedKeyOff) == 0xc1e67068u);  // the key stays
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    CHECK(s[Resource::Comms].value == 11150);  // the others untouched
    CHECK(s[Resource::Food].value == 4075);
}

TEST_CASE("WriteResource sets the skill points as a plain int") {
    FakeMem m = GameStore();
    const ResourceWrite w = WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::SkillPoints, 120);
    CHECK(w.ok);
    CHECK(w.old_value == 92);
    CHECK(m.Get<int32_t>(kPoints + kSkillPointsOff) == 120);
}

TEST_CASE("WriteResource refuses amounts outside 0 to the resource's maximum and writes nothing") {
    FakeMem m = GameStore();
    CHECK_FALSE(WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::Fuel, -1).ok);
    CHECK_FALSE(WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::Fuel, 1000000).ok);
    CHECK_FALSE(WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::SkillPoints, 10000).ok);
    CHECK(m.writes == 0);
    CHECK(WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::Comms, 999999).ok);
    CHECK(WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::SkillPoints, 9999).ok);
}

TEST_CASE("WriteResource writes nothing when the store cannot be read") {
    FakeMem m = GameStore();
    m.Set(kStore + kResourceListOff, kList | 3);
    const ResourceWrite w = WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::Food, 5000);
    CHECK_FALSE(w.ok);
    CHECK(w.error != nullptr);
    CHECK(m.writes == 0);
}

TEST_CASE("WriteResource reports a write the game's memory did not keep") {
    FakeMem m = GameStore();
    m.drop_writes = true;
    const ResourceWrite w = WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::Medicine, 500);
    CHECK_FALSE(w.ok);
    CHECK(w.error != nullptr);
}

TEST_CASE("WriteResource puts the old bytes back when a write fails halfway") {
    FakeMem m = GameStore();
    m.fail_at = 2;  // the third byte of the four
    const ResourceWrite w = WriteResource(kBase, &FakeMem::Read, &FakeMem::Write, &m, Resource::Fuel, 5650);
    CHECK_FALSE(w.ok);
    const ResourceState s = ReadResources(kBase, &FakeMem::Read, &m);
    REQUIRE(s[Resource::Fuel].ok);
    CHECK(s[Resource::Fuel].value == 4650);
}
