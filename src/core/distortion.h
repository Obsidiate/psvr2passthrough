#pragma once

#include <DirectXMath.h>
#include <vector>
#include <array>

namespace psvr2pt {

struct UndistortVertex {
    DirectX::XMFLOAT3 Pos;   // NDC position in [-1, 1]
    DirectX::XMFLOAT2 Tex;   // UV into the raw (distorted) source image
};

// Mirrors the upstream layout. The driver writes 20 distortion coefficients
// per eye into the shared-memory header. The exact polynomial model is not
// publicly documented; we treat it as opaque and re-use the upstream forward
// projection function in get_distorted_point().
struct CameraParameters {
    double coeffs[20];
};

struct CameraIntrinsics {
    double cx, fx, cy, fy;
};

DirectX::XMFLOAT2 get_distorted_point(double x, double y, const CameraParameters& params);

// Builds a tessellated quad whose UVs sample the distorted source such that
// the on-screen result is undistorted AND rectified to the forward direction.
// signed_toe_out_rad: +toe for left eye (camera looks left), -toe for right eye.
// tilt_down_rad: downward tilt of camera relative to headset-forward (always positive).
void create_undistortion_mesh(int textureWidth, int textureHeight,
                              float zoomFactor,
                              float signed_toe_out_rad,
                              float tilt_down_rad,
                              float signed_roll_rad,
                              const CameraIntrinsics& intrinsics,
                              const CameraParameters& params,
                              std::vector<UndistortVertex>& outVertices,
                              std::vector<unsigned long>& outIndices,
                              unsigned long meshDensityX = 256,
                              unsigned long meshDensityY = 256);

// Trivial pass-through (no undistortion) used as a fallback / for debugging.
void create_default_mesh(int imageWidth, int imageHeight,
                        int textureWidth, int textureHeight,
                        std::vector<UndistortVertex>& outVertices,
                        std::vector<unsigned long>& outIndices);

}  // namespace psvr2pt
