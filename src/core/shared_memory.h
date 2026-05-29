#pragma once

// Shared-memory IPC interface to the SteamVR PSVR2 driver camera feed.

#include "distortion.h"
#include "frame.h"

#include <windows.h>
#include <stdexcept>
#include <string>

namespace psvr2pt {

struct SharedMemoryData {
    HANDLE   hMapFile         = nullptr;
    LPVOID   pBuf             = nullptr;
    HANDLE   hImageEvent      = nullptr;
    HANDLE   hImageMutex      = nullptr;
    char*    imageMemBase     = nullptr;
    uint32_t last_copied_ts   = 0;  // timestamp of last slot actually copied; skip if unchanged
};

// Throws std::runtime_error if the driver shared memory is unavailable.
void setup_shared_memory(SharedMemoryData& data);
void cleanup_shared_memory(SharedMemoryData& data);

// Inter-camera rigid-body transform as stored in the driver calibration region.
// mat is a row-major 3x3 rotation; pos is the translation vector.
// Units, sign convention, and reference points are unverified — inspect the
// calibration dump before drawing any conclusions from these values.
struct CameraExtrinsic {
    int   from_id = -1;
    int   to_id   = -1;
    float mat[9]  = {};
    float pos[3]  = {};
};

// Reads per-camera intrinsics + distortion coefficients out of the shared
// memory header region. cameraId is 0 (left) or 1 (right).
bool get_distortion_config(SharedMemoryData& data,
                           int cameraId,
                           CameraParameters& params,
                           CameraIntrinsics& intrinsics);

// Reads the three inter-camera extrinsic transforms that immediately follow
// the four CameraConfig entries in the calibration region.
// Returns false if shared memory is unavailable or the mutex times out.
bool get_camera_extrinsics(SharedMemoryData& data, CameraExtrinsic out[3]);

// Blocks (with a timeout) for a new frame, then copies both eye buffers into
// the supplied destinations. If out_pose is non-null, the SLAM-corrected head
// pose for the captured frame is written there.
bool copy_latest_image_buffer(SharedMemoryData& data,
                              void* leftCameraData,
                              void* rightCameraData,
                              size_t cameraDataSize,
                              DWORD timeout_ms = 50,
                              Pose3f* out_pose = nullptr);

// Non-blocking: reads the most recent available pose from the slot ring
// without waiting for a new image event. Returns false if no valid slot found.
bool read_latest_pose(SharedMemoryData& data, Pose3f& out_pose);

}  // namespace psvr2pt
