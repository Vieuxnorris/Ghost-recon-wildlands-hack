#include "scan/matrix_detect.h"
#include <cmath>

namespace grw::scan {

namespace {

constexpr float kTol = 2e-3f;
constexpr float kMaxTranslation = 1e5f;

// Element (r, c) of the 3x3 rotation, read the way the layout stores it (row-vector: rows are axes).
float R(const float* m, Layout l, int r, int c) { return l == Layout::TransRow3 ? m[r * 4 + c] : m[c * 4 + r]; }

bool Finite(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}

bool RotationOk(const float* m, Layout l) {
    for (int a = 0; a < 3; ++a) {
        const Vec3 u{R(m, l, a, 0), R(m, l, a, 1), R(m, l, a, 2)};
        if (std::fabs(u.Dot(u) - 1.0f) > kTol) return false;
        for (int b = a + 1; b < 3; ++b) {
            const Vec3 v{R(m, l, b, 0), R(m, l, b, 1), R(m, l, b, 2)};
            if (std::fabs(u.Dot(v)) > kTol) return false;
        }
    }
    bool identity = true;
    for (int r = 0; r < 3 && identity; ++r)
        for (int c = 0; c < 3; ++c)
            if (R(m, l, r, c) != (r == c ? 1.0f : 0.0f)) {
                identity = false;
                break;
            }
    return !identity;
}

bool TranslationOk(const Vec3& t) {
    if (t.x == 0.0f && t.y == 0.0f && t.z == 0.0f) return false;
    return std::fabs(t.x) < kMaxTranslation && std::fabs(t.y) < kMaxTranslation && std::fabs(t.z) < kMaxTranslation;
}

bool Tiny(float v, float tol) { return std::fabs(v) <= tol; }

bool PerspRowOk(const float* m, PerspInfo& out) {
    if (!(m[0] > 0.0f && m[5] > 0.0f) || m[15] != 0.0f || std::fabs(std::fabs(m[11]) - 1.0f) > 1e-6f) return false;
    const int zeros[] = {1, 2, 3, 4, 6, 7, 12, 13};
    for (int i : zeros)
        if (!Tiny(m[i], 1e-6f)) return false;
    if (!Tiny(m[8], 0.01f) || !Tiny(m[9], 0.01f)) return false;
    const float aspect = m[5] / m[0];
    const float fov = 2.0f * std::atan(1.0f / m[5]) * (180.0f / 3.14159265f);
    if (aspect < 0.5f || aspect > 4.0f || fov < 5.0f || fov > 150.0f) return false;
    out.aspect = aspect;
    out.fov_y_deg = fov;
    return true;
}

}  // namespace

bool IsRigid(const float* m, RigidInfo& out) {
    if (m[15] != 1.0f || !Finite(m)) return false;
    if (m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f) {
        const Vec3 t{m[12], m[13], m[14]};
        if (RotationOk(m, Layout::TransRow3) && TranslationOk(t)) {
            out = {Layout::TransRow3, t};
            return true;
        }
    }
    if (m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f) {
        const Vec3 t{m[3], m[7], m[11]};
        if (RotationOk(m, Layout::TransCol3) && TranslationOk(t)) {
            out = {Layout::TransCol3, t};
            return true;
        }
    }
    return false;
}

bool IsPerspective(const float* m, PerspInfo& out) {
    if (!Finite(m)) return false;
    if (PerspRowOk(m, out)) {
        out.transposed = false;
        return true;
    }
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[c * 4 + r] = m[r * 4 + c];
    if (PerspRowOk(t, out)) {
        out.transposed = true;
        return true;
    }
    return false;
}

RigidDecode Decode(const float* m, Layout l) {
    RigidDecode d;
    const Vec3 t = l == Layout::TransRow3 ? Vec3{m[12], m[13], m[14]} : Vec3{m[3], m[7], m[11]};
    for (int a = 0; a < 3; ++a) d.axis[a] = {R(m, l, a, 0), R(m, l, a, 1), R(m, l, a, 2)};
    d.pos_if_world = t;
    // View matrix: t = -pos * R (row-vector) or -R * pos (column-vector); both give pos_j = -sum_i t_i R(j, i)
    // once R is read through the layout.
    float p[3];
    for (int j = 0; j < 3; ++j) p[j] = -(t.x * R(m, l, j, 0) + t.y * R(m, l, j, 1) + t.z * R(m, l, j, 2));
    d.pos_if_view = {p[0], p[1], p[2]};
    return d;
}

Mat4 ToMat4(const float* m, Layout l) {
    Mat4 out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[r][c] = l == Layout::TransRow3 ? m[c * 4 + r] : m[r * 4 + c];
    return out;
}

}  // namespace grw::scan
