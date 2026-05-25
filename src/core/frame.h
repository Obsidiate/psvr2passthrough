#pragma once

#include <cstdint>
#include <vector>
#include <chrono>

namespace psvr2pt {

// PSVR2 lower-camera native resolution per eye.
// Shared memory contains BC4_UNORM block-compressed data at 1024×1024 (POT-padded).
// The camera background thread decompresses BC4→R8 before storing in StereoFrame,
// so StereoFrame holds kCameraWidth*kCameraHeight bytes of raw R8 per eye.
inline constexpr int kCameraWidth  = 1024;
inline constexpr int kCameraHeight = 1024;

enum class CameraId : int { Left = 0, Right = 1 };

struct StereoFrame {
    // Raw grayscale bytes, row-major, kCameraWidth * kCameraHeight per eye.
    std::vector<uint8_t> left;
    std::vector<uint8_t> right;
    std::chrono::steady_clock::time_point captured_at{};
    uint64_t sequence = 0;

    [[nodiscard]] bool valid() const noexcept {
        const size_t expected = static_cast<size_t>(kCameraWidth) * kCameraHeight;
        return left.size() == expected && right.size() == expected;
    }
};

}  // namespace psvr2pt
