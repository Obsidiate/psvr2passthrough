#pragma once

#include "distortion.h"   // CameraIntrinsics

namespace psvr2pt {

// Per-eye field of view as four half-angles in radians, using the OpenXR
// XrFovf sign convention: left/down are negative, right/up are positive.
// Runtime-agnostic so both the OpenXR layer and the OpenVR overlay can reuse it.
struct EyeFov {
    float angle_left;
    float angle_right;
    float angle_up;
    float angle_down;
};

// Analytic FOV derived from the camera intrinsics (principal point + focal
// length) for an image of the given pixel size, scaled by an optional zoom.
// This is the camera frustum the undistorted output fills.
EyeFov fov_from_intrinsics(const CameraIntrinsics& intr,
                           int image_width, int image_height,
                           float zoom = 1.0f);

// Per-eye toe-out delta (radians) compensating for the lateral offset between
// each fixed camera and the corresponding eye/lens as the user changes IPD.
// The cameras are fixed to the headset body; the lenses move with the IPD
// slider. Positive offset = camera further outward than eye. Result is applied
// on top of the static toe-out: out_delta_l is negative, out_delta_r positive.
void ipd_toe_deltas(float ipd_m, float camera_separation_m,
                    float& out_delta_l, float& out_delta_r);

}  // namespace psvr2pt
