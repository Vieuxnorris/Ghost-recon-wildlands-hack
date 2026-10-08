#include "scan/ray_filter.h"
#include <algorithm>
#include <cmath>

namespace grw::scan {

namespace {

Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

Vec3 Normalized(const Vec3& v) {
    const float n = v.Length();
    return n > 0 ? v * (1.0f / n) : Vec3{};
}

}  // namespace

RayFilter RayFilter::Make(const Vec3& origin, const Vec3& dir, float min_along, float max_along, float max_lateral,
                          float min_vertical, float max_vertical) {
    RayFilter f;
    f.origin = origin;
    f.dir = Normalized(dir);
    // world Z-up; a vertical shot has no "right": take the world X axis
    Vec3 r = Cross(f.dir, {0, 0, 1});
    f.right = r.Length() > 1e-3f ? Normalized(r) : Vec3{1, 0, 0};
    f.up = Cross(f.right, f.dir);
    f.min_along = min_along;
    f.max_along = max_along;
    f.max_lateral = max_lateral;
    f.min_vertical = min_vertical;
    f.max_vertical = max_vertical;
    const float margin = std::max({max_lateral, -min_vertical, max_vertical}) + 1.0f;
    const Vec3 a = origin + f.dir * min_along, b = origin + f.dir * max_along;
    f.lo = {std::min(a.x, b.x) - margin, std::min(a.y, b.y) - margin, std::min(a.z, b.z) - margin};
    f.hi = {std::max(a.x, b.x) + margin, std::max(a.y, b.y) + margin, std::max(a.z, b.z) + margin};
    return f;
}

RayFilter::Where RayFilter::Locate(const Vec3& p) const {
    const Vec3 r = p - origin;
    return {r.Dot(dir), r.Dot(right), r.Dot(up)};
}

}  // namespace grw::scan
