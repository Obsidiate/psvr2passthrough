#pragma once

#include "frame.h"
#include "distortion.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace psvr2pt {

struct RecorderConfig {
    std::filesystem::path output_dir   = "recordings";
    int      capture_every_n           = 10;    // save 1 in N frames (~6 fps at 60 fps camera)
    float    max_angular_velocity      = 0.8f;  // rad/s; skip frames above this (motion blur)
    uint32_t max_frames                = 0;     // 0 = unlimited

    // Camera calibration written once into a session_meta.json alongside frames.
    // Populate from CameraSource::intrinsics() / params() after camera starts.
    CameraIntrinsics intrinsics[2]{};
    CameraParameters params[2]{};
    bool             calibration_valid = false;
};

// Receives frames from the passthrough pipeline and writes PNG + JSON metadata
// to disk on a background thread.  The hot-path call is submitFrame(), which
// never blocks the caller: it decodes BC4 to grayscale synchronously (fast,
// purely arithmetic) and enqueues the result.  Disk I/O happens on a
// dedicated writer thread.
//
// Usage:
//   recorder.start(config);
//   // in compose_layer, after camera_->try_get_latest():
//   recorder.submitFrame(cached_frame_, angular_velocity_magnitude);
//   recorder.stop();  // blocks until queue is drained
class FrameRecorder {
public:
    FrameRecorder();
    ~FrameRecorder();

    bool start(const RecorderConfig& cfg);
    void stop();    // drains queue then joins writer thread
    bool running() const { return running_.load(std::memory_order_relaxed); }

    // Called every frame from the compose_layer hot path.
    // angular_vel_magnitude: magnitude of HMD angular velocity in rad/s,
    // derived from captured_pose quaternion delta or XR velocity query.
    void submitFrame(const StereoFrame& frame, float angular_vel_magnitude);

    uint32_t frames_saved()   const { return frames_saved_.load(std::memory_order_relaxed); }
    uint32_t frames_skipped() const { return frames_skipped_.load(std::memory_order_relaxed); }

private:
    struct PendingFrame {
        std::vector<uint8_t> left_gray;   // 1024×1024 R8 decoded from BC4
        std::vector<uint8_t> right_gray;
        uint64_t             sequence{};
        int64_t              timestamp_ns{};
        float                angular_velocity{};
        bool                 motion_flagged{};
    };

    static std::vector<uint8_t> decodeBC4(const std::vector<uint8_t>& bc4,
                                          int width, int height);
    void writerLoop_();
    void writeFrame_(const PendingFrame& pf);
    void writeSessionMeta_();   // writes session_meta.json with camera calibration

    RecorderConfig cfg_;

    std::thread             writer_thread_;
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::vector<PendingFrame> queue_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       stop_requested_{false};

    uint64_t                frame_counter_{0};   // total frames submitted, for stride logic
    std::atomic<uint32_t>   frames_saved_{0};
    std::atomic<uint32_t>   frames_skipped_{0};
};

}  // namespace psvr2pt
