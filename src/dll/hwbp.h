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
    float dir0[3];        // launch vector +0x130 (not unit: its length sets the speed, camera-findings.md 7b)
    float V, v0, first_segment;  // the weapon setup's constants (+0x1b4, +0x1bc, +0x168)
    float dir[3];         // current direction +0x140 (unit)
    float v;              // speed +0x1b8: computed by the previous step (this step's runs after the sample)
    int32_t mode;         // +0x4c: 4 = bullet
};
using ProjFn = void (*)(const ProjView& step, void* ctx);

// Writer thread: pushes the recorded hits as "hwbp_hit" telemetry lines, and passes each to on_hit if set;
// the projectile steps go to on_proj.
void Drain(Telemetry& tel, HitFn on_hit = nullptr, void* ctx = nullptr, ProjFn on_proj = nullptr);
// Projectile tracker: hits of this slot (an instruction breakpoint on the projectile speed function, called
// once per simulation step with the projectile in rcx) are not recorded as hits: the projectile is sampled
// when it was fired by the player (SetCamera), and Drain pushes "proj" lines. -1 = off. Writer thread.
void SetTracker(int slot);
// The player's bullets' path, while the tracker slot is armed: 0 = the game's, 1 = straight (their polyline of points
// moved onto the line of their first segment, at the first steps), 2 = straight and teleported (the first step's ray
// reaches the end of that line at once: a hitscan). Any thread.
void SetBulletPath(int mode);
// Whether the tracker copies each step of the player's projectiles for Drain ("proj" lines, the trajectory learning):
// off, it only rewrites their paths. Any thread.
void SetTrackSamples(bool on);
// Diagnostics: copy what the collision test of each magic bullet's last segment met (MagicHits). Any thread.
void SetMagicDebug(bool on);
struct StraightStats {
    uint64_t paths = 0;     // projectiles straightened since injection
    float drop_m = 0;       // the largest correction of the last one (its drop at the end of its path)
    float first_m = 0;      // the length of its first segment
    int points = 0;         // its path's points
    uint64_t teleported = 0;  // projectiles teleported since injection
};
StraightStats Straightened();
// Magic bullets, while the tracker slot is armed: the targets, nearest the crosshair first (render thread, every
// frame; n = 0: off, back to SetBulletPath's behaviour). Each of the player's next bullets skips, without any
// collision test, to just short of one of these heads (extrapolated with its velocity, m/s), arriving along `dir`
// (unit, start -> head; zero = from the bullet's side), and tests only through it: one bullet per target in turn.
inline constexpr int kMaxMagicTargets = 32;
struct MagicTarget {
    uintptr_t addr = 0;
    float head[3] = {}, vel[3] = {}, dir[3] = {};
    float before = 0;  // m short of the head where the last test starts (past an obstacle); 0 = the default 12 m
    float after = 0;   // m past it where the test ends; 0 = the default 1.5 m
};
void SetMagicTargets(const MagicTarget* targets, int n);
// Their penetration energy (0 = the weapon's own): what crossing a vehicle's body or a wall costs is taken from it,
// and the bullet stops when it runs out. Any thread.
void SetMagicPierce(float energy);
struct MagicShot {
    uintptr_t addr = 0;
    float dist = 0;    // m from the bullet's origin
    int approach = 0;  // 0 = from the bullet's side, 1 = a direction of its own
    uint64_t seq = 0;  // its number (MagicHits)
};
// Render thread: the magic bullets sent since `seq` (advanced), at most max.
int MagicShotsSince(uint64_t& seq, MagicShot* out, int max);
// Diagnostics: what the collision test of a magic bullet's last segment met, read at the step after its jump (before
// the game applies the hits). Two ray queries (0x154703CF0): forward from the segment's start, reverse from its end;
// then the hits it kept for damage.
inline constexpr int kMagicDumpHits = 8, kMagicDumpKept = 4;
struct MagicRayHit {
    uint8_t raw[0x80];  // position +0x00, normal +0x10, object +0x30, distance +0x48
    uint64_t vtable = 0, owner = 0;  // the object's vtable and [object+0x10]
};
struct MagicKeptHit {
    uint8_t raw[0xB0];  // position +0x10, object +0x40, flag +0xA4
};
struct MagicHitDump {
    uintptr_t target = 0;
    float from[3] = {}, to[3] = {};  // the segment tested (+0x190 -> +0x1A0)
    float head[3] = {};              // the head aimed at (extrapolated)
    float flown = 0, segment = 0;    // +0x164, +0x124
    // the projectile around its hits: +0xC8 (damage +0xCC, +0xD4), +0x12E0 (energy +0x12F0..), +0x13C0 (the last hit
    // kept; +0x1420 = where it stops, else the segment's start), +0x1480
    uint8_t s0[0x10] = {}, s1[0x30] = {}, s2[0x70] = {}, s3[0x20] = {};
    double ms = 0;                   // when (Clock::NowMs)
    int n_fwd = 0, n_rev = 0, n_kept = 0;  // counts (the first kMagicDumpHits / kMagicDumpKept are kept)
    MagicRayHit fwd[kMagicDumpHits], rev[kMagicDumpHits];
    MagicKeptHit kept[kMagicDumpKept];
};
// Render thread: the dump of shot `seq` (MagicShot::seq) once recorded; false before or once overwritten.
bool MagicHits(uint64_t seq, MagicHitDump& out);
struct MagicStats {
    uint64_t bullets = 0;  // magic bullets since injection
    float last_dist_m = 0;  // the distance of the last one's jump
};
MagicStats Magic();
// Triggered capture: a hit of `slot` opens a window of `window_ms`; the other slots' hits are recorded only
// inside a window (outside, they are not counted against max_hits). slot -1 = record every hit. Writer thread.
void SetTrigger(int slot, int window_ms);
// Since injection: exceptions taken by the tracker (one per simulation step of any projectile in the game while
// it is armed: its cost) and steps of the player's projectiles kept.
uint64_t TrackerCalls();
uint64_t TrackerSamples();
// Render thread: the camera position and forward axis, and the player's head (ok = false: unknown), to keep the
// player's own projectiles: fired within 1 m of the head (third person: they start there), or within 8 m of
// the camera less than 5 degrees from where it looks.
void SetCamera(const float (&pos)[3], const float (&forward)[3]);
void SetPlayerHead(const float (&pos)[3], bool ok);
// Weapon mods: an instruction breakpoint whose hits multiply floats of a game object by a factor, when the
// register `reg` holds that object at that instruction (the player's, not the other characters'). Not recorded,
// not counted against max_hits. Writer thread: SetScaleMod before arming its slot, slot -1 after disarming.
enum class Reg { Rax, Rdi };
struct ScaleMod {
    int slot = -1;  // DR slot, -1 = off
    Reg reg = Reg::Rdi;
    uintptr_t object = 0;
    uint16_t offs[8] = {};
    int n = 0;
    float factor = 1.0f;
};
inline constexpr int kMaxScaleMods = 2;
void SetScaleMod(int id, const ScaleMod& m);
uint64_t ScaleModHits(int id);  // hits on the player's object since injection
// Bullets per shot: an instruction breakpoint at `after_shot` in the fire handler (model.h, kFireAfterShotRva). When rsi
// is `weapon`, the fire parameters' pellet count ([rbx + pellets_off]) becomes `count` (a shotgun's own, if larger,
// stays), [rbx + next_off] = 1, and the handler goes on at `loop`: the game fires count - 1 more bullets itself. Writer
// thread, like SetScaleMod: set before arming its slot, slot -1 after disarming.
struct PelletMod {
    int slot = -1;
    uintptr_t weapon = 0, loop = 0;
    size_t pellets_off = 0, next_off = 0;
    int count = 1;
};
void SetPelletMod(const PelletMod& m);
uint64_t PelletShots();  // the player's shots multiplied since injection
// The same breakpoint times every shot of the player's weapon (count 1 included): the cadence.
struct ShotStats {
    uint64_t count = 0;      // shots since injection
    float interval_ms = 0;   // median interval between the last 32 shots, those under 500 ms (bursts); 0 = none
};
ShotStats ShotStatsNow();
uint64_t LastShotHandler();  // the fire handler object (rdi) at the player's last shot, 0 = none yet
// The times (Clock::NowMs base) of the player's shots since `seq` (advanced; at most the last 32), at most max.
int ShotTimesSince(uint64_t& seq, double* ms, int max);
// DLL main thread at unload: clears the debug registers and removes the handler.
void Shutdown();

}  // namespace grw::hwbp
