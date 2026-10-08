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

// Writer thread. addrs[0..3] -> DR0..DR3 (0 = unused; all 0 disarms), each with its own mode. len: 1, 2, 4
// or 8 (ignored for Execute: instruction addresses). Re-arming with the same arguments is a no-op.
void Configure(const uintptr_t (&addrs)[4], const Mode (&modes)[4], int len, int max_hits);
struct HitView {
    uint64_t rip_rva;  // image-relative
    const uint64_t* regs;  // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    const uint8_t* mem_rcx;  // 0x100 bytes at [rcx] / [rdx], when readable
    const uint8_t* mem_rdx;
    bool ok_rcx, ok_rdx;
};
using HitFn = void (*)(const HitView& hit, void* ctx);

// One simulation step of one of the player's projectiles (SetTracker).
struct ProjView {
    uint64_t obj;
    float elapsed;        // s since the shot (+0x120)
    float pos[3];         // +0x190
    float origin[3];      // +0x150
    float dir0[3];        // launch direction +0x130
    float V, v0, first_segment;  // the weapon setup's constants (+0x1b4, +0x1bc, +0x168)
};
using ProjFn = void (*)(const ProjView& step, void* ctx);

// Writer thread: pushes the recorded hits as "hwbp_hit" telemetry lines, and passes each to on_hit if set;
// the projectile steps go to on_proj.
void Drain(Telemetry& tel, HitFn on_hit = nullptr, void* ctx = nullptr, ProjFn on_proj = nullptr);
// Projectile tracker: hits of this slot (an instruction breakpoint on the projectile speed function, called
// once per simulation step with the projectile in rcx) are not recorded as hits: the projectile is sampled
// when it was fired by the player (SetCamera), and Drain pushes "proj" lines. -1 = off. Writer thread.
void SetTracker(int slot);
// Since injection: exceptions taken by the tracker (one per simulation step of any projectile in the game while
// it is armed: its cost) and steps of the player's projectiles kept.
uint64_t TrackerCalls();
uint64_t TrackerSamples();
// Render thread: the camera position and forward axis, to keep the player's own projectiles (fired within
// 8 m of the camera, less than 5 degrees from where it looks).
void SetCamera(const float (&pos)[3], const float (&forward)[3]);
// DLL main thread at unload: clears the debug registers and removes the handler.
void Shutdown();

}  // namespace grw::hwbp
