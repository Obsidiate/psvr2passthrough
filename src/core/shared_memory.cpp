#include "shared_memory.h"
#include "logging.h"

#include <cmath>
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

// Extracts the SLAM-corrected head pose from a slot info block.
// Layout (starting at kSlotStatusOff[i], read as float array):
//   float[3..5]  = base position (x, y, z)
//   float[6..9]  = base quaternion (x, y, z, w)
//   float[10..12]= SLAM correction position offset (in local camera space)
//   float[13..16]= SLAM correction quaternion (x, y, z, w)
static Pose3f extract_pose(const char* slot_base) {
    const float* fp = reinterpret_cast<const float*>(slot_base);

    const float bpx = fp[3], bpy = fp[4], bpz = fp[5];
    const float bqx = fp[6], bqy = fp[7], bqz = fp[8], bqw = fp[9];

    const float dx = fp[10], dy = fp[11], dz = fp[12];
    const float dqx = fp[13], dqy = fp[14], dqz = fp[15], dqw = fp[16];

    // Rotate SLAM offset position by base quaternion: v' = q*v*q_inv
    const float tx = 2.0f * (bqy * dz - bqz * dy);
    const float ty = 2.0f * (bqz * dx - bqx * dz);
    const float tz = 2.0f * (bqx * dy - bqy * dx);

    Pose3f out;
    out.px = bpx + dx + bqw * tx + bqy * tz - bqz * ty;
    out.py = bpy + dy + bqw * ty + bqz * tx - bqx * tz;
    out.pz = bpz + dz + bqw * tz + bqx * ty - bqy * tx;

    // Final quaternion: q_final = q_base * q_slam (Hamilton product)
    out.qx = bqw*dqx + bqx*dqw + bqy*dqz - bqz*dqy;
    out.qy = bqw*dqy - bqx*dqz + bqy*dqw + bqz*dqx;
    out.qz = bqw*dqz + bqx*dqy - bqy*dqx + bqz*dqw;
    out.qw = bqw*dqw - bqx*dqx - bqy*dqy - bqz*dqz;

    const float len = std::sqrt(out.qx*out.qx + out.qy*out.qy +
                                out.qz*out.qz + out.qw*out.qw);
    if (len > 1e-6f) {
        out.qx /= len; out.qy /= len; out.qz /= len; out.qw /= len;
    }
    out.valid = true;
    return out;
}

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
            PT_LOG_INFO("camera {} calibration: widthPx={} heightPx={} (expect 1016=tight-stride 520192, 1024=padded-stride 524288)",
                        cameraId, configs[i].widthPx, configs[i].heightPx);
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

bool get_camera_extrinsics(SharedMemoryData& data, CameraExtrinsic out[3]) {
    if (!data.imageMemBase) return false;

    HANDLE hCalib = OpenMutexA(SYNCHRONIZE, FALSE, names::kCalibMutex);
    if (!hCalib) return false;

    if (WaitForSingleObject(hCalib, 5000) != WAIT_OBJECT_0) {
        CloseHandle(hCalib);
        return false;
    }

#pragma pack(push, 1)
    struct RawTransform {
        uint8_t from_id;
        uint8_t to_id;
        uint8_t pad[2];
        float   mat[9];
        float   pos[3];
    };
#pragma pack(pop)

    const size_t transforms_offset =
        layout::kCalibBaseOff + 4 * sizeof(CameraConfig);

    const auto* raw = reinterpret_cast<const RawTransform*>(
        data.imageMemBase + transforms_offset);

    for (int i = 0; i < 3; ++i) {
        out[i].from_id = static_cast<int>(raw[i].from_id);
        out[i].to_id   = static_cast<int>(raw[i].to_id);
        for (int j = 0; j < 9; ++j) out[i].mat[j] = raw[i].mat[j];
        for (int j = 0; j < 3; ++j) out[i].pos[j] = raw[i].pos[j];
    }

    ReleaseMutex(hCalib);
    CloseHandle(hCalib);
    return true;
}

bool copy_latest_image_buffer(SharedMemoryData& data,
                              void* leftCameraData,
                              void* rightCameraData,
                              size_t cameraDataSize,
                              DWORD timeout_ms,
                              Pose3f* out_pose) {
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

    // Skip if this is the same frame we already copied - the image event is a
    // manual-reset that stays permanently signaled, so without this check the
    // thread would spin re-reading stale data at ~50,000 iterations/second.
    if (latestTimestamp == data.last_copied_ts) {
        ReleaseMutex(data.hImageMutex);
        return false;
    }
    data.last_copied_ts = latestTimestamp;

    if (out_pose)
        *out_pose = extract_pose(data.imageMemBase + layout::kSlotStatusOff[latestIndex]);

    const char* slot = data.imageMemBase
                     + layout::kImageBufOffset
                     + latestIndex * layout::kPerSlotStride;

    std::memcpy(leftCameraData,  slot,                  cameraDataSize);
    std::memcpy(rightCameraData, slot + cameraDataSize, cameraDataSize);

    ReleaseMutex(data.hImageMutex);
    return true;
}

bool read_latest_pose(SharedMemoryData& data, Pose3f& out_pose) {
    if (!data.imageMemBase) return false;

    uint32_t latestTimestamp = 0;
    int      latestIndex     = -1;

    for (size_t i = 0; i < layout::kNumSlots; ++i) {
        const int status = *reinterpret_cast<const int*>(
            data.imageMemBase + layout::kSlotStatusOff[i]);
        if (static_cast<unsigned>(status - 1) >= 2) continue;

        const uint32_t ts = *reinterpret_cast<const uint32_t*>(
            data.imageMemBase + layout::kSlotTimestampOff[i]);
        if (latestIndex < 0 || ts > latestTimestamp) {
            latestTimestamp = ts;
            latestIndex     = static_cast<int>(i);
        }
    }

    if (latestIndex < 0) return false;
    out_pose = extract_pose(data.imageMemBase + layout::kSlotStatusOff[latestIndex]);
    return true;
}

}  // namespace psvr2pt
