#pragma once
#include <cstdint>
#include "math/vec.h"

namespace grw::scan {

// TransRow3: D3D row-vector storage, translation in floats 12..14, floats 3,7,11 = 0, 15 = 1.
// TransCol3: column-vector storage, translation in floats 3,7,11, floats 12..14 = 0, 15 = 1.
enum class Layout : uint8_t { TransRow3, TransCol3 };

struct RigidInfo {
    Layout layout = Layout::TransRow3;
    Vec3 t;
};

// Orthonormal 3x3 rotation (|det| = 1) + finite translation under 1e5 m. Rejects exact identity rotations
// and exact zero translations (static objects, uninitialised matrices).
bool IsRigid(const float* m16, RigidInfo& out);

struct PerspInfo {
    float fov_y_deg = 0;
    float aspect = 0;
    bool transposed = false;  // stored column-vector style (the +-1 is in float 14, not 11)
};

// Perspective projection: x/y scales, w taken from z (|m[11]| = 1, m[15] = 0), FOV 5..150 deg,
// aspect 0.5..4. Off-centre terms up to 0.01 are accepted (TAA jitter).
bool IsPerspective(const float* m16, PerspInfo& out);

// Both readings of a rigid matrix: as camera-to-world (position = translation) and as a view matrix
// (position = -R^T t). axis[i] = the i-th row (TransRow3) or column (TransCol3) of the rotation.
struct RigidDecode {
    Vec3 pos_if_world;
    Vec3 pos_if_view;
    Vec3 axis[3];
};
RigidDecode Decode(const float* m16, Layout l);

// The same transform as a grw::Mat4 (column-vector convention).
Mat4 ToMat4(const float* m16, Layout l);

}  // namespace grw::scan
