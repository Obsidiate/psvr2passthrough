#pragma once

#include "HandDetection.h"
#include "frame.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

// Forward-declare ORT types to avoid polluting every TU with onnxruntime headers.
namespace Ort { class Session; class Env; class SessionOptions; class MemoryInfo; }

namespace psvr2pt {

struct BespokeDetectorConfig {
    std::filesystem::path model_path;
    int   input_size            = 192;       // model expects input_size × input_size
    float confidence_threshold  = 0.40f;     // suppress results below this
    float motion_gate_rad_s     = 1.5f;      // skip inference when HMD velocity exceeds this
    bool  enable_kalman         = true;

    // Kalman filter process / measurement noise (tune after deployment).
    float kalman_process_noise  = 1e-4f;
    float kalman_measurement_noise = 1e-2f;
};

// Thread-safe wrist detector backed by a bespoke ONNX model.
//
// Owns the OrtSession; one instance per eye (left and right camera frames
// produce independent detection results, or you can run a single instance
// on the left eye only — the model output covers both wrists).
//
// Interface contract:
//   - loadModel() must succeed before any call to detect().
//   - detect() is safe to call from any thread after loadModel().
//   - detect() is non-blocking; it never waits on I/O or the GPU.
class BespokeDetector {
public:
    BespokeDetector();
    ~BespokeDetector();

    // Loads and verifies the ONNX model. Returns false on failure; logs reason.
    [[nodiscard]] bool loadModel(const BespokeDetectorConfig& cfg);

    // Run inference on a single 1024×1024 R8 grayscale frame.
    // gray_r8: pointer to kCameraWidth * kCameraHeight bytes.
    // angular_vel_magnitude: HMD angular velocity in rad/s (used for motion gate).
    // Returns HandDetectionResult::valid == false when inference is skipped.
    [[nodiscard]] HandDetectionResult detect(const uint8_t* gray_r8,
                                             int            src_width,
                                             int            src_height,
                                             float          angular_vel_magnitude);

    bool isLoaded() const { return loaded_.load(std::memory_order_acquire); }

private:
    void resizeGrayToInput_(const uint8_t* src, int src_w, int src_h,
                             float* dst) const;

    struct KalmanState {
        float x = 0.f, y = 0.f;
        float vx = 0.f, vy = 0.f;
        float P[4][4]{};   // 4×4 covariance, identity × large initial
        bool  initialised = false;
        void  reset();
        void  predict(float dt, float q);
        void  update(float mx, float my, float r);
        float px() const { return x; }
        float py() const { return y; }
    };

    BespokeDetectorConfig cfg_;

    // ORT objects are heap-allocated to avoid pulling ORT headers into every TU.
    struct OrtState;
    std::unique_ptr<OrtState> ort_;

    std::vector<float> input_buf_;   // reused across frames
    mutable std::mutex infer_mutex_; // serialise ORT session (not reentrant)

    KalmanState kf_left_;
    KalmanState kf_right_;
    std::chrono::steady_clock::time_point last_detect_time_{};

    std::atomic<bool> loaded_{false};
};

}  // namespace psvr2pt
