#pragma once
#include <cstddef>
#include <cstdint>
#include "math/vec.h"

// Game structures found in GRW.exe (Steam build 25120584, SHA-256 1284e4d5...f751), offline campaign.
// Evidence: ghost-recon-wildlands-re/2026-10-08/camera-findings.md sections 3d-4d.
namespace grw::game {

// Camera object: single instance, found by its vtable.
inline constexpr uintptr_t kCameraVtableRva = 0x39B5820;
inline constexpr size_t kCameraFovOff = 0x180;     // f32, vertical FOV in radians (changes when aiming)
inline constexpr size_t kCameraMatrixOff = 0x190;  // 4x4 f32: rows right, forward, up, position (world Z-up)

// Character object "A": one per character (player, squad, NPCs, plus a pool of inactive ones at the origin).
inline constexpr uintptr_t kCharVtableRva = 0x39E8C50;
inline constexpr size_t kCharOwnerOff = 0x10;    // -> entity "O"
inline constexpr size_t kCharMatrixOff = 0x160;  // 4x4 f32, same layout as the camera; row 3 = feet
inline constexpr size_t kCharPoseOff = 0x270;    // -> pose object (null when not animated)
inline constexpr size_t kCharReadSize = kCharPoseOff + 8;

// Entity "O" = [A + 0x10].
inline constexpr uintptr_t kEntityVtableRva = 0x39C6DF8;
inline constexpr size_t kEntityFlagsOff = 0xD0;  // u32: category bits, see Classify()

// Pose: [[A + 0x270] + 0x178] = model-space bones {pos xyz, 0, quat xyzw}, +y forward, +z up.
inline constexpr size_t kPoseBonesOff = 0x178;
inline constexpr size_t kBoneStride = 0x20;
inline constexpr size_t kBoneFloats = kBoneStride / 4;
inline constexpr size_t kBonesRead = 96;  // head bones sit within the first ~80 on every skeleton seen
inline constexpr size_t kNeckBone = 4;    // spine 0-4 is the same on every skeleton seen

inline constexpr uint32_t kFlagPlayer = 0x1;
inline constexpr uint32_t kFlagAi = 0x2;
inline constexpr uint32_t kFlagCombatant = 0x8;
inline constexpr uint32_t kFlagNpc = 0x10;
inline constexpr uint32_t kFlagAlly = 0x400;
inline constexpr uint32_t kFlagUnidad = 0x800;  // also set on civilians and the squad
inline constexpr uint32_t kFlagFaction = 0x1000;
inline constexpr uint32_t kFlagDead = 0x4000;
inline constexpr uint32_t kFlagVehicle = 0x20000;

enum class Category : uint8_t { Unknown, Player, Squad, Ally, SantaBlanca, Unidad, Civilian, Npc, Dead };
const char* CategoryName(Category c);

// From entity flags as labelled in game (squad 0xd0a, player 0xd09, civilians 0x912, Santa Blanca 0x111a,
// Unidad 0x191a, rebels 0x1d1a presumed); +0x4000 dead, +0x20000 in a vehicle.
Category Classify(uint32_t flags);

// Row-major 4x4 as stored by the game: rows right, forward, up, position (row-vector convention).
struct GameMatrix {
    float m[4][4];
    Vec3 Row(int r) const { return {m[r][0], m[r][1], m[r][2]}; }
    Vec3 Position() const { return Row(3); }
    Vec3 ToWorld(const Vec3& local) const;  // position + x*right + y*forward + z*up
    bool Plausible() const;                 // finite, orthonormal rotation
};

// Head centre and crown in model space. Head bones: within 0.45 m of the neck bone and >= 0.15 m above it
// (excludes a hand raised above the head). False when no bone qualifies.
bool HeadFromBones(const float* bones, size_t count, Vec3& head, Vec3& crown);

struct CameraView {
    Vec3 pos, right, forward, up;
    float fov_v = 0;  // radians
};

// Perspective projection with the game's vertical FOV; false when behind the near plane (0.1 m).
bool Project(const CameraView& cam, const Vec3& world, float width, float height, Vec2& out, float& depth);

}  // namespace grw::game
