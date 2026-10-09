#pragma once
#include <cstddef>
#include <cstdint>
#include "math/vec.h"

// Game structures found in GRW.exe (Steam build 25120584, SHA-256 1284e4d5...f751), offline campaign.
// Evidence: ghost-recon-wildlands-re/2026-10-08/camera-findings.md sections 3d-4d.
namespace grw::game {

// Camera object: single instance, pointed to by the image (camera-findings.md section 8), else found by its vtable.
inline constexpr uintptr_t kCameraPtrRva = 0x4BBB330;
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
// [O + 0x78] = the entity's components: the character object A is one of the qwords at +0xB0..+0x128.
inline constexpr size_t kEntityComponentsOff = 0x78;
inline constexpr size_t kComponentsSearch = 0x200;

// The world's list of live entities (camera-findings.md section 8): world = [base + 0x4B90298],
// list = [world + 0xE0], slots of 0x18 bytes {entity O, component, flags} at [list + 0xE0], u16 slot count at
// list + 0xE8. Every active character (player, squad, NPCs) is in it, often in two slots; the pool of
// inactive ones is not.
inline constexpr uintptr_t kWorldRootRva = 0x4B90298;
inline constexpr uintptr_t kWorldVtableRva = 0x39CBB90;
inline constexpr size_t kWorldEntityListOff = 0xE0;
inline constexpr uintptr_t kEntityListVtableRva = 0x39C0D28;
inline constexpr size_t kEntityListDataOff = 0xE0;
inline constexpr size_t kEntityListCountOff = 0xE8;
inline constexpr size_t kEntityListStride = 0x18;
inline constexpr size_t kEntityListMaxSlots = 4096;

// The game's per-frame character update job (camera-findings.md section 10): job = [[base + 0x4D5B0B0] + 0x128]
// (holder vtable 0x3A080B0, job vtable 0x3A78220, its function at +0x1C8 = base + 0x10B73A0); the character
// objects A it updates: an hkArray at job + 0x1D8 {A*, u32 size at + 0x1E0}. Every live character, the player
// included, and nothing else (56/56); survives where the entity list above does not (game restart).
inline constexpr uintptr_t kCharJobHolderRva = 0x4D5B0B0;
inline constexpr uintptr_t kCharJobHolderVtableRva = 0x3A080B0;
inline constexpr size_t kHolderCharJobOff = 0x128;
inline constexpr uintptr_t kJobVtableRva = 0x3A78220;
inline constexpr size_t kJobFnOff = 0x1C8;
inline constexpr uintptr_t kCharUpdateFnRva = 0x10B73A0;
inline constexpr size_t kJobCharsOff = 0x1D8;
inline constexpr size_t kJobCharsSizeOff = 0x1E0;
inline constexpr size_t kJobCharsMax = 1024;

// Pose: [[A + 0x270] + 0x178] = model-space bones {pos xyz, 0, quat xyzw}, +y forward, +z up.
inline constexpr size_t kPoseBonesOff = 0x178;
inline constexpr size_t kBoneStride = 0x20;
inline constexpr size_t kBoneFloats = kBoneStride / 4;
inline constexpr size_t kBonesRead = 128;  // face bones reach index ~107 on squad skeletons (live/50)
inline constexpr size_t kNeckBone = 4;     // spine 0-4 is the same on every skeleton seen
inline constexpr size_t kSpineBaseBone = 2;
inline constexpr float kHeadMaxDist = 0.45f;        // from the neck bone
inline constexpr float kHeadMinAlongSpine = 0.12f;  // beyond the neck bone, along spine 2 -> 4

// Player's aim component: [[O + 0x78] + 0x170] (O = the player's entity), the view-angle accumulator.
// A +1 deg write turns the camera within one frame and is not overwritten (camera-findings.md section 5).
inline constexpr size_t kEntityAimHolderOff = 0x78;
inline constexpr size_t kAimHolderCompOff = 0x170;
inline constexpr uintptr_t kAimCompVtableRva = 0x3BCA868;
inline constexpr size_t kAimYawOff = 0x2E0;       // f32 rad = camera heading - 90 deg
inline constexpr size_t kAimPitchOff = 0x2E4;     // f32 rad = camera pitch
inline constexpr size_t kAimPitchMinOff = 0x2E8;  // f32 rad (about -75 deg)
inline constexpr size_t kAimPitchMaxOff = 0x2EC;  // f32 rad (about +75 deg)
inline constexpr float kAimYawToHeading = 1.5707963f;  // heading = accumulator yaw + 90 deg
// Recoil (camera-findings.md section 12): the view = accumulator + the recoil slots of the aim component, cleared
// and recomputed every frame by the weapon's recoil update. The instruction right after its last write
// (rax = the aim component): the recoil mod scales the slots there.
inline constexpr uintptr_t kRecoilApplyRva = 0x1483EAD0;
inline constexpr uint16_t kAimRecoilOffs[8] = {0x2F0, 0x2F4, 0x368, 0x36C, 0x3E0, 0x3E4, 0x3E8, 0x3EC};

// Active weapon of the player (camera-findings.md section 6): the weapon state component (single instance) holds
// at +0x48 a weak handle to the weapon logic object W; [W+0x118] = the weapon's design data (with its parts).
inline constexpr uintptr_t kWeaponStateVtableRva = 0x3B87F20;
inline constexpr size_t kWeaponStateWeaponOff = 0x48;
// From the player's entity: [[[O + 0x78] + 0x180] + 0x420] = the weapon state (section 8).
inline constexpr size_t kComponentsWeaponHolderOff = 0x180;
inline constexpr uintptr_t kWeaponHolderVtableRva = 0x3B83E60;
inline constexpr size_t kWeaponHolderStateOff = 0x420;
inline constexpr uintptr_t kWeaponVtableRva = 0x3905C18;
inline constexpr size_t kWeaponDataOff = 0x118;
inline constexpr uintptr_t kWeaponDataVtableRva = 0x38C1250;
inline constexpr size_t kWeaponDataSpeedOff = 0x208;   // m/s: R4 889, MP5 680, P45T 200.66 (not the bullets' speed)
inline constexpr size_t kWeaponDataDamageOff = 0x398;  // R4 77, MP5 50, P45T 40 (the damage in the fire parameters)
// D+0x34 is not the cadence (x3 on it: shots still 72 ms apart). Pistol 278, rifles 709-843, sniper 781: the muzzle
// velocity in m/s? Not proven. The cadence is not found (camera-findings.md section 15).
// The fire handler 0x154726310 (this, fire parameters): it consumes one round (0x140611D40), fires once (0x1429C43C0
// -> 0x15471F8E0), then, for a shotgun (weapon type 6), fires again while the parameters' pellet count +0x30 > 1
// (decremented), with +0x04 = 1 (the next pellets'). At 0x1547263EE the first shot is done: its result in ebp, the
// parameters in rbx, the weapon W in rsi, the handler in rdi; the pellet loop starts at 0x154726410.
inline constexpr uintptr_t kFireAfterShotRva = 0x147263EE;
inline constexpr uintptr_t kFirePelletLoopRva = 0x14726410;
inline constexpr size_t kFireParamsPelletsOff = 0x30;    // i32
inline constexpr size_t kFireParamsNextPelletOff = 0x04;  // u8
// A global object of the game's rules (vtable 0x38CD028), [GRW + 0x4B87948]: its byte at +0x30 set, firing consumes
// no ammo (0x140611D40) and a reload fills the magazine without taking from the reserve (0x148DC5200); two item
// capacity getters read it too. Every weapon's, the NPCs' included. The magazine counter itself (W + 0x180) is
// obfuscated and its refill path carries an anti-tamper trigger: it is never written.
inline constexpr uintptr_t kGameRulesPtrRva = 0x4B87948;
inline constexpr uintptr_t kGameRulesVtableRva = 0x38CD028;
inline constexpr size_t kRulesInfiniteAmmoOff = 0x30;
// The weapon's effective ballistics, its parts included (camera-findings.md section 7b): [W + 0x128], read by the
// projectile setup through the shooter's getters. +0x1C = V (projectile +0x1B4), +0x30 = first segment (+0x168).
inline constexpr size_t kWeaponBallisticsOff = 0x128;
inline constexpr uintptr_t kBallisticsVtableRva = 0x38E7B88;
inline constexpr size_t kBallisticsSpeedOff = 0x1C;
inline constexpr size_t kBallisticsFirstSegmentOff = 0x30;
// The shooter S = handle [W + 0x110] (the fire function's first argument); its spread component [S + 0x100]: the
// current spread at +0x28 (0 or below 1e-5: the shot's direction is left alone) and the aimed point's distance cap
// at +0x2C (100 m). With spread, the launch vector is L / sqrt(L^2 + 1) long (L = distance to the aimed point).
inline constexpr size_t kWeaponShooterOff = 0x110;
inline constexpr uintptr_t kShooterVtableRva = 0x3BBFB00;
// The shooter's projectile request [S + 0x58]: the bullet creation copies V, v0 and the first segment from it,
// over what the setup read from the ballistics block (so a V written there alone changes nothing).
inline constexpr size_t kShooterRequestOff = 0x58;
inline constexpr uintptr_t kRequestVtableRva = 0x3BBFFA0;
inline constexpr size_t kRequestSpeedOff = 0x30;
// Plausibility bound for a bullet speed read from the game: above what the speed mods write (instant bullets).
inline constexpr float kMaxBulletSpeed = 100000.0f;
inline constexpr float kMaxFirstSegment = 100000.0f;  // plausibility bound for the first segment read
// Teleported bullets (camera-findings.md section 14): after their first segment, one hop of kTeleportChunkM per
// simulation step, a step every ~kSimStepS (measured: 6.1-7.8 ms between the steps of a projectile).
inline constexpr float kTeleportChunkM = 200.0f;
inline constexpr float kSimStepS = 0.0067f;
inline constexpr size_t kRequestFirstSegmentOff = 0x38;
inline constexpr size_t kShooterSpreadOff = 0x100;
inline constexpr uintptr_t kSpreadVtableRva = 0x3BC6AF8;
inline constexpr size_t kSpreadValueOff = 0x28;
inline constexpr size_t kSpreadRangeOff = 0x2C;
// The spread function's load of the spread it is about to apply ([rdi + kSpreadValueOff], rdi = the spread
// component), right after it refreshed it from the attribute below: the weapon mod scales it there.
inline constexpr uintptr_t kSpreadApplyRva = 0x147E9FEA;
// That spread is attribute 0x48 of a global container, [[GRW + 0x4B99010] + 0x40]: entries of 0x20 bytes, each
// pointing at a small value object (vtable 0x3B7AC00, value at +8) replaced at every update.
inline constexpr uintptr_t kAttrRootRva = 0x4B99010;
inline constexpr size_t kAttrContainerOff = 0x40;
inline constexpr size_t kAttrEntryStride = 0x20;
inline constexpr uint32_t kAttrSpreadId = 0x48;
inline constexpr uintptr_t kAttrValueVtableRva = 0x3B7AC00;
inline constexpr size_t kAttrValueOff = 0x8;
inline constexpr size_t kBallisticsDamageOff = 0x78;  // per bullet, before the game's attribute multiplier

// The weapon's parts and stats (camera-findings.md section 11).
// Names as the menu shows them: handle [W + 0x4A8] -> rows of 0x40 bytes (vtable 0x3B2D328; the handle points at
// the second one), slot label at +0x20, part name at +0x30: game strings {u32 length, u32 capacity, u32, UTF-8}.
inline constexpr size_t kWeaponPartNamesOff = 0x4A8;
inline constexpr uintptr_t kPartRowVtableRva = 0x3B2D328;
inline constexpr size_t kPartRowStride = 0x40;
inline constexpr size_t kPartRowLabelOff = 0x20;
inline constexpr size_t kPartRowNameOff = 0x30;
inline constexpr size_t kGameStringTextOff = 12;
inline constexpr size_t kMaxParts = 8;
// Stats as the menu computes them: D's base (0-100) * mul + add, {add, mul} summed over the parts in [D + 0x178].
// Order: damage, accuracy, handling, range, rate of fire, noise reduction, penetration.
inline constexpr size_t kStatCount = 7;
inline constexpr size_t kDataStatsOff = 0xB8;
inline constexpr size_t kDataModsOff = 0x178;
inline constexpr size_t kModsStatPairsOff = 0xD8;
// The equipped parts: handles in D, one per slot (proven by swapping parts): optic, barrel, muzzle, magazine.
inline constexpr size_t kDataOpticOff = 0x138;
inline constexpr size_t kDataBarrelOff = 0x140;
inline constexpr size_t kDataMuzzleOff = 0x160;
inline constexpr size_t kDataMagazineOff = 0x170;
inline constexpr size_t kMagazineRoundsOff = 0x60;  // i32

// Fire function (camera-findings.md section 6): fire(shooter S = [W+0x110], params*), one call per shot of
// any character; params: origin +0x10, unit direction +0x20 (3 floats each).
inline constexpr uintptr_t kFireRva = 0x1471F8E0;
inline constexpr size_t kFireOriginOff = 0x10;
inline constexpr size_t kFireDirOff = 0x20;
// Projectile speed function (live/72): called once per simulation step of every projectile, rcx = projectile
// (position +0x190, next position +0x1a0, direction +0x140, origin +0x150, elapsed time +0x120, speed +0x1b8).
inline constexpr uintptr_t kProjSpeedFnRva = 0x14707340;

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

// Head centre (median of the head bones) and crown (highest head bone) in model space. Head bones: within
// 0.45 m of the neck bone and more than 0.12 m beyond it along the spine axis (bone 2 -> 4), so the rule
// follows a torso leaning forward (crouched, sprinting) and still excludes a hand raised above the head.
// False when no bone qualifies.
bool HeadFromBones(const float* bones, size_t count, Vec3& head, Vec3& crown);

struct CameraView {
    Vec3 pos, right, forward, up;
    float fov_v = 0;  // radians
};

// Perspective projection with the game's vertical FOV; false when behind the near plane (0.1 m).
bool Project(const CameraView& cam, const Vec3& world, float width, float height, Vec2& out, float& depth);

}  // namespace grw::game
