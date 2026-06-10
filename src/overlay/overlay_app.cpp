#include "overlay_app.h"

#include "logging.h"
#include "fov.h"

#include <thread>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace psvr2pt {

namespace {
constexpr char kOverlayKey[]  = "com.psvr2passthrough.overlay";
constexpr char kOverlayName[] = "PSVR2 Passthrough";

// Per-eye output resolution for the undistortion pass. Matches the camera
// native height; width rounded to camera stride. Side-by-side target is 2x wide.
constexpr UINT kEyeOutW = 1016;
constexpr UINT kEyeOutH = 1016;
}  // namespace

OverlayApp::OverlayApp()  = default;
OverlayApp::~OverlayApp() { shutdown(); }

bool OverlayApp::initialise() {
    if (!init_d3d_())               return false;
    if (!init_vr_())                return false;
    if (!init_camera_and_compositor_()) return false;
    return true;
}

bool OverlayApp::init_d3d_() {
    UINT flags = 0;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 1, D3D11_SDK_VERSION,
        &device_, &got, &ctx_);
    if (FAILED(hr)) {
        PT_LOG_ERROR("D3D11CreateDevice failed: 0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }
    PT_LOG_INFO("Overlay D3D11 device created.");
    return true;
}

bool OverlayApp::init_vr_() {
    // Wait for SteamVR to come up rather than failing immediately, so the
    // overlay survives being autolaunched before the runtime is ready.
    vr::EVRInitError err = vr::VRInitError_None;
    for (int attempt = 0; !should_quit_; ++attempt) {
        err = vr::VRInitError_None;
        vr_system_ = vr::VR_Init(&err, vr::VRApplication_Overlay);
        if (err == vr::VRInitError_None && vr_system_) break;

        if (attempt == 0 || attempt % 10 == 0) {
            PT_LOG_INFO("Waiting for SteamVR (VR_Init: {})",
                        vr::VR_GetVRInitErrorAsEnglishDescription(err));
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!vr_system_) return false;

    if (!vr::VROverlay()) {
        PT_LOG_ERROR("IVROverlay interface unavailable.");
        return false;
    }
    vr::EVROverlayError oerr =
        vr::VROverlay()->CreateOverlay(kOverlayKey, kOverlayName, &overlay_handle_);
    if (oerr != vr::VROverlayError_None) {
        PT_LOG_ERROR("CreateOverlay failed: {}",
                     vr::VROverlay()->GetOverlayErrorNameFromEnum(oerr));
        return false;
    }

    // Commit-2 baseline placement: opaque, modest width, in front of the HMD.
    // Proper FOV-derived sizing + head-locked transform land in Commit 3.
    vr::VROverlay()->SetOverlayAlpha(overlay_handle_, 1.0f);
    vr::VROverlay()->SetOverlayWidthInMeters(overlay_handle_, 1.0f);
    vr::VROverlay()->SetOverlayFlag(
        overlay_handle_, vr::VROverlayFlags_SideBySide_Parallel, true);

    PT_LOG_INFO("Overlay created (handle={}).", overlay_handle_);
    return true;
}

bool OverlayApp::init_camera_and_compositor_() {
    camera_ = std::make_unique<CameraSource>();
    if (!camera_->start()) {
        PT_LOG_WARN("Overlay: camera unavailable at startup; will retry per-frame.");
        // Not fatal — the per-frame loop simply won't produce frames until the
        // driver shared memory appears. We still create the compositor lazily.
    }

    eye_w_ = kEyeOutW;
    eye_h_ = kEyeOutH;

    compositor_ = std::make_unique<Compositor>();
    CameraIntrinsics in[2] = { camera_->intrinsics(CameraId::Left),
                               camera_->intrinsics(CameraId::Right) };
    CameraParameters pa[2] = { camera_->params(CameraId::Left),
                               camera_->params(CameraId::Right) };
    compositor_ready_ = compositor_->initialise(device_.Get(), eye_w_, eye_h_, in, pa);
    if (!compositor_ready_) {
        PT_LOG_ERROR("Overlay: compositor failed to initialise.");
        return false;
    }
    return ensure_output_texture_();
}

bool OverlayApp::ensure_output_texture_() {
    if (overlay_tex_) return true;

    D3D11_TEXTURE2D_DESC td{};
    td.Width  = eye_w_ * 2;   // side-by-side: left | right
    td.Height = eye_h_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &overlay_tex_);
    if (FAILED(hr)) {
        PT_LOG_ERROR("Overlay: create side-by-side texture failed: 0x{:08x}",
                     static_cast<uint32_t>(hr));
        return false;
    }
    return true;
}

void OverlayApp::render_and_submit_() {
    if (!compositor_ready_ || !overlay_tex_) return;

    // Pull the newest camera frame (non-blocking; the run() loop gates timing).
    const bool have_new = camera_ && camera_->try_get_latest(cached_frame_);
    if (have_new) {
        compositor_->upload_frame(cached_frame_);
    }
    // Re-render every wake so config/IPD changes take effect; if no frame has
    // ever arrived the compositor renders its (empty) targets harmlessly.
    compositor_->render(comp_cfg_);

    // Copy each eye into its half of the side-by-side target.
    for (int eye = 0; eye < 2; ++eye) {
        const D3D11_BOX src{ 0, 0, 0, eye_w_, eye_h_, 1 };
        ctx_->CopySubresourceRegion(
            overlay_tex_.Get(), 0,
            /*dstX=*/eye * eye_w_, /*dstY=*/0, /*dstZ=*/0,
            compositor_->eye(eye).texture.Get(), 0, &src);
    }

    vr::Texture_t tex{};
    tex.handle      = overlay_tex_.Get();
    tex.eType       = vr::TextureType_DirectX;
    tex.eColorSpace = vr::ColorSpace_Auto;  // gamma/linear verified in Commit 3
    vr::VROverlay()->SetOverlayTexture(overlay_handle_, &tex);

    // Commit-2 baseline: always visible so we can see the texture path working.
    // Toggle + show/hide arrives in Commit 4.
    vr::VROverlay()->ShowOverlay(overlay_handle_);
}

void OverlayApp::pump_vr_events_() {
    if (!vr_system_) return;
    vr::VREvent_t ev{};
    while (vr_system_->PollNextEvent(&ev, sizeof(ev))) {
        if (ev.eventType == vr::VREvent_Quit) {
            PT_LOG_INFO("Received VREvent_Quit; shutting down.");
            vr_system_->AcknowledgeQuit_Exiting();
            should_quit_ = true;
        }
    }
}

void OverlayApp::run() {
    PT_LOG_INFO("Overlay run loop started.");
    while (!should_quit_) {
        pump_vr_events_();
        if (should_quit_) break;

        render_and_submit_();

        // Commit-2 placeholder pacing. Commit 3 replaces this with a wait on the
        // camera frame event so we wake at camera rate, not a fixed sleep.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    PT_LOG_INFO("Overlay run loop exited.");
}

void OverlayApp::shutdown() {
    if (overlay_handle_ != vr::k_ulOverlayHandleInvalid && vr::VROverlay()) {
        vr::VROverlay()->DestroyOverlay(overlay_handle_);
        overlay_handle_ = vr::k_ulOverlayHandleInvalid;
    }
    if (vr_system_) {
        vr::VR_Shutdown();
        vr_system_ = nullptr;
    }
    if (camera_) {
        camera_->stop();
        camera_.reset();
    }
    compositor_.reset();
    overlay_tex_.Reset();
    ctx_.Reset();
    device_.Reset();
}

}  // namespace psvr2pt
