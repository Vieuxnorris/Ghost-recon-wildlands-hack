#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include "scan/matrix_detect.h"

using namespace grw;
using namespace grw::scan;
using Catch::Approx;

namespace {
// World (camera-to-world) matrix, D3D row-vector convention: rows = right, up, forward, position.
void WorldRow(float yaw, float pitch, Vec3 pos, float out[16]) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const Vec3 fwd{cp * cy, cp * sy, sp};
    const Vec3 right{sy, -cy, 0};
    const Vec3 up{-sp * cy, -sp * sy, cp};
    const float m[16] = {right.x, right.y, right.z, 0, up.x, up.y, up.z, 0,
                         fwd.x,   fwd.y,   fwd.z,   0, pos.x, pos.y, pos.z, 1};
    std::memcpy(out, m, sizeof m);
}
// Inverse of a rigid row-vector matrix: R^T and -t*R^T.
void InverseRow(const float w[16], float out[16]) {
    std::memset(out, 0, 16 * sizeof(float));
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = w[c * 4 + r];
    for (int c = 0; c < 3; ++c)
        out[12 + c] = -(w[12] * out[0 * 4 + c] + w[13] * out[1 * 4 + c] + w[14] * out[2 * 4 + c]);
    out[15] = 1;
}
void Transpose(const float in[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = in[r * 4 + c];
}
// D3D left-handed perspective, row-vector convention.
void PerspRow(float fov_y_deg, float aspect, float out[16]) {
    const float ys = 1.0f / std::tan(fov_y_deg * 3.14159265f / 360.0f), zn = 0.1f, zf = 5000.0f;
    const float m[16] = {ys / aspect, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    std::memcpy(out, m, sizeof m);
}
}  // namespace

TEST_CASE("IsRigid accepts a camera world matrix in both layouts") {
    float w[16], t[16];
    WorldRow(0.7f, -0.2f, {1520.5f, -830.25f, 112.0f}, w);
    RigidInfo info;
    REQUIRE(IsRigid(w, info));
    CHECK(info.layout == Layout::TransRow3);
    CHECK(info.t.x == Approx(1520.5f));
    Transpose(w, t);
    REQUIRE(IsRigid(t, info));
    CHECK(info.layout == Layout::TransCol3);
    CHECK(info.t.z == Approx(112.0f));
}

TEST_CASE("IsRigid rejects scaled, identity, zero-translation and NaN matrices") {
    float w[16];
    RigidInfo info;
    WorldRow(0.3f, 0.1f, {10, 20, 30}, w);
    float scaled[16];
    std::memcpy(scaled, w, sizeof w);
    for (int i = 0; i < 11; ++i) scaled[i] *= 2.0f;
    CHECK_FALSE(IsRigid(scaled, info));

    const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
    CHECK_FALSE(IsRigid(ident, info));

    float zero_t[16];
    std::memcpy(zero_t, w, sizeof w);
    zero_t[12] = zero_t[13] = zero_t[14] = 0;
    CHECK_FALSE(IsRigid(zero_t, info));

    float nan[16];
    std::memcpy(nan, w, sizeof w);
    nan[5] = std::nanf("");
    CHECK_FALSE(IsRigid(nan, info));

    float huge[16];
    std::memcpy(huge, w, sizeof w);
    huge[12] = 2e5f;  // beyond any Wildlands coordinate
    CHECK_FALSE(IsRigid(huge, info));
}

TEST_CASE("Decode recovers the camera position from a world or a view matrix") {
    float w[16], v[16];
    const Vec3 pos{-2100.0f, 3300.5f, 250.0f};
    WorldRow(-1.2f, 0.35f, pos, w);
    InverseRow(w, v);
    const RigidDecode dw = Decode(w, Layout::TransRow3);
    CHECK(dw.pos_if_world.x == Approx(pos.x));
    const RigidDecode dv = Decode(v, Layout::TransRow3);
    CHECK(dv.pos_if_view.x == Approx(pos.x).margin(1e-2));
    CHECK(dv.pos_if_view.y == Approx(pos.y).margin(1e-2));
    CHECK(dv.pos_if_view.z == Approx(pos.z).margin(1e-2));
    float vt[16];
    Transpose(v, vt);
    const RigidDecode dvt = Decode(vt, Layout::TransCol3);
    CHECK(dvt.pos_if_view.y == Approx(pos.y).margin(1e-2));
}

TEST_CASE("ToMat4 gives a column-vector matrix that maps points like the stored one") {
    float w[16];
    WorldRow(0.4f, 0.0f, {7, 8, 9}, w);
    const Mat4 m = ToMat4(w, Layout::TransRow3);
    const Vec4 p = m.Transform({0, 0, 0});
    CHECK(p.x == Approx(7));
    CHECK(p.z == Approx(9));
    CHECK(p.w == Approx(1));
}

TEST_CASE("IsPerspective reads FOV and aspect, also transposed and with TAA jitter") {
    float p[16], t[16];
    PerspInfo info;
    PerspRow(60.0f, 16.0f / 9.0f, p);
    REQUIRE(IsPerspective(p, info));
    CHECK(info.fov_y_deg == Approx(60.0f).margin(0.01));
    CHECK(info.aspect == Approx(16.0f / 9.0f).margin(1e-3));
    CHECK_FALSE(info.transposed);
    Transpose(p, t);
    REQUIRE(IsPerspective(t, info));
    CHECK(info.transposed);
    p[8] = 0.0004f;  // sub-pixel jitter
    p[9] = -0.0003f;
    CHECK(IsPerspective(p, info));
}

TEST_CASE("IsPerspective rejects rigid and orthographic matrices") {
    float w[16];
    PerspInfo info;
    WorldRow(0.1f, 0.2f, {1, 2, 3}, w);
    CHECK_FALSE(IsPerspective(w, info));
    const float ortho[16] = {0.01f, 0, 0, 0, 0, 0.0178f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0, 1};
    CHECK_FALSE(IsPerspective(ortho, info));
}
