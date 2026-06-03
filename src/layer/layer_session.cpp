#include "layer_session.h"
#include "layer_dispatch.h"
#include "logging.h"

#include <cmath>
#include <algorithm>


namespace psvr2pt {

// Computes the angular velocity magnitude (rad/s) from the quaternion delta
// between two consecutive driver poses.  The delta quaternion's half-angle gives
// the rotation magnitude; dividing by dt converts to rad/s.
// Returns 0 if dt is too small to be meaningful or either pose is invalid.
float LayerSession::pose_angular_velocity_(const Pose3f& prev, const Pose3f& curr,
                                           float dt_seconds) noexcept {
    if (!prev.valid || !curr.valid || dt_seconds < 1e-6f) return 0.f;

    // delta_q = curr * conjugate(prev)
    const float ax = -prev.qx, ay = -prev.qy, az = -prev.qz, aw = prev.qw;
    const float dw = curr.qw*aw - curr.qx*ax - curr.qy*ay - curr.qz*az;
    const float dx = curr.qw*ax + curr.qx*aw + curr.qy*az - curr.qz*ay;
    const float dy = curr.qw*ay - curr.qx*az + curr.qy*aw + curr.qz*ax;
    const float dz = curr.qw*az + curr.qx*ay - curr.qy*ax + curr.qz*aw;

    // Half-angle = acos(|dw|), clamped for numerical safety.
    const float half_angle = std::acos(std::clamp(std::abs(dw), 0.f, 1.f));
    return (2.f * half_angle) / dt_seconds;
}

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

    if (!recorder_cfg_.output_dir.empty() && recorder_cfg_.max_frames != 0) {
        if (camera_->is_running()) {
            recorder_cfg_.intrinsics[0]    = camera_->intrinsics(CameraId::Left);
            recorder_cfg_.intrinsics[1]    = camera_->intrinsics(CameraId::Right);
            recorder_cfg_.params[0]        = camera_->params(CameraId::Left);
            recorder_cfg_.params[1]        = camera_->params(CameraId::Right);
            recorder_cfg_.calibration_valid = true;
        }
        if (!recorder_.start(recorder_cfg_))
            PT_LOG_WARN("LayerSession: FrameRecorder failed to start; recording disabled.");
    }

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
    static uint64_t last_log_seq = 0;
    static auto     last_log_time = std::chrono::steady_clock::now();
    ++frame_n;
    if (frame_n % 300 == 0) {
        const uint64_t seq_now  = cached_frame_.sequence;
        const auto     now      = std::chrono::steady_clock::now();
        const double   elapsed  = std::chrono::duration<double>(now - last_log_time).count();
        const double   cam_fps  = (elapsed > 0.0) ? (seq_now - last_log_seq) / elapsed : 0.0;
        PT_LOG_INFO("compose_layer frame {} | camera seq={} fps={:.1f}", frame_n, seq_now, cam_fps);
        last_log_seq  = seq_now;
        last_log_time = now;
    }

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

    // Dynamic IPD correction: derive per-eye toe-out delta from the current IPD
    // reported by the runtime and the known camera physical separation.
    // The cameras are fixed to the headset body; the lenses/eyes move with the IPD
    // slider. IPD is read by locating views in VIEW space (head-local), where the
    // eye X separation equals the true IPD independent of head orientation.
    if (ipd_correction_enabled_) {
        // Locate eyes in VIEW space (head-local) so the X separation equals the true
        // IPD regardless of head orientation in the world. Using game_proj->space
        // (stage/local) gives world-space positions whose X difference varies wildly
        // with head rotation, causing continuous spurious mesh rebuilds.
        XrViewLocateInfo ipd_vli{ XR_TYPE_VIEW_LOCATE_INFO };
        ipd_vli.viewConfigurationType = view_config_type_;
        ipd_vli.displayTime           = original->displayTime;
        ipd_vli.space                 = passthrough_space_;   // VIEW space = head-local

        XrViewState ipd_vs{ XR_TYPE_VIEW_STATE };
        std::array<XrView, 2> ipd_views{};
        ipd_views[0].type = XR_TYPE_VIEW;
        ipd_views[1].type = XR_TYPE_VIEW;
        uint32_t ipd_view_count = 0;
        const XrResult ipd_lr = dispatch_->xrLocateViews(
            session_, &ipd_vli, &ipd_vs, 2, &ipd_view_count, ipd_views.data());

        constexpr uint32_t kBothValid =
            XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        const float raw_ipd = (XR_SUCCEEDED(ipd_lr) &&
                               (ipd_vs.viewStateFlags & kBothValid) == kBothValid)
            ? std::abs(ipd_views[1].pose.position.x - ipd_views[0].pose.position.x)
            : (last_ipd_m_ > 0.f ? last_ipd_m_ : 0.064f);  // fallback: last known or 64mm

        if (std::abs(raw_ipd - last_ipd_m_) > 0.0005f) {
            last_ipd_m_ = raw_ipd;

            // Lateral offset: positive = camera is further outward than eye.
            const float offset  = camera_separation_m_ * 0.5f - raw_ipd * 0.5f;
            // Angular equivalent at nominal depth 0.7 m (hand-to-shoulder reach) —
            // prioritises near-field interactions over distant objects.
            const float delta   = std::atan2(offset, 0.7f);
            config_.ipd_toe_delta_l = -delta;
            config_.ipd_toe_delta_r =  delta;

            PT_LOG_INFO("IPD correction: ipd={:.1f}mm cam_sep={:.1f}mm "
                        "offset={:.2f}mm delta={:.4f}rad",
                        raw_ipd * 1000.f,
                        camera_separation_m_ * 1000.f,
                        offset * 1000.f,
                        delta);
        }
    } else {
        config_.ipd_toe_delta_l = 0.f;
        config_.ipd_toe_delta_r = 0.f;
    }

