#include "dll/hwbp.h"
#include "core/debug_regs.h"
#include "game/model.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "telemetry/json_line.h"
#include "telemetry/telemetry.h"

namespace grw::hwbp {

namespace {

constexpr int kStackSlots = 512;  // qwords scanned above RSP (an event dispatcher keeps 0x200 bytes of locals)
constexpr int kMaxReturns = 24;  // image addresses kept per hit
constexpr int kRing = 256;
constexpr uintptr_t kNone[4] = {};
constexpr Mode kWrites[4] = {};
constexpr int kDump = 0x100;  // bytes copied at [rcx] and [rdx] on each hit

struct Hit {
    uint32_t tid;
    int slot;  // which DR fired
    uint64_t rip;
    uint64_t regs[16];  // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    uint64_t returns[kMaxReturns];
    int n_returns;
    uint8_t mem_rcx[kDump], mem_rdx[kDump];
    bool ok_rcx, ok_rdx;
};

Hit g_ring[kRing];
std::atomic<uint32_t> g_written{0};  // hits recorded by the handler
uint32_t g_read = 0;                 // writer thread only
std::atomic<int> g_hits{0};
std::atomic<int> g_max_hits{8};
std::atomic<int> g_trigger_slot{-1};
std::atomic<int> g_window_ms{1500};
std::atomic<uint64_t> g_window_until{0};  // GetTickCount64 deadline of the current trigger window
uint64_t g_dropped = 0;                   // writer thread only: hits overwritten before Drain read them
std::atomic<bool> g_armed{false};
PVOID g_veh = nullptr;
uintptr_t g_image_lo = 0, g_image_hi = 0;
uintptr_t g_self_lo = 0, g_self_hi = 0;  // this DLL: its own reads (ESP scans) are not the game's
uintptr_t g_addrs[4] = {};
int g_len = 0;
Mode g_modes[4] = {};
// The configuration the handler gives a thread found with another one (MODLOG 2026-10-10): a thread inside the handler
// while ApplyAll set it gets the old registers back from its exception context when it resumes. A seqlock, written
// by Configure: DR0-DR3, the types (exec bits 0-3, read bits 4-7), the length.
std::atomic<uint32_t> g_want_seq{0};
std::atomic<uint64_t> g_want[6] = {};
std::atomic<uint64_t> g_stale{0};  // threads set again by the handler since injection

dr::Slots ToSlots(const uintptr_t (&addrs)[4], const Mode (&modes)[4], int len) {
    dr::Slots s;
    for (int i = 0; i < 4; ++i) {
        s.addr[i] = addrs[i];
        s.exec[i] = modes[i] == Mode::Execute;
        s.read[i] = modes[i] == Mode::ReadWrite;
    }
    s.len = len;
    return s;
}

void SetWant(const dr::Slots& s) {  // writer thread
    g_want_seq.fetch_add(1, std::memory_order_acq_rel);  // odd: being written
    uint64_t types = 0;
    for (int i = 0; i < 4; ++i) {
        g_want[i].store(s.addr[i], std::memory_order_relaxed);
        types |= uint64_t(s.exec[i]) << i | uint64_t(s.read[i]) << (4 + i);
    }
    g_want[4].store(types, std::memory_order_relaxed);
    g_want[5].store(uint64_t(s.len), std::memory_order_relaxed);
    g_want_seq.fetch_add(1, std::memory_order_release);
}

bool ReadWant(dr::Slots& s) {  // any thread; false while it is being written
    const uint32_t q = g_want_seq.load(std::memory_order_acquire);
    if (q & 1) return false;
    for (int i = 0; i < 4; ++i) s.addr[i] = g_want[i].load(std::memory_order_relaxed);
    const uint64_t types = g_want[4].load(std::memory_order_relaxed);
    s.len = int(g_want[5].load(std::memory_order_relaxed));
    for (int i = 0; i < 4; ++i) {
        s.exec[i] = (types >> i) & 1;
        s.read[i] = (types >> (4 + i)) & 1;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    return g_want_seq.load(std::memory_order_relaxed) == q;
}

// SEH-guarded copy (no C++ objects here): the pointer in a register may be anything.
bool SafeCopy(void* dst, uint64_t src, size_t n) {
    if (src < 0x10000 || src > 0x7FFFFFFFFFFF - n) return false;
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Projectile tracker (SetTracker): one sample per simulation step of each projectile fired near the camera.
// Multi-producer ring (the game's worker threads); a slot is complete when its seq is its index + 1.
constexpr int kProjRing = 4096;
constexpr size_t kProjHead = 0x60, kProjBodyFrom = 0x110, kProjBody = 0xC0;
constexpr size_t kProjOriginOff = 0x150, kProjDir0Off = 0x130;
constexpr size_t kProjSegmentOff = 0x124, kProjPathOff = 0x17C, kProjPathCountOff = 0x186;  // its path (polyline)
constexpr size_t kProjSpeedVOff = 0x1B4;  // its V: the speed function computes its speed from it
constexpr size_t kProjDistOff = 0x164;    // the distance flown
constexpr size_t kProjNextOff = 0x1A0;    // its next position: where the next advance starts
constexpr float kTeleportSpeed = 1e7f;     // m/s: a step covers any segment
constexpr float kTeleportChunk = game::kTeleportChunkM;  // m: one segment per step
constexpr float kMagicSpeed = 1.1e7f;                      // m/s, and the mark of a magic bullet
// m: its last collision test, around the head. It starts outside the target's 10 m sensor sphere (layer 25,
// camera-findings.md section 9): tested from 0.5 m, a ray through the head hit 1 time in 6. Vehicles do not block
// the bullets' rays; a wall within 12 m of the target does.
constexpr float kMagicBefore = 12.0f, kMagicAfter = 1.5f;
struct ProjSample {
    std::atomic<uint64_t> seq{0};
    uint64_t obj;
    int64_t qpc;
    uint8_t head[kProjHead];  // obj + 0x00
    uint8_t body[kProjBody];  // obj + 0x110
};
ProjSample g_proj[kProjRing];
std::atomic<uint64_t> g_proj_written{0};
uint64_t g_proj_read = 0;  // writer thread
std::atomic<int> g_tracker_slot{-1};
std::atomic<uint64_t> g_tracker_calls{0};
std::atomic<float> g_cam[3], g_fwd[3], g_head[3];
std::atomic<bool> g_head_ok{false};
std::atomic<int> g_bullet_path{0};  // SetBulletPath
std::atomic<bool> g_track_samples{true};  // SetTrackSamples
std::atomic<bool> g_magic_debug{false};   // SetMagicDebug
std::atomic<uint64_t> g_straight_paths{0}, g_teleported{0};
std::atomic<float> g_straight_drop{0}, g_straight_first{0};
std::atomic<int> g_straight_points{0};
// SetMagicTargets: the targets of the magic bullets and when they were read. A slot may be read while rewritten:
// at worst a bullet goes to a position one frame old.
struct MagicSlot {
    std::atomic<uintptr_t> addr{0};
    std::atomic<float> head[3], vel[3], dir[3], before{0}, after{0};
    std::atomic<uint32_t> shots{0};  // bullets sent since the last publish
};
MagicSlot g_magic_slots[kMaxMagicTargets];
std::atomic<int> g_magic_count{0};
// SetMagicPierce. A bullet's penetration energy: +0x12F0 at the shot, +0x12F4 left (the collision test ends the flight
// at <= 0), +0x12F8. Each hit takes its material's cost from +0x12F4 (0.112 for the R4; a helicopter's body 1.5, its
// glass 0.0125: magic bullets stopped 2-12 m short of a pilot's head); the damage falls with the fraction used,
// 100 x (+0x12F0 - +0x12F4) / +0x12F0 (0x1546FEB70), so a large energy at the start keeps it whole.
std::atomic<float> g_magic_pierce{0};
constexpr size_t kProjEnergyOff = 0x12F0;
std::atomic<int64_t> g_magic_qpc{0};
std::atomic<uint64_t> g_magic_hits{0};
std::atomic<float> g_magic_dist{0};
double g_qpc_s = 0;  // seconds per QPC tick, set once by SetMagicTargets
// the magic bullets, for the render thread (MagicShotsSince)
constexpr int kMagicRing = 64;
struct MagicShotRec {
    std::atomic<uint64_t> seq{0};
    uintptr_t addr;
    float dist;
    int approach;
    uint64_t obj;        // the projectile
    float head[3];       // where it was sent
    std::atomic<bool> dumped{false};
};
MagicShotRec g_magic_ring[kMagicRing];
std::atomic<uint64_t> g_magic_written{0};
// MagicHits: one dump per shot, at the same index; complete when its seq is the shot's + 1
MagicHitDump g_magic_dumps[kMagicRing];
std::atomic<uint64_t> g_magic_dump_seq[kMagicRing];
constexpr size_t kFwdHitsOff = 0x1E0, kRevHitsOff = 0xA60, kKeptHitsOff = 0x1310;  // arrays: data, u16 count at +0xA
// weapon mods (SetScaleMod): offs and n are written before slot (release), read after it (acquire)
struct ModState {
    std::atomic<int> slot{-1};
    std::atomic<int> reg{0};
    std::atomic<uintptr_t> object{0};
    std::atomic<float> factor{1.0f};
    uint16_t offs[8] = {};
    int n = 0;
    std::atomic<uint64_t> hits{0};
};
ModState g_mods[kMaxScaleMods];
// SetPelletMod: the fields are written before slot (release), read after it (acquire)
struct PelletState {
    std::atomic<int> slot{-1};
    std::atomic<uintptr_t> weapon{0};
    std::atomic<int> count{1};
    uintptr_t loop = 0;
    size_t pellets_off = 0, next_off = 0;
    std::atomic<uint64_t> shots{0};
};
PelletState g_pellets;

// The player's shots (one per trigger event, pellets aside): their times, for the cadence (ShotStatsNow).
constexpr int kShotRing = 32;
std::atomic<int64_t> g_shot_qpc[kShotRing];
std::atomic<uint64_t> g_shot_count{0};
std::atomic<uint64_t> g_shot_handler{0};  // the fire handler (rdi) of the last shot

// The fire handler's first shot is done: its time, then more bullets through its pellet loop (SetPelletMod).
void PelletJump(CONTEXT* c) {
    if (c->Rsi != g_pellets.weapon.load(std::memory_order_relaxed)) return;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_shot_qpc[g_shot_count.load(std::memory_order_relaxed) % kShotRing].store(q.QuadPart, std::memory_order_relaxed);
    g_shot_handler.store(c->Rdi, std::memory_order_relaxed);
    g_shot_count.fetch_add(1, std::memory_order_release);
    const int n = g_pellets.count.load(std::memory_order_relaxed);
    if (n <= 1) return;
    __try {
        int* pellets = reinterpret_cast<int*>(c->Rbx + g_pellets.pellets_off);
        if (*pellets < n) *pellets = n;
        *reinterpret_cast<uint8_t*>(c->Rbx + g_pellets.next_off) = 1;
        c->Rip = g_pellets.loop;
        g_pellets.shots.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ScaleFloat(uintptr_t addr, float k) {
    __try {
        float* p = reinterpret_cast<float*>(addr);
        *p *= k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The player's own projectiles (SetCamera, SetPlayerHead).
bool IsPlayers(uint64_t obj) {
    float o[3], d[3];
    if (!SafeCopy(o, obj + kProjOriginOff, sizeof o) || !SafeCopy(d, obj + kProjDir0Off, sizeof d)) return false;
    const auto rel = std::memory_order_relaxed;
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dl > 0.5f)) return false;
    // third person: the player's bullets start at its head (within 0.15 m)
    const float hx = o[0] - g_head[0].load(rel), hy = o[1] - g_head[1].load(rel), hz = o[2] - g_head[2].load(rel);
    if (!(g_head_ok.load(rel) && hx * hx + hy * hy + hz * hz < 1.0f)) {
        const float dx = o[0] - g_cam[0].load(rel), dy = o[1] - g_cam[1].load(rel), dz = o[2] - g_cam[2].load(rel);
        if (!(dx * dx + dy * dy + dz * dz < 64.0f)) return false;  // fired more than 8 m from the camera: not ours
        // ours go where the camera looks (squad mates firing next to the player do not); the launch vector is
        // shorter than 1 with spread
        if (!(d[0] * g_fwd[0].load(rel) + d[1] * g_fwd[1].load(rel) + d[2] * g_fwd[2].load(rel) > 0.9962f * dl))
            return false;
    }
    return true;
}

// Straight bullets: a projectile follows a polyline of world points computed at its launch, the drop included
// ([obj+0x17C], [obj+0x186] points of 3 floats, one segment at most per step; camera-findings.md section 14). Its
// points go onto the line of its first segment (which has no drop), at their distance along it. Idempotent: done
// at each step of its first segment.
// Teleport: then the segments after the first become kTeleportChunk long, and its own V is made huge: each step
// covers one of them whole (one segment at most per step), and the step's collision ray (position +0x190 -> next
// position +0x1A0, cast by 0x154703CF0 right after the advance, before the speed function) spans it. A single
// segment to the end missed everything beyond ~300-450 m of its start (targets at 516-600 m never hit, one at 311 m
// killed). Writing +0x1A0 here would only move the bullet: this step's ray is already cast.
// The step adds its length to the distance flown (+0x164), and the damage falls off with it (0x154703CF0: +0xD4 =
// +0xCC x f(+0x164) x ...): it is set back before each step, so every hit keeps the first segment's damage.
void StraightenPath(uint64_t obj, bool teleport) {
    __try {
        float* p = *reinterpret_cast<float* const*>(obj + kProjPathOff);
        const int n = *reinterpret_cast<const uint16_t*>(obj + kProjPathCountOff);
        if (!p || n < 3 || n > 4096) return;
        float u[3] = {p[3] - p[0], p[4] - p[1], p[5] - p[2]};
        const float first = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (!(first > 0.5f)) return;
        float* dist = reinterpret_cast<float*>(obj + kProjDistOff);
        if (teleport && *reinterpret_cast<const float*>(obj + kProjSpeedVOff) == kTeleportSpeed) {  // teleported
            *dist = first - kTeleportChunk;
            return;
        }
        if (*reinterpret_cast<const float*>(obj + kProjSegmentOff) > 1.0f) return;
        for (float& x : u) x /= first;
        // each point at its distance along the original path: the same segment lengths, never backwards (the end of
        // a long path can fall almost vertically); on a straight path this changes nothing
        float moved = 0, along = first;
        float prev[3] = {p[3], p[4], p[5]};
        for (int i = 2; i < n; ++i) {
            float* q = p + 3 * i;
            const float s[3] = {q[0] - prev[0], q[1] - prev[1], q[2] - prev[2]};
            along += std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            prev[0] = q[0];
            prev[1] = q[1];
            prev[2] = q[2];
            const float t[3] = {p[0] + u[0] * along, p[1] + u[1] * along, p[2] + u[2] * along};
            const float e[3] = {q[0] - t[0], q[1] - t[1], q[2] - t[2]};
            moved = std::max(moved, std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
            q[0] = t[0];
            q[1] = t[1];
            q[2] = t[2];
        }
        if (teleport && n >= 4) {
            // chunks up to the end of the path (along = its length; the last one as long as the points allow), then
            // points 1 m apart, since a segment of zero length would end the flight before that step's ray
            float d = first;
            bool end = false;
            for (int i = 2; i < n; ++i) {
                if (end) {
                    d += 1.0f;
                } else {
                    d = i == n - 1 ? along : std::min(d + kTeleportChunk, along);
                    end = d >= along;
                }
                float* q = p + 3 * i;
                q[0] = p[0] + u[0] * d;
                q[1] = p[1] + u[1] * d;
                q[2] = p[2] + u[2] * d;
            }
            *reinterpret_cast<float*>(obj + kProjSpeedVOff) = kTeleportSpeed;  // the speed function reads it next
            *dist = first - kTeleportChunk;
            g_teleported.fetch_add(1, std::memory_order_relaxed);
        }
        if (moved > 1e-3f) {
            g_straight_paths.fetch_add(1, std::memory_order_relaxed);
            g_straight_drop.store(moved, std::memory_order_relaxed);
            g_straight_points.store(n, std::memory_order_relaxed);
            g_straight_first.store(first, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Magic bullets, diagnostics (MagicHits): at the speed function of the step after the jump, what that step's collision
// test met. 0x154703CF0 keeps the results of its two ray queries in the projectile until its next test: forward at
// +0x1E0, reverse at +0xA60 (hits of 0x80 bytes); the hits it kept for damage are at +0x1310 (0xB0 bytes), applied
// and cleared by 0x154705C00 after the speed function.
template <class Hit>
int CopyHits(uint64_t obj, size_t list_off, Hit* out, int max) {
    uint64_t data = 0;
    uint16_t count = 0;
    if (!SafeCopy(&data, obj + list_off, sizeof data) || !SafeCopy(&count, obj + list_off + 0xA, sizeof count) || !data)
        return 0;
    for (int i = 0; i < count && i < max; ++i)
        if (!SafeCopy(out[i].raw, data + uint64_t(i) * sizeof out[i].raw, sizeof out[i].raw)) return i;
    return count;
}

void DumpMagicHits(uint64_t obj) {
    // its shot: the newest record of this projectile (a projectile object is reused)
    const uint64_t w = g_magic_written.load(std::memory_order_acquire);
    for (uint64_t k = w; k > 0 && k + kMagicRing > w; --k) {
        MagicShotRec& rec = g_magic_ring[(k - 1) % kMagicRing];
        if (rec.seq.load(std::memory_order_acquire) != k || rec.obj != obj) continue;
        if (rec.dumped.exchange(true, std::memory_order_relaxed)) return;
        const uint64_t shot = k - 1;
        std::atomic<uint64_t>& done = g_magic_dump_seq[shot % kMagicRing];
        done.store(0, std::memory_order_relaxed);
        MagicHitDump& d = g_magic_dumps[shot % kMagicRing];
        d.target = rec.addr;
        for (int i = 0; i < 3; ++i) d.head[i] = rec.head[i];
        SafeCopy(d.from, obj + 0x190, sizeof d.from);
        SafeCopy(d.to, obj + kProjNextOff, sizeof d.to);
        SafeCopy(&d.flown, obj + kProjDistOff, sizeof d.flown);
        SafeCopy(&d.segment, obj + kProjSegmentOff, sizeof d.segment);
        SafeCopy(d.s0, obj + 0xC8, sizeof d.s0);
        SafeCopy(d.s1, obj + 0x12E0, sizeof d.s1);
        SafeCopy(d.s2, obj + 0x13C0, sizeof d.s2);
        SafeCopy(d.s3, obj + 0x1480, sizeof d.s3);
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        d.ms = double(q.QuadPart) * g_qpc_s * 1000.0;
        const int nf = CopyHits(obj, kFwdHitsOff, d.fwd, kMagicDumpHits);
        const int nr = CopyHits(obj, kRevHitsOff, d.rev, kMagicDumpHits);
        d.n_kept = CopyHits(obj, kKeptHitsOff, d.kept, kMagicDumpKept);
        d.n_fwd = nf;
        d.n_rev = nr;
        for (int i = 0; i < 2 * kMagicDumpHits; ++i) {
            MagicRayHit& h = i < kMagicDumpHits ? d.fwd[i] : d.rev[i - kMagicDumpHits];
            const int n = i < kMagicDumpHits ? nf : nr;
            h.vtable = h.owner = 0;
            if ((i % kMagicDumpHits) >= n) continue;
            uint64_t o;
            std::memcpy(&o, h.raw + 0x30, sizeof o);
            if (o && SafeCopy(&h.vtable, o, sizeof h.vtable)) SafeCopy(&h.owner, o + 0x10, sizeof h.owner);
        }
        done.store(shot + 1, std::memory_order_release);
        return;
    }
}

// Magic bullets (SetMagicTargets): at the first step of one of the player's bullets, its next position (+0x1A0)
// goes kMagicBefore short of a target's head (extrapolated to the next step), on the side of its approach, and its
// path through that head. The next advance starts there (+0x190 = +0x1A0) without any collision test on the way,
// and that step's test (0x154703CF0, from +0x190 to the new +0x1A0) spans only the last metres. The target: the one
// sent the fewest bullets since the last publish, nearest the crosshair first. True when done now or before.
bool MagicPath(uint64_t obj) {
    __try {
        float* V = reinterpret_cast<float*>(obj + kProjSpeedVOff);
        if (*V == kMagicSpeed) {
            if (g_magic_debug.load(std::memory_order_relaxed)) DumpMagicHits(obj);
            return true;
        }
        const int count = g_magic_count.load(std::memory_order_acquire);
        if (count <= 0) return false;
        if (*reinterpret_cast<const float*>(obj + kProjSegmentOff) > 1.0f) return false;
        float* p = *reinterpret_cast<float* const*>(obj + kProjPathOff);
        const int n = *reinterpret_cast<const uint16_t*>(obj + kProjPathCountOff);
        if (!p || n < 4 || n > 4096) return false;
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        const float age = float(double(q.QuadPart - g_magic_qpc.load(std::memory_order_relaxed)) * g_qpc_s) +
                          game::kSimStepS;
        if (!(age >= 0.0f && age < 0.25f)) return false;  // stale
        int best = -1;
        uint32_t fewest = UINT32_MAX;
        for (int i = 0; i < count && i < kMaxMagicTargets; ++i) {
            const uint32_t s = g_magic_slots[i].shots.load(std::memory_order_relaxed);
            if (g_magic_slots[i].addr.load(std::memory_order_relaxed) && s < fewest) {
                best = i;
                fewest = s;
            }
        }
        if (best < 0) return false;
        MagicSlot& t = g_magic_slots[best];
        t.shots.fetch_add(1, std::memory_order_relaxed);
        float h[3], u[3];
        for (int i = 0; i < 3; ++i) {
            h[i] = t.head[i].load(std::memory_order_relaxed) + t.vel[i].load(std::memory_order_relaxed) * age;
            u[i] = t.dir[i].load(std::memory_order_relaxed);
        }
        const float to[3] = {h[0] - p[0], h[1] - p[1], h[2] - p[2]};
        const float len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (!(len > 1.0f)) return false;
        int approach = 0;
        if (u[0] == 0.0f && u[1] == 0.0f && u[2] == 0.0f) {  // from the bullet's side
            for (int i = 0; i < 3; ++i) u[i] = to[i] / len;
        } else {
            approach = 1;
        }
        const float want = t.before.load(std::memory_order_relaxed);
        const float before = std::fmin(want > 0.0f ? want : kMagicBefore, len - 0.5f);  // close: from near the muzzle
        const float after_want = t.after.load(std::memory_order_relaxed);
        const float after = after_want > 0.0f ? after_want : kMagicAfter;
        for (int i = 1; i < n; ++i) {  // point 1 short of the head, point 2 past it, then 1 m apart
            const float d = i == 1 ? -before : after + float(i - 2);
            float* r = p + 3 * i;
            r[0] = h[0] + u[0] * d;
            r[1] = h[1] + u[1] * d;
            r[2] = h[2] + u[2] * d;
        }
        float* next = reinterpret_cast<float*>(obj + kProjNextOff);
        next[0] = p[3];
        next[1] = p[4];
        next[2] = p[5];
        *reinterpret_cast<float*>(obj + kProjSegmentOff) = 1.0f;
        *V = kMagicSpeed;
        if (const float e = g_magic_pierce.load(std::memory_order_relaxed); e > 0.0f) {
            float* energy = reinterpret_cast<float*>(obj + kProjEnergyOff);
            energy[0] = energy[1] = energy[2] = e;
        }
        g_magic_hits.fetch_add(1, std::memory_order_relaxed);
        g_magic_dist.store(len, std::memory_order_relaxed);
        const uint64_t k = g_magic_written.fetch_add(1);
        MagicShotRec& rec = g_magic_ring[k % kMagicRing];
        rec.seq.store(0, std::memory_order_relaxed);
        rec.addr = t.addr.load(std::memory_order_relaxed);
        rec.dist = len;
        rec.approach = approach;
        rec.obj = obj;
        for (int i = 0; i < 3; ++i) rec.head[i] = h[i];
        rec.dumped.store(false, std::memory_order_relaxed);
        rec.seq.store(k + 1, std::memory_order_release);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TrackProjectile(uint64_t obj) {
    const uint64_t i = g_proj_written.fetch_add(1);
    ProjSample& s = g_proj[i % kProjRing];
    s.seq.store(0, std::memory_order_relaxed);
    s.obj = obj;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    s.qpc = q.QuadPart;
    if (!SafeCopy(s.head, obj, kProjHead) || !SafeCopy(s.body, obj + kProjBodyFrom, kProjBody)) return;
    s.seq.store(i + 1, std::memory_order_release);
}

// Shutdown: set while the breakpoints are taken down; calls of the handler in flight.
std::atomic<bool> g_closing{false};
std::atomic<int> g_inflight{0};

LONG HandlerImpl(EXCEPTION_POINTERS* ep);

// A thread inside the handler while ApplyAll disarms it gets its breakpoints back from its exception context when
// it resumes: during Shutdown the handler clears them there, so no thread is left armed once the handler is gone
// (it was: a crash of the game at an unload while the tracker fired at every step of every projectile).
LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    g_inflight.fetch_add(1, std::memory_order_acq_rel);
    CONTEXT* c = ep->ContextRecord;
    const bool ours_step = ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && (c->Dr6 & 0xF);
    LONG r;
    if (g_closing.load(std::memory_order_acquire) && ours_step) {
        c->Dr6 = 0;
        c->EFlags |= 0x10000;  // RF: an instruction breakpoint is not taken again at once
        r = EXCEPTION_CONTINUE_EXECUTION;
    } else {
        r = HandlerImpl(ep);
    }
    if (g_closing.load(std::memory_order_acquire) && ours_step && r == EXCEPTION_CONTINUE_EXECUTION) {
        c->Dr7 &= ~uint64_t(0xFFFF00FF);
        c->Dr0 = c->Dr1 = c->Dr2 = c->Dr3 = 0;
    }
    g_inflight.fetch_sub(1, std::memory_order_acq_rel);
    return r;
}

LONG HandlerImpl(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const uint64_t fired = c->Dr6 & 0xF;
    if (!fired) return EXCEPTION_CONTINUE_SEARCH;  // not one of our DR0-DR3
    c->Dr6 = 0;
    // An instruction breakpoint fires before the instruction: resume with RF so it is not hit again at once. Its type
    // comes from this thread's own DR7, not from the configuration: a breakpoint left from an earlier one (the recoil
    // mod's, in a slot that watches data now) got no RF and fired again in a loop (MODLOG 2026-10-10).
    for (int i = 0; i < 4; ++i)
        if (((fired >> i) & 1) && dr::IsExec(c->Dr7, i)) c->EFlags |= 0x10000;
    // A thread that kept another configuration: the current one, and the hit is dropped.
    dr::Slots want;
    uint64_t regs[4] = {c->Dr0, c->Dr1, c->Dr2, c->Dr3};
    if (ReadWant(want) && dr::Stale(regs, c->Dr7, want)) {
        uint64_t dr7 = c->Dr7;
        dr::Apply(regs, dr7, want);
        c->Dr0 = regs[0];
        c->Dr1 = regs[1];
        c->Dr2 = regs[2];
        c->Dr3 = regs[3];
        c->Dr7 = dr7;
        g_stale.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (c->Rip >= g_self_lo && c->Rip < g_self_hi) return EXCEPTION_CONTINUE_EXECUTION;
    if (const int ps = g_pellets.slot.load(std::memory_order_acquire); ps >= 0 && (fired & (uint64_t(1) << ps))) {
        PelletJump(c);
        if (!(fired & ~(uint64_t(1) << ps))) return EXCEPTION_CONTINUE_EXECUTION;
    }
    uint64_t mod_slots = 0;
    for (ModState& m : g_mods) {
        const int slot = m.slot.load(std::memory_order_acquire);
        if (slot < 0) continue;
        mod_slots |= uint64_t(1) << slot;
        if (!(fired & (uint64_t(1) << slot))) continue;
        const uintptr_t obj = m.object.load(std::memory_order_relaxed);
        const uint64_t reg = m.reg.load(std::memory_order_relaxed) == int(Reg::Rax) ? c->Rax : c->Rdi;
        if (obj && reg == obj) {
            const float k = m.factor.load(std::memory_order_relaxed);
            for (int i = 0; i < m.n; ++i) ScaleFloat(obj + m.offs[i], k);
            m.hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (mod_slots && !(fired & ~mod_slots)) return EXCEPTION_CONTINUE_EXECUTION;
    const int tracker = g_tracker_slot.load(std::memory_order_relaxed);
    if (tracker >= 0 && (fired & (uint64_t(1) << tracker))) {  // not counted against max_hits
        g_tracker_calls.fetch_add(1, std::memory_order_relaxed);
        if (IsPlayers(c->Rcx)) {
            const int mode = g_bullet_path.load(std::memory_order_relaxed);
            if (!MagicPath(c->Rcx) && mode) StraightenPath(c->Rcx, mode == 2);
            if (g_track_samples.load(std::memory_order_relaxed)) TrackProjectile(c->Rcx);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (const int trig = g_trigger_slot.load(std::memory_order_relaxed); trig >= 0) {
        const uint64_t now = GetTickCount64();
        if (fired & (uint64_t(1) << trig))
            g_window_until.store(now + uint64_t(g_window_ms.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        else if (now > g_window_until.load(std::memory_order_relaxed))
            return EXCEPTION_CONTINUE_EXECUTION;  // outside a window: neither recorded nor counted
    }
    const int n = g_hits.fetch_add(1) + 1;
    if (n > g_max_hits.load()) {
        // stop on this thread (Configure(all 0) clears the others), except the projectile tracker
        uint64_t enables = 0xFF;
        if (tracker >= 0) enables &= ~(uint64_t(3) << (2 * tracker));
        for (ModState& m : g_mods)
            if (const int mod = m.slot.load(std::memory_order_relaxed); mod >= 0)
                enables &= ~(uint64_t(3) << (2 * mod));
        if (const int ps = g_pellets.slot.load(std::memory_order_relaxed); ps >= 0)
            enables &= ~(uint64_t(3) << (2 * ps));
        c->Dr7 &= ~enables;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const uint32_t slot = g_written.load() % kRing;
    Hit& h = g_ring[slot];
    h.tid = GetCurrentThreadId();
    h.slot = (fired & 1) ? 0 : (fired & 2) ? 1 : (fired & 4) ? 2 : 3;
    h.rip = c->Rip;
    const uint64_t r[16] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi,
                            c->R8,  c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
    for (int i = 0; i < 16; ++i) h.regs[i] = r[i];
    h.n_returns = 0;
    const auto* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
    // the faulting thread's own stack (this thread's): readable up to its base
    const uint64_t top = reinterpret_cast<uint64_t>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
    const int slots = int(std::min<uint64_t>(kStackSlots, c->Rsp < top ? (top - c->Rsp) / 8 : 0));
    for (int i = 0; i < slots && h.n_returns < kMaxReturns; ++i) {
        const uint64_t v = sp[i];
        if (v >= g_image_lo && v < g_image_hi) h.returns[h.n_returns++] = v;
    }
    h.ok_rcx = SafeCopy(h.mem_rcx, c->Rcx, kDump);
    h.ok_rdx = SafeCopy(h.mem_rdx, c->Rdx, kDump);
    g_written.fetch_add(1);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Applies DR0/DR7 to every other thread of the process. No allocation while a thread is suspended:
// it may hold the heap lock.
void ApplyAll(const uintptr_t (&addrs)[4], int len, const Mode (&modes)[4]) {
    std::vector<DWORD> tids;
    tids.reserve(512);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
    CloseHandle(snap);

    const dr::Slots s = ToSlots(addrs, modes, len);
    for (DWORD tid : tids) {
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                uint64_t regs[4], dr7 = ctx.Dr7;
                dr::Apply(regs, dr7, s);
                ctx.Dr0 = regs[0];
                ctx.Dr1 = regs[1];
                ctx.Dr2 = regs[2];
                ctx.Dr3 = regs[3];
                ctx.Dr7 = dr7;
                ctx.Dr6 = 0;
                SetThreadContext(t, &ctx);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
}

}  // namespace

void Configure(const uintptr_t (&addrs)[4], const Mode (&modes)[4], int len, int max_hits) {
    const bool any = addrs[0] || addrs[1] || addrs[2] || addrs[3];
    bool same = len == g_len;
    for (int i = 0; i < 4; ++i) same = same && addrs[i] == g_addrs[i] && modes[i] == g_modes[i];
    if (same && (!any || g_armed.load())) return;
    if (!g_image_lo) {
        auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        g_image_lo = reinterpret_cast<uintptr_t>(base);
        g_image_hi = g_image_lo + nt->OptionalHeader.SizeOfImage;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Handler), &self);
        auto* sb = reinterpret_cast<const uint8_t*>(self);
        const auto* snt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            sb + reinterpret_cast<const IMAGE_DOS_HEADER*>(sb)->e_lfanew);
        g_self_lo = reinterpret_cast<uintptr_t>(sb);
        g_self_hi = g_self_lo + snt->OptionalHeader.SizeOfImage;
    }
    for (int i = 0; i < 4; ++i) {
        g_addrs[i] = addrs[i];
        g_modes[i] = modes[i];
    }
    g_len = len;
    if (any) {
        g_hits = 0;
        g_max_hits = max_hits;
        SetWant(ToSlots(addrs, modes, len));
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &Handler);
        ApplyAll(addrs, len, modes);
        g_armed = true;
    } else {
        SetWant(dr::Slots{});
        ApplyAll(kNone, 0, kWrites);
        g_armed = false;
    }
}

void SetTracker(int slot) { g_tracker_slot = slot; }
void SetBulletPath(int mode) { g_bullet_path.store(mode, std::memory_order_relaxed); }
void SetTrackSamples(bool on) { g_track_samples.store(on, std::memory_order_relaxed); }
void SetMagicDebug(bool on) { g_magic_debug.store(on, std::memory_order_relaxed); }
void SetMagicPierce(float energy) { g_magic_pierce.store(energy, std::memory_order_relaxed); }
void SetMagicTargets(const MagicTarget* t, int n) {
    if (g_qpc_s == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_s = 1.0 / double(f.QuadPart);
    }
    n = std::clamp(n, 0, kMaxMagicTargets);
    for (int i = 0; i < kMaxMagicTargets; ++i) {
        MagicSlot& s = g_magic_slots[i];
        s.shots.store(0, std::memory_order_relaxed);
        if (i >= n) {
            s.addr.store(0, std::memory_order_relaxed);
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            s.head[k].store(t[i].head[k], std::memory_order_relaxed);
            s.vel[k].store(t[i].vel[k], std::memory_order_relaxed);
            s.dir[k].store(t[i].dir[k], std::memory_order_relaxed);
        }
        s.before.store(t[i].before, std::memory_order_relaxed);
        s.after.store(t[i].after, std::memory_order_relaxed);
        s.addr.store(t[i].addr, std::memory_order_relaxed);
    }
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_magic_qpc.store(q.QuadPart, std::memory_order_relaxed);
    g_magic_count.store(n, std::memory_order_release);
}
int MagicShotsSince(uint64_t& seq, MagicShot* out, int max) {
    int n = 0;
    const uint64_t w = g_magic_written.load(std::memory_order_acquire);
    if (w > seq + kMagicRing) seq = w - kMagicRing;
    for (; seq < w && n < max; ++seq) {
        const MagicShotRec& r = g_magic_ring[seq % kMagicRing];
        if (r.seq.load(std::memory_order_acquire) != seq + 1) break;  // still being written: next time
        out[n++] = {r.addr, r.dist, r.approach, seq};
    }
    return n;
}
bool MagicHits(uint64_t seq, MagicHitDump& out) {
    const std::atomic<uint64_t>& done = g_magic_dump_seq[seq % kMagicRing];
    if (done.load(std::memory_order_acquire) != seq + 1) return false;
    out = g_magic_dumps[seq % kMagicRing];
    return done.load(std::memory_order_acquire) == seq + 1;  // not rewritten meanwhile
}
MagicStats Magic() { return {g_magic_hits.load(std::memory_order_relaxed), g_magic_dist.load(std::memory_order_relaxed)}; }
StraightStats Straightened() {
    const auto rel = std::memory_order_relaxed;
    return {g_straight_paths.load(rel), g_straight_drop.load(rel), g_straight_first.load(rel),
            g_straight_points.load(rel), g_teleported.load(rel)};
}
void SetTrigger(int slot, int window_ms) {
    g_window_ms = window_ms;
    g_trigger_slot = slot;
}
uint64_t TrackerCalls() { return g_tracker_calls.load(std::memory_order_relaxed); }
uint64_t TrackerSamples() { return g_proj_written.load(std::memory_order_relaxed); }
uint64_t StaleThreads() { return g_stale.load(std::memory_order_relaxed); }

void SetCamera(const float (&pos)[3], const float (&forward)[3]) {
    for (int i = 0; i < 3; ++i) {
        g_cam[i].store(pos[i], std::memory_order_relaxed);
        g_fwd[i].store(forward[i], std::memory_order_relaxed);
    }
}

void SetScaleMod(int id, const ScaleMod& m) {
    if (id < 0 || id >= kMaxScaleMods) return;
    ModState& s = g_mods[id];
    if (m.slot < 0) {
        s.slot.store(-1, std::memory_order_release);
        return;
    }
    if (s.slot.load(std::memory_order_relaxed) < 0) {  // offsets only while it is off
        s.n = std::min(m.n, 8);
        for (int i = 0; i < s.n; ++i) s.offs[i] = m.offs[i];
    }
    s.reg.store(int(m.reg), std::memory_order_relaxed);
    s.object.store(m.object, std::memory_order_relaxed);
    s.factor.store(m.factor, std::memory_order_relaxed);
    s.slot.store(m.slot, std::memory_order_release);
}
uint64_t ScaleModHits(int id) {
    return id >= 0 && id < kMaxScaleMods ? g_mods[id].hits.load(std::memory_order_relaxed) : 0;
}

void SetPelletMod(const PelletMod& m) {
    if (m.slot < 0) {
        g_pellets.slot.store(-1, std::memory_order_release);
        return;
    }
    if (g_pellets.slot.load(std::memory_order_relaxed) < 0) {  // addresses only while it is off
        g_pellets.loop = m.loop;
        g_pellets.pellets_off = m.pellets_off;
        g_pellets.next_off = m.next_off;
    }
    g_pellets.weapon.store(m.weapon, std::memory_order_relaxed);
    g_pellets.count.store(m.count, std::memory_order_relaxed);
    g_pellets.slot.store(m.slot, std::memory_order_release);
}
uint64_t PelletShots() { return g_pellets.shots.load(std::memory_order_relaxed); }
uint64_t LastShotHandler() { return g_shot_handler.load(std::memory_order_relaxed); }
int ShotTimesSince(uint64_t& seq, double* ms, int max) {
    static const double ms_per_tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / double(f.QuadPart);
    }();
    const uint64_t w = g_shot_count.load(std::memory_order_acquire);
    if (w > seq + kShotRing) seq = w - kShotRing;
    int n = 0;
    for (; seq < w && n < max; ++seq)
        ms[n++] = double(g_shot_qpc[seq % kShotRing].load(std::memory_order_relaxed)) * ms_per_tick;
    return n;
}
ShotStats ShotStatsNow() {
    static const double ms_per_tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / double(f.QuadPart);
    }();
    ShotStats s;
    s.count = g_shot_count.load(std::memory_order_acquire);
    const int n = int(std::min<uint64_t>(s.count, kShotRing));
    int64_t t[kShotRing];
    for (int i = 0; i < n; ++i) t[i] = g_shot_qpc[(s.count - n + i) % kShotRing].load(std::memory_order_relaxed);
    float d[kShotRing];
    int m = 0;
    for (int i = 1; i < n; ++i) {
        const double ms = double(t[i] - t[i - 1]) * ms_per_tick;
        if (ms > 0 && ms < 500) d[m++] = float(ms);  // within a burst
    }
    if (m) {
        std::nth_element(d, d + m / 2, d + m);
        s.interval_ms = d[m / 2];
    }
    return s;
}

void SetPlayerHead(const float (&pos)[3], bool ok) {
    for (int i = 0; i < 3; ++i) g_head[i].store(pos[i], std::memory_order_relaxed);
    g_head_ok.store(ok, std::memory_order_relaxed);
}

namespace {

float F(const uint8_t* p, size_t off) {
    float v;
    std::memcpy(&v, p + off, 4);
    return v;
}

void DrainProjectiles(Telemetry& tel, ProjFn on_proj, void* ctx) {
    static double qpc_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / double(f.QuadPart);
    }();
    static uint64_t last_obj = 0;
    static float last_el = 0;
    const uint64_t w = g_proj_written.load(std::memory_order_acquire);
    if (w - g_proj_read > kProjRing) g_proj_read = w - kProjRing;
    char buf[24], hex[kProjHead * 2 + 1];
    for (; g_proj_read < w; ++g_proj_read) {
        const ProjSample& s = g_proj[g_proj_read % kProjRing];
        if (s.seq.load(std::memory_order_acquire) != g_proj_read + 1) {
            if (w - g_proj_read < 64) break;  // still being written: next time
            continue;                         // lost (copy failed)
        }
        const uint8_t* b = s.body;  // obj + 0x110
        auto at = [&](size_t obj_off) { return F(b, obj_off - kProjBodyFrom); };
        std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(s.obj));
        const float el = at(0x120);
        int32_t mode;
        std::memcpy(&mode, s.head + 0x4c, 4);
        if (s.obj != last_obj || el < last_el) {  // a new projectile: its constant part, once
            for (int part = 0; part < 2; ++part) {
                const uint8_t* m = part ? s.body : s.head;
                for (size_t i = 0; i < kProjHead; ++i) std::snprintf(hex + i * 2, 3, "%02x", m[i]);
                Line l;
                JsonLine(l, "proj_head", tel.SinceStartMs(), 0)
                    .Str("obj", buf)
                    .Int("off", part ? int(kProjBodyFrom) : 0)
                    .Str("hex", hex)
                    .Finish();
                tel.Push(Channel::Writer, l);
            }
        }
        last_obj = s.obj;
        last_el = el;
        if (on_proj) {
            ProjView v{s.obj, el, {at(0x190), at(0x194), at(0x198)}, {at(0x150), at(0x154), at(0x158)},
                       {at(0x130), at(0x134), at(0x138)}, at(0x1b4), at(0x1bc), at(0x168),
                       {at(0x140), at(0x144), at(0x148)}, at(0x1b8), mode};
            on_proj(v, ctx);
        }
        Line l;
        JsonLine(l, "proj", tel.SinceStartMs(), 0)
            .Str("obj", buf)
            .Num("ms", double(s.qpc) * qpc_ms)
            .Num("el", el)
            .Num("seg", at(0x124))
            .Num("px", at(0x190)).Num("py", at(0x194)).Num("pz", at(0x198))
            .Num("nx", at(0x1a0)).Num("ny", at(0x1a4)).Num("nz", at(0x1a8))
            .Num("dx", at(0x140)).Num("dy", at(0x144)).Num("dz", at(0x148))
            .Num("v", at(0x1b8))
            .Num("v0", at(0x1bc))
            .Num("V", at(0x1b4))
            .Num("dist", at(0x164))
            .Int("mode", mode)
            .Finish();
        tel.Push(Channel::Writer, l);
    }
}

}  // namespace

void Drain(Telemetry& tel, HitFn on_hit, void* ctx, ProjFn on_proj) {
    DrainProjectiles(tel, on_proj, ctx);
    const uint32_t w = g_written.load();
    char buf[24];
    static const char* const kReg[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                         "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    if (w - g_read > uint32_t(kRing)) {  // the handler lapped us: those slots hold newer hits
        g_dropped += w - g_read - kRing;
        g_read = w - kRing;
        Line l;
        JsonLine(l, "hwbp_dropped", tel.SinceStartMs(), 0).Int("total", int64_t(g_dropped)).Finish();
        tel.Push(Channel::Writer, l);
    }
    for (; g_read < w; ++g_read) {
        const Hit h = g_ring[g_read % kRing];
        if (on_hit) on_hit({h.rip - g_image_lo, h.regs, h.mem_rcx, h.mem_rdx, h.ok_rcx, h.ok_rdx}, ctx);
        Line l1;
        JsonLine j1(l1, "hwbp_hit", tel.SinceStartMs(), 0);
        j1.Int("n", g_read).Int("tid", h.tid).Int("slot", h.slot);
        std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h.rip - g_image_lo));
        j1.Str("rip_rva", buf);
        for (int i = 0; i < 16; ++i) {
            std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h.regs[i]));
            j1.Str(kReg[i], buf);
        }
        j1.Finish();
        tel.Push(Channel::Writer, l1);
        Line l2;
        JsonLine j2(l2, "hwbp_stack", tel.SinceStartMs(), 0);
        j2.Int("n", g_read);
        char key[8];
        for (int i = 0; i < h.n_returns; ++i) {
            std::snprintf(key, sizeof key, "r%d", i);
            std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(h.returns[i] - g_image_lo));
            j2.Str(key, buf);
        }
        j2.Finish();
        tel.Push(Channel::Writer, l2);
        // memory at [rcx] and [rdx], 0x40 bytes of hex per line
        char hex[0x40 * 2 + 1];
        for (int which = 0; which < 2; ++which) {
            const bool ok = which ? h.ok_rdx : h.ok_rcx;
            if (!ok) continue;
            const uint8_t* m = which ? h.mem_rdx : h.mem_rcx;
            for (int off = 0; off < kDump; off += 0x40) {
                for (int i = 0; i < 0x40; ++i) std::snprintf(hex + i * 2, 3, "%02x", m[off + i]);
                Line l3;
                JsonLine(l3, "hwbp_mem", tel.SinceStartMs(), 0)
                    .Int("n", g_read)
                    .Str("reg", which ? "rdx" : "rcx")
                    .Int("off", off)
                    .Str("hex", hex)
                    .Finish();
                tel.Push(Channel::Writer, l3);
            }
        }
    }
}

void Shutdown() {
    auto drain = [] {
        for (int i = 0; i < 1000 && g_inflight.load(std::memory_order_acquire) != 0; ++i) Sleep(1);
    };
    g_closing.store(true, std::memory_order_release);
    ApplyAll(kNone, 0, kWrites);
    g_armed = false;
    // the threads that were inside the handler leave it disarmed (Handler); a second pass for any other
    drain();
    Sleep(100);
    ApplyAll(kNone, 0, kWrites);
    drain();
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
    drain();
    Sleep(50);  // a call between its entry and the counter
}

}  // namespace grw::hwbp
