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

}  // namespace

RegistryPass EntityRegistry::Read(uintptr_t base, ReadFn read, void* ctx, std::vector<uintptr_t>& chars) {
    RegistryPass r;
    chars.clear();
    ++pass_;
    uintptr_t world = 0, list = 0, data = 0;
    uint64_t vt = 0;
    uint16_t count = 0;
    if (!Get(read, ctx, base + kWorldRootRva, world) || !PlausiblePointer(world) || !Get(read, ctx, world, vt) ||
        vt != base + kWorldVtableRva)
        return r;
    if (!Get(read, ctx, world + kWorldEntityListOff, list) || !PlausiblePointer(list) || !Get(read, ctx, list, vt) ||
        vt != base + kEntityListVtableRva)
        return r;
    if (!Get(read, ctx, list + kEntityListDataOff, data) || !Get(read, ctx, list + kEntityListCountOff, count))
        return r;
    const size_t n = std::min<size_t>(count, kEntityListMaxSlots);
    slots_.resize(n * kEntityListStride);
    if (n && (!PlausiblePointer(data) || !read(data, slots_.data(), slots_.size(), ctx))) return r;
    r.ok = true;
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
