#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <windows.h>
#include "core/mem.h"

using namespace grw;

namespace {
constexpr size_t kPage = 4096;

uint8_t* Alloc(size_t pages, DWORD protect = PAGE_READWRITE) {
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, pages * kPage, MEM_RESERVE | MEM_COMMIT, protect));
}
uintptr_t U(const void* p) { return reinterpret_cast<uintptr_t>(p); }
}  // namespace

TEST_CASE("Read returns the bytes of readable memory") {
    mem::ClearThreadCache();
    uint8_t* p = Alloc(1);
    *reinterpret_cast<uint64_t*>(p + 16) = 0x1122334455667788ull;
    uint64_t v = 0;
    REQUIRE(mem::Read(U(p + 16), v));
    CHECK(v == 0x1122334455667788ull);
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("Read rejects null, low and overflowing addresses without crashing") {
    mem::ClearThreadCache();
    uint64_t v = 0;
    CHECK_FALSE(mem::Read(0, v));
    CHECK_FALSE(mem::Read(0x10, v));
    CHECK_FALSE(mem::Read(UINTPTR_MAX - 2, &v, sizeof v));
}

TEST_CASE("One VirtualQuery serves a whole region") {
    mem::ClearThreadCache();
    uint8_t* p = Alloc(16);
    uint32_t v = 0;
    for (int i = 0; i < 1000; ++i) REQUIRE(mem::Read(U(p + (i * 61) % (16 * kPage - 4)), v));
    CHECK(mem::ThreadCacheStats().queries == 1);
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("Memory freed after being cached reads as false, not as a crash") {
    mem::ClearThreadCache();
    uint8_t* p = Alloc(1);
    uint32_t v = 0;
    REQUIRE(mem::Read(U(p), v));
    VirtualFree(p, 0, MEM_RELEASE);
    CHECK_FALSE(mem::Read(U(p), v));
    CHECK(mem::ThreadCacheStats().faults == 1);
    CHECK_FALSE(mem::Read(U(p), v));  // re-queried: MEM_FREE, refused without a fault
    CHECK(mem::ThreadCacheStats().faults == 1);
}

TEST_CASE("A page made inaccessible after caching is caught") {
    mem::ClearThreadCache();
    uint8_t* p = Alloc(1);
    uint32_t v = 0;
    REQUIRE(mem::Read(U(p), v));
    DWORD old = 0;
    REQUIRE(VirtualProtect(p, kPage, PAGE_NOACCESS, &old));
    CHECK_FALSE(mem::Read(U(p), v));
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("Write only succeeds on writable memory") {
    mem::ClearThreadCache();
    uint8_t* rw = Alloc(1);
    CHECK(mem::Write(U(rw), uint32_t{42}));
    CHECK(*reinterpret_cast<uint32_t*>(rw) == 42);

    uint8_t* ro = Alloc(1, PAGE_READONLY);
    CHECK_FALSE(mem::Write(U(ro), uint32_t{42}));
    CHECK(*reinterpret_cast<uint32_t*>(ro) == 0);
    VirtualFree(rw, 0, MEM_RELEASE);
    VirtualFree(ro, 0, MEM_RELEASE);
}

TEST_CASE("A read crossing into an inaccessible region is refused") {
    mem::ClearThreadCache();
    uint8_t* p = Alloc(2);
    DWORD old = 0;
    REQUIRE(VirtualProtect(p + kPage, kPage, PAGE_NOACCESS, &old));
    uint64_t v = 0;
    CHECK(mem::Read(U(p + kPage - 8), v));
    CHECK_FALSE(mem::Read(U(p + kPage - 4), v));
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("Expired or cleared entries are queried again") {
    mem::ClearThreadCache();
    uint8_t* p = Alloc(1);
    uint32_t v = 0;
    REQUIRE(mem::Read(U(p), v));
    mem::ClearThreadCache();
    REQUIRE(mem::Read(U(p), v));
    CHECK(mem::ThreadCacheStats().queries == 1);
    CHECK(mem::ThreadCacheStats().entries == 1);
    VirtualFree(p, 0, MEM_RELEASE);
}
