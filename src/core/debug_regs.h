#pragma once
#include <cstdint>

// The CPU debug registers of one thread (DR0-DR3, DR7), as hwbp sets them on every thread of the game. Pure, so the
// rules the exception handler follows are tested here.
namespace grw::dr {

// The DR7 fields ours: L0-L3/G0-G3 (bits 0-7) and the RW/LEN fields of the four slots (bits 16-31).
inline constexpr uint64_t kOwnedMask = 0xFFFF00FF;

// A configuration: per slot its address (0 = unused) and type, instruction (exec) or data (read: read/write, else
// write only); the data slots' length, 1, 2, 4 or 8 bytes.
struct Slots {
    uint64_t addr[4] = {};
    bool exec[4] = {};
    bool read[4] = {};
    int len = 4;
};

// DR7 for this configuration, the bits of `dr7` that are not ours kept.
uint64_t Dr7(uint64_t dr7, const Slots& s);
// A thread's registers set to this configuration.
void Apply(uint64_t (&regs)[4], uint64_t& dr7, const Slots& s);
// Whether the slot is an instruction breakpoint in this DR7 (a thread's own: what fired, whatever the configuration
// is now). Such a hit must resume with RF, or it fires again at once.
bool IsExec(uint64_t dr7, int slot);
// Whether a thread's registers belong to another configuration: an address, or the type or length of a used slot,
// differs. The enable bits are not compared (the handler clears them on a thread after max_hits).
bool Stale(const uint64_t (&regs)[4], uint64_t dr7, const Slots& s);

}  // namespace grw::dr
