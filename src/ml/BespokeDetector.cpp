#include "BespokeDetector.h"
#include "logging.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace psvr2pt {

// ── ORT internal state ────────────────────────────────────────────────────────

struct BespokeDetector::OrtState {
    Ort::Env            env{ORT_LOGGING_LEVEL_WARNING, "BespokeDetector"};
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo     mem_info{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
    std::string         input_name;
    std::string         output_name;
};

// ── Kalman filter ─────────────────────────────────────────────────────────────
// 4-state constant-velocity filter: [x, y, vx, vy].

void BespokeDetector::KalmanState::reset() {
    x = y = vx = vy = 0.f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            P[i][j] = (i == j) ? 1.f : 0.f;
    P[2][2] = P[3][3] = 100.f;  // high velocity uncertainty on init
    initialised = false;
}

void BespokeDetector::KalmanState::predict(float dt, float q) {
    // State transition: x += vx*dt, y += vy*dt
    x += vx * dt;
    y += vy * dt;

    // P = F*P*F' + Q  (simplified diagonal Q)
    // F = [[1,0,dt,0],[0,1,0,dt],[0,0,1,0],[0,0,0,1]]
    P[0][2] += dt * P[2][2];  P[0][3] += dt * P[3][3];
    P[1][2] += dt * P[2][2];  P[1][3] += dt * P[3][3];
    P[2][0] += dt * P[2][2];  P[3][0] += dt * P[3][3];
    P[2][1] += dt * P[2][2];  P[3][1] += dt * P[3][3];
    // add process noise
    P[0][0] += q; P[1][1] += q;
    P[2][2] += q; P[3][3] += q;
}

void BespokeDetector::KalmanState::update(float mx, float my, float r) {
    // H = [[1,0,0,0],[0,1,0,0]]  (observe x and y only)
    // S = H*P*H' + R
    float S00 = P[0][0] + r;
    float S11 = P[1][1] + r;

    // K = P*H' * S^-1
    float K00 = P[0][0] / S00;
    float K10 = P[1][0] / S00;
    float K20 = P[2][0] / S00;
    float K30 = P[3][0] / S00;
    float K01 = P[0][1] / S11;
    float K11 = P[1][1] / S11;
    float K21 = P[2][1] / S11;
    float K31 = P[3][1] / S11;

    float ex = mx - x;
    float ey = my - y;

    x  += K00 * ex + K01 * ey;
    y  += K10 * ex + K11 * ey;
    vx += K20 * ex + K21 * ey;
    vy += K30 * ex + K31 * ey;

    // P = (I - K*H)*P
    P[0][0] -= K00*P[0][0] + K01*P[1][0];
    P[0][1] -= K00*P[0][1] + K01*P[1][1];
    P[1][0] -= K10*P[0][0] + K11*P[1][0];
    P[1][1] -= K10*P[0][1] + K11*P[1][1];
    P[2][0] -= K20*P[0][0] + K21*P[1][0];
    P[2][1] -= K20*P[0][1] + K21*P[1][1];
    P[3][0] -= K30*P[0][0] + K31*P[1][0];
    P[3][1] -= K30*P[0][1] + K31*P[1][1];

    initialised = true;
}

// ── BespokeDetector ───────────────────────────────────────────────────────────

BespokeDetector::BespokeDetector() = default;
BespokeDetector::~BespokeDetector() = default;

bool BespokeDetector::loadModel(const BespokeDetectorConfig& cfg) {
    cfg_ = cfg;

    const int n = cfg_.input_size * cfg_.input_size;
    input_buf_.assign(static_cast<size_t>(n), 0.f);

    kf_left_.reset();
    kf_right_.reset();

    try {
        ort_ = std::make_unique<OrtState>();
        ort_->opts.SetIntraOpNumThreads(1);
        ort_->opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const std::wstring wpath = cfg_.model_path.wstring();
        ort_->session = std::make_unique<Ort::Session>(
            ort_->env, wpath.c_str(), ort_->opts);

        // Cache input/output name strings
        Ort::AllocatorWithDefaultOptions alloc;
        auto in_name_ptr  = ort_->session->GetInputNameAllocated(0, alloc);
        auto out_name_ptr = ort_->session->GetOutputNameAllocated(0, alloc);
        ort_->input_name  = std::string(in_name_ptr.get());
        ort_->output_name = std::string(out_name_ptr.get());

        // Verify input shape [1, 1, 192, 192]
        auto in_info   = ort_->session->GetInputTypeInfo(0);
        auto shape_vec = in_info.GetTensorTypeAndShapeInfo().GetShape();
        if (shape_vec.size() != 4 ||
            shape_vec[0] != 1 ||
            shape_vec[1] != 1 ||
            shape_vec[2] != cfg_.input_size ||
            shape_vec[3] != cfg_.input_size) {
            PT_LOG_ERROR("BespokeDetector: unexpected input shape. Expected [1,1,{0},{0}].",
                         cfg_.input_size);
            ort_.reset();
            return false;
        }

        PT_LOG_INFO("BespokeDetector: loaded '{}' input='{}' output='{}'",
                    cfg_.model_path.string(),
                    ort_->input_name, ort_->output_name);
        loaded_.store(true, std::memory_order_release);
        return true;

    } catch (const Ort::Exception& e) {
        PT_LOG_ERROR("BespokeDetector: ORT exception loading model: {}", e.what());
        ort_.reset();
        return false;
    }
}

void BespokeDetector::resizeGrayToInput_(const uint8_t* src, int src_w, int src_h,
                                          float* dst) const {
    const int dst_size = cfg_.input_size;
    const float sx = static_cast<float>(src_w) / dst_size;
    const float sy = static_cast<float>(src_h) / dst_size;

    for (int dy = 0; dy < dst_size; ++dy) {
        const int sy_ = static_cast<int>(dy * sy);
        const int sy_c = std::min(sy_ + 1, src_h - 1);
        const float fy = dy * sy - sy_;
        for (int dx = 0; dx < dst_size; ++dx) {
            const int sx_ = static_cast<int>(dx * sx);
            const int sx_c = std::min(sx_ + 1, src_w - 1);
            const float fx = dx * sx - sx_;
            // Bilinear interpolation
            float v =
                src[sy_  * src_w + sx_ ] * (1-fx)*(1-fy)
              + src[sy_  * src_w + sx_c] *    fx *(1-fy)
              + src[sy_c * src_w + sx_ ] * (1-fx)*   fy
              + src[sy_c * src_w + sx_c] *    fx *    fy;
            dst[dy * dst_size + dx] = v / 255.f;
        }
    }
}

HandDetectionResult BespokeDetector::detect(const uint8_t* gray_r8,
                                              int            src_width,
                                              int            src_height,
                                              float          angular_vel) {
    HandDetectionResult result{};

    if (!loaded_.load(std::memory_order_acquire)) return result;

    // Motion gate: skip inference when HMD is moving fast (motion blur).
    if (angular_vel > cfg_.motion_gate_rad_s) return result;

    const auto now = std::chrono::steady_clock::now();
    const float dt = last_detect_time_.time_since_epoch().count() > 0
        ? std::chrono::duration<float>(now - last_detect_time_).count()
        : 0.f;
    last_detect_time_ = now;

    // Resize source frame to model input size.
    resizeGrayToInput_(gray_r8, src_width, src_height, input_buf_.data());

    float raw_output[6]{};
    {
        std::lock_guard lock(infer_mutex_);
        const int64_t shape[4] = {1, 1, cfg_.input_size, cfg_.input_size};
        auto input_tensor = Ort::Value::CreateTensor<float>(
            ort_->mem_info,
            input_buf_.data(),
            static_cast<size_t>(cfg_.input_size * cfg_.input_size),
            shape, 4);

        const char* in_names[]  = { ort_->input_name.c_str() };
        const char* out_names[] = { ort_->output_name.c_str() };

        auto outputs = ort_->session->Run(
            Ort::RunOptions{nullptr},
            in_names,  &input_tensor, 1,
            out_names, 1);

        const float* data = outputs[0].GetTensorData<float>();
        std::copy(data, data + 6, raw_output);
    }

    // raw_output: [lx, ly, lconf, rx, ry, rconf]
    const float lx = raw_output[0], ly = raw_output[1], lconf = raw_output[2];
    const float rx = raw_output[3], ry = raw_output[4], rconf = raw_output[5];

    if (cfg_.enable_kalman && dt > 0.f) {
        const float q = cfg_.kalman_process_noise;
        const float r = cfg_.kalman_measurement_noise;

        if (!kf_left_.initialised) { kf_left_.x = lx; kf_left_.y = ly; }
        kf_left_.predict(dt, q);
        if (lconf >= cfg_.confidence_threshold)
            kf_left_.update(lx, ly, r);

        if (!kf_right_.initialised) { kf_right_.x = rx; kf_right_.y = ry; }
        kf_right_.predict(dt, q);
        if (rconf >= cfg_.confidence_threshold)
            kf_right_.update(rx, ry, r);

        result.left  = { kf_left_.px(),  kf_left_.py(),  lconf };
        result.right = { kf_right_.px(), kf_right_.py(), rconf };
    } else {
        result.left  = { lx, ly, lconf };
        result.right = { rx, ry, rconf };
    }

    // Suppress positions below confidence threshold (zero out, keep conf value).
    if (result.left.conf  < cfg_.confidence_threshold) { result.left.x  = 0.f; result.left.y  = 0.f; }
    if (result.right.conf < cfg_.confidence_threshold) { result.right.x = 0.f; result.right.y = 0.f; }

    result.valid = true;
    return result;
}

}  // namespace psvr2pt
