#include "game/resources.h"
#include "game/model.h"

namespace grw::game {

namespace {

template <class T> bool Get(ReadFn read, void* ctx, uintptr_t addr, T& out) { return read(addr, &out, sizeof out, ctx); }

// The largest last index of a list read (the game's has 7).
constexpr uint64_t kMaxLastIndex = 63;

bool FromId(uint32_t id, Resource& r) {
    switch (id) {
        case kFuelId: r = Resource::Fuel; return true;
        case kMedicineId: r = Resource::Medicine; return true;
        case kCommsId: r = Resource::Comms; return true;
        case kFoodId: r = Resource::Food; return true;
        default: return false;
    }
}

bool ReadProtected(ReadFn read, void* ctx, uintptr_t elem, ResourceEntry& e) {
    uint64_t ptrs[4] = {};
    uint32_t key = 0;
    if (!elem || !read(elem, ptrs, sizeof ptrs, ctx) || !Get(read, ctx, elem + kProtectedKeyOff, key)) return false;
    uint8_t b[4];
    for (int k = 0; k < 4; ++k)
        if (!ptrs[k] || !Get(read, ctx, uintptr_t(ptrs[k]), b[k])) return false;
    e = {};
    e.ok = true;
    e.addr = elem;
    for (int k = 0; k < 4; ++k) e.bytes[k] = uintptr_t(ptrs[k]);
    e.key = key;
    e.value = DecodeProtected(b, key);
    return true;
}

// The four protected amounts (model.h, kResourceOwnerRva): all of them, or an error and none.
const char* ReadStore(uintptr_t base, ReadFn read, void* ctx, ResourceState& s) {
    uintptr_t owner = 0, store = 0, vt = 0;
    if (!Get(read, ctx, base + kResourceOwnerRva, owner) || !owner || !Get(read, ctx, owner, vt) ||
        vt != base + kResourceOwnerVtableRva)
        return "resources: owner not found";
    if (!Get(read, ctx, owner + kResourceStoreOff, store) || !store || !Get(read, ctx, store, vt) ||
        vt != base + kResourceStoreVtableRva)
        return "resources: store not found";
    uint64_t list = 0;
    if (!Get(read, ctx, store + kResourceListOff, list) || !list) return "resources: no list in the store";
    if ((list & 7) != 1) return "resources: list in a layout not handled";
    const uintptr_t r = uintptr_t(list & ~uint64_t(7));
    uint64_t last = 0;
    if (!Get(read, ctx, r, last) || last > kMaxLastIndex) return "resources: list unreadable";
    uint8_t first = 0;
    if (!Get(read, ctx, base + kResourceFirstSlotRva + uintptr_t(last), first) || first < last + 2)
        return "resources: slot table unreadable";
    ResourceEntry found[kResourceCount] = {};
    for (uint64_t i = 0; i <= last; ++i) {
        uintptr_t key = 0, elem = 0;
        uint32_t id = 0;
        if (!Get(read, ctx, r + 8 + 8 * uintptr_t(i), key) || !key || !Get(read, ctx, key, vt) ||
            vt != base + kResourceKeyVtableRva || !Get(read, ctx, key + kResourceKeyIdOff, id))
            return "resources: a key of another kind";
        Resource res;
        if (!FromId(id, res)) continue;
        ResourceEntry& e = found[size_t(res)];
        if (e.ok) return "resources: an id twice";
        if (!Get(read, ctx, r + 8 * (uintptr_t(first) + uintptr_t(i)), elem) || !ReadProtected(read, ctx, elem, e))
            return "resources: an amount unreadable";
    }
    for (size_t k = 0; k < kResourceCount; ++k)
        if (Resource(k) != Resource::SkillPoints && !found[k].ok) return "resources: one missing from the store";
    for (size_t k = 0; k < kResourceCount; ++k)
        if (Resource(k) != Resource::SkillPoints) s.entries[k] = found[k];
    return nullptr;
}

const char* ReadPoints(uintptr_t base, ReadFn read, void* ctx, ResourceEntry& e) {
    uintptr_t obj = 0, vt = 0;
    int32_t v = 0;
    if (!Get(read, ctx, base + kSkillPointsRootRva, obj) || !obj || !Get(read, ctx, obj, vt) ||
        vt != base + kSkillPointsVtableRva || !Get(read, ctx, obj + kSkillPointsOff, v))
        return "skill points not found";
    e = {};
    e.ok = true;
    e.addr = obj + kSkillPointsOff;
    e.value = v;
    return nullptr;
}

}  // namespace

int32_t DecodeProtected(const uint8_t (&bytes)[4], uint32_t key) {
    uint32_t v = 0;
    for (int i = 0; i < 32; ++i) v |= uint32_t((bytes[i % 4] >> (i / 4)) & 1) << i;
    return int32_t(v ^ key);
}

void EncodeProtected(int32_t value, uint32_t key, uint8_t (&bytes)[4]) {
    const uint32_t v = uint32_t(value) ^ key;
    for (uint8_t& b : bytes) b = 0;
    for (int i = 0; i < 32; ++i) bytes[i % 4] |= uint8_t(((v >> i) & 1) << (i / 4));
}

const char* ResourceName(Resource r) {
    static const char* const kNames[kResourceCount] = {"skill_points", "fuel", "medicine", "comms", "food"};
    return size_t(r) < kResourceCount ? kNames[size_t(r)] : "?";
}

int32_t ResourceMax(Resource r) { return r == Resource::SkillPoints ? 9999 : 999999; }

ResourceState ReadResources(uintptr_t image_base, ReadFn read, void* ctx) {
    ResourceState s;
    const char* store = ReadStore(image_base, read, ctx, s);
    const char* points = ReadPoints(image_base, read, ctx, s.entries[size_t(Resource::SkillPoints)]);
    s.error = store ? store : points;
    return s;
}

ResourceWrite WriteResource(uintptr_t image_base, ReadFn read, WriteFn write, void* ctx, Resource r, int32_t value) {
    ResourceWrite w;
    if (size_t(r) >= kResourceCount) {
        w.error = "unknown resource";
        return w;
    }
    if (value < 0 || value > ResourceMax(r)) {
        w.error = "amount out of range";
        return w;
    }
    const ResourceState s = ReadResources(image_base, read, ctx);
    const ResourceEntry& e = s[r];
    if (!e.ok) {
        w.error = s.error ? s.error : "not found";
        return w;
    }
    w.old_value = e.value;
    if (r == Resource::SkillPoints) {
        if (!write(e.addr, &value, sizeof value, ctx)) {
            w.error = "write failed";
            return w;
        }
    } else {
        uint8_t b[4], old[4];
        EncodeProtected(value, e.key, b);
        EncodeProtected(e.value, e.key, old);  // the bytes as they are
        for (int k = 0; k < 4; ++k)
            if (!write(e.bytes[k], &b[k], 1, ctx)) {
                for (int j = 0; j < k; ++j) write(e.bytes[j], &old[j], 1, ctx);  // no half-written amount
                w.error = "write failed";
                return w;
            }
    }
    const ResourceState after = ReadResources(image_base, read, ctx);
    if (!after[r].ok || after[r].value != value) {
        w.error = "the game's memory did not keep it";
        return w;
    }
    w.ok = true;
    return w;
}

}  // namespace grw::game
