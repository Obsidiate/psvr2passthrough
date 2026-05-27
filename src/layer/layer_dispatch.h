#pragma once

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "config.h"

#include <mutex>
#include <unordered_map>
#include <memory>

namespace psvr2pt {

class LayerSession;       // forward

// Per-instance dispatch table — holds pointers to the *next* layer's
// implementations of each function we intercept. We forward to these.
struct InstanceDispatch {
    // Lifecycle.
    PFN_xrGetInstanceProcAddr     xrGetInstanceProcAddr     = nullptr;
    PFN_xrDestroyInstance         xrDestroyInstance         = nullptr;

    // Sessions / frames — the bits we care about.
    PFN_xrCreateSession           xrCreateSession           = nullptr;
    PFN_xrDestroySession          xrDestroySession          = nullptr;
    PFN_xrBeginSession            xrBeginSession            = nullptr;
    PFN_xrEndSession              xrEndSession              = nullptr;
    PFN_xrEndFrame                xrEndFrame                = nullptr;
    PFN_xrLocateViews             xrLocateViews             = nullptr;
    PFN_xrWaitFrame               xrWaitFrame               = nullptr;
    PFN_xrBeginFrame              xrBeginFrame              = nullptr;
    PFN_xrCreateReferenceSpace    xrCreateReferenceSpace    = nullptr;
    PFN_xrDestroySpace            xrDestroySpace            = nullptr;
    PFN_xrLocateSpace             xrLocateSpace             = nullptr;

    // Swapchains (we need to create our own for the passthrough overlay).
    PFN_xrCreateSwapchain         xrCreateSwapchain         = nullptr;
    PFN_xrDestroySwapchain        xrDestroySwapchain        = nullptr;
    PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage   xrAcquireSwapchainImage   = nullptr;
    PFN_xrWaitSwapchainImage      xrWaitSwapchainImage      = nullptr;
    PFN_xrReleaseSwapchainImage   xrReleaseSwapchainImage   = nullptr;

};

struct InstanceState {
    XrInstance                       instance{XR_NULL_HANDLE};
    InstanceDispatch                 next{};
    std::unique_ptr<LayerSession>    session;            // 1 session per instance in practice
    Config                           config{};           // loaded once at instance creation
    XrViewConfigurationType          view_config_type{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
};

// Singleton-ish global instance table. OpenXR allows multiple instances per
// process in theory; in practice for games it's always one.
class LayerState {
public:
    static LayerState& get();

    InstanceState* add(XrInstance instance);
    InstanceState* find(XrInstance instance);
    void           remove(XrInstance instance);

    InstanceState* find_for_session(XrSession session);
    void           register_session(XrSession session, InstanceState* inst);
    void           unregister_session(XrSession session);

private:
    std::mutex mu_;
    std::unordered_map<XrInstance, std::unique_ptr<InstanceState>> instances_;
    std::unordered_map<XrSession, InstanceState*> sessions_;
};

// The single entry point exported by the layer. Called by the OpenXR loader
// during xrCreateInstance to wire up our chain link.
extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                   const char* layerName,
                                   XrNegotiateApiLayerRequest* layerRequest);

// Our overrides of the OpenXR entry points.
XrResult XRAPI_CALL pt_xrGetInstanceProcAddr(XrInstance instance,
                                             const char* name,
                                             PFN_xrVoidFunction* function);
XrResult XRAPI_CALL pt_xrCreateApiLayerInstance(const XrInstanceCreateInfo* createInfo,
                                                const XrApiLayerCreateInfo* layerInfo,
                                                XrInstance* instance);
XrResult XRAPI_CALL pt_xrDestroyInstance(XrInstance instance);

XrResult XRAPI_CALL pt_xrCreateSession(XrInstance instance,
                                       const XrSessionCreateInfo* createInfo,
                                       XrSession* session);
XrResult XRAPI_CALL pt_xrDestroySession(XrSession session);
XrResult XRAPI_CALL pt_xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo);
XrResult XRAPI_CALL pt_xrWaitFrame(XrSession session,
                                    const XrFrameWaitInfo* frameWaitInfo,
                                    XrFrameState* frameState);
XrResult XRAPI_CALL pt_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo);

}  // namespace psvr2pt
