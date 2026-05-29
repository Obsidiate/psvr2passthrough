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
        }
        have_frame_.store(true);
    }
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
