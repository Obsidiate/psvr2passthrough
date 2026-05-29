#pragma once

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "camera_source.h"
#include "compositor.h"
#include "input_binding.h"

#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <vector>
#include <array>
#include <memory>

namespace psvr2pt {

struct InstanceDispatch;

class LayerSession {
public:
    LayerSession(XrSession xr_session,
                 InstanceDispatch* dispatch,
                 ID3D11Device* device);
    ~LayerSession();

    const XrCompositionLayerBaseHeader* compose_layer(const XrFrameEndInfo* original);

    XrSession handle() const { return session_; }
    XrSpace   passthrough_space() const { return passthrough_space_; }

    CompositorConfig& config() { return config_; }

    // Called by layer_main when the disk config is applied at session startup.
    void set_passthrough_binding(const PassthroughBinding& b) {
        poller_.set_binding(b);
        binding_is_none_ = b.is_none();
    }
    void set_toggle_mode(bool t) { toggle_mode_ = t; }
    void set_force_on(bool f)    { force_on_ = f; }
    void update_clock_offset(int64_t ns)                 { clock_offset_ns_.store(ns, std::memory_order_relaxed); }
    void set_view_config_type(XrViewConfigurationType t) { view_config_type_ = t; }
    void set_camera_latency_offset_ns(int64_t ns)        { camera_latency_offset_ns_ = ns; }
    void set_debug_reproj_stats(bool v)                  { debug_reproj_stats_ = v; }
    void set_ipd_correction(bool enabled, float camera_sep_mm) {
        ipd_correction_enabled_ = enabled;
        camera_separation_m_    = camera_sep_mm / 1000.f;
    }

private:
    bool ensure_swapchain_(uint32_t width, uint32_t height);

    XrSession         session_{XR_NULL_HANDLE};
    InstanceDispatch* dispatch_{nullptr};

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;

    std::unique_ptr<CameraSource>  camera_;
    std::unique_ptr<Compositor>    compositor_;
    StereoFrame cached_frame_;   // persists between frames so try_get_latest swap recycles buffers

    // Passthrough visibility state.
    BindingPoller poller_;
    bool binding_is_none_    = true;   // true when no button binding is configured
    bool toggle_mode_        = false;  // false = hold, true = toggle
    bool force_on_           = false;  // debug: bypass binding, always show passthrough
    bool passthrough_visible_= false;  // starts hidden; binding or force_on makes it visible
    bool prev_button_state_  = false;

    struct EyeSwapchain {
        XrSwapchain                           handle{XR_NULL_HANDLE};
        std::vector<XrSwapchainImageD3D11KHR> images;
        uint32_t                              width  = 0;
        uint32_t                              height = 0;
    };
    std::array<EyeSwapchain, 2> swapchains_;

    XrSpace passthrough_space_{XR_NULL_HANDLE};

    XrCompositionLayerProjection                   composition_layer_{};
    std::array<XrCompositionLayerProjectionView, 2> projection_views_{};

    CompositorConfig config_{};

    // OpenXR eye poses captured at the moment each new camera frame arrives.
    // Submitted as the layer pose so ATW corrects from capture-time orientation
    // to display-time orientation - entirely in OpenXR space.
    std::array<XrPosef, 2> captured_eye_pose_{};
    bool has_captured_eye_pose_ = false;

    // Clock calibration: predictedDisplayTime - steady_clock_ns, updated each xrWaitFrame.
    // Includes runtime display prediction window bias (~8ms typical); absorbed into
    // camera_latency_offset_ns_ during empirical tuning.
    std::atomic<int64_t>    clock_offset_ns_{0};
    XrViewConfigurationType view_config_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    int64_t                 camera_latency_offset_ns_{16'000'000};
    bool                    debug_reproj_stats_{false};

    // IPD correction state.
    bool  ipd_correction_enabled_ = false;
    float camera_separation_m_    = 0.080f;
    float last_ipd_m_             = -1.f;   // sentinel — forces update on first frame

    // Reprojection probe and 1Hz stats state.
    bool    reproj_probe_logged_     = false;
    int64_t reproj_invalid_total_    = 0;
    bool    reproj_stat_initialized_ = false;
    int64_t reproj_stat_count_       = 0;
    int64_t reproj_stat_invalid_     = 0;
    double  reproj_stat_delta_sum_   = 0.0;
    double  reproj_stat_delta_max_   = 0.0;
    std::chrono::steady_clock::time_point reproj_stat_epoch_{};

    bool ready_ = false;
};

}  // namespace psvr2pt
