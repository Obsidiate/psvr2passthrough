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

private:
    bool ensure_swapchain_(uint32_t width, uint32_t height);

    XrSession         session_{XR_NULL_HANDLE};
    InstanceDispatch* dispatch_{nullptr};

    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;

    std::unique_ptr<CameraSource>  camera_;
    std::unique_ptr<Compositor>    compositor_;

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

    bool ready_ = false;
};

}  // namespace psvr2pt
