#pragma once

#include "frame.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace psvr2pt {

// Per-hand detection result in normalised camera UV coordinates [0, 1].
struct DetectedHand {
    float uv_x     = 0.f;
    float uv_y     = 0.f;
    float confidence = 0.f;
};

struct HandTrackResult {
    std::array<DetectedHand, 2> hands{};
    int count = 0;  // 0, 1, or 2
};

// Runs MediaPipe BlazePalm lite on a background thread.
// Uses the DirectML execution provider when available (GPU), falls back to CPU.
// Feed frames via push_frame(); poll results with get_result().
class HandTracker {
public:
    HandTracker();
    ~HandTracker();

    HandTracker(const HandTracker&)            = delete;
    HandTracker& operator=(const HandTracker&) = delete;

    // Load the ONNX model and start the background thread.
    // model_path: path to palm_detection_lite.onnx.
    // Returns false if the model cannot be loaded.
    bool start(const std::filesystem::path& model_path);
    void stop();
    bool is_running() const { return running_.load(); }

    // Push the left-eye BC4 frame for detection (non-blocking; drops if busy).
    void push_frame(const std::vector<uint8_t>& bc4_left);

    // Thread-safe: return latest detection result.
    HandTrackResult get_result() const;

private:
    void thread_loop_();

    // BC4 decode + bilinear downsample + channel replication → float32 NHWC [1,192,192,3]
    void preprocess_(const std::vector<uint8_t>& bc4, std::vector<float>& out);

    // Forward declarations avoid including ONNX Runtime in this header.
    struct OrtImpl;
    std::unique_ptr<OrtImpl> ort_;

    std::atomic<bool> running_{false};
    std::thread       worker_;

    // Latest frame (single-slot; newest overwrites older if the thread is busy).
    std::vector<uint8_t> pending_;
    bool                 pending_ready_ = false;
    mutable std::mutex   pending_mutex_;
    std::condition_variable pending_cv_;

    // Latest detection result.
    HandTrackResult        result_{};
    mutable std::mutex     result_mutex_;

    // Preprocessed input buffer (reused each frame to avoid allocation).
    std::vector<float> input_buf_;
};

} // namespace psvr2pt