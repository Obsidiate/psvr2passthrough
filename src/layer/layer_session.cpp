#include "layer_session.h"
#include "layer_dispatch.h"
#include "logging.h"

#include <cmath>

namespace psvr2pt {

LayerSession::LayerSession(XrSession xr_session,
                           InstanceDispatch* dispatch,
                           ID3D11Device* device)
    : session_(xr_session)
    , dispatch_(dispatch)
    , device_(device)
{
    device_->GetImmediateContext(&ctx_);

    camera_ = std::make_unique<CameraSource>();
    if (!camera_->start())
        PT_LOG_WARN("LayerSession: camera unavailable; layer will pass through inert.");

    XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rsci.poseInReferenceSpace = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
    if (dispatch_->xrCreateReferenceSpace) {
        XrResult r = dispatch_->xrCreateReferenceSpace(session_, &rsci, &passthrough_space_);
        if (XR_FAILED(r)) {
            PT_LOG_ERROR("xrCreateReferenceSpace(VIEW) failed: {}", static_cast<int>(r));
            passthrough_space_ = XR_NULL_HANDLE;
        }
    }

    compositor_ = std::make_unique<Compositor>();

    ready_ = (passthrough_space_ != XR_NULL_HANDLE);
    PT_LOG_INFO("LayerSession constructed (ready={})", ready_);
}

LayerSession::~LayerSession() {
    if (dispatch_) {
        for (auto& sc : swapchains_) {
            if (sc.handle != XR_NULL_HANDLE && dispatch_->xrDestroySwapchain)
                dispatch_->xrDestroySwapchain(sc.handle);
        }
        if (passthrough_space_ != XR_NULL_HANDLE && dispatch_->xrDestroySpace)
            dispatch_->xrDestroySpace(passthrough_space_);
    }
}

bool LayerSession::ensure_swapchain_(uint32_t width, uint32_t height) {
    if (swapchains_[0].handle != XR_NULL_HANDLE &&
        swapchains_[0].width == width && swapchains_[0].height == height)
        return true;
    if (!dispatch_->xrCreateSwapchain || !dispatch_->xrEnumerateSwapchainImages) return false;

    for (int eye = 0; eye < 2; ++eye) {
        if (swapchains_[eye].handle != XR_NULL_HANDLE && dispatch_->xrDestroySwapchain) {
            dispatch_->xrDestroySwapchain(swapchains_[eye].handle);
            swapchains_[eye].handle = XR_NULL_HANDLE;
        }

        XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                        | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format      = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        sci.sampleCount = 1;
        sci.width       = width;
        sci.height      = height;
        sci.faceCount   = 1;
        sci.arraySize   = 1;
        sci.mipCount    = 1;
        XrResult r = dispatch_->xrCreateSwapchain(session_, &sci, &swapchains_[eye].handle);
        if (XR_FAILED(r)) {
            PT_LOG_ERROR("xrCreateSwapchain failed for eye {}: {}", eye, static_cast<int>(r));
            return false;
        }

        uint32_t count = 0;
        dispatch_->xrEnumerateSwapchainImages(swapchains_[eye].handle, 0, &count, nullptr);
        swapchains_[eye].images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        dispatch_->xrEnumerateSwapchainImages(
            swapchains_[eye].handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchains_[eye].images.data()));
        swapchains_[eye].width  = width;
        swapchains_[eye].height = height;
    }

    CameraIntrinsics in[2] = { camera_->intrinsics(CameraId::Left),
                                camera_->intrinsics(CameraId::Right) };
    CameraParameters pa[2] = { camera_->params(CameraId::Left),
                                camera_->params(CameraId::Right) };
    if (!compositor_->initialise(device_.Get(), width, height, in, pa)) {
        PT_LOG_ERROR("Compositor failed to initialise");
        return false;
    }

    return true;
}