    const uint32_t w = static_cast<uint32_t>(kCameraWidth);
    const uint32_t h = static_cast<uint32_t>(kCameraHeight);
    if (!ensure_swapchain_(w, h)) return nullptr;

    // try_get_latest swaps new data into cached_frame_, donating its old buffers
    // back to the producer for recycling. If no new frame arrived this tick we
    // reuse the last valid one rather than dropping passthrough entirely.
    const bool new_camera_frame = camera_->try_get_latest(cached_frame_);
    if (!cached_frame_.valid()) return nullptr;

    if (new_camera_frame && recorder_.running()) {
        const auto now = cached_frame_.captured_at;
        const float dt = prev_recorded_time_.time_since_epoch().count() > 0
            ? std::chrono::duration<float>(now - prev_recorded_time_).count()
            : 0.f;
        const float ang_vel = pose_angular_velocity_(
            prev_recorded_pose_, cached_frame_.captured_pose, dt);
        recorder_.submitFrame(cached_frame_, ang_vel);
        prev_recorded_pose_ = cached_frame_.captured_pose;
        prev_recorded_time_ = now;
    }

    // Determine captured_eye_pose_ for layer submission.
    // When reprojection is enabled, locate views at the camera capture timestamp so
    // the compositor's ATW warps from the measured past moment to scanout time.
    // When new_camera_frame is false the cached pose is reused unchanged — both
    // display frames consuming one camera image get the same capture-time pose,
    // giving ATW smooth per-scanout warp deltas instead of a ghosting differential.
    if (new_camera_frame || !has_captured_eye_pose_) {
        if (config_.reprojection_enabled) {
            const int64_t offset = clock_offset_ns_.load(std::memory_order_relaxed);
            const int64_t steady_capture_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    cached_frame_.captured_at.time_since_epoch()).count();
            // clock_offset includes the runtime display prediction window (~8ms typical)
            // as a constant bias; absorbed into camera_latency_offset_ns_ during tuning.
            // Frame-to-frame variance in the prediction interval is a residual noise
            // floor that cannot be eliminated without the KHR QPC extension.
            const XrTime xr_capture = static_cast<XrTime>(
                steady_capture_ns + offset - camera_latency_offset_ns_);

            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = view_config_type_;
            vli.displayTime           = xr_capture;
            vli.space                 = game_proj->space;

            XrViewState vs{XR_TYPE_VIEW_STATE};
            std::array<XrView, 2> located_views{};
            located_views[0].type = XR_TYPE_VIEW;
            located_views[1].type = XR_TYPE_VIEW;
            uint32_t view_count_out = 0;
            const XrResult lr = dispatch_->xrLocateViews(
                session_, &vli, &vs, 2, &view_count_out, located_views.data());

            constexpr uint32_t kBothValid =
                XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            if (XR_SUCCEEDED(lr) && (vs.viewStateFlags & kBothValid) == kBothValid) {
                for (uint32_t e = 0; e < 2; ++e)
                    captured_eye_pose_[e] = located_views[e].pose;
                has_captured_eye_pose_ = true;

                if (!reproj_probe_logged_) {
                    reproj_probe_logged_ = true;
                    const double dpx = located_views[0].pose.position.x - game_proj->views[0].pose.position.x;
                    const double dpy = located_views[0].pose.position.y - game_proj->views[0].pose.position.y;
                    const double dpz = located_views[0].pose.position.z - game_proj->views[0].pose.position.z;
                    PT_LOG_INFO("Reprojection probe OK: xrLocateViews accepted past timestamp. "
                                "clock_offset_ns={} camera_latency_offset_ns={} "
                                "display_minus_capture={:.1f}ms "
                                "capture_vs_game_pos_delta=({:.4f},{:.4f},{:.4f}). "
                                "clock_offset includes ~8ms display prediction window bias. "
                                "Effective lookup = captured_at + clock_offset - camera_latency_offset. "
                                "Tune camera_latency_offset for USB+exposure latency and prediction bias. "
                                "Empirical optimum far from 16ms indicates different actual bias composition.",
                                offset, camera_latency_offset_ns_,
                                (static_cast<double>(original->displayTime) - static_cast<double>(xr_capture)) / 1.0e6,
                                dpx, dpy, dpz);
                }

                if (debug_reproj_stats_) {
                    const double delta_ms =
                        (static_cast<double>(original->displayTime) - static_cast<double>(xr_capture)) / 1.0e6;
                    reproj_stat_delta_sum_ += delta_ms;
                    if (delta_ms > reproj_stat_delta_max_) reproj_stat_delta_max_ = delta_ms;
                    ++reproj_stat_count_;
                }
            } else {
                // xrLocateViews failed or returned invalid flags — runtime history window
                // may not extend to xr_capture. Fall back to game predicted pose.
                for (uint32_t e = 0; e < 2 && e < game_proj->viewCount; ++e)
                    captured_eye_pose_[e] = game_proj->views[e].pose;
                has_captured_eye_pose_ = true;
                ++reproj_invalid_total_;
                if (debug_reproj_stats_) ++reproj_stat_invalid_;
                if (reproj_invalid_total_ == 1 || reproj_invalid_total_ % 100 == 0) {
                    PT_LOG_WARN("xrLocateViews invalid pose (result={} flags={:#x}) - "
                                "falling back to game pose (count={}). "
                                "Reduce camera_latency_offset_ns or check runtime history window.",
                                static_cast<int>(lr),
                                static_cast<uint32_t>(vs.viewStateFlags),
                                reproj_invalid_total_);
                }
            }
        } else {
            // Reprojection disabled: snapshot game predicted pose for layer submission.
            for (uint32_t e = 0; e < 2 && e < game_proj->viewCount; ++e)
                captured_eye_pose_[e] = game_proj->views[e].pose;
            has_captured_eye_pose_ = true;
        }
    }

    if (debug_reproj_stats_) {
        const auto stats_now = std::chrono::steady_clock::now();
        if (!reproj_stat_initialized_) {
            reproj_stat_epoch_       = stats_now;
            reproj_stat_initialized_ = true;
        } else if (std::chrono::duration<double>(stats_now - reproj_stat_epoch_).count() >= 1.0) {
            const double mean_ms = (reproj_stat_count_ > 0)
                ? reproj_stat_delta_sum_ / static_cast<double>(reproj_stat_count_) : 0.0;
            PT_LOG_INFO("Reproj stats: locate_ok={} mean_delta={:.1f}ms max_delta={:.1f}ms invalid={}",
                        reproj_stat_count_, mean_ms, reproj_stat_delta_max_, reproj_stat_invalid_);
            reproj_stat_count_     = 0;
            reproj_stat_delta_sum_ = 0.0;
            reproj_stat_delta_max_ = 0.0;
            reproj_stat_invalid_   = 0;
            reproj_stat_epoch_     = stats_now;
        }
    }

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

        // Use the OpenXR eye pose captured at camera-frame-arrive time.
        // ATW corrects for the rotation delta between that snapshot and actual
        // display time. If no snapshot yet, fall back to the current predicted pose.
        // NOTE: we submit EYE positions (not camera positions) intentionally.
        // The cameras are 79mm apart; the eyes are at IPD (~62mm). Submitting
        // camera positions would declare the wider baseline to the compositor,
        // amplifying the perceived stereo mismatch. The angular correction in the
        // undistortion mesh handles the directional component for distant objects.
        const XrPosef layer_pose = (has_captured_eye_pose_ && config_.reprojection_enabled)
                                 ? captured_eye_pose_[eye]
                                 : game_proj->views[eye].pose;

        if (frame_n % 300 == 0) {
            PT_LOG_INFO("Passthrough pose submit: new_frame={} pos=({:.3f},{:.3f},{:.3f}) ori=({:.3f},{:.3f},{:.3f},{:.3f})",
                        new_camera_frame,
                        layer_pose.position.x, layer_pose.position.y, layer_pose.position.z,
                        layer_pose.orientation.x, layer_pose.orientation.y,
                        layer_pose.orientation.z, layer_pose.orientation.w);
        }

        projection_views_[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        projection_views_[eye].pose = layer_pose;
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
