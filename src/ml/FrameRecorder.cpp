#include "FrameRecorder.h"
#include "logging.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace psvr2pt {

// ---- BC4 decode ---------------------------------------------------------------
// BC4_UNORM: 4×4 pixel blocks, 8 bytes each.
// Bytes 0-1: ref0, ref1 (uint8). Bytes 2-7: 48-bit index table (3 bits per pixel).
// Interpolation table matches D3D spec for BC4_UNORM.
std::vector<uint8_t> FrameRecorder::decodeBC4(const std::vector<uint8_t>& bc4,
                                               int width, int height) {
    std::vector<uint8_t> out(static_cast<size_t>(width * height), 0);
    const int bx = width  / 4;
    const int by = height / 4;

    for (int by_ = 0; by_ < by; ++by_) {
        for (int bx_ = 0; bx_ < bx; ++bx_) {
            const uint8_t* b = bc4.data() + (static_cast<size_t>(by_ * bx + bx_)) * 8;
            const uint8_t r0 = b[0], r1 = b[1];

            uint8_t pal[8];
            pal[0] = r0; pal[1] = r1;
            if (r0 > r1) {
                pal[2] = static_cast<uint8_t>((6*r0 + 1*r1) / 7);
                pal[3] = static_cast<uint8_t>((5*r0 + 2*r1) / 7);
                pal[4] = static_cast<uint8_t>((4*r0 + 3*r1) / 7);
                pal[5] = static_cast<uint8_t>((3*r0 + 4*r1) / 7);
                pal[6] = static_cast<uint8_t>((2*r0 + 5*r1) / 7);
                pal[7] = static_cast<uint8_t>((1*r0 + 6*r1) / 7);
            } else {
                pal[2] = static_cast<uint8_t>((4*r0 + 1*r1) / 5);
                pal[3] = static_cast<uint8_t>((3*r0 + 2*r1) / 5);
                pal[4] = static_cast<uint8_t>((2*r0 + 3*r1) / 5);
                pal[5] = static_cast<uint8_t>((1*r0 + 4*r1) / 5);
                pal[6] = 0;
                pal[7] = 255;
            }

            // 48-bit index block packed in bytes 2-7, little-endian, 3 bits per pixel.
            uint64_t bits = 0;
            for (int i = 2; i < 8; ++i)
                bits |= (static_cast<uint64_t>(b[i]) << ((i - 2) * 8));

            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int shift   = (py * 4 + px) * 3;
                    const uint8_t idx = static_cast<uint8_t>((bits >> shift) & 0x7);
                    const int ox = bx_ * 4 + px;
                    const int oy = by_ * 4 + py;
                    if (ox < width && oy < height)
                        out[static_cast<size_t>(oy * width + ox)] = pal[idx];
                }
            }
        }
    }
    return out;
}

// ---- FrameRecorder ------------------------------------------------------------

FrameRecorder::FrameRecorder() = default;

FrameRecorder::~FrameRecorder() {
    stop();
}

bool FrameRecorder::start(const RecorderConfig& cfg) {
    if (running_.load()) return false;
    cfg_ = cfg;

    std::error_code ec;
    std::filesystem::create_directories(cfg_.output_dir, ec);
    if (ec) {
        PT_LOG_ERROR("FrameRecorder: cannot create output dir '{}': {}",
                     cfg_.output_dir.string(), ec.message());
        return false;
    }

    frame_counter_   = 0;
    frames_saved_    = 0;
    frames_skipped_  = 0;
    stop_requested_  = false;
    running_         = true;

    if (cfg_.calibration_valid)
        writeSessionMeta_();

    writer_thread_ = std::thread(&FrameRecorder::writerLoop_, this);
    PT_LOG_INFO("FrameRecorder started → '{}'  every_n={}  max_vel={:.2f}  max_frames={}",
                cfg_.output_dir.string(), cfg_.capture_every_n,
                cfg_.max_angular_velocity, cfg_.max_frames);
    return true;
}

void FrameRecorder::stop() {
    if (!running_.load()) return;
    {
        std::lock_guard lk(queue_mutex_);
        stop_requested_ = true;
    }
    queue_cv_.notify_one();
    if (writer_thread_.joinable())
        writer_thread_.join();
    running_ = false;
    PT_LOG_INFO("FrameRecorder stopped. saved={} skipped={}",
                frames_saved_.load(), frames_skipped_.load());
}

