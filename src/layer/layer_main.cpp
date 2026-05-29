#include "layer_dispatch.h"
#include "layer_session.h"
#include "logging.h"

#include <chrono>
#include <cstring>
#include <vector>

namespace psvr2pt {

// ===========================================================================
// Loader negotiation
// ===========================================================================
//
// The OpenXR loader calls xrNegotiateLoaderApiLayerInterface exactly once,
// before xrCreateInstance, to fetch our GetInstanceProcAddr and the special
// xrCreateApiLayerInstance entry point. This is the *only* symbol the layer
// DLL must export by name.

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                   const char* /*layerName*/,
                                   XrNegotiateApiLayerRequest* layerRequest) {
    init_logging();
    PT_LOG_INFO("xrNegotiateLoaderApiLayerInterface");

    if (!loaderInfo || !layerRequest) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize    != sizeof(XrNegotiateLoaderInfo)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (layerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        layerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        layerRequest->structSize    != sizeof(XrNegotiateApiLayerRequest)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    layerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    layerRequest->layerApiVersion       = XR_CURRENT_API_VERSION;
    layerRequest->getInstanceProcAddr   = pt_xrGetInstanceProcAddr;
    layerRequest->createApiLayerInstance = pt_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}

// ===========================================================================
// xrCreateApiLayerInstance — wire up the next-layer dispatch
// ===========================================================================

XrResult XRAPI_CALL pt_xrCreateApiLayerInstance(const XrInstanceCreateInfo* createInfo,
                                                const XrApiLayerCreateInfo* layerInfo,
                                                XrInstance* instance) {
    if (!layerInfo || !layerInfo->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;

    // Build a copy of the layer-info chain advanced by one link, so the next
    // layer sees the rest of the chain (not us).
    XrApiLayerCreateInfo nextLayerInfo = *layerInfo;
    nextLayerInfo.nextInfo = layerInfo->nextInfo->next;

    XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(
        createInfo, &nextLayerInfo, instance);
    if (XR_FAILED(r)) return r;

    auto* state = LayerState::get().add(*instance);
    state->instance = *instance;

    // Cache the next layer's xrGetInstanceProcAddr so we can resolve everything
    // through it.
    state->next.xrGetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;

    auto resolve = [&](const char* name, PFN_xrVoidFunction* out) {
        state->next.xrGetInstanceProcAddr(*instance, name, out);
    };

    resolve("xrDestroyInstance",         reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrDestroyInstance));
    resolve("xrCreateSession",           reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrCreateSession));
    resolve("xrDestroySession",          reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrDestroySession));
    resolve("xrBeginSession",            reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrBeginSession));
    resolve("xrEndSession",              reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrEndSession));
    resolve("xrEndFrame",                reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrEndFrame));
    resolve("xrLocateViews",             reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrLocateViews));
    resolve("xrWaitFrame",               reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrWaitFrame));
    resolve("xrBeginFrame",              reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrBeginFrame));
    resolve("xrCreateReferenceSpace",    reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrCreateReferenceSpace));
    resolve("xrDestroySpace",            reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrDestroySpace));
    resolve("xrLocateSpace",             reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrLocateSpace));

    resolve("xrCreateSwapchain",         reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrCreateSwapchain));
    resolve("xrDestroySwapchain",        reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrDestroySwapchain));
    resolve("xrEnumerateSwapchainImages",reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrEnumerateSwapchainImages));
    resolve("xrAcquireSwapchainImage",   reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrAcquireSwapchainImage));
    resolve("xrWaitSwapchainImage",      reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrWaitSwapchainImage));
    resolve("xrReleaseSwapchainImage",   reinterpret_cast<PFN_xrVoidFunction*>(&state->next.xrReleaseSwapchainImage));

    // Load config once. Changes apply on next process launch.
    state->config = load_config();

    PT_LOG_INFO("Layer instance created");
    return XR_SUCCESS;
}

// ===========================================================================
// xrGetInstanceProcAddr — intercept only the entry points we override
// ===========================================================================

