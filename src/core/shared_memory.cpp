#include "shared_memory.h"
#include "frame.h"
#include "logging.h"

#include <cstring>

namespace psvr2pt {

// IPC object names created by the SteamVR PSVR2 driver.
namespace names {
    constexpr const char* kFileMapping = "SHARE_VRT2_WIN";
    constexpr const char* kImageEvent  = "SHARE_VRT2_WIN_IMAGE_EVT";
    constexpr const char* kImageMutex  = "SHARE_VRT2_WIN_IMAGE_MTX";
    constexpr const char* kCalibMutex  = "SHARE_VRT2_WIN_CALIB_MTX";
}

// Shared memory layout — reverse-engineered from the PSVR2 driver ABI.
namespace layout {
    constexpr DWORD  kTotalSize        = 0x2000000;       // 32 MB
    constexpr DWORD  kImageBufOffset   = 0x10ba00 + 256;  // image ring buffer base
    constexpr size_t kPerSlotStride    = 0x200100;        // bytes between slot N and N+1
    constexpr size_t kNumSlots         = 8;

    // Slot metadata — status word (int, must be 1 or 2) and timestamp (uint32_t).
    constexpr size_t kSlotStatusOff[8]    = { 0x3c10, 0x4488, 0x4d00, 0x5578,
                                               0x5df0, 0x6668, 0x6ee0, 0x7758 };
    constexpr size_t kSlotTimestampOff[8] = { 0x3c18, 0x4490, 0x4d08, 0x5580,
                                               0x5df8, 0x6670, 0x6ee8, 0x7760 };

    // Calibration: array of up to 4 CameraConfig structs starting here.
    constexpr size_t kCalibBaseOff = 0x524;
}

// Per-camera calibration record inside the shared memory region.
// Packed to match the driver's ABI exactly.
#pragma pack(push, 1)
struct CameraConfig {
    uint32_t camId;
    uint16_t widthPx;
    uint16_t heightPx;
    float    pxMat[9];   // row-major 3x3: [0]=fx, [2]=cx, [4]=fy, [5]=cy
    double   coff[20];   // distortion coefficients
    uint32_t zeros[6];
};
#pragma pack(pop)

void setup_shared_memory(SharedMemoryData& data) {
    data.hMapFile = OpenFileMappingA(FILE_MAP_READ, FALSE, names::kFileMapping);
    if (!data.hMapFile) {
        throw std::runtime_error(
            "Could not open PSVR2 camera shared memory (SHARE_VRT2_WIN). "
            "Is SteamVR running with the PSVR2 headset connected?");
    }

    data.pBuf = MapViewOfFile(data.hMapFile, FILE_MAP_READ, 0, 0, layout::kTotalSize);
    if (!data.pBuf) {
        CloseHandle(data.hMapFile);
        data.hMapFile = nullptr;
        throw std::runtime_error("MapViewOfFile failed for PSVR2 shared memory.");
    }
    data.imageMemBase = static_cast<char*>(data.pBuf);

    data.hImageEvent = OpenEventA(SYNCHRONIZE, FALSE, names::kImageEvent);
    if (!data.hImageEvent) {
        cleanup_shared_memory(data);
        throw std::runtime_error(
            "Could not open PSVR2 camera image event (SHARE_VRT2_WIN_IMAGE_EVT).");
    }

    data.hImageMutex = OpenMutexA(SYNCHRONIZE, FALSE, names::kImageMutex);
    if (!data.hImageMutex) {
        cleanup_shared_memory(data);
        throw std::runtime_error(
            "Could not open PSVR2 camera image mutex (SHARE_VRT2_WIN_IMAGE_MTX).");
    }

    PT_LOG_INFO("PSVR2 shared memory attached: base={}", static_cast<void*>(data.imageMemBase));
}

void cleanup_shared_memory(SharedMemoryData& data) {
    if (data.hImageMutex) { CloseHandle(data.hImageMutex); data.hImageMutex = nullptr; }
    if (data.hImageEvent) { CloseHandle(data.hImageEvent); data.hImageEvent = nullptr; }
    if (data.pBuf)        { UnmapViewOfFile(data.pBuf);    data.pBuf        = nullptr; }
    if (data.hMapFile)    { CloseHandle(data.hMapFile);    data.hMapFile    = nullptr; }
    data.imageMemBase = nullptr;
}

bool get_distortion_config(SharedMemoryData& data,
                           int cameraId,
                           CameraParameters& params,
                           CameraIntrinsics& intrinsics) {
    if (!data.imageMemBase) return false;

    HANDLE hCalib = OpenMutexA(SYNCHRONIZE, FALSE, names::kCalibMutex);
    if (!hCalib) return false;

    if (WaitForSingleObject(hCalib, 5000) != WAIT_OBJECT_0) {
        CloseHandle(hCalib);
        return false;
    }

    const auto* configs = reinterpret_cast<const CameraConfig*>(
        data.imageMemBase + layout::kCalibBaseOff);

    bool found = false;
    for (int i = 0; i < 4; ++i) {
        if (static_cast<int>(configs[i].camId) == cameraId) {
            intrinsics.fx = configs[i].pxMat[0];
            intrinsics.fy = configs[i].pxMat[4];
            intrinsics.cx = configs[i].pxMat[2];
            intrinsics.cy = configs[i].pxMat[5];
            for (int j = 0; j < 20; ++j)
                params.coeffs[j] = configs[i].coff[j];
            found = true;
            break;
        }
    }

    ReleaseMutex(hCalib);
    CloseHandle(hCalib);
    return found;
}

bool copy_latest_image_buffer(SharedMemoryData& data,
                              void* leftCameraData,
                              void* rightCameraData,
                              size_t cameraDataSize,
                              DWORD timeout_ms) {
    if (!data.imageMemBase || !data.hImageEvent || !data.hImageMutex) return false;

    if (WaitForSingleObject(data.hImageEvent, timeout_ms) != WAIT_OBJECT_0) return false;
    if (WaitForSingleObject(data.hImageMutex, 50) != WAIT_OBJECT_0) return false;

    // Scan all 8 slots; pick the one with the highest valid timestamp.
    uint32_t latestTimestamp = 0;
    uint32_t latestIndex     = 0;
    bool     anyValid        = false;

    for (size_t i = 0; i < layout::kNumSlots; ++i) {
        const int status = *reinterpret_cast<const int*>(
            data.imageMemBase + layout::kSlotStatusOff[i]);
        if (static_cast<unsigned>(status - 1) >= 2) continue;  // valid only if 1 or 2

        const uint32_t ts = *reinterpret_cast<const uint32_t*>(
            data.imageMemBase + layout::kSlotTimestampOff[i]);
        if (!anyValid || ts > latestTimestamp) {
            latestTimestamp = ts;
            latestIndex     = static_cast<uint32_t>(i);
            anyValid        = true;
        }
    }

    if (!anyValid) {
        ReleaseMutex(data.hImageMutex);
        return false;
    }

    const char* slot = data.imageMemBase
                     + layout::kImageBufOffset
                     + latestIndex * layout::kPerSlotStride;

    std::memcpy(leftCameraData,  slot,                  cameraDataSize);
    std::memcpy(rightCameraData, slot + cameraDataSize, cameraDataSize);

    ReleaseMutex(data.hImageMutex);
    return true;
}

}  // namespace psvr2pt
