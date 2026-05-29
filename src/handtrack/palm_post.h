#pragma once
// BlazePalm lite post-processing: anchor generation, regressor decoding, NMS.
// Model: palm_detection_lite.onnx
//   Input:  input_1  [1, 192, 192, 3] float32 NHWC, values in [-1, 1]
//   Output: Identity   [1, 2016, 18] regressors (cx_off, cy_off, w, h, 7×kp_xy) in pixels
//           Identity_1 [1, 2016,  1] raw logit scores

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace psvr2pt {

inline constexpr int kPalmInputSize  = 192;
inline constexpr int kPalmNumAnchors = 2016;

struct PalmAnchor { float cx, cy; }; // normalized [0,1]

// Precompute the 2016 anchors for a 192×192 input.
// Layer 0: stride 8  → 24×24 feature map, 2 anchors/cell → 1152
// Layer 1: stride 16 → 12×12 feature map, 6 anchors/cell →  864
inline std::array<PalmAnchor, kPalmNumAnchors> make_palm_anchors() {
    std::array<PalmAnchor, kPalmNumAnchors> a;
    int idx = 0;
    // Layer 0
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x)
            for (int k = 0; k < 2; ++k)
                a[idx++] = { (x + 0.5f) / 24.f, (y + 0.5f) / 24.f };
    // Layer 1
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 12; ++x)
            for (int k = 0; k < 6; ++k)
                a[idx++] = { (x + 0.5f) / 12.f, (y + 0.5f) / 12.f };
    return a;
}

struct PalmDetection {
    float cx, cy;   // normalized [0,1] in input image coords
    float w,  h;    // normalized
    float score;
};

// Decode one anchor's regressor + score into a detection.
inline PalmDetection decode_anchor(const PalmAnchor& anchor,
                                   const float* reg18,   // regressors for this anchor
                                   float raw_score) {
    const float inv = 1.f / kPalmInputSize;
    PalmDetection d;
    d.cx    = anchor.cx + reg18[0] * inv;
    d.cy    = anchor.cy + reg18[1] * inv;
    d.w     = reg18[2] * inv;
    d.h     = reg18[3] * inv;
    d.score = 1.f / (1.f + std::exp(-raw_score));  // sigmoid
    return d;
}

// Intersection-over-union of two detections (center+size form).
inline float iou(const PalmDetection& a, const PalmDetection& b) {
    const float ax0 = a.cx - a.w * 0.5f, ax1 = a.cx + a.w * 0.5f;
    const float ay0 = a.cy - a.h * 0.5f, ay1 = a.cy + a.h * 0.5f;
    const float bx0 = b.cx - b.w * 0.5f, bx1 = b.cx + b.w * 0.5f;
    const float by0 = b.cy - b.h * 0.5f, by1 = b.cy + b.h * 0.5f;
    const float ix = std::max(0.f, std::min(ax1, bx1) - std::max(ax0, bx0));
    const float iy = std::max(0.f, std::min(ay1, by1) - std::max(ay0, by0));
    const float inter = ix * iy;
    const float uni   = a.w * a.h + b.w * b.h - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

// Decode the full model output into up to max_hands detections via greedy NMS.
// regressors: [2016 × 18], scores: [2016 × 1]
inline std::vector<PalmDetection> decode_palms(
        const float* regressors,
        const float* scores,
        const std::array<PalmAnchor, kPalmNumAnchors>& anchors,
        float score_thresh = 0.5f,
        float iou_thresh   = 0.3f,
        int   max_hands    = 2)
{
    // Collect candidates above threshold.
    std::vector<PalmDetection> cands;
    cands.reserve(64);
    for (int i = 0; i < kPalmNumAnchors; ++i) {
        float s = 1.f / (1.f + std::exp(-scores[i]));
        if (s < score_thresh) continue;
        cands.push_back(decode_anchor(anchors[i], regressors + i * 18, scores[i]));
    }

    // Sort by score descending.
    std::sort(cands.begin(), cands.end(),
              [](const PalmDetection& a, const PalmDetection& b){ return a.score > b.score; });

    // Greedy NMS.
    std::vector<PalmDetection> out;
    std::vector<bool> suppressed(cands.size(), false);
    for (int i = 0; i < static_cast<int>(cands.size()) && static_cast<int>(out.size()) < max_hands; ++i) {
        if (suppressed[i]) continue;
        out.push_back(cands[i]);
        for (int j = i + 1; j < static_cast<int>(cands.size()); ++j)
            if (!suppressed[j] && iou(cands[i], cands[j]) > iou_thresh)
                suppressed[j] = true;
    }
    return out;
}

} // namespace psvr2pt