#include "game/registry.h"
#include <algorithm>
#include <cstring>
#include "game/model.h"

namespace grw::game {

namespace {

constexpr uint64_t kRetryPasses = 5;  // an entity without a character object: search again 5 passes later

template <class T> bool Get(ReadFn read, void* ctx, uintptr_t addr, T& out) { return read(addr, &out, sizeof out, ctx); }

bool PlausiblePointer(uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFF0000 && (p & 7) == 0; }

// a: a character object owned by entity o
bool IsCharOf(uintptr_t base, ReadFn read, void* ctx, uintptr_t a, uintptr_t o) {
    uint64_t d[3];
    return PlausiblePointer(a) && read(a, d, sizeof d, ctx) && d[0] == base + kCharVtableRva &&
           d[kCharOwnerOff / 8] == o;
}

uintptr_t FindChar(uintptr_t base, ReadFn read, void* ctx, uintptr_t o) {
    uintptr_t comps = 0;
    uint64_t q[kComponentsSearch / 8];
    if (!Get(read, ctx, o + kEntityComponentsOff, comps) || !PlausiblePointer(comps) || !read(comps, q, sizeof q, ctx))
        return 0;
    for (uint64_t a : q)
        if (IsCharOf(base, read, ctx, a, o)) return a;
    return 0;
}

uintptr_t WeaponState(uintptr_t base, ReadFn read, void* ctx, uintptr_t player) {
    uintptr_t comps = 0, holder = 0, state = 0;
    uint64_t vt = 0;
    if (!Get(read, ctx, player + kEntityComponentsOff, comps) || !PlausiblePointer(comps) ||
        !Get(read, ctx, comps + kComponentsWeaponHolderOff, holder) || !PlausiblePointer(holder) ||
        !Get(read, ctx, holder, vt) || vt != base + kWeaponHolderVtableRva ||
        !Get(read, ctx, holder + kWeaponHolderStateOff, state) || !PlausiblePointer(state) ||
        !Get(read, ctx, state, vt) || vt != base + kWeaponStateVtableRva)
        return 0;
    return state;
}

// A slot array {entity, component, flags} x count: at least 2 entities (by vtable) among the first slots and a
// quarter of the non-empty ones.
bool LooksLikeSlots(uintptr_t base, ReadFn read, void* ctx, uintptr_t data, uint16_t count) {
    if (!PlausiblePointer(data) || count == 0 || count > kEntityListMaxSlots) return false;
    const size_t n = std::min<size_t>(count, 32);
    uint8_t buf[32 * kEntityListStride];
    if (!read(data, buf, n * kEntityListStride, ctx)) return false;
    size_t used = 0, entities = 0;
    for (size_t i = 0; i < n; ++i) {
        uintptr_t o;
        std::memcpy(&o, buf + i * kEntityListStride, 8);
        if (!o) continue;
        ++used;
        uint64_t vt = 0;
        if (PlausiblePointer(o) && Get(read, ctx, o, vt) && vt == base + kEntityVtableRva) ++entities;
    }
    return entities >= 2 && entities * 4 >= used;
}

}  // namespace

// The slot array: [[world + 0xE0] + 0xE0] (camera-findings.md section 8). After a zone reload (2026-10-09
// afternoon) that list object holds other data (floats at + 0xE0): rejected, so the caller falls back to its
// scans. The arrays then seen elsewhere ([world + 0xBA0], [[world + 0xE0] - 0x200 + 0x38]) each held only part
// of the characters (15-17 of 26-40): not used.
bool EntityRegistry::Resolve(uintptr_t base, ReadFn read, void* ctx, uintptr_t world, uintptr_t& data,
                             uint16_t& count) {
    uintptr_t list = 0;
    uint64_t vt = 0;
    return Get(read, ctx, world + kWorldEntityListOff, list) && PlausiblePointer(list) && Get(read, ctx, list, vt) &&
           vt == base + kEntityListVtableRva && Get(read, ctx, list + kEntityListDataOff, data) &&
           Get(read, ctx, list + kEntityListCountOff, count) && LooksLikeSlots(base, read, ctx, data, count);
}

// The character update job's array (model.h, kCharJobHolderRva): the character objects themselves.
bool EntityRegistry::ReadJob(uintptr_t base, ReadFn read, void* ctx, std::vector<uintptr_t>& chars,
                             RegistryPass& r) {
    uintptr_t holder = 0, job = 0, data = 0;
    uint64_t vt = 0;
    uint32_t size = 0;
    if (!Get(read, ctx, base + kCharJobHolderRva, holder) || !PlausiblePointer(holder) ||
        !Get(read, ctx, holder, vt) || vt != base + kCharJobHolderVtableRva ||
        !Get(read, ctx, holder + kHolderCharJobOff, job) || !PlausiblePointer(job) || !Get(read, ctx, job, vt) ||
        vt != base + kJobVtableRva || !Get(read, ctx, job + kJobFnOff, vt) || vt != base + kCharUpdateFnRva ||
        !Get(read, ctx, job + kJobCharsOff, data) || !Get(read, ctx, job + kJobCharsSizeOff, size) ||
        size > kJobCharsMax)
        return false;
    job_chars_.resize(size);
    if (size && (!PlausiblePointer(data) || !read(data, job_chars_.data(), size * sizeof(uintptr_t), ctx)))
        return false;
    r.ok = true;
    r.source = 1;
    r.slots = size;
    for (uintptr_t a : job_chars_) {
        uint64_t d[3];
        if (!PlausiblePointer(a) || !read(a, d, sizeof d, ctx) || d[0] != base + kCharVtableRva) continue;
        const uintptr_t o = d[kCharOwnerOff / 8];
        uint8_t od[kEntityFlagsOff + 4];
        if (!PlausiblePointer(o) || !read(o, od, sizeof od, ctx)) continue;
        std::memcpy(&vt, od, 8);
        if (vt != base + kEntityVtableRva) continue;
        uint32_t flags;
        std::memcpy(&flags, od + kEntityFlagsOff, 4);
        ++r.entities;
        if (Classify(flags) == Category::Player) r.player_entity = o;
        chars.push_back(a);
    }
    if (r.player_entity) r.weapon_state = WeaponState(base, read, ctx, r.player_entity);
    return true;
}

RegistryPass EntityRegistry::Read(uintptr_t base, ReadFn read, void* ctx, std::vector<uintptr_t>& chars) {
    RegistryPass r;
    chars.clear();
    ++pass_;
    if (ReadJob(base, read, ctx, chars, r)) return r;
    chars.clear();
    r = {};
    uintptr_t world = 0, data = 0;
    uint64_t vt = 0;
    uint16_t count = 0;
    if (!Get(read, ctx, base + kWorldRootRva, world) || !PlausiblePointer(world) || !Get(read, ctx, world, vt) ||
        vt != base + kWorldVtableRva)
        return r;
    if (!Resolve(base, read, ctx, world, data, count)) return r;
    const size_t n = std::min<size_t>(count, kEntityListMaxSlots);
    slots_.resize(n * kEntityListStride);
    if (n && (!PlausiblePointer(data) || !read(data, slots_.data(), slots_.size(), ctx))) return r;
    r.ok = true;
    r.source = 2;
    r.slots = uint32_t(n);

    entities_.clear();
    for (size_t i = 0; i < n; ++i) {
        uintptr_t o;
        std::memcpy(&o, slots_.data() + i * kEntityListStride, 8);
        if (PlausiblePointer(o)) entities_.push_back(o);
    }
    std::sort(entities_.begin(), entities_.end());
    entities_.erase(std::unique(entities_.begin(), entities_.end()), entities_.end());

    next_.clear();
    auto kit = known_.begin();
    for (uintptr_t o : entities_) {
        uint8_t od[kEntityFlagsOff + 4];
        if (!read(o, od, sizeof od, ctx)) continue;
        std::memcpy(&vt, od, 8);
        if (vt != base + kEntityVtableRva) continue;  // a slot of another kind
        uint32_t flags;
        std::memcpy(&flags, od + kEntityFlagsOff, 4);
        ++r.entities;
        if (Classify(flags) == Category::Player) r.player_entity = o;

        while (kit != known_.end() && kit->entity < o) ++kit;
        Known k{o, 0, 0};
        if (kit != known_.end() && kit->entity == o) k = *kit;
        if (k.chr && !IsCharOf(base, read, ctx, k.chr, o)) k = {o, 0, 0};
        if (!k.chr && pass_ >= k.retry) {
            ++r.searched;
            k.chr = FindChar(base, read, ctx, o);
            if (!k.chr) k.retry = pass_ + kRetryPasses;
        }
        next_.push_back(k);
        if (k.chr) chars.push_back(k.chr);
    }
    known_.swap(next_);
    if (r.player_entity) r.weapon_state = WeaponState(base, read, ctx, r.player_entity);
    return r;
}

}  // namespace grw::game
