#pragma once
#include "math/vec.h"

namespace grw::scan {

// Is a float triplet a point near a line of fire? Along = distance along the unit direction from the
// origin; lateral = horizontal offset to the right; vertical = offset in the vertical plane of the shot
// (negative below the straight line, where a falling bullet goes).
struct RayFilter {
    Vec3 origin, dir, right, up;
    float min_along = 0, max_along = 0;
    float max_lateral = 0, min_vertical = 0, max_vertical = 0;
    Vec3 lo, hi;  // bounding box of the accepted volume, for a cheap reject

    static RayFilter Make(const Vec3& origin, const Vec3& dir, float min_along, float max_along, float max_lateral,
                          float min_vertical, float max_vertical);

    struct Where {
        float along, lateral, vertical;
    };
    Where Locate(const Vec3& p) const;
    bool Test(const float* p, Where& w) const {
        if (p[0] < lo.x || p[0] > hi.x || p[1] < lo.y || p[1] > hi.y || p[2] < lo.z || p[2] > hi.z) return false;
        w = Locate({p[0], p[1], p[2]});
        return w.along >= min_along && w.along <= max_along && w.lateral >= -max_lateral &&
               w.lateral <= max_lateral && w.vertical >= min_vertical && w.vertical <= max_vertical;
    }
};

}  // namespace grw::scan
