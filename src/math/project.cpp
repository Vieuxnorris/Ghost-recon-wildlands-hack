#include "math/project.h"

namespace grw {

namespace { constexpr float kMinW = 1e-3f; }

bool WorldToScreen(const Mat4& view_proj, const Vec3& world, const Viewport& vp, Vec2& out) {
    const Vec4 c = view_proj.Transform(world);
    if (c.w < kMinW) return false;
    const float ndc_x = c.x / c.w;
    const float ndc_y = c.y / c.w;
    out.x = (ndc_x * 0.5f + 0.5f) * vp.width;
    out.y = (0.5f - ndc_y * 0.5f) * vp.height;
    return true;
}

bool OnScreen(const Vec2& p, const Viewport& vp, float margin) {
    return p.x >= -margin && p.y >= -margin && p.x <= vp.width + margin && p.y <= vp.height + margin;
}

}  // namespace grw
