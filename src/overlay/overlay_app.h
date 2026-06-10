#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <openvr.h>

#include <memory>

#include "config.h"
#include "compositor.h"
#include "camera_source.h"
#include "frame.h"

namespace psvr2pt {

// Standalone OpenVR overlay application. Owns its own D3D11 device (unlike the
// layer, which borrows the game's), drives the shared CameraSource + Compositor
// to produce undistorted stereo, and submits the result to SteamVR as a single
// head-locked side-by-side overlay.
//
// Commit 2 scope: init VR + D3D, wire camera/compositor, get a side-by-side
// texture onto the overlay. Geometry/IPD correctness and input land in later
// commits.
class OverlayApp {
public:
    OverlayApp();
    ~OverlayApp();

    OverlayApp(const OverlayApp&) = delete;
    OverlayApp& operator=(const OverlayApp&) = delete;

    // Brings up D3D11 + OpenVR (VRApplication_Overlay) and creates the overlay.
    // Retries waiting for SteamVR. Returns false on unrecoverable failure.
    bool initialise();

    // Runs the per-frame loop until a quit is requested (VREvent_Quit or
    // should_quit_). Blocks on the camera frame event so it only wakes at
    // camera rate.
    void run();

    // Releases the overlay, D3D resources, camera, and shuts down OpenVR.
    void shutdown();

private:
    bool init_d3d_();
    bool init_vr_();                 // VR_Init + CreateOverlay, with wait loop
    bool init_camera_and_compositor_();

    void apply_config_();            // disk Config -> CompositorConfig + geometry
    void update_overlay_placement_(); // head-locked transform + FOV-derived width
    void update_ipd_();              // live IPD -> ipd_toe_deltas, rebuild on change
    void render_and_submit_();       // one frame: camera -> compositor -> overlay
    bool ensure_output_texture_();   // (re)create the side-by-side RGBA target
    void pump_vr_events_();          // VREvent_Quit etc.

    // --- D3D11 (own device) ---
    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;

    // Single side-by-side (left | right) RGBA target handed to SteamVR.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> overlay_tex_;
    UINT eye_w_ = 0;     // per-eye output width
    UINT eye_h_ = 0;     // per-eye output height

    // --- OpenVR ---
    vr::IVRSystem*       vr_system_  = nullptr;
    vr::VROverlayHandle_t overlay_handle_ = vr::k_ulOverlayHandleInvalid;

    // --- shared core ---
    std::unique_ptr<CameraSource> camera_;
    std::unique_ptr<Compositor>   compositor_;
    StereoFrame                   cached_frame_;
    Config                        config_{};      // on-disk config (shared with layer/GUI)
    CompositorConfig              comp_cfg_{};

    // Overlay geometry. Head-locked at this distance; width is derived from the
    // camera FOV so the quad fills the camera frustum at that depth.
    float overlay_distance_m_ = 1.5f;

    // Live IPD tracking (mirrors the layer's >0.5mm rebuild threshold).
    float last_ipd_m_ = -1.f;   // sentinel: forces first update
    float camera_separation_m_ = 0.079f;

    bool compositor_ready_ = false;
    bool placement_done_   = false;   // transform/width set once after init
    bool should_quit_      = false;
};

}  // namespace psvr2pt
