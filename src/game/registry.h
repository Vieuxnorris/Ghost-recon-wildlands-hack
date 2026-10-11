#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace grw::game {

// Reads size bytes at addr; false when unreadable. ctx is passed through (tests use a fake memory).
using ReadFn = bool (*)(uintptr_t addr, void* dst, size_t size, void* ctx);
// Writes size bytes at addr; false when it could not.
using WriteFn = bool (*)(uintptr_t addr, const void* src, size_t size, void* ctx);

struct RegistryPass {
    bool ok = false;          // a list resolved from the image
    uint32_t source = 0;      // 1 the character update job's array, 2 the world's entity list
    uint32_t slots = 0;       // list slots read
    uint32_t entities = 0;    // distinct live entities in them
    uint32_t searched = 0;    // entities whose components were searched for their character this pass
    uintptr_t player_entity = 0;
    uintptr_t weapon_state = 0;  // the player's weapon state component, 0 = not resolved
};

// The game's own lists, read in a few small copies instead of scanning memory: a character appears on the
// first pass after it spawns. First the character update job's array of character objects (model.h,
// kCharJobHolderRva); else the world's list of live entities (kWorldRootRva), where each entity's character
// object is searched once among its components, then only checked (vtable, owner) on later passes.
class EntityRegistry {
public:
    // chars: the character objects (A) of the live entities, each once.
    RegistryPass Read(uintptr_t image_base, ReadFn read, void* ctx, std::vector<uintptr_t>& chars);

private:
    bool ReadJob(uintptr_t base, ReadFn read, void* ctx, std::vector<uintptr_t>& chars, RegistryPass& r);
    std::vector<uintptr_t> job_chars_;
    bool Resolve(uintptr_t base, ReadFn read, void* ctx, uintptr_t world, uintptr_t& data, uint16_t& count);
    struct Known {
        uintptr_t entity;
        uintptr_t chr;   // 0 = none found (yet)
        uint64_t retry;  // pass at which an entity without a character is searched again
    };
    std::vector<uintptr_t> entities_;
    std::vector<Known> known_, next_;  // sorted by entity
    std::vector<uint8_t> slots_;
    uint64_t pass_ = 0;
};

}  // namespace grw::game
