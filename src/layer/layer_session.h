#pragma once

#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
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

// Which graphics path the host game uses. D3D11Native is the original,
// zero-overhead path. D3D11On12 means the game is D3D12: we composite with the
// same D3D11 Compositor but on a D3D11-on-12 interop device, and bridge into the
// game's D3D12 swapchain images via wrapped resources.
enum class GraphicsMode { D3D11Native, D3D11On12 };

class LayerSession {
public:
    // D3D11 native host game. `device` is the game's ID3D11Device.
    LayerSession(XrSession xr_session,
                 InstanceDispatch* dispatch,
                 ID3D11Device* device);

    // D3D12 host game. `interop_device` is the ID3D11Device produced by
    // D3D11On12CreateDevice on the game's ID3D12Device/queue; `on12` is its
    // ID3D11On12Device interface. The Compositor still sees a plain
    // ID3D11Device, so it is unchanged. ComPtr refs to the game's
    // ID3D12Device/queue are held to keep them alive under our interop device.
    LayerSession(XrSession xr_session,
                 InstanceDispatch* dispatch,
                 ID3D11Device* interop_device,
                 ID3D11On12Device* on12,
                 ID3D12Device* game_device,
                 ID3D12CommandQueue* game_queue);
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
    void common_init_();   // shared ctor body for both graphics modes
    bool ensure_swapchain_(uint32_t width, uint32_t height);

    XrSession         session_{XR_NULL_HANDLE};
    InstanceDispatch* dispatch_{nullptr};

    GraphicsMode mode_ = GraphicsMode::D3D11Native;

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;

    // D3D12 interop (only populated when mode_ == D3D11On12). device_/ctx_ above
    // are then the interop D3D11 device/context returned by D3D11On12CreateDevice.
    Microsoft::WRL::ComPtr<ID3D11On12Device>    on12_;
    Microsoft::WRL::ComPtr<ID3D12Device>        game_d3d12_device_;  // app-owned; held to keep alive
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>  game_d3d12_queue_;   // app-owned; held to keep alive

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
        // Image arrays are mode-specific; only one is populated per session.
        std::vector<XrSwapchainImageD3D11KHR> images;       // D3D11Native
        std::vector<XrSwapchainImageD3D12KHR> images12;     // D3D11On12
        // Per-image D3D11 wrapped views over the D3D12 swapchain textures,
        // created once (the swapchain image ring is fixed) and reused. Parallel
        // to images12 by index. D3D11On12 only.
        std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> wrapped;
        uint32_t                              width  = 0;
        uint32_t                              height = 0;
    };
    std::array<EyeSwapchain, 2> swapchains_;

    // Per-eye acquired swapchain image index for the current frame (D3D11On12
    // path, where acquire and copy are split across the batch sequence).
    std::array<uint32_t, 2> eye_image_idx_{0, 0};

    // Negotiated swapchain format (chosen via xrEnumerateSwapchainFormats in
    // D3D11On12 mode; the native path keeps its historical hard-coded format).
    DXGI_FORMAT swapchain_format_ = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    // Picks an R8G8B8A8-family swapchain format the runtime supports, so the
    // compositor's R8G8B8A8_UNORM output stays CopyResource-compatible. Returns
    // false (and the session goes inert) if none is offered. D3D11On12 only.
    bool negotiate_swapchain_format_();

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
