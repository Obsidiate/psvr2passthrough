#include "fov.h"

#include <cmath>

namespace psvr2pt {

EyeFov fov_from_intrinsics(const CameraIntrinsics& intr,
                           int image_width, int image_height,
                           float zoom) {
    const float W_f = static_cast<float>(image_width);
    const float H_f = static_cast<float>(image_height);

    EyeFov fov{};
    fov.angle_left  = -std::atan(static_cast<float>(intr.cx)        * zoom / static_cast<float>(intr.fx));
    fov.angle_right =  std::atan((W_f - static_cast<float>(intr.cx)) * zoom / static_cast<float>(intr.fx));
    fov.angle_up    =  std::atan(static_cast<float>(intr.cy)        * zoom / static_cast<float>(intr.fy));
    fov.angle_down  = -std::atan((H_f - static_cast<float>(intr.cy)) * zoom / static_cast<float>(intr.fy));
    return fov;
}

void ipd_toe_deltas(float ipd_m, float camera_separation_m,
                    float& out_delta_l, float& out_delta_r) {
    // Lateral offset: positive = camera is further outward than eye.
    const float offset = camera_separation_m * 0.5f - ipd_m * 0.5f;
    // Angular equivalent at nominal depth 0.7 m (hand-to-shoulder reach) —
    // prioritises near-field interactions over distant objects.
    const float delta  = std::atan2(offset, 0.7f);
    out_delta_l = -delta;
    out_delta_r =  delta;
}

}  // namespace psvr2pt
