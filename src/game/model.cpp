#include "game/model.h"
#include <algorithm>
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
    if (count > kBonesRead) count = kBonesRead;
    auto at = [&](size_t i) { return Vec3{bones[i * kBoneFloats], bones[i * kBoneFloats + 1], bones[i * kBoneFloats + 2]}; };
    const Vec3 neck = at(kNeckBone);
    Vec3 axis = neck - at(kSpineBaseBone);
    const float len = axis.Length();
    axis = (len > 0.05f && std::isfinite(len)) ? axis * (1.0f / len) : Vec3{0, 0, 1};

    float xs[kBonesRead], ys[kBonesRead], zs[kBonesRead];
    Vec3 picked[kBonesRead];
    size_t k = 0;
    for (size_t i = 0; i < count; ++i) {
        const Vec3 p = at(i);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        const Vec3 d = p - neck;
        if (d.Length() >= kHeadMaxDist || d.Dot(axis) <= kHeadMinAlongSpine) continue;
        picked[k] = p;
        xs[k] = p.x;
        ys[k] = p.y;
        zs[k] = p.z;
        ++k;
    }
    if (k == 0) return false;
    auto median = [k](float* v) {
        std::nth_element(v, v + k / 2, v + k);
        return v[k / 2];
    };
    head = {median(xs), median(ys), median(zs)};
    crown = head;
    for (size_t i = 0; i < k; ++i)
        if ((picked[i] - head).Length() < 0.25f && picked[i].z > crown.z) crown = picked[i];
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
