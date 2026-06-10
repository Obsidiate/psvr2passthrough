#pragma once

#include "frame.h"
#include "distortion.h"
#include "shared_memory.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>

namespace psvr2pt {

// Owns the producer thread that reads camera frames from the PSVR2 driver
// shared memory. Consumers call `try_get_latest()` to grab the most recent
// stereo frame without blocking.
class CameraSource {
public:
    CameraSource();
    ~CameraSource();

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    // Returns false if the driver shared memory is unavailable. In that case
    // the layer falls through and behaves as a no-op.
    bool start();
    void stop();

    // Non-blocking. Returns false if no frame has been received yet.
    bool try_get_latest(StereoFrame& out);

    // Blocks until a new frame is available or the timeout elapses. Returns true
    // if a new frame is ready (the caller should then call try_get_latest).
    // Lets a consumer with no external pacing (e.g. the OpenVR overlay) wake at
    // camera rate instead of spinning. The OpenXR layer doesn't use this — it is
    // paced by the host game's frame loop.
    bool wait_for_frame(unsigned timeout_ms);

    // Non-blocking. Reads the most recent driver pose from shared memory.
    // Safe to call from any thread while the source is running.
    bool get_latest_pose(Pose3f& out) const;

    // Calibration is fetched once at start; safe to call any time afterwards.
    const CameraIntrinsics& intrinsics(CameraId id) const;
    const CameraParameters& params(CameraId id)     const;

    bool is_running() const { return running_.load(); }

private:
    void thread_loop();

    std::atomic<bool> running_{false};
    std::thread       worker_;

    // Double-buffered most-recent frame.
    StereoFrame       front_;
    std::mutex        front_mutex_;
    std::atomic<bool> have_frame_{false};
    std::condition_variable frame_cv_;   // notified by producer on each new frame
    uint64_t          last_seq_ = 0;

    CameraIntrinsics  intrinsics_[2]{};
    CameraParameters  params_[2]{};

    struct Impl;            // pImpl to hide Windows headers from public users
    std::unique_ptr<Impl> impl_;
};

}  // namespace psvr2pt
