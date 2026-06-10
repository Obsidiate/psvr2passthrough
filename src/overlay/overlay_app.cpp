#include "overlay_app.h"

#include "logging.h"
#include "fov.h"
#include "frame.h"

#include <windows.h>

#include <cmath>
#include <filesystem>
#include <thread>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace psvr2pt {

namespace {
constexpr char kOverlayKey[]  = "com.psvr2passthrough.overlay";
constexpr char kOverlayName[] = "PSVR2 Passthrough";
constexpr char kAppKey[]      = "com.psvr2passthrough.overlay";

// Per-eye output resolution for the undistortion pass. Matches the camera
// native height; width rounded to camera stride. Side-by-side target is 2x wide.
constexpr UINT kEyeOutW = 1016;
constexpr UINT kEyeOutH = 1016;

// Directory containing the running exe — manifests are shipped next to it.
std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

std::string manifest_path()      { return (exe_dir() / "manifest.vrmanifest").string(); }
std::string action_manifest_path() { return (exe_dir() / "actions.json").string(); }
}  // namespace

OverlayApp::OverlayApp()  = default;
OverlayApp::~OverlayApp() { shutdown(); }

bool OverlayApp::initialise() {
    if (!init_d3d_())               return false;
    if (!init_vr_())                return false;
    if (!init_camera_and_compositor_()) return false;
    apply_config_();
    init_input_();
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

    // Opaque side-by-side overlay. Width + head-locked transform are computed
    // from the camera FOV in update_overlay_placement_() once the compositor is up.
    vr::VROverlay()->SetOverlayAlpha(overlay_handle_, 1.0f);
    vr::VROverlay()->SetOverlayFlag(
        overlay_handle_, vr::VROverlayFlags_SideBySide_Parallel, true);
    // Sort in front of other overlays and don't let it become interactive.
    vr::VROverlay()->SetOverlayInputMethod(overlay_handle_, vr::VROverlayInputMethod_None);

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

void OverlayApp::apply_config_() {
    // Load the shared on-disk config (same file the layer and GUI use) and map
    // it into the CompositorConfig, mirroring layer_main.cpp's apply path.
    config_ = load_config();

    comp_cfg_.global_alpha           = config_.global_alpha;
    comp_cfg_.brightness_enabled     = config_.brightness_enabled;
    comp_cfg_.brightness             = config_.brightness;
    comp_cfg_.contrast_enabled       = config_.contrast_enabled;
    comp_cfg_.contrast               = config_.contrast;
    comp_cfg_.enhancements_enabled   = config_.enhancements_enabled;
    comp_cfg_.unsharp_amount         = config_.unsharp_amount;
    comp_cfg_.unsharp_radius         = config_.unsharp_radius;
    comp_cfg_.apply_undistortion     = config_.apply_undistortion;
    comp_cfg_.zoom_factor            = config_.zoom_factor;
    comp_cfg_.camera_toe_out_rad_l   = config_.camera_toe_out_rad_l;
    comp_cfg_.camera_tilt_down_rad_l = config_.camera_tilt_down_rad_l;
    comp_cfg_.camera_roll_rad_l      = config_.camera_roll_rad_l;
    comp_cfg_.camera_toe_out_rad_r   = config_.camera_toe_out_rad_r;
    comp_cfg_.camera_tilt_down_rad_r = config_.camera_tilt_down_rad_r;
    comp_cfg_.camera_roll_rad_r      = config_.camera_roll_rad_r;

    camera_separation_m_ = config_.camera_separation_mm / 1000.f;

    // Reprojection is OpenXR-pose driven and irrelevant to the overlay; the
    // SteamVR compositor reprojects the head-locked quad for us.
    comp_cfg_.reprojection_enabled = false;

    // Legacy (keyboard/XInput/DInput) binding shared with the layer.
    poller_.set_binding(config_.passthrough_binding);
}

void OverlayApp::init_input_() {
    // Register the action manifest so the toggle action and its default PSVR2
    // bindings are available (and remappable in SteamVR's binding UI).
    if (vr::VRInput()) {
        const std::string am = action_manifest_path();
        vr::EVRInputError ierr = vr::VRInput()->SetActionManifestPath(am.c_str());
        if (ierr != vr::VRInputError_None) {
            PT_LOG_WARN("SetActionManifestPath('{}') failed: {}", am, static_cast<int>(ierr));
        } else {
            vr::VRInput()->GetActionSetHandle("/actions/main", &action_set_main_);
            vr::VRInput()->GetActionHandle("/actions/main/in/toggle_passthrough",
                                           &action_toggle_);
            PT_LOG_INFO("SteamVR input ready (action manifest loaded).");
        }
    }
}

bool OverlayApp::update_visibility_() {
    // Debug force-on bypasses all input.
    if (config_.force_passthrough_on) {
        passthrough_visible_ = true;
        return true;
    }

    // --- Legacy binding (keyboard/XInput/DInput), same state machine as layer ---
    if (!config_.passthrough_binding.is_none()) {
        const bool cur = poller_.poll();
        if (config_.toggle_mode) {
            if (cur && !prev_button_state_) passthrough_visible_ = !passthrough_visible_;
        } else {
            passthrough_visible_ = cur;
        }
        prev_button_state_ = cur;
    }

    // --- SteamVR input (IVRInput) ---
    if (vr::VRInput() && action_set_main_ != vr::k_ulInvalidActionSetHandle) {
        vr::VRActiveActionSet_t active{};
        active.ulActionSet = action_set_main_;
        vr::VRInput()->UpdateActionState(&active, sizeof(active), 1);

        vr::InputDigitalActionData_t data{};
        if (vr::VRInput()->GetDigitalActionData(
                action_toggle_, &data, sizeof(data),
                vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && data.bActive) {
            // The toggle action is a momentary press; flip on the rising edge,
            // matching the legacy toggle behaviour so both inputs feel the same.
            if (data.bState && !prev_steamvr_state_) {
                passthrough_visible_ = !passthrough_visible_;
            }
            prev_steamvr_state_ = data.bState;
        } else {
            prev_steamvr_state_ = false;
        }
    }

    return passthrough_visible_;
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

void OverlayApp::update_overlay_placement_() {
    // Head-locked: overlay pose is expressed relative to the HMD tracked device,
    // so the quad rides with the head (correct, since the cameras are head-mounted).
    // SteamVR's overlay reprojection handles frame timing.
    //
    // OpenVR matrix is row-major 3x4 [R | t]. We place the quad centred on the
    // HMD forward axis (-Z in HMD space) at overlay_distance_m_, facing the user.
    vr::HmdMatrix34_t xform{};
    xform.m[0][0] = 1.f; xform.m[0][1] = 0.f; xform.m[0][2] = 0.f; xform.m[0][3] = 0.f;
    xform.m[1][0] = 0.f; xform.m[1][1] = 1.f; xform.m[1][2] = 0.f; xform.m[1][3] = 0.f;
    xform.m[2][0] = 0.f; xform.m[2][1] = 0.f; xform.m[2][2] = 1.f; xform.m[2][3] = -overlay_distance_m_;
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
        overlay_handle_, vr::k_unTrackedDeviceIndex_Hmd, &xform);

    // Width: fill the camera horizontal frustum at the chosen depth. The
    // side-by-side overlay shows one eye's frustum per eye, so size to a single
    // eye's horizontal angular extent.
    const CameraIntrinsics intr = camera_->intrinsics(CameraId::Left);
    if (intr.fx <= 0.0 || intr.fy <= 0.0) {
        // Calibration not available yet (camera not connected). Retry next frame
        // once the driver shared memory appears; transform is already set.
        return;
    }
    const EyeFov fov = fov_from_intrinsics(intr, kCameraWidth, kCameraHeight,
                                           config_.zoom_factor);
    const float width_m =
        overlay_distance_m_ * (std::tan(fov.angle_right) + std::tan(-fov.angle_left));
    vr::VROverlay()->SetOverlayWidthInMeters(overlay_handle_, width_m);

    PT_LOG_INFO("Overlay placement: distance={:.2f}m width={:.2f}m", overlay_distance_m_, width_m);
    placement_done_ = true;
}

void OverlayApp::update_ipd_() {
    if (!config_.ipd_correction_enabled) {
        if (comp_cfg_.ipd_toe_delta_l != 0.f || comp_cfg_.ipd_toe_delta_r != 0.f) {
            comp_cfg_.ipd_toe_delta_l = 0.f;
            comp_cfg_.ipd_toe_delta_r = 0.f;
        }
        return;
    }

    vr::ETrackedPropertyError perr = vr::TrackedProp_Success;
    float ipd = vr_system_->GetFloatTrackedDeviceProperty(
        vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &perr);
    if (perr != vr::TrackedProp_Success || ipd <= 0.f) {
        ipd = (last_ipd_m_ > 0.f) ? last_ipd_m_ : 0.064f;  // fallback: last known or 64mm
    }

    if (std::abs(ipd - last_ipd_m_) > 0.0005f) {   // 0.5mm threshold (matches layer)
        last_ipd_m_ = ipd;
        ipd_toe_deltas(ipd, camera_separation_m_,
                       comp_cfg_.ipd_toe_delta_l, comp_cfg_.ipd_toe_delta_r);
        PT_LOG_INFO("Overlay IPD correction: ipd={:.1f}mm cam_sep={:.1f}mm delta={:.4f}rad",
                    ipd * 1000.f, camera_separation_m_ * 1000.f, comp_cfg_.ipd_toe_delta_r);
    }
}

void OverlayApp::render_and_submit_() {
    if (!compositor_ready_ || !overlay_tex_) return;

    // Visibility from legacy + SteamVR input. Hidden = nothing to render; just
    // ensure the overlay is hidden and bail (cheap idle).
    const bool visible = update_visibility_();
    if (!visible) {
        if (overlay_shown_) {
            vr::VROverlay()->HideOverlay(overlay_handle_);
            overlay_shown_ = false;
        }
        return;
    }

    if (!placement_done_) update_overlay_placement_();
    update_ipd_();

    // Pull the newest camera frame (non-blocking; run() gates timing on the
    // camera event). If none is new we still re-render so config/IPD changes and
    // last-frame persistence hold (resilient to camera dropout — no crash).
    if (camera_ && camera_->try_get_latest(cached_frame_)) {
        compositor_->upload_frame(cached_frame_);
    }
    compositor_->render(comp_cfg_);

    // Copy each eye into its half of the side-by-side target (left | right).
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
    // Compositor output is R8G8B8A8_UNORM (linear, premultiplied alpha), matching
    // the OpenXR layer's swapchain. ColorSpace_Linear avoids a double gamma.
    tex.eColorSpace = vr::ColorSpace_Linear;
    vr::VROverlay()->SetOverlayTexture(overlay_handle_, &tex);

    if (!overlay_shown_) {
        vr::VROverlay()->ShowOverlay(overlay_handle_);
        overlay_shown_ = true;
    }
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

        // Wake at camera rate, not a render-rate spin. The timeout doubles as a
        // heartbeat so VR events (quit) and IPD/placement are still serviced even
        // if the camera feed stalls (dropout shows the last frame, no crash).
        camera_->wait_for_frame(/*timeout_ms=*/100);

        render_and_submit_();
    }
    PT_LOG_INFO("Overlay run loop exited.");
}

// ---------------------------------------------------------------------------
// CLI registration: install the app manifest with SteamVR and set autolaunch so
// the overlay starts with SteamVR. Uses a transient Utility-mode VR session.
// ---------------------------------------------------------------------------

namespace {
struct ScopedVR {
    bool ok = false;
    ScopedVR() {
        vr::EVRInitError e = vr::VRInitError_None;
        vr::VR_Init(&e, vr::VRApplication_Utility);
        ok = (e == vr::VRInitError_None);
        if (!ok) {
            PT_LOG_ERROR("VR_Init(Utility) failed: {}",
                         vr::VR_GetVRInitErrorAsEnglishDescription(e));
        }
    }
    ~ScopedVR() { if (ok) vr::VR_Shutdown(); }
};
}  // namespace

int register_application() {
    ScopedVR vr_session;
    if (!vr_session.ok || !vr::VRApplications()) return 1;

    const std::string mpath = manifest_path();
    vr::EVRApplicationError aerr =
        vr::VRApplications()->AddApplicationManifest(mpath.c_str());
    if (aerr != vr::VRApplicationError_None) {
        PT_LOG_ERROR("AddApplicationManifest('{}') failed: {}", mpath,
                     vr::VRApplications()->GetApplicationsErrorNameFromEnum(aerr));
        return 1;
    }
    aerr = vr::VRApplications()->SetApplicationAutoLaunch(kAppKey, true);
    if (aerr != vr::VRApplicationError_None) {
        PT_LOG_ERROR("SetApplicationAutoLaunch(true) failed: {}",
                     vr::VRApplications()->GetApplicationsErrorNameFromEnum(aerr));
        return 1;
    }
    PT_LOG_INFO("Registered with SteamVR and enabled autolaunch ({}).", mpath);
    return 0;
}

int unregister_application() {
    ScopedVR vr_session;
    if (!vr_session.ok || !vr::VRApplications()) return 1;

    vr::VRApplications()->SetApplicationAutoLaunch(kAppKey, false);
    const std::string mpath = manifest_path();
    vr::EVRApplicationError aerr =
        vr::VRApplications()->RemoveApplicationManifest(mpath.c_str());
    if (aerr != vr::VRApplicationError_None) {
        PT_LOG_WARN("RemoveApplicationManifest('{}') returned: {}", mpath,
                    vr::VRApplications()->GetApplicationsErrorNameFromEnum(aerr));
    }
    PT_LOG_INFO("Unregistered from SteamVR autolaunch.");
    return 0;
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
