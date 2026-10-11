#pragma once
#include <cstddef>
#include <cstdint>
#include "game/registry.h"

// The rebels' resources and the skill points, read and set the way the game keeps them (model.h, kResourceOwnerRva
// and kSkillPointsRootRva; MODLOG 2026-10-10, ressources).
namespace grw::game {

// The game's protected int: bit i of (value ^ key) is bit i / 4 of bytes[i % 4].
int32_t DecodeProtected(const uint8_t (&bytes)[4], uint32_t key);
void EncodeProtected(int32_t value, uint32_t key, uint8_t (&bytes)[4]);

// In the order of the skills menu's bar.
enum class Resource { SkillPoints, Fuel, Medicine, Comms, Food, Count };
inline constexpr size_t kResourceCount = size_t(Resource::Count);
const char* ResourceName(Resource r);  // "skill_points", "fuel", ...
int32_t ResourceMax(Resource r);       // the largest amount set: what the game's bar shows safely

struct ResourceEntry {
    bool ok = false;
    int32_t value = 0;
    uintptr_t addr = 0;      // the skill points' i32, or the protected int
    uintptr_t bytes[4] = {};  // a protected int's bytes
    uint32_t key = 0;
};

struct ResourceState {
    const char* error = nullptr;  // why the store or the points were not read (nullptr: both read)
    ResourceEntry entries[kResourceCount];
    const ResourceEntry& operator[](Resource r) const { return entries[size_t(r)]; }
};

ResourceState ReadResources(uintptr_t image_base, ReadFn read, void* ctx);

struct ResourceWrite {
    bool ok = false;
    int32_t old_value = 0;
    const char* error = nullptr;
};

// Reads the store again, then writes value in 0..ResourceMax(r): a protected amount encoded with its own key, the
// skill points as they are. Checked by reading it back.
ResourceWrite WriteResource(uintptr_t image_base, ReadFn read, WriteFn write, void* ctx, Resource r, int32_t value);

}  // namespace grw::game