XrResult XRAPI_CALL pt_xrGetInstanceProcAddr(XrInstance instance,
                                             const char* name,
                                             PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;

    auto* state = LayerState::get().find(instance);
    if (!state) {
        // Before xrCreateApiLayerInstance, only our negotiation function exists.
        if (std::strcmp(name, "xrCreateApiLayerInstance") == 0) {
            *function = reinterpret_cast<PFN_xrVoidFunction>(pt_xrCreateApiLayerInstance);
            return XR_SUCCESS;
        }
        return XR_ERROR_HANDLE_INVALID;
    }

    auto match = [&](const char* n, auto fn) -> bool {
        if (std::strcmp(name, n) != 0) return false;
        *function = reinterpret_cast<PFN_xrVoidFunction>(fn);
        return true;
    };

    if (match("xrGetInstanceProcAddr",     pt_xrGetInstanceProcAddr))   return XR_SUCCESS;
    if (match("xrDestroyInstance",         pt_xrDestroyInstance))       return XR_SUCCESS;
    if (match("xrCreateSession",           pt_xrCreateSession))         return XR_SUCCESS;
    if (match("xrDestroySession",          pt_xrDestroySession))        return XR_SUCCESS;
    if (match("xrBeginSession",            pt_xrBeginSession))          return XR_SUCCESS;
    if (match("xrWaitFrame",               pt_xrWaitFrame))             return XR_SUCCESS;
    if (match("xrEndFrame",                pt_xrEndFrame))              return XR_SUCCESS;

    // Otherwise pass through to the next layer.
    return state->next.xrGetInstanceProcAddr(instance, name, function);
}

XrResult XRAPI_CALL pt_xrDestroyInstance(XrInstance instance) {
    auto* state = LayerState::get().find(instance);
    XrResult r = XR_SUCCESS;
    if (state) {
        state->session.reset();
        r = state->next.xrDestroyInstance ? state->next.xrDestroyInstance(instance) : XR_SUCCESS;
        LayerState::get().remove(instance);
    }
    return r;
}

// ===========================================================================
// xrCreateSession — observe D3D11 binding and stand up our LayerSession
// ===========================================================================

XrResult XRAPI_CALL pt_xrCreateSession(XrInstance instance,
                                       const XrSessionCreateInfo* createInfo,
                                       XrSession* session) {
    auto* state = LayerState::get().find(instance);
    if (!state || !state->next.xrCreateSession) return XR_ERROR_HANDLE_INVALID;

    XrResult r = state->next.xrCreateSession(instance, createInfo, session);
    if (XR_FAILED(r)) return r;

    // Register the session handle before constructing LayerSession so that any
    // concurrent call from the app's input thread (e.g. DCS calling
    // xrAttachSessionActionSets while the render thread is still inside
    // xrCreateSession) can find the session via find_for_session().
    LayerState::get().register_session(*session, state);

    // Walk the next chain looking for a D3D11 binding. If the app is using
    // D3D12 / Vulkan / OpenGL we currently bail out (this could be extended
    // later by adding cross-API texture interop).
    ID3D11Device* device = nullptr;
    for (const XrBaseInStructure* n = static_cast<const XrBaseInStructure*>(createInfo->next);
         n != nullptr;
         n = n->next) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            device = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(n)->device;
            break;
        }
    }

    if (device) {
        state->session = std::make_unique<LayerSession>(*session, &state->next, device);
        // Apply the configuration we loaded at instance creation. Knobs are
        // captured once and used for the lifetime of this session.
        auto& cc = state->session->config();
        cc.global_alpha             = state->config.global_alpha;
        cc.brightness_enabled       = state->config.brightness_enabled;
        cc.brightness               = state->config.brightness;
        cc.contrast_enabled         = state->config.contrast_enabled;
        cc.contrast                 = state->config.contrast;
        cc.enhancements_enabled     = state->config.enhancements_enabled;
        cc.unsharp_amount           = state->config.unsharp_amount;
        cc.unsharp_radius           = state->config.unsharp_radius;
        cc.apply_undistortion       = state->config.apply_undistortion;
        cc.zoom_factor              = state->config.zoom_factor;
        cc.reprojection_enabled     = state->config.reprojection_enabled;
        state->session->set_camera_latency_offset_ns(state->config.camera_latency_offset_ns);
        state->session->set_debug_reproj_stats(state->config.debug_reprojection_stats);
        cc.camera_toe_out_rad_l     = state->config.camera_toe_out_rad_l;
        cc.camera_tilt_down_rad_l   = state->config.camera_tilt_down_rad_l;
        cc.camera_roll_rad_l        = state->config.camera_roll_rad_l;
        cc.camera_toe_out_rad_r     = state->config.camera_toe_out_rad_r;
        cc.camera_tilt_down_rad_r   = state->config.camera_tilt_down_rad_r;
        cc.camera_roll_rad_r        = state->config.camera_roll_rad_r;
        state->session->set_passthrough_binding(state->config.passthrough_binding);
        state->session->set_toggle_mode(state->config.toggle_mode);
        state->session->set_force_on(state->config.force_passthrough_on);
        state->session->set_ipd_correction(state->config.ipd_correction_enabled,
                                           state->config.camera_separation_mm);
        PT_LOG_INFO("D3D11 session adopted by layer");
    } else {
        PT_LOG_WARN("Non-D3D11 graphics binding detected; passthrough layer inert.");
    }
    return r;
}

