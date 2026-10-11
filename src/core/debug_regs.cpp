#include "core/debug_regs.h"

namespace grw::dr {

namespace {

constexpr uint64_t kTypeLenMask = 0xFFFF0000;  // the RW/LEN fields of the four slots

}  // namespace

uint64_t Dr7(uint64_t dr7, const Slots& s) {
    const uint64_t len = s.len == 1 ? 0 : s.len == 2 ? 1 : s.len == 8 ? 2 : 3;
    dr7 &= ~kOwnedMask;
    for (int i = 0; i < 4; ++i) {
        if (!s.addr[i]) continue;
        const uint64_t rw = s.exec[i] ? 0 : s.read[i] ? 3 : 1;
        dr7 |= (uint64_t(1) << (2 * i)) | (rw << (16 + 4 * i)) | ((s.exec[i] ? 0 : len) << (18 + 4 * i));
    }
    return dr7;
}

void Apply(uint64_t (&regs)[4], uint64_t& dr7, const Slots& s) {
    for (int i = 0; i < 4; ++i) regs[i] = s.addr[i];
    dr7 = Dr7(dr7, s);
}

bool IsExec(uint64_t dr7, int slot) { return ((dr7 >> (16 + 4 * slot)) & 3) == 0; }

bool Stale(const uint64_t (&regs)[4], uint64_t dr7, const Slots& s) {
    for (int i = 0; i < 4; ++i)
        if (regs[i] != s.addr[i]) return true;
    return (dr7 & kTypeLenMask) != (Dr7(0, s) & kTypeLenMask);
}

}  // namespace grw::dr
