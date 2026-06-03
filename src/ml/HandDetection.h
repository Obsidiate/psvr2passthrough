#pragma once

// Shared output types for hand/wrist detectors.
// All UV coordinates are normalised to [0, 1] relative to the camera frame.

#include <cstdint>

namespace psvr2pt {

struct WristPose {
    float x    = 0.f;   // UV horizontal, 0 = left edge
    float y    = 0.f;   // UV vertical,   0 = top edge
    float conf = 0.f;   // detection confidence [0, 1]
};

struct HandDetectionResult {
    WristPose left;
    WristPose right;
    bool      valid = false;   // false if inference was skipped (motion gate, not initialised, etc.)
};

}  // namespace psvr2pt
