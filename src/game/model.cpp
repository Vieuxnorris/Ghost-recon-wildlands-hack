#include "game/model.h"
#include <cmath>

namespace grw::game {

const char* CategoryName(Category c) {
    switch (c) {
        case Category::Player: return "YOU";
        case Category::Squad: return "TEAM";
        case Category::Ally: return "ALLY";
        case Category::SantaBlanca: return "SB";
        case Category::Unidad: return "UNIDAD";
        case Category::Civilian: return "CIV";
        case Category::Npc: return "NPC";
        case Category::Dead: return "DEAD";
        default: return "?";
    }
}

Category Classify(uint32_t flags) {
    if (flags & kFlagDead) return Category::Dead;
    const uint32_t b = flags & 0x1FFF;
    if ((b & kFlagPlayer) && !(b & kFlagAi)) return Category::Player;
    if (b & kFlagAlly) return (b & kFlagNpc) ? Category::Ally : Category::Squad;
    if (b & kFlagFaction) return (b & kFlagUnidad) ? Category::Unidad : Category::SantaBlanca;
    if (!(b & kFlagCombatant)) return Category::Civilian;
    return Category::Npc;
}

Vec3 GameMatrix::ToWorld(const Vec3& v) const {
    return Position() + Row(0) * v.x + Row(1) * v.y + Row(2) * v.z;
}

bool GameMatrix::Plausible() const {
    for (const auto& row : m)
        for (float f : row)
            if (!std::isfinite(f)) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float d = Row(i).Dot(Row(j)) - (i == j ? 1.0f : 0.0f);
            if (std::fabs(d) > 1e-2f) return false;
        }
    return true;
}

bool HeadFromBones(const float* bones, size_t count, Vec3& head, Vec3& crown) {
    if (count <= kNeckBone) return false;
    const float* n = bones + kNeckBone * kBoneFloats;
    const Vec3 neck{n[0], n[1], n[2]};
    Vec3 sum;
    int k = 0;
    for (size_t i = 0; i < count; ++i) {
        const float* b = bones + i * kBoneFloats;
        const Vec3 p{b[0], b[1], b[2]};
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        if ((p - neck).Length() >= 0.45f || p.z <= neck.z + 0.15f) continue;
        sum = sum + p;
        if (k == 0 || p.z > crown.z) crown = p;
        ++k;
    }
    if (k == 0) return false;
    head = sum * (1.0f / float(k));
    return true;
}

bool Project(const CameraView& cam, const Vec3& world, float width, float height, Vec2& out, float& depth) {
    const Vec3 d = world - cam.pos;
    depth = d.Dot(cam.forward);
    if (depth < 0.1f) return false;
    const float sy = 1.0f / std::tan(cam.fov_v * 0.5f);
    const float sx = sy * height / width;
    out.x = width * 0.5f * (1.0f + sx * d.Dot(cam.right) / depth);
    out.y = height * 0.5f * (1.0f - sy * d.Dot(cam.up) / depth);
    return true;
}

}  // namespace grw::game