void FrameRecorder::submitFrame(const StereoFrame& frame, float angular_vel) {
    if (!running_.load(std::memory_order_relaxed)) return;
    if (!frame.valid()) return;

    ++frame_counter_;

    if (frame_counter_ % static_cast<uint64_t>(cfg_.capture_every_n) != 0) return;

    if (cfg_.max_frames > 0 && frames_saved_.load() >= cfg_.max_frames) {
        stop();
        return;
    }

    const bool motion = (angular_vel > cfg_.max_angular_velocity);
    if (motion) {
        ++frames_skipped_;
        return;
    }

    // Decode BC4 → R8 on the calling thread (fast, no allocations after first frame).
    PendingFrame pf;
    pf.sequence         = frame.sequence;
    pf.angular_velocity = angular_vel;
    pf.motion_flagged   = motion;
    pf.timestamp_ns     = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              frame.captured_at.time_since_epoch()).count();
    pf.left_gray  = decodeBC4(frame.left,  kCameraWidth, kCameraHeight);
    pf.right_gray = decodeBC4(frame.right, kCameraWidth, kCameraHeight);

    {
        std::lock_guard lk(queue_mutex_);
        queue_.push_back(std::move(pf));
    }
    queue_cv_.notify_one();
}

void FrameRecorder::writerLoop_() {
    while (true) {
        PendingFrame pf;
        {
            std::unique_lock lk(queue_mutex_);
            queue_cv_.wait(lk, [this] {
                return !queue_.empty() || stop_requested_.load();
            });
            if (queue_.empty()) break;
            pf = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        writeFrame_(pf);
    }
}

void FrameRecorder::writeSessionMeta_() {
    const auto path = cfg_.output_dir / "session_meta.json";
    std::ofstream f(path);

    auto write_intrinsics = [&](const CameraIntrinsics& in) {
        f << "    { \"fx\": " << in.fx << ", \"fy\": " << in.fy
          << ", \"cx\": " << in.cx << ", \"cy\": " << in.cy << " }";
    };

    auto write_params = [&](const CameraParameters& p) {
        f << "    [ ";
        for (int i = 0; i < 20; ++i) {
            f << p.coeffs[i];
            if (i < 19) f << ", ";
        }
        f << " ]";
    };

    f << "{\n"
      << "  \"camera_width\": "  << kCameraWidth  << ",\n"
      << "  \"camera_height\": " << kCameraHeight << ",\n"
      << "  \"intrinsics\": [\n";
    write_intrinsics(cfg_.intrinsics[0]); f << ",\n";
    write_intrinsics(cfg_.intrinsics[1]); f << "\n";
    f << "  ],\n"
      << "  \"distortion_coeffs\": [\n";
    write_params(cfg_.params[0]); f << ",\n";
    write_params(cfg_.params[1]); f << "\n";
    f << "  ]\n}\n";

    if (!f)
        PT_LOG_WARN("FrameRecorder: failed writing session_meta.json");
    else
        PT_LOG_INFO("FrameRecorder: wrote session_meta.json");
}

void FrameRecorder::writeFrame_(const PendingFrame& pf) {
    // File stem: <timestamp_ns>_<sequence>
    std::ostringstream stem;
    stem << pf.timestamp_ns << "_" << std::setw(8) << std::setfill('0') << pf.sequence;
    const std::string s = stem.str();

    auto write_png = [&](const std::string& eye_tag, const std::vector<uint8_t>& gray) {
        const auto path = (cfg_.output_dir / (s + "_" + eye_tag + ".png")).string();
        const int ok = stbi_write_png(path.c_str(), kCameraWidth, kCameraHeight,
                                      1, gray.data(), kCameraWidth);
        if (!ok)
            PT_LOG_WARN("FrameRecorder: stbi_write_png failed for '{}'", path);
    };

    write_png("L", pf.left_gray);
    write_png("R", pf.right_gray);

    // JSON metadata
    const auto json_path = cfg_.output_dir / (s + ".json");
    std::ofstream f(json_path);
    f << "{\n"
      << "  \"timestamp_ns\": " << pf.timestamp_ns << ",\n"
      << "  \"frame_index\": "  << pf.sequence     << ",\n"
      << "  \"hmd_angular_velocity\": " << pf.angular_velocity << ",\n"
      << "  \"motion_flagged\": " << (pf.motion_flagged ? "true" : "false") << "\n"
      << "}\n";
    if (!f)
        PT_LOG_WARN("FrameRecorder: failed writing JSON for seq {}", pf.sequence);

    ++frames_saved_;
    if (frames_saved_.load() % 50 == 0)
        PT_LOG_INFO("FrameRecorder: {} frames saved", frames_saved_.load());
}

}  // namespace psvr2pt
