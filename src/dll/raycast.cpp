#include "dll/raycast.h"

#include <windows.h>
#include <cstring>
#include "core/mem.h"
#include "game/model.h"

namespace grw::raycast {
namespace {

// Image-relative addresses (camera-findings.md section 9). Thunks where the game calls through one.
constexpr uintptr_t kFilterCtorRva = 0x1912D20;    // ctx(ctx*, u64 mask, PW*, bool flag) -> ctx*
constexpr uintptr_t kFilterDtorRva = 0x1919360;    // ~ctx(ctx*)
constexpr uintptr_t kLockSharedRva = 0x3621560;    // AcquireSRWLockShared + reader count
constexpr uintptr_t kUnlockSharedRva = 0x3624C40;  // reader count - 1 + ReleaseSRWLockShared
constexpr uintptr_t kCastRayRva = 0x30F96C0;       // hknpWorld::castRay(world, query*, collector*)
constexpr uintptr_t kClosestHitVtRva = 0x3CC58B8;  // hknpClosestHitCollector
constexpr uintptr_t kCollectorVtRva = 0x3CC4FF0;   // hknpCollisionQueryCollector (what its destructor leaves)

// [[base + kWorldRootRva] + 0xD08] = PW; world = [PW + 0x10]; lock = [PW + 0x40]; filter = [world + 0x9D8]
constexpr size_t kPhysicsOff = 0xD08;
constexpr size_t kHkWorldOff = 0x10;
constexpr size_t kLockOff = 0x40;
constexpr size_t kLockOwnerOff = 0xC;  // u32 thread id of an exclusive owner
constexpr size_t kFilterOff = 0x9D8;

using FilterCtorFn = void*(__fastcall*)(void* ctx, uint64_t mask, void* pw, bool flag);
using VoidFn = void(__fastcall*)(void* p);
using CastRayFn = void(__fastcall*)(void* world, void* query, void* collector);

struct Game {
    uintptr_t base = 0;
    FilterCtorFn filter_ctor = nullptr;
    VoidFn filter_dtor = nullptr, lock = nullptr, unlock = nullptr;
    CastRayFn cast = nullptr;
};

const Game& G() {
    static const Game g = [] {
        Game x;
        x.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        x.filter_ctor = reinterpret_cast<FilterCtorFn>(x.base + kFilterCtorRva);
        x.filter_dtor = reinterpret_cast<VoidFn>(x.base + kFilterDtorRva);
        x.lock = reinterpret_cast<VoidFn>(x.base + kLockSharedRva);
        x.unlock = reinterpret_cast<VoidFn>(x.base + kUnlockSharedRva);
        x.cast = reinterpret_cast<CastRayFn>(x.base + kCastRayRva);
        return x;
    }();
    return g;
}

template <class T> void Put(uint8_t* p, size_t off, T v) { std::memcpy(p + off, &v, sizeof v); }
template <class T> T Get(const uint8_t* p, size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

// hkcdRay as the game builds it: origin, direction (= to - from, w = 1), inverse direction (0 -> FLT_MAX)
// with w = 0.5f's bits | a bit per axis whose direction is >= 0.
void BuildQuery(uint8_t* q, uint64_t filter, uint64_t filter_data, const Vec3& from, const Vec3& to) {
    std::memset(q, 0, 0x70);
    Put<uint64_t>(q, 0x00, filter);
    Put<uint64_t>(q, 0x08, filter_data);
    Put<uint16_t>(q, 0x10, 0xFFFF);  // material id: none
    Put<uint32_t>(q, 0x20, 2);       // level of detail, as in every query seen
    const float d[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
    const float o[3] = {from.x, from.y, from.z};
    uint32_t signs = 0;
    for (int i = 0; i < 3; ++i) {
        Put<float>(q, 0x30 + 4 * i, o[i]);
        Put<float>(q, 0x40 + 4 * i, d[i]);
        uint32_t inv_bits = 0x7F7FFFEE;
        if (d[i] != 0.0f) {
            const float inv = 1.0f / d[i];
            std::memcpy(&inv_bits, &inv, 4);
        }
        Put<uint32_t>(q, 0x50 + 4 * i, inv_bits);
        if (d[i] >= 0.0f) signs |= 1u << i;
    }
    Put<float>(q, 0x4C, 1.0f);
    Put<uint32_t>(q, 0x5C, 0x3F000000u | signs);
}

// hknpClosestHitCollector, initialised field by field like the game's ray sites (FC0E0B2).
void BuildCollector(uint8_t* c, uintptr_t vtable) {
    std::memset(c, 0, 0xA0);
    Put<uint64_t>(c, 0x00, vtable);
    for (int i = 0; i < 4; ++i) Put<uint32_t>(c, 0x20 + 4 * i, 0x5F7FFFF0);  // early-out fraction: none
    Put<uint32_t>(c, 0x50, 0x7F7FFFEE);  // closest fraction: FLT_MAX
    Put<uint32_t>(c, 0x58, 0x00FFFFFF);  // body id: invalid
    Put<uint16_t>(c, 0x5C, 0xFFFF);
    Put<int32_t>(c, 0x60, -1);
    Put<int32_t>(c, 0x68, -1);
    Put<uint32_t>(c, 0x78, 0x00FFFFFF);
    Put<uint16_t>(c, 0x7C, 0xFFFF);
    Put<int32_t>(c, 0x80, -1);
    Put<int32_t>(c, 0x88, -1);
}

int Capture(EXCEPTION_POINTERS* ep, Result& r) {
    r.fault_code = ep->ExceptionRecord->ExceptionCode;
    const uint64_t at = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
    const uint64_t base = G().base;
    r.fault_rva = at >= base && at < base + 0x20000000 ? at - base : at;
    if (ep->ExceptionRecord->NumberParameters >= 2) r.fault_data = ep->ExceptionRecord->ExceptionInformation[1];
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects here: __try needs a frame without unwinding.
bool CallGuarded(const Game& g, uint8_t* ctx, uint8_t* query, uint8_t* coll, uintptr_t pw, uintptr_t world,
                 uintptr_t lock, uint64_t mask, bool flag, const Vec3& from, const Vec3& to, uint64_t filter,
                 Result& r) {
    volatile bool locked = false;
    volatile int stage = 0;
    __try {
        stage = 1;
        g.filter_ctor(ctx, mask, reinterpret_cast<void*>(pw), flag);
        BuildQuery(query, filter, Get<uint64_t>(ctx, 0x08), from, to);
        BuildCollector(coll, g.base + kClosestHitVtRva);
        stage = 2;
        const uint32_t owner = *reinterpret_cast<volatile uint32_t*>(lock + kLockOwnerOff);
        if (owner != GetCurrentThreadId()) {
            g.lock(reinterpret_cast<void*>(lock));
            locked = true;
        }
        stage = 3;
        g.cast(reinterpret_cast<void*>(world), query, coll);
        stage = 4;
        if (locked) {
            locked = false;
            g.unlock(reinterpret_cast<void*>(lock));
        }
        Put<uint64_t>(coll, 0x00, g.base + kCollectorVtRva);
        stage = 5;
        g.filter_dtor(ctx);
        r.stage = stage;
        return true;
    } __except (Capture(GetExceptionInformation(), r)) {
        r.fault = true;
        r.stage = stage;
    }
    __try {
        if (locked) g.unlock(reinterpret_cast<void*>(lock));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;  // the context lives on our stack: nothing to free if its destructor was not reached
}

}  // namespace

Result Cast(const Vec3& from, const Vec3& to, uint64_t mask, bool flag) {
    Result r;
    const Game& g = G();
    uintptr_t game_world = 0, pw = 0, world = 0, lock = 0, filter = 0;
    if (!mem::Read(g.base + game::kWorldRootRva, game_world) || !game_world) return r;
    if (!mem::Read(game_world + kPhysicsOff, pw) || !pw) return r;
    if (!mem::Read(pw + kHkWorldOff, world) || !world) return r;
    if (!mem::Read(pw + kLockOff, lock) || !lock) return r;
    if (!mem::Read(world + kFilterOff, filter)) return r;
    uint32_t owner = 0;
    if (!mem::Read(lock + kLockOwnerOff, owner)) return r;

    alignas(16) uint8_t ctx[0x80], query[0x70], coll[0xA0];
    if (!CallGuarded(g, ctx, query, coll, pw, world, lock, mask, flag, from, to, filter, r)) return r;
    r.ok = true;
    r.hit = Get<uint32_t>(coll, 0x0C) != 0;
    if (r.hit) {
        r.pos = {Get<float>(coll, 0x30), Get<float>(coll, 0x34), Get<float>(coll, 0x38)};
        r.normal = {Get<float>(coll, 0x40), Get<float>(coll, 0x44), Get<float>(coll, 0x48)};
        r.fraction = Get<float>(coll, 0x50);
        r.body = Get<uint32_t>(coll, 0x78);
        r.material = Get<uint16_t>(coll, 0x7C);
        r.filter_info = Get<uint32_t>(coll, 0x84);
        std::memcpy(r.raw, coll + 0x50, sizeof r.raw);
    }
    return r;
}

}  // namespace grw::raycast
