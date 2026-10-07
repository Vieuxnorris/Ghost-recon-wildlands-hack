#pragma once
#include "math/vec.h"

namespace grw {

struct Viewport { float width = 0, height = 0; };

// False when the point is behind (or on) the camera plane.
bool WorldToScreen(const Mat4& view_proj, const Vec3& world, const Viewport& vp, Vec2& out);
bool OnScreen(const Vec2& p, const Viewport& vp, float margin);

}  // namespace grw