XrResult XRAPI_CALL pt_xrDestroySession(XrSession session) {
    auto* state = LayerState::get().find_for_session(session);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    LayerState::get().unregister_session(session);
    state->session.reset();
    return state->next.xrDestroySession ? state->next.xrDestroySession(session) : XR_SUCCESS;
}

// ===========================================================================
// xrBeginSession — cache view config type for xrLocateViews calls.
// ===========================================================================

XrResult XRAPI_CALL pt_xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) {
    auto* state = LayerState::get().find_for_session(session);
    if (!state || !state->next.xrBeginSession) return XR_ERROR_HANDLE_INVALID;

    const XrResult r = state->next.xrBeginSession(session, beginInfo);
    if (XR_SUCCEEDED(r) && beginInfo) {
        state->view_config_type = beginInfo->primaryViewConfigurationType;
        if (state->session)
            state->session->set_view_config_type(beginInfo->primaryViewConfigurationType);
    }
    return r;
}

// ===========================================================================
// xrWaitFrame — update clock calibration offset for reprojection timestamps.
// ===========================================================================

XrResult XRAPI_CALL pt_xrWaitFrame(XrSession session,
                                    const XrFrameWaitInfo* frameWaitInfo,
                                    XrFrameState* frameState) {
    auto* state = LayerState::get().find_for_session(session);
    if (!state || !state->next.xrWaitFrame) return XR_ERROR_HANDLE_INVALID;

    const XrResult r = state->next.xrWaitFrame(session, frameWaitInfo, frameState);
    if (XR_SUCCEEDED(r) && frameState && state->session) {
        const int64_t steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        state->session->update_clock_offset(
            static_cast<int64_t>(frameState->predictedDisplayTime) - steady_ns);
    }
    return r;
}

// ===========================================================================
// xrEndFrame — the money shot. Compose our passthrough layer and inject it.
// ===========================================================================

XrResult XRAPI_CALL pt_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    auto* state = LayerState::get().find_for_session(session);
    if (!state || !state->next.xrEndFrame) return XR_ERROR_HANDLE_INVALID;

    if (!state->session || !state->config.enabled)
        return state->next.xrEndFrame(session, frameEndInfo);

    const XrCompositionLayerBaseHeader* extra = state->session->compose_layer(frameEndInfo);
    if (!extra)
        return state->next.xrEndFrame(session, frameEndInfo);

    // Append our layer above the game's projection so it composites on top.
    std::vector<const XrCompositionLayerBaseHeader*> layers(
        frameEndInfo->layers,
        frameEndInfo->layers + frameEndInfo->layerCount);
    layers.push_back(extra);

    XrFrameEndInfo fei = *frameEndInfo;
    fei.layerCount = static_cast<uint32_t>(layers.size());
    fei.layers     = layers.data();
    XrResult r = state->next.xrEndFrame(session, &fei);
    if (XR_FAILED(r))
        PT_LOG_WARN("xrEndFrame downstream failed: {}", static_cast<int>(r));
    return r;
}

}  // namespace psvr2pt
