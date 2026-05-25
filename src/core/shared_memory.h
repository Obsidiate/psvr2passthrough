#pragma once

// Shared-memory IPC interface to the SteamVR PSVR2 driver camera feed.

#include "distortion.h"

#include <windows.h>
#include <stdexcept>
#include <string>

namespace psvr2pt {

struct SharedMemoryData {
    HANDLE hMapFile      = nullptr;
    LPVOID pBuf          = nullptr;
    HANDLE hImageEvent   = nullptr;
    HANDLE hImageMutex   = nullptr;
    char*  imageMemBase  = nullptr;
};

// Throws std::runtime_error if the driver shared memory is unavailable.
void setup_shared_memory(SharedMemoryData& data);
void cleanup_shared_memory(SharedMemoryData& data);

// Reads per-camera intrinsics + distortion coefficients out of the shared
// memory header region. cameraId is 0 (left) or 1 (right).
bool get_distortion_config(SharedMemoryData& data,
                           int cameraId,
                           CameraParameters& params,
                           CameraIntrinsics& intrinsics);

// Blocks (with a timeout) for a new frame, then copies both eye buffers into
// the supplied destinations. cameraDataSize must equal kCameraWidth * kCameraHeight.
bool copy_latest_image_buffer(SharedMemoryData& data,
                              void* leftCameraData,
                              void* rightCameraData,
                              size_t cameraDataSize,
                              DWORD timeout_ms = 50);

}  // namespace psvr2pt
