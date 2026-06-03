#pragma once

#include <cstdint>
#include <vector>
#include <chrono>

namespace psvr2pt {

// PSVR2 lower-camera native resolution per eye.
// Driver confirmed (via CameraConfig.widthPx/heightPx): 1016x1016, tight-packed in shared memory.
// Stride width is 1024 (BC4 block-aligned), actual height is 1016 - no row padding.
// StereoFrame holds the raw BC4 bytes; decompression happens on the GPU via
// DXGI_FORMAT_BC4_UNORM texture sampling.
inline constexpr int kCameraWidth  = 1024; // BC4 block-row stride (width rounds up to 1024)
inline constexpr int kCameraHeight = 1016; // confirmed tight-packed height from driver calibration

// BC4 bytes per eye: (width/4 blocks) × (height/4 blocks) × 8 bytes/block.
inline constexpr int kBC4DataSize = (kCameraWidth / 4) * (kCameraHeight / 4) * 8; // 520,192

enum class CameraId : int { Left = 0, Right = 1 };

// Head orientation and position as reported by the PSVR2 driver at capture time.
struct Pose3f {
    float px = 0, py = 0, pz = 0;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    bool  valid = false;
};

struct StereoFrame {
    // BC4_UNORM compressed bytes, kBC4DataSize per eye.
    std::vector<uint8_t> left;
    std::vector<uint8_t> right;
    std::chrono::steady_clock::time_point captured_at{};
    uint64_t sequence = 0;
    Pose3f   captured_pose{};

    [[nodiscard]] bool valid() const noexcept {
        return left.size() == static_cast<size_t>(kBC4DataSize)
            && right.size() == static_cast<size_t>(kBC4DataSize);
    }
};

}  // namespace psvr2pt
