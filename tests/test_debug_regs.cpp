#include <catch2/catch_test_macros.hpp>
#include "core/debug_regs.h"

using namespace grw;

namespace {

constexpr uint64_t kReserved = 0x400;  // DR7 bit 10, always 1: not ours, kept

dr::Slots Recoil() {  // the weapon mods on: spread (DR2) and recoil (DR3), instruction breakpoints
    dr::Slots s;
    s.addr[2] = 0x7FF7008C1000;
    s.addr[3] = 0x7FF7008C0000 + 0x1483EAD0;
    s.exec[2] = s.exec[3] = true;
    return s;
}

dr::Slots Research() {  // MODLOG 2026-10-10: the mods off, four read watchpoints of 8 bytes
    dr::Slots s;
    const uintptr_t a[4] = {0x1C497B939F0, 0x7FF47C5DD900, 0x7FF480BB4980, 0x1C4CCDBDAF0};
    for (int i = 0; i < 4; ++i) s.addr[i] = a[i], s.read[i] = true;
    s.len = 8;
    return s;
}

}  // namespace

TEST_CASE("Dr7 encodes each used slot and keeps the bits that are not ours") {
    dr::Slots s;
    s.addr[0] = 0x1000;  // read/write, 8 bytes
    s.read[0] = true;
    s.addr[2] = 0x2000;  // write, 8 bytes
    s.addr[3] = 0x3000;  // instruction
    s.exec[3] = true;
    s.len = 8;
    CHECK(dr::Dr7(kReserved | dr::kOwnedMask, s) == (kReserved | 0x90B0051));

    dr::Slots w;  // write, 4 bytes, in DR1
    w.addr[1] = 0x1000;
    CHECK(dr::Dr7(0, w) == 0xD00004);
}

TEST_CASE("IsExec reads the slot's type in the thread's own DR7") {
    dr::Slots s = Recoil();
    s.addr[0] = 0x1000;
    s.read[0] = true;
    const uint64_t dr7 = dr::Dr7(kReserved, s);
    CHECK(dr::IsExec(dr7, 3));
    CHECK(dr::IsExec(dr7, 2));
    CHECK_FALSE(dr::IsExec(dr7, 0));
}

TEST_CASE("a thread left with the recoil mod's breakpoint is stale once DR3 watches data, and needs RF") {
    // The flood of 2026-10-10: recoil turned off while research took the four slots. One thread kept the recoil
    // instruction breakpoint; the configuration said "data" for DR3, so its hits got no RF and fired again at once.
    const dr::Slots old = Recoil(), cur = Research();
    uint64_t regs[4] = {old.addr[0], old.addr[1], old.addr[2], old.addr[3]};
    uint64_t dr7 = dr::Dr7(kReserved, old);
    CHECK(dr::Stale(regs, dr7, cur));
    CHECK(dr::IsExec(dr7, 3));  // RF from the thread's registers, not from the configuration

    dr::Apply(regs, dr7, cur);  // what the handler then gives that thread
    CHECK_FALSE(dr::Stale(regs, dr7, cur));
    CHECK(regs[3] == cur.addr[3]);
    CHECK(dr7 == dr::Dr7(kReserved, cur));
    CHECK_FALSE(dr::IsExec(dr7, 3));
}

TEST_CASE("a thread whose watchpoints were stopped after max_hits is not stale") {
    const dr::Slots cur = Research();
    const uint64_t regs[4] = {cur.addr[0], cur.addr[1], cur.addr[2], cur.addr[3]};
    const uint64_t dr7 = dr::Dr7(kReserved, cur) & ~uint64_t(0xFF);  // enables cleared on that thread only
    CHECK_FALSE(dr::Stale(regs, dr7, cur));
}

TEST_CASE("the same address with another type is stale; the current configuration is not") {
    dr::Slots exec, data;
    exec.addr[0] = data.addr[0] = 0x1C497B939F0;
    exec.exec[0] = true;
    data.read[0] = true;
    const uint64_t regs[4] = {exec.addr[0], 0, 0, 0};
    CHECK(dr::Stale(regs, dr::Dr7(kReserved, exec), data));
    CHECK_FALSE(dr::Stale(regs, dr::Dr7(kReserved, exec), exec));

    const uint64_t none[4] = {};
    CHECK_FALSE(dr::Stale(none, kReserved, dr::Slots{}));  // disarmed everywhere
}
