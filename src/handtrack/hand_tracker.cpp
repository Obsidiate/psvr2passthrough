#include "hand_tracker.h"
#include "bc4_decode.h"
#include "palm_post.h"
#include "logging.h"

#include <onnxruntime_cxx_api.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace psvr2pt {

// ---------------------------------------------------------------------------
// DirectML EP: loaded dynamically so the layer works with CPU-only ORT builds.
// ---------------------------------------------------------------------------

namespace {

typedef OrtStatus* (*AppendDmlFn)(OrtSessionOptions*, int);

bool try_add_dml_ep(Ort::SessionOptions& opts) {
    // ORT ships as a single DLL regardless of EP configuration.
    HMODULE m = GetModuleHandleW(L"onnxruntime.dll");
    if (!m) return false;
    auto fn = reinterpret_cast<AppendDmlFn>(
        GetProcAddress(m, "OrtSessionOptionsAppendExecutionProvider_DML"));
    if (!fn) return false;
    OrtStatus* s = fn(static_cast<OrtSessionOptions*>(opts), 0);
    if (s) { Ort::GetApi().ReleaseStatus(s); return false; }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// OrtImpl — wraps Ort objects so ONNX Runtime headers stay out of hand_tracker.h
// ---------------------------------------------------------------------------

struct HandTracker::OrtImpl {
    Ort::Env     env{ ORT_LOGGING_LEVEL_WARNING, "psvr2_handtrack" };
    Ort::Session session{ nullptr };
    std::array<PalmAnchor, kPalmNumAnchors> anchors = make_palm_anchors();
};

// ---------------------------------------------------------------------------
// HandTracker public API
// ---------------------------------------------------------------------------

HandTracker::HandTracker()  = default;
HandTracker::~HandTracker() { stop(); }

bool HandTracker::start(const std::filesystem::path& model_path) {
    if (running_.load()) return true;

    ort_ = std::make_unique<OrtImpl>();

    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (try_add_dml_ep(opts))
        PT_LOG_INFO("HandTracker: DirectML (GPU) execution provider active");
    else
        PT_LOG_WARN("HandTracker: DirectML unavailable, running on CPU");

    try {
        ort_->session = Ort::Session(ort_->env, model_path.wstring().c_str(), opts);
    } catch (const Ort::Exception& e) {
        PT_LOG_ERROR("HandTracker: failed to load model '{}': {}",
                     model_path.string(), e.what());
        ort_.reset();
        return false;
    }

    input_buf_.resize(1 * kPalmInputSize * kPalmInputSize * 3);

    running_.store(true);
    worker_ = std::thread([this]{ thread_loop_(); });
    PT_LOG_INFO("HandTracker: started");
    return true;
}

void HandTracker::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard lk(pending_mutex_);
        pending_ready_ = true;  // wake the thread so it can exit
    }
    pending_cv_.notify_one();
    if (worker_.joinable()) worker_.join();
    ort_.reset();
    PT_LOG_INFO("HandTracker: stopped");
}

void HandTracker::push_frame(const std::vector<uint8_t>& bc4_left) {
    {
        std::lock_guard lk(pending_mutex_);
        pending_       = bc4_left;  // overwrite; newest wins
        pending_ready_ = true;
    }
    pending_cv_.notify_one();
}

HandTrackResult HandTracker::get_result() const {
    std::lock_guard lk(result_mutex_);
    return result_;
}

// ---------------------------------------------------------------------------
// Background thread
// ---------------------------------------------------------------------------

void HandTracker::thread_loop_() {
    std::vector<uint8_t> work_bc4;

    while (running_.load()) {
        // Wait for a new frame.
        {
            std::unique_lock lk(pending_mutex_);
            pending_cv_.wait(lk, [this]{ return pending_ready_; });
            if (!running_.load()) break;
            work_bc4.swap(pending_);
            pending_ready_ = false;
        }

        if (work_bc4.size() != static_cast<size_t>(kBC4DataSize)) continue;

        // Preprocess: BC4 → grayscale → float32 RGB 192×192.
        preprocess_(work_bc4, input_buf_);

        // Build input tensor (NHWC layout on CPU memory; DML will upload internally).
        const std::array<int64_t, 4> shape{ 1, kPalmInputSize, kPalmInputSize, 3 };
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            mem, input_buf_.data(), input_buf_.size(),
            shape.data(), shape.size());

        // Run inference.
        static const char* in_names[]  = { "input_1" };
        static const char* out_names[] = { "Identity", "Identity_1" };
        std::vector<Ort::Value> outputs;
        try {
            outputs = ort_->session.Run(
                Ort::RunOptions{},
                in_names,  &input_tensor, 1,
                out_names, 2);
        } catch (const Ort::Exception& e) {
            PT_LOG_ERROR("HandTracker: inference error: {}", e.what());
            continue;
        }

        const float* regressors = outputs[0].GetTensorData<float>(); // [1,2016,18]
        const float* scores     = outputs[1].GetTensorData<float>(); // [1,2016,1]

        auto palms = decode_palms(regressors, scores, ort_->anchors,
                                  /*score_thresh=*/0.5f,
                                  /*iou_thresh=*/0.3f,
                                  /*max_hands=*/2);

        HandTrackResult res;
        res.count = static_cast<int>(palms.size());
        for (int i = 0; i < res.count; ++i) {
            res.hands[i].uv_x      = palms[i].cx;
            res.hands[i].uv_y      = palms[i].cy;
            res.hands[i].confidence = palms[i].score;
        }

        {
            std::lock_guard lk(result_mutex_);
            result_ = res;
        }
    }
}

// ---------------------------------------------------------------------------
// Preprocessing
// ---------------------------------------------------------------------------

void HandTracker::preprocess_(const std::vector<uint8_t>& bc4, std::vector<float>& out) {
    // Decode BC4 → 1024×1024 grayscale.
    thread_local std::vector<uint8_t> gray(kCameraWidth * kCameraHeight);
    decode_bc4(bc4.data(), gray.data(), kCameraWidth, kCameraHeight);

    // Bilinear downsample 1024→192 and normalise to [-1, 1], replicating to 3 ch.
    const float scale = static_cast<float>(kCameraWidth) / kPalmInputSize;
    float* dst = out.data();
    for (int y = 0; y < kPalmInputSize; ++y) {
        const float sy = (y + 0.5f) * scale - 0.5f;
        const int   y0 = std::max(0, static_cast<int>(sy));
        const int   y1 = std::min(kCameraHeight - 1, y0 + 1);
        const float fy = sy - y0;
        for (int x = 0; x < kPalmInputSize; ++x) {
            const float sx = (x + 0.5f) * scale - 0.5f;
            const int   x0 = std::max(0, static_cast<int>(sx));
            const int   x1 = std::min(kCameraWidth - 1, x0 + 1);
            const float fx = sx - x0;
            float v =
                gray[y0 * kCameraWidth + x0] * (1.f - fx) * (1.f - fy) +
                gray[y0 * kCameraWidth + x1] * fx           * (1.f - fy) +
                gray[y1 * kCameraWidth + x0] * (1.f - fx) * fy +
                gray[y1 * kCameraWidth + x1] * fx           * fy;
            const float norm = v / 127.5f - 1.f;
            *dst++ = norm;  // R
            *dst++ = norm;  // G
            *dst++ = norm;  // B
        }
    }
}

} // namespace psvr2pt