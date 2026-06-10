#include "camera_source.h"
#include "shared_memory.h"
#include "logging.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <thread>

namespace psvr2pt {

struct CameraSource::Impl {
    SharedMemoryData shm{};
};

// ---------------------------------------------------------------------------
// Geometry derivation — experimental, logged only, no rendering effect.
// ---------------------------------------------------------------------------

namespace {

// Applies row-major 3x3 rotation R to vector v → out.
static void rot3(const float R[9], const float v[3], float out[3]) {
    out[0] = R[0]*v[0] + R[1]*v[1] + R[2]*v[2];
    out[1] = R[3]*v[0] + R[4]*v[1] + R[5]*v[2];
    out[2] = R[6]*v[0] + R[7]*v[1] + R[8]*v[2];
}

// Applies transpose of row-major 3x3 rotation R to vector v → out.
static void rotT3(const float R[9], const float v[3], float out[3]) {
    out[0] = R[0]*v[0] + R[3]*v[1] + R[6]*v[2];
    out[1] = R[1]*v[0] + R[4]*v[1] + R[7]*v[2];
    out[2] = R[2]*v[0] + R[5]*v[1] + R[8]*v[2];
}

// Translation part of the inverse of a rigid-body transform: -R^T * t.
static void inv_pos(const float R[9], const float t[3], float out[3]) {
    rotT3(R, t, out);
    out[0] = -out[0]; out[1] = -out[1]; out[2] = -out[2];
}

static void normalize3(float v[3]) {
    float m = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (m > 1e-6f) { v[0]/=m; v[1]/=m; v[2]/=m; }
}

static void cross3(const float a[3], const float b[3], float c[3]) {
    c[0] = a[1]*b[2] - a[2]*b[1];
    c[1] = a[2]*b[0] - a[0]*b[2];
    c[2] = a[0]*b[1] - a[1]*b[0];
}

// Convention confirmed from BFS analysis:
//   T[from→to].pos  = position of 'from' in 'to' frame
//   T[from→to].mat  = rotation from 'from' frame to 'to' frame
//
// Therefore:
//   cam1_in_cam0 = inv(T01) = -R01^T * T01.pos
//   cam2_in_cam0 = T20.pos  (directly, since T20 is cam2-in-cam0)
//   cam3_in_cam2 = inv(T23) = -R23^T * T23.pos
//   cam3_in_cam0 = cam2_in_cam0 + R_T20^T * cam3_in_cam2
//                (R_T20 rotates from cam2 to cam0, so its transpose rotates cam2→cam0 too?
//                 Actually R_T20 = rotation FROM cam2 TO cam0 = T20.mat,
//                 so cam3_in_cam0 = cam2_in_cam0 + T20.mat * cam3_in_cam2)
//
// The derived headset-forward is logged for inspection only.
static void log_derived_geometry(const CameraExtrinsic ex[3], std::ofstream& f) {
    const CameraExtrinsic* T01 = nullptr;  // cam0 → cam1  (bottom pair)
    const CameraExtrinsic* T23 = nullptr;  // cam2 → cam3  (top pair)
    const CameraExtrinsic* T20 = nullptr;  // cam2 → cam0  (cross, from=2 to=0)

    for (int i = 0; i < 3; ++i) {
        int fr = ex[i].from_id, to = ex[i].to_id;
        if ((fr==0 && to==1) || (fr==1 && to==0)) T01 = &ex[i];
        else if ((fr==2 && to==3) || (fr==3 && to==2)) T23 = &ex[i];
        else T20 = &ex[i];  // the remaining transform connects top↔bottom
    }

    if (!T01 || !T23 || !T20) {
        f << "  (unexpected transform IDs — skipping geometry derivation)\n\n";
        return;
    }

    // Ensure T01 is oriented from=0 and T20 is oriented from=2,to=0.
    // If the stored direction is reversed we just negate the pos after inv.
    // The key property we need: cam1-in-cam0 and cam2-in-cam0.

    // cam1 in cam0's frame.
    float cam1[3];
    if (T01->from_id == 0) {
        // T01 = cam0-in-cam1 → cam1_in_cam0 = inv(T01).pos = -R^T * t
        inv_pos(T01->mat, T01->pos, cam1);
    } else {
        // T01 = cam1-in-cam0 → pos is directly cam1_in_cam0
        cam1[0]=T01->pos[0]; cam1[1]=T01->pos[1]; cam1[2]=T01->pos[2];
    }

    // cam2 in cam0's frame.
    float cam2[3];
    if (T20->to_id == 0) {
        // T20 = cam2-in-cam0 → pos is directly cam2_in_cam0
        cam2[0]=T20->pos[0]; cam2[1]=T20->pos[1]; cam2[2]=T20->pos[2];
    } else {
        // T20 = cam0-in-cam2 → cam2_in_cam0 = inv(T20).pos
        inv_pos(T20->mat, T20->pos, cam2);
    }

    // cam3 in cam2's frame, then rotate to cam0's frame.
    float cam3_in_cam2[3];
    if (T23->from_id == 2) {
        // T23 = cam2-in-cam3 → cam3_in_cam2 = inv(T23).pos
        inv_pos(T23->mat, T23->pos, cam3_in_cam2);
    } else {
        // T23 = cam3-in-cam2 → pos is directly cam3_in_cam2
        cam3_in_cam2[0]=T23->pos[0]; cam3_in_cam2[1]=T23->pos[1]; cam3_in_cam2[2]=T23->pos[2];
    }

    // T20.mat rotates vectors from cam2 frame to cam0 frame.
    float cam3_rot[3];
    if (T20->to_id == 0)
        rot3(T20->mat, cam3_in_cam2, cam3_rot);
    else
        rotT3(T20->mat, cam3_in_cam2, cam3_rot);

    float cam3[3] = { cam2[0]+cam3_rot[0], cam2[1]+cam3_rot[1], cam2[2]+cam3_rot[2] };

    // Midpoints.
    float bot_mid[3] = { cam1[0]*0.5f, cam1[1]*0.5f, cam1[2]*0.5f };
    float top_mid[3] = { (cam2[0]+cam3[0])*0.5f,
                         (cam2[1]+cam3[1])*0.5f,
                         (cam2[2]+cam3[2])*0.5f };

    // Basis vectors (unnormalised first for logging, then normalise).
    float right[3] = { cam1[0], cam1[1], cam1[2] };
    float up[3]    = { top_mid[0]-bot_mid[0],
                       top_mid[1]-bot_mid[1],
                       top_mid[2]-bot_mid[2] };
    normalize3(right);
    normalize3(up);

    // Headset forward = up × right.
    // In camera Y-down convention, if 'up' correctly points upward in cam0's frame
    // (negative Y) and 'right' points rightward (+X), then up×right gives +Z (forward).
    // If the Z component comes out negative the cross product order is wrong; we flip.
    float fwd[3];
    cross3(up, right, fwd);
    if (fwd[2] < 0.f) { fwd[0]=-fwd[0]; fwd[1]=-fwd[1]; fwd[2]=-fwd[2]; }
    normalize3(fwd);

    // Derived angles.
    // tilt_down: angle between cam0's Z-axis and fwd, projected onto the vertical (YZ) plane.
    //   Positive = cam0 tilts downward (fwd has negative Y in camera Y-down convention).
    // toe_out: angle in horizontal (XZ) plane — cross-check against known 15.1 deg.
    const float pi = std::acos(-1.f);
    const float tilt_rad = std::atan2(-fwd[1], fwd[2]);  // negated: -Y = up in cam convention
    const float toe_rad  = std::atan2( fwd[0], fwd[2]);

    f << "=== Derived Headset Geometry (experimental — validate before acting on values) ===\n";
    f << std::setprecision(1) << std::fixed;
    f << "Camera positions in cam0 frame (mm):\n";
    f << "  cam0: (  0.0,   0.0,   0.0)\n";
    f << "  cam1: (" << cam1[0]*1000.f << ", " << cam1[1]*1000.f << ", " << cam1[2]*1000.f << ")\n";
    f << "  cam2: (" << cam2[0]*1000.f << ", " << cam2[1]*1000.f << ", " << cam2[2]*1000.f << ")\n";
    f << "  cam3: (" << cam3[0]*1000.f << ", " << cam3[1]*1000.f << ", " << cam3[2]*1000.f << ")\n";
    f << "Bottom pair midpoint: (" << bot_mid[0]*1000.f << ", "
                                   << bot_mid[1]*1000.f << ", "
                                   << bot_mid[2]*1000.f << ")\n";
    f << "Top pair midpoint:    (" << top_mid[0]*1000.f << ", "
                                   << top_mid[1]*1000.f << ", "
                                   << top_mid[2]*1000.f << ")\n";
    f << "Headset-forward in cam0 frame (Z should be largest, positive):\n";
    f << std::setprecision(4);
    f << "  (" << fwd[0] << ", " << fwd[1] << ", " << fwd[2] << ")\n";
    f << std::setprecision(2);
    f << "Implied toe_out  (cross-check: expect ~15.1 deg): "
      << (toe_rad  * 180.f / pi) << " deg  (" << toe_rad  << " rad)\n";
    f << "Implied tilt_down (positive = cam tilts downward):  "
      << (tilt_rad * 180.f / pi) << " deg  (" << tilt_rad << " rad)\n\n";
}

} // namespace

CameraSource::CameraSource() : impl_(std::make_unique<Impl>()) {}

CameraSource::~CameraSource() { stop(); }

bool CameraSource::start() {
    if (running_.load()) return true;
    try {
        setup_shared_memory(impl_->shm);
    } catch (const std::exception& e) {
        PT_LOG_WARN("CameraSource: shared memory unavailable: {}", e.what());
        return false;
    }

    for (int eye = 0; eye < 2; ++eye) {
        if (!get_distortion_config(impl_->shm, eye, params_[eye], intrinsics_[eye])) {
            PT_LOG_WARN("CameraSource: failed to read calibration for eye {}", eye);
        } else {
            PT_LOG_INFO("CameraSource eye {} intrinsics: fx={} fy={} cx={} cy={}",
                        eye, intrinsics_[eye].fx, intrinsics_[eye].fy,
                        intrinsics_[eye].cx, intrinsics_[eye].cy);
            PT_LOG_INFO("CameraSource eye {} distortion coeffs[0..3]: {:.6f} {:.6f} {:.6f} {:.6f}",
                        eye, params_[eye].coeffs[0], params_[eye].coeffs[1],
                        params_[eye].coeffs[2], params_[eye].coeffs[3]);
        }
    }

    CameraExtrinsic extrinsics[3]{};
    const bool has_extrinsics = get_camera_extrinsics(impl_->shm, extrinsics);
    if (has_extrinsics) {
        for (int i = 0; i < 3; ++i) {
            const float* p = extrinsics[i].pos;
            const float  mag = std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            PT_LOG_INFO("CameraSource extrinsic[{}]: cam{}->cam{}  "
                        "pos=({:.4f},{:.4f},{:.4f}) |pos|={:.4f}",
                        i, extrinsics[i].from_id, extrinsics[i].to_id,
                        p[0], p[1], p[2], mag);
        }
    } else {
        PT_LOG_WARN("CameraSource: failed to read camera extrinsics");
    }

    // Write calibration_dump.txt — created fresh each session so it always
    // reflects the current headset.
    try {
        const auto dump_path = get_layer_data_dir() / "calibration_dump.txt";
        std::ofstream f(dump_path, std::ios::out | std::ios::trunc);
        f << std::fixed;

        // Timestamp header
        std::time_t now = std::time(nullptr);
        char tbuf[32]{};
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        f << "# PSVR2 Passthrough Layer — Camera Calibration Dump\n";
        f << "# Generated: " << tbuf << "\n\n";

        // Factory intrinsics
        f << "=== Factory Intrinsics ===\n";
        f << std::left
          << std::setw(5)  << "Eye"
          << std::setw(14) << "fx"
          << std::setw(14) << "fy"
          << std::setw(14) << "cx"
          << std::setw(14) << "cy" << "\n";
        for (int eye = 0; eye < 2; ++eye) {
            f << std::setw(5)  << eye
              << std::setw(14) << std::setprecision(4) << intrinsics_[eye].fx
              << std::setw(14) << intrinsics_[eye].fy
              << std::setw(14) << intrinsics_[eye].cx
              << std::setw(14) << intrinsics_[eye].cy << "\n";
        }
        f << "\n";

        // Distortion coefficients
        f << "=== Distortion Coefficients (20 per eye) ===\n";
        for (int eye = 0; eye < 2; ++eye) {
            f << "Eye " << eye << ":\n";
            for (int j = 0; j < 20; ++j) {
                f << "  k" << std::setw(2) << std::left << j << " = "
                  << std::setprecision(10) << std::setw(18) << params_[eye].coeffs[j];
                if (j % 4 == 3) f << "\n";
            }
            if (20 % 4 != 0) f << "\n";
            f << "\n";
        }

        // Inter-camera extrinsics
        f << "=== Inter-camera Extrinsics (unverified units/convention) ===\n";
        if (has_extrinsics) {
            for (int i = 0; i < 3; ++i) {
                const auto& e = extrinsics[i];
                const float mag = std::sqrt(e.pos[0]*e.pos[0] +
                                            e.pos[1]*e.pos[1] +
                                            e.pos[2]*e.pos[2]);
                f << "Transform[" << i << "]  cam" << e.from_id
                  << " -> cam" << e.to_id << "\n";
                f << std::setprecision(6);
                f << "  pos : (" << e.pos[0] << ", "
                                 << e.pos[1] << ", "
                                 << e.pos[2] << ")  |pos| = " << mag << "\n";
                f << "  mat : [" << e.mat[0] << ", " << e.mat[1] << ", " << e.mat[2] << "]\n";
                f << "        [" << e.mat[3] << ", " << e.mat[4] << ", " << e.mat[5] << "]\n";
                f << "        [" << e.mat[6] << ", " << e.mat[7] << ", " << e.mat[8] << "]\n";
                f << "\n";
            }
        } else {
            f << "  (unavailable)\n\n";
        }

        // Derived geometry — experimental.
        if (has_extrinsics)
            log_derived_geometry(extrinsics, f);

        f.flush();
        PT_LOG_INFO("CameraSource: calibration dump started at {}", dump_path.string());
    } catch (const std::exception& e) {
        PT_LOG_WARN("CameraSource: failed to write calibration dump: {}", e.what());
    }

    front_.left.resize(kBC4DataSize);
    front_.right.resize(kBC4DataSize);

    running_.store(true);
    worker_ = std::thread([this] { thread_loop(); });
    PT_LOG_INFO("CameraSource started");
    return true;
}

void CameraSource::stop() {
    if (!running_.exchange(false)) return;
    frame_cv_.notify_all();   // wake any consumer blocked in wait_for_frame
    if (worker_.joinable()) worker_.join();
    cleanup_shared_memory(impl_->shm);
    PT_LOG_INFO("CameraSource stopped");
}

void CameraSource::thread_loop() {
    StereoFrame staging;
    staging.left.resize(kBC4DataSize);
    staging.right.resize(kBC4DataSize);

    while (running_.load()) {
        Pose3f frame_pose{};
        const bool ok = copy_latest_image_buffer(
            impl_->shm,
            staging.left.data(),
            staging.right.data(),
            kBC4DataSize,
            /*timeout_ms=*/50,
            &frame_pose);
        if (!ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        staging.captured_pose = frame_pose;

        staging.captured_at = std::chrono::steady_clock::now();
        staging.sequence    = ++last_seq_;

        {
            std::lock_guard lock(front_mutex_);
            front_.left.swap(staging.left);
            front_.right.swap(staging.right);
            front_.captured_at   = staging.captured_at;
            front_.sequence      = staging.sequence;
            front_.captured_pose = staging.captured_pose;
            staging.left.resize(kBC4DataSize);
            staging.right.resize(kBC4DataSize);
            have_frame_.store(true);
        }
        frame_cv_.notify_all();
    }
}

bool CameraSource::wait_for_frame(unsigned timeout_ms) {
    std::unique_lock lock(front_mutex_);
    if (have_frame_.load()) return true;
    return frame_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [this] { return have_frame_.load(); });
}

bool CameraSource::try_get_latest(StereoFrame& out) {
    if (!have_frame_.load()) return false;
    std::lock_guard lock(front_mutex_);
    // Swap rather than copy — out donates its old (already-allocated) buffers to
    // front_ so the producer can recycle them without reallocating.
    // Reset have_frame_ so the next call blocks until the producer writes a new
    // frame; this prevents re-consuming a stale front_ when the game renders
    // faster than the camera driver delivers frames.
    out.left.swap(front_.left);
    out.right.swap(front_.right);
    out.captured_at   = front_.captured_at;
    out.sequence      = front_.sequence;
    out.captured_pose = front_.captured_pose;
    have_frame_.store(false);
    return true;
}

bool CameraSource::get_latest_pose(Pose3f& out) const {
    if (!impl_ || !impl_->shm.imageMemBase) return false;
    return read_latest_pose(impl_->shm, out);
}

const CameraIntrinsics& CameraSource::intrinsics(CameraId id) const {
    return intrinsics_[static_cast<int>(id)];
}

const CameraParameters& CameraSource::params(CameraId id) const {
    return params_[static_cast<int>(id)];
}

}  // namespace psvr2pt
