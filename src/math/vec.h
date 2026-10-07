#pragma once
#include <cmath>

namespace grw {

struct Vec2 { float x = 0, y = 0; };
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float Length() const { return std::sqrt(Dot(*this)); }
};

// Row-major storage, column-vector convention: clip = M * [x y z 1].
// A game matrix in row-vector convention is read and then Transposed().
struct Mat4 {
    float m[4][4] = {};

    Vec4 Transform(const Vec3& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3],
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3],
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3],
                m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3]};
    }

    Mat4 Transposed() const {
        Mat4 t;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) t.m[c][r] = m[r][c];
        return t;
    }
};

}  // namespace grw
