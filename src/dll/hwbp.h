#pragma once
#include <cstddef>
#include <cstdint>

namespace grw {
class Telemetry;
}

// Data watchpoint through the CPU debug registers (DR0/DR7), set on every thread of the game; no debugger
// attached and no game code modified. A vectored exception handler records the instruction that wrote the
// watched address (RIP after the write), the general registers and the image return addresses found on
// the stack, then lets the game continue. Disarms itself after max_hits.
namespace grw::hwbp {

enum class Mode { Write = 0, ReadWrite = 1, Execute = 2 };

// Writer thread. addrs[0..3] -> DR0..DR3 (0 = unused; all 0 disarms). len: 1, 2, 4 or 8 (ignored for
// Execute: instruction addresses). Re-arming with the same arguments is a no-op.
void Configure(const uintptr_t (&addrs)[4], int len, int max_hits, Mode mode);
// Writer thread: pushes the recorded hits as "hwbp_hit" telemetry lines.
void Drain(Telemetry& tel);
// DLL main thread at unload: clears the debug registers and removes the handler.
void Shutdown();

}  // namespace grw::hwbp
