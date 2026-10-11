#include "game/stealth.h"

namespace grw::game {

namespace {

constexpr size_t kMultBytes = sizeof(float) * kVisibilityMultCount;

bool WriteMults(uintptr_t v, const float (&m)[kVisibilityMultCount], WriteFn write, void* ctx) {
    return write(v + kVisibilityMultOff, m, kMultBytes, ctx);
}

}  // namespace

bool IsPlayerVisibility(uintptr_t v, uintptr_t image_base, uintptr_t player_entity, ReadFn read, void* ctx) {
    uint64_t vt = 0, owner = 0;
    return v && player_entity && read(v, &vt, sizeof vt, ctx) && vt == image_base + kVisibilityVtableRva &&
           read(v + kVisibilityOwnerOff, &owner, sizeof owner, ctx) && owner == player_entity;
}

bool SilenceNoise(uintptr_t v, uintptr_t image_base, uintptr_t player_entity, ReadFn read, WriteFn write, void* ctx,
                  float* before) {
    static const float kZero = 0.0f;
    if (!IsPlayerVisibility(v, image_base, player_entity, read, ctx)) return false;  // freed and reused, or another's
    if (before && !read(v + kVisibilityNoiseOff, before, sizeof *before, ctx)) return false;
    return write(v + kVisibilityNoiseOff, &kZero, sizeof kZero, ctx) &&
           write(v + kVisibilityNoiseCopyOff, &kZero, sizeof kZero, ctx);
}

const char* StealthEventName(StealthEvent e) {
    switch (e) {
        case StealthEvent::None: return "none";
        case StealthEvent::Applied: return "applied";
        case StealthEvent::Rewritten: return "rewritten";
        case StealthEvent::Restored: return "restored";
        case StealthEvent::Lost: return "lost";
        case StealthEvent::Failed: return "failed";
    }
    return "?";
}

StealthEvent Stealth::Update(bool on, uintptr_t v, ReadFn read, WriteFn write, void* ctx) {
    static const float kZeros[kVisibilityMultCount] = {};
    if (!on || !v) {
        if (!applied_) return StealthEvent::None;
        const uintptr_t was = applied_;
        applied_ = 0;
        if (on || v != was) return StealthEvent::Lost;  // gone: nothing of ours to put back
        return WriteMults(was, game_, write, ctx) ? StealthEvent::Restored : StealthEvent::Failed;
    }
    float cur[kVisibilityMultCount];
    if (!read(v + kVisibilityMultOff, cur, kMultBytes, ctx)) return StealthEvent::Failed;
    if (v != applied_) {  // a component we never changed (the first, or the game made a new one)
        float game[kVisibilityMultCount];
        for (size_t k = 0; k < kVisibilityMultCount; ++k)
            game[k] = cur[k] != 0.0f ? cur[k] : kVisibilityMultDefault;  // 0: left by an earlier session
        if (!WriteMults(v, kZeros, write, ctx)) return StealthEvent::Failed;
        for (size_t k = 0; k < kVisibilityMultCount; ++k) game_[k] = game[k];
        applied_ = v;
        return StealthEvent::Applied;
    }
    bool rewritten = false;
    for (size_t k = 0; k < kVisibilityMultCount; ++k)
        if (cur[k] != 0.0f) {
            game_[k] = cur[k];
            rewritten = true;
        }
    if (!rewritten) return StealthEvent::None;
    return WriteMults(v, kZeros, write, ctx) ? StealthEvent::Rewritten : StealthEvent::Failed;
}

}  // namespace grw::game
