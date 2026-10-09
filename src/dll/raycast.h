#pragma once
#include <cstdint>
#include "math/vec.h"

// One ray through the game's own physics world (Havok hknpWorld::castRay), built exactly like the game's ray
// wrapper does it (camera-findings.md section 9): collision filter context from a 64-bit layer mask, the
// world's shared read lock, a closest-hit collector. Calls game code: any thread, guarded by SEH.
namespace grw::raycast {

// The collision mask and flag of the bullets' segment rays (fbb3746 with r9b = 1 around each shot).
inline constexpr uint64_t kBulletMask = 0x31175fd67full;
inline constexpr bool kBulletFlag = true;

struct Result {
    bool ok = false;     // the world was found and the call returned
    bool fault = false;  // an exception was caught inside the game's code (lock released)
    bool hit = false;
    float fraction = 1.0f;  // along from -> to
    Vec3 pos{}, normal{};
    uint32_t body = 0xFFFFFFFF;  // hknpBodyId of the closest hit
    uint16_t material = 0xFFFF;  // its material (8 = a character's hitbox)
    uint32_t filter_info = 0;    // its collision filter info (low 6 bits: the layer)
    uint8_t raw[0x50] = {};      // the collector's result block +0x50..+0xA0 (fraction, two body infos)
    // On a fault: the exception, where (image-relative when inside GRW.exe) and the step reached
    // (1 filter context, 2 lock, 3 castRay, 4 unlock, 5 filter destructor).
    uint32_t fault_code = 0;
    uint64_t fault_rva = 0;
    uint64_t fault_data = 0;  // access violations: the address accessed
    int stage = 0;
};

Result Cast(const Vec3& from, const Vec3& to, uint64_t mask = kBulletMask, bool flag = kBulletFlag);

}  // namespace grw::raycast