const XrCompositionLayerBaseHeader*
LayerSession::compose_layer(const XrFrameEndInfo* original) {
    if (!ready_ || !camera_ || !camera_->is_running()) return nullptr;
    if (!original || original->layerCount == 0) return nullptr;

    // --- Passthrough visibility logic ---
    if (force_on_) {
        passthrough_visible_ = true;
    } else {
        const bool cur_pressed = poller_.poll();
        if (toggle_mode_) {
            if (cur_pressed && !prev_button_state_)
                passthrough_visible_ = !passthrough_visible_;
        } else {
            passthrough_visible_ = cur_pressed;
        }
        prev_button_state_ = cur_pressed;
    }

    if (!passthrough_visible_) return nullptr;

    static uint64_t frame_n = 0;
    if (++frame_n % 300 == 0) PT_LOG_INFO("compose_layer frame {}", frame_n);

    const XrCompositionLayerProjection* game_proj = nullptr;
    for (uint32_t i = 0; i < original->layerCount; ++i) {
        if (original->layers[i] &&
            original->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            game_proj = reinterpret_cast<const XrCompositionLayerProjection*>(original->layers[i]);
            break;
        }
    }
    if (!game_proj || game_proj->viewCount < 2) return nullptr;
    if (game_proj->viewCount != 2 && game_proj->viewCount != 4) return nullptr;

    const uint32_t w = static_cast<uint32_t>(kCameraWidth);
    const uint32_t h = static_cast<uint32_t>(kCameraHeight);
    if (!ensure_swapchain_(w, h)) return nullptr;

    // try_get_latest swaps new data into cached_frame_, donating its old buffers
    // back to the producer for recycling. If no new frame arrived this tick we
    // reuse the last valid one rather than dropping passthrough entirely.
    camera_->try_get_latest(cached_frame_);
    if (!cached_frame_.valid()) return nullptr;

    compositor_->upload_frame(cached_frame_);
    compositor_->render(config_);

    for (uint32_t eye = 0; eye < 2; ++eye) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(dispatch_->xrAcquireSwapchainImage(swapchains_[eye].handle, &ai, &idx))) {
            PT_LOG_WARN("xrAcquireSwapchainImage failed eye={}", eye);
            return nullptr;
        }

        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = 100'000'000LL;
        XrResult wr = dispatch_->xrWaitSwapchainImage(swapchains_[eye].handle, &wi);
        if (wr != XR_SUCCESS) {
            PT_LOG_WARN("xrWaitSwapchainImage eye={} result={}", eye, static_cast<int>(wr));
            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            dispatch_->xrReleaseSwapchainImage(swapchains_[eye].handle, &ri);
            return nullptr;
        }

        ID3D11Texture2D* dst = swapchains_[eye].images[idx].texture;
        ctx_->CopyResource(dst, compositor_->eye(eye).texture.Get());

        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        dispatch_->xrReleaseSwapchainImage(swapchains_[eye].handle, &ri);

        const CameraId cam_id = (eye == 0) ? CameraId::Left : CameraId::Right;
        const CameraIntrinsics& intr = camera_->intrinsics(cam_id);
        const float zoom = config_.zoom_factor;
        const float W_f  = static_cast<float>(w);
        const float H_f  = static_cast<float>(h);

        XrFovf cam_fov{};
        cam_fov.angleLeft  = -std::atan(static_cast<float>(intr.cx)        * zoom / static_cast<float>(intr.fx));
        cam_fov.angleRight =  std::atan((W_f - static_cast<float>(intr.cx)) * zoom / static_cast<float>(intr.fx));
        cam_fov.angleUp    =  std::atan(static_cast<float>(intr.cy)        * zoom / static_cast<float>(intr.fy));
        cam_fov.angleDown  = -std::atan((H_f - static_cast<float>(intr.cy)) * zoom / static_cast<float>(intr.fy));

        projection_views_[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        projection_views_[eye].pose = game_proj->views[eye].pose;
        projection_views_[eye].fov  = cam_fov;
        projection_views_[eye].subImage.swapchain        = swapchains_[eye].handle;
        projection_views_[eye].subImage.imageArrayIndex  = 0;
        projection_views_[eye].subImage.imageRect.offset = { 0, 0 };
        projection_views_[eye].subImage.imageRect.extent = { static_cast<int32_t>(w),
                                                              static_cast<int32_t>(h) };
    }

    composition_layer_ = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    composition_layer_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    composition_layer_.space      = game_proj->space;
    composition_layer_.viewCount  = 2;
    composition_layer_.views      = projection_views_.data();

    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&composition_layer_);
}

}  // namespace psvr2pt
