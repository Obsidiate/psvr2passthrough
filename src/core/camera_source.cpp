#include "camera_source.h"
#include "shared_memory.h"
#include "logging.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>

namespace psvr2pt {

// BC4_UNORM compressed bytes per eye from shared memory.
// BC4 = 4 bits/pixel; 1024×1024 texture = (256 blocks × 256 blocks) × 8 bytes/block.
static constexpr int kBC4DataSize = (kCameraWidth / 4) * (kCameraHeight / 4) * 8;  // 524,288

// Decompress one BC4_UNORM block (8 bytes) into a 4×4 patch of R8 pixels.
// dst_stride is the row pitch of the destination R8 image in bytes.
static void decompress_bc4_block(const uint8_t* src, uint8_t* dst, int dst_stride) {
    const uint8_t r0 = src[0], r1 = src[1];
    uint8_t lut[8];
    lut[0] = r0;
    lut[1] = r1;
    if (r0 > r1) {
        lut[2] = static_cast<uint8_t>((6 * r0 + 1 * r1 + 3) / 7);
        lut[3] = static_cast<uint8_t>((5 * r0 + 2 * r1 + 3) / 7);
        lut[4] = static_cast<uint8_t>((4 * r0 + 3 * r1 + 3) / 7);
        lut[5] = static_cast<uint8_t>((3 * r0 + 4 * r1 + 3) / 7);
        lut[6] = static_cast<uint8_t>((2 * r0 + 5 * r1 + 3) / 7);
        lut[7] = static_cast<uint8_t>((1 * r0 + 6 * r1 + 3) / 7);
    } else {
        lut[2] = static_cast<uint8_t>((4 * r0 + 1 * r1 + 2) / 5);
        lut[3] = static_cast<uint8_t>((3 * r0 + 2 * r1 + 2) / 5);
        lut[4] = static_cast<uint8_t>((2 * r0 + 3 * r1 + 2) / 5);
        lut[5] = static_cast<uint8_t>((1 * r0 + 4 * r1 + 2) / 5);
        lut[6] = 0;
        lut[7] = 255;
    }
    // 48-bit index field packed in bytes [2..7] (3 bits per pixel, 16 pixels).
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(src[2 + i]) << (8 * i);
    for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
            const int shift = (py * 4 + px) * 3;
            dst[py * dst_stride + px] = lut[(bits >> shift) & 7];
        }
    }
}

static void decompress_bc4(const uint8_t* bc4, uint8_t* r8) {
    const int blocks_x = kCameraWidth  / 4;
    const int blocks_y = kCameraHeight / 4;
    for (int by = 0; by < blocks_y; ++by) {
        for (int bx = 0; bx < blocks_x; ++bx) {
            const uint8_t* src = bc4 + (by * blocks_x + bx) * 8;
            uint8_t*       dst = r8  + (by * 4) * kCameraWidth + (bx * 4);
            decompress_bc4_block(src, dst, kCameraWidth);
        }
    }
}

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

        f.flush();
        PT_LOG_INFO("CameraSource: calibration dump started at {}", dump_path.string());
    } catch (const std::exception& e) {
        PT_LOG_WARN("CameraSource: failed to write calibration dump: {}", e.what());
    }

    const size_t r8_size = static_cast<size_t>(kCameraWidth) * kCameraHeight;
    front_.left.resize(r8_size);
    front_.right.resize(r8_size);

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
    // Intermediate BC4 buffer (what shared memory contains).
    std::vector<uint8_t> bc4_left(kBC4DataSize), bc4_right(kBC4DataSize);

    // Decompressed R8 staging (what we expose to the compositor).
    StereoFrame staging;
    const size_t r8_size = static_cast<size_t>(kCameraWidth) * kCameraHeight;
    staging.left.resize(r8_size);
    staging.right.resize(r8_size);

    while (running_.load()) {
        const bool ok = copy_latest_image_buffer(
            impl_->shm,
            bc4_left.data(),
            bc4_right.data(),
            kBC4DataSize,
            /*timeout_ms=*/50);
        if (!ok) continue;

        // Decompress BC4→R8 on the camera thread so upload_frame never stalls.
        decompress_bc4(bc4_left.data(),  staging.left.data());
        decompress_bc4(bc4_right.data(), staging.right.data());

        staging.captured_at = std::chrono::steady_clock::now();
        staging.sequence    = ++last_seq_;

        {
            std::lock_guard lock(front_mutex_);
            front_.left.swap(staging.left);
            front_.right.swap(staging.right);
            front_.captured_at = staging.captured_at;
            front_.sequence    = staging.sequence;
            staging.left.resize(r8_size);
            staging.right.resize(r8_size);
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
    // frame; this prevents re-consuming an empty front_ when the game renders
    // faster than the camera (e.g. 120 Hz game vs ~60 Hz camera).
    out.left.swap(front_.left);
    out.right.swap(front_.right);
    out.captured_at = front_.captured_at;
    out.sequence    = front_.sequence;
    have_frame_.store(false);
    return true;
}

const CameraIntrinsics& CameraSource::intrinsics(CameraId id) const {
    return intrinsics_[static_cast<int>(id)];
}

const CameraParameters& CameraSource::params(CameraId id) const {
    return params_[static_cast<int>(id)];
}

}  // namespace psvr2pt
