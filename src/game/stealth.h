#pragma once
#include <cstddef>
#include <cstdint>
#include "game/model.h"
#include "game/registry.h"

// Invisibility: the player's visibility multipliers at 0 (model.h, kVisibilityVtableRva; MODLOG 2026-10-10,
// invisibilite), the game's values put back when it ends.
namespace grw::game {

// v is the player's visibility component: its vtable, and owned by the player's entity (non-zero).
bool IsPlayerVisibility(uintptr_t v, uintptr_t image_base, uintptr_t player_entity, ReadFn read, void* ctx);

// The player's noise radius (and its copy) at 0, when v is still the player's component (else nothing written).
// before: the radius as the game had it.
bool SilenceNoise(uintptr_t v, uintptr_t image_base, uintptr_t player_entity, ReadFn read, WriteFn write, void* ctx,
                  float* before);

enum class StealthEvent {
    None,
    Applied,    // the multipliers of a component set to 0
    Rewritten,  // the game had put a value back: taken as its value, set to 0 again
    Restored,   // the game's values put back
    Lost,       // the component it was applied to is gone (nothing to put back)
    Failed,     // a read or a write failed
};
const char* StealthEventName(StealthEvent e);

class Stealth {
public:
    // One step. on: invisibility wanted; v: the player's component as checked by the caller (0 = none now).
    StealthEvent Update(bool on, uintptr_t v, ReadFn read, WriteFn write, void* ctx);
    uintptr_t Applied() const { return applied_; }  // the component holding our zeros, 0 = none
    float GameValue(size_t k) const { return game_[k]; }

private:
    uintptr_t applied_ = 0;
    float game_[kVisibilityMultCount] = {};
};

}  // namespace grw::game
