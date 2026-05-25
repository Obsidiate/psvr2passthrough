#include "config_window.h"

#include <imgui.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cmath>
#include <sstream>

namespace psvr2pt {

namespace {

bool configs_equal(const Config& a, const Config& b) {
    return a.enabled               == b.enabled
        && a.force_passthrough_on  == b.force_passthrough_on
        && a.global_alpha          == b.global_alpha
        && a.brightness            == b.brightness
        && a.toggle_mode         == b.toggle_mode
        && a.passthrough_binding.type               == b.passthrough_binding.type
        && a.passthrough_binding.vk_code            == b.passthrough_binding.vk_code
        && a.passthrough_binding.xinput_controller  == b.passthrough_binding.xinput_controller
        && a.passthrough_binding.xinput_button_mask == b.passthrough_binding.xinput_button_mask
        && a.passthrough_binding.dinput_device_guid == b.passthrough_binding.dinput_device_guid
        && a.passthrough_binding.dinput_button_index== b.passthrough_binding.dinput_button_index
        && a.apply_undistortion       == b.apply_undistortion
        && a.zoom_factor              == b.zoom_factor
        && a.camera_eyes_linked       == b.camera_eyes_linked
        && a.camera_toe_out_rad_l     == b.camera_toe_out_rad_l
        && a.camera_tilt_down_rad_l   == b.camera_tilt_down_rad_l
        && a.camera_roll_rad_l        == b.camera_roll_rad_l
        && a.camera_toe_out_rad_r     == b.camera_toe_out_rad_r
        && a.camera_tilt_down_rad_r   == b.camera_tilt_down_rad_r
        && a.camera_roll_rad_r        == b.camera_roll_rad_r;
}

}  // namespace

ConfigWindow::ConfigWindow()  = default;
ConfigWindow::~ConfigWindow() = default;

void ConfigWindow::initialise(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    device_   = device;
    ctx_      = ctx;
    on_disk_  = load_config();
    working_  = on_disk_;
    dirty_    = false;
    capturer_.open_devices();   // open DInput devices now so they are warm
}

void ConfigWindow::shutdown() {
    capturer_.close_devices();
}

void ConfigWindow::draw() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##root", nullptr, flags);

    ImGui::TextUnformatted("PSVR2 Passthrough Layer — Configuration");
    ImGui::Separator();
    ImGui::TextDisabled("Changes apply when DCS / MSFS / iRacing next starts. Save then restart your sim.");
    ImGui::Spacing();

    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_BordersInnerV
                                     | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch, 0.60f);
        ImGui::TableSetupColumn("status",   ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        draw_main_panel();

        ImGui::TableSetColumnIndex(1);
        draw_about_panel();

        ImGui::EndTable();
    }

    ImGui::End();

    dirty_ = !configs_equal(working_, on_disk_);
    if (saved_recently_ && (ImGui::GetTime() - saved_at_) > 2.0)
        saved_recently_ = false;

    // Run capturer scan every frame while capturing.
    if (capturing_) {
        if (capturer_.scan()) {
            working_.passthrough_binding = capturer_.captured();
            capturing_ = false;
        }
    }
}

void ConfigWindow::draw_main_panel() {

    // -----------------------------------------------------------------------
    ImGui::SeparatorText("Save / restore");

    const ImVec4 dirty_col(1.0f, 0.85f, 0.4f, 1.0f);
    const ImVec4 saved_col(0.4f, 1.0f, 0.5f, 1.0f);

    if (ImGui::Button("Save")) {
        if (save_config(working_)) {
            on_disk_        = working_;
            dirty_          = false;
            saved_recently_ = true;
            saved_at_       = ImGui::GetTime();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) {
        on_disk_ = load_config();
        working_ = on_disk_;
        dirty_   = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset defaults")) {
        working_ = Config{};
    }
    ImGui::SameLine();
    if (dirty_)               ImGui::TextColored(dirty_col, "Unsaved changes");
    else if (saved_recently_) ImGui::TextColored(saved_col, "Saved.");
    else                      ImGui::TextDisabled("No changes.");

    ImGui::TextDisabled("Config: %s", config_file_path().string().c_str());
    ImGui::Spacing();

    // -----------------------------------------------------------------------
    ImGui::SeparatorText("Master switch");
    ImGui::Checkbox("Layer enabled", &working_.enabled);
    ImGui::TextDisabled("If disabled, the layer loads but stays inert — no camera,");
    ImGui::TextDisabled("no compositing. Useful for A/B comparisons.");
    ImGui::Spacing();
    ImGui::Checkbox("Force passthrough always visible (debug)", &working_.force_passthrough_on);
    ImGui::TextDisabled("Ignores button binding. Use to verify passthrough is working.");
    ImGui::TextDisabled("Disable before normal use — passthrough will overlay all games.");
    ImGui::Spacing();

    // -----------------------------------------------------------------------
    ImGui::SeparatorText("Passthrough transparency");
    ImGui::SliderFloat("Opacity", &working_.global_alpha, 0.0f, 1.0f, "%.2f");
    ImGui::TextDisabled("1.0 = fully opaque passthrough.  0.5 = semi-transparent.");
    ImGui::TextDisabled("0.0 = invisible (layer still composites but draws nothing).");
    ImGui::Spacing();

    // -----------------------------------------------------------------------
    ImGui::SeparatorText("Passthrough button binding");

    // Current binding display.
    const std::string bname = working_.passthrough_binding.display_name();
    ImGui::Text("Current: %s", bname.c_str());
    ImGui::Spacing();

    draw_binding_capture_button();

    ImGui::Spacing();
    if (!working_.passthrough_binding.is_none()) {
        if (ImGui::Button("Clear binding")) {
            working_.passthrough_binding = PassthroughBinding{};
            if (capturing_) { capturer_.stop(); capturing_ = false; }
        }
        ImGui::TextDisabled("Passthrough will be always visible when no binding is set.");
        ImGui::Spacing();

        ImGui::SeparatorText("Activation mode");
        int mode = working_.toggle_mode ? 1 : 0;
        ImGui::RadioButton("Hold to show", &mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Toggle on/off", &mode, 1);
        working_.toggle_mode = (mode == 1);
        if (working_.toggle_mode)
            ImGui::TextDisabled("Press once to show passthrough, press again to hide.");
        else
            ImGui::TextDisabled("Passthrough visible only while button is held.");
    } else {
        ImGui::TextDisabled("No binding set — passthrough is always visible.");
        ImGui::TextDisabled("Set a binding to control it with a button.");
    }
    ImGui::Spacing();

    // -----------------------------------------------------------------------
    ImGui::SeparatorText("Camera image");
    ImGui::SliderFloat("Brightness", &working_.brightness, 0.5f, 4.0f, "%.2f");
    ImGui::TextDisabled("Boost to match native passthrough brightness. Default 1.3.");
    ImGui::Checkbox("Apply lens undistortion", &working_.apply_undistortion);
    ImGui::SliderFloat("Zoom factor", &working_.zoom_factor, 0.5f, 4.0f, "%.2f");
    ImGui::TextDisabled("Higher zoom = narrower FOV, fewer lens artefacts at edges.");
    ImGui::Spacing();

    // -----------------------------------------------------------------------
    ImGui::SeparatorText("Stereo geometry calibration");
    {
        static constexpr float kR2D = 180.f / 3.14159265f;
        static constexpr float kD2R = 3.14159265f / 180.f;

        if (working_.camera_eyes_linked) {
            if (ImGui::Button("Unlock eyes"))
                working_.camera_eyes_linked = false;
            ImGui::SameLine();
            ImGui::TextDisabled("Both eyes adjust symmetrically.");

            float toe_deg  =  working_.camera_toe_out_rad_l * kR2D;
            float tilt_deg =  working_.camera_tilt_down_rad_l * kR2D;
            float roll_deg =  working_.camera_roll_rad_l * kR2D;
            bool changed = false;
            changed |= ImGui::SliderFloat("Toe-out (deg)",  &toe_deg,  0.0f,  45.8f, "%.1f");
            ImGui::TextDisabled("Outward rotation per eye. Too high = cross-eyed. Too low = wall-eyed.");
            changed |= ImGui::SliderFloat("Tilt down (deg)", &tilt_deg, 0.0f,  45.8f, "%.1f");
            ImGui::TextDisabled("Corrects cameras pointing downward. Higher = more upward shift.");
            changed |= ImGui::SliderFloat("Roll (deg)",      &roll_deg, -22.9f, 22.9f, "%.1f");
            ImGui::TextDisabled("Corrects physical camera twist. Negative = CCW correction.");
            if (changed) {
                working_.camera_toe_out_rad_l   =  toe_deg  * kD2R;
                working_.camera_tilt_down_rad_l =  tilt_deg * kD2R;
                working_.camera_roll_rad_l      =  roll_deg * kD2R;
                working_.camera_toe_out_rad_r   = -toe_deg  * kD2R;
                working_.camera_tilt_down_rad_r =  tilt_deg * kD2R;
                working_.camera_roll_rad_r      = -roll_deg * kD2R;
            }
        } else {
            if (ImGui::Button("Lock eyes")) {
                // Re-lock: mirror left eye to right eye.
                working_.camera_toe_out_rad_r   = -working_.camera_toe_out_rad_l;
                working_.camera_tilt_down_rad_r =  working_.camera_tilt_down_rad_l;
                working_.camera_roll_rad_r      = -working_.camera_roll_rad_l;
                working_.camera_eyes_linked = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Eyes adjust independently. Re-lock mirrors left to right.");
            ImGui::Spacing();

            if (ImGui::BeginTable("eye_sliders", 3,
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 0.28f);
                ImGui::TableSetupColumn("Left eye", ImGuiTableColumnFlags_WidthStretch, 0.36f);
                ImGui::TableSetupColumn("Right eye", ImGuiTableColumnFlags_WidthStretch, 0.36f);
                ImGui::TableHeadersRow();

                // Toe-out row
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Toe-out (deg)");
                ImGui::TableSetColumnIndex(1);
                { float v = working_.camera_toe_out_rad_l * kR2D;
                  ImGui::SetNextItemWidth(-1.f);
                  if (ImGui::SliderFloat("##toe_l", &v, -45.8f, 45.8f, "%.1f"))
                      working_.camera_toe_out_rad_l = v * kD2R; }
                ImGui::TableSetColumnIndex(2);
                { float v = working_.camera_toe_out_rad_r * kR2D;
                  ImGui::SetNextItemWidth(-1.f);
                  if (ImGui::SliderFloat("##toe_r", &v, -45.8f, 45.8f, "%.1f"))
                      working_.camera_toe_out_rad_r = v * kD2R; }

                // Tilt down row
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Tilt down (deg)");
                ImGui::TableSetColumnIndex(1);
                { float v = working_.camera_tilt_down_rad_l * kR2D;
                  ImGui::SetNextItemWidth(-1.f);
                  if (ImGui::SliderFloat("##tilt_l", &v, 0.0f, 45.8f, "%.1f"))
                      working_.camera_tilt_down_rad_l = v * kD2R; }
                ImGui::TableSetColumnIndex(2);
                { float v = working_.camera_tilt_down_rad_r * kR2D;
                  ImGui::SetNextItemWidth(-1.f);
                  if (ImGui::SliderFloat("##tilt_r", &v, 0.0f, 45.8f, "%.1f"))
                      working_.camera_tilt_down_rad_r = v * kD2R; }

                // Roll row
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Roll (deg)");
                ImGui::TableSetColumnIndex(1);
                { float v = working_.camera_roll_rad_l * kR2D;
                  ImGui::SetNextItemWidth(-1.f);
                  if (ImGui::SliderFloat("##roll_l", &v, -22.9f, 22.9f, "%.1f"))
                      working_.camera_roll_rad_l = v * kD2R; }
                ImGui::TableSetColumnIndex(2);
                { float v = working_.camera_roll_rad_r * kR2D;
                  ImGui::SetNextItemWidth(-1.f);
                  if (ImGui::SliderFloat("##roll_r", &v, -22.9f, 22.9f, "%.1f"))
                      working_.camera_roll_rad_r = v * kD2R; }

                ImGui::EndTable();
            }
            ImGui::TextDisabled("Toe/roll: positive = camera rotates outward. Tilt: positive = camera tilts down.");
        }
    }
    ImGui::Spacing();

}

void ConfigWindow::draw_binding_capture_button() {
    if (capturing_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Cancel")) {
            capturer_.stop();
            capturing_ = false;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "Press your button now.");
    } else {
        if (ImGui::Button("Set binding...")) {
            capturer_.start();
            capturing_ = true;
        }
        ImGui::TextDisabled("Keyboard keys, XInput gamepad buttons,");
        ImGui::TextDisabled("and DirectInput HOTAS / joystick buttons are all supported.");
    }
}

void ConfigWindow::draw_about_panel() {
    ImGui::SeparatorText("About");
    ImGui::TextWrapped(
        "This configurator writes settings to a JSON file the layer reads "
        "once at start-up. Changes take effect when you next launch your sim.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Passthrough button binding supports keyboard keys, Xbox/gamepad "
        "buttons (XInput), and any HOTAS or joystick (DirectInput) — no "
        "intermediate mapping required.");
    ImGui::Spacing();

    // Lazy-load intrinsics from the calibration dump written by the layer DLL.
    if (intrinsics_text_.empty()) {
        const auto dump_path = config_file_path().parent_path() / "calibration_dump.txt";
        std::ifstream f(dump_path);
        if (f) {
            std::ostringstream ss;
            std::string line;
            bool in_section = false;
            while (std::getline(f, line)) {
                if (line.find("=== Factory Intrinsics ===") != std::string::npos) {
                    in_section = true;
                }
                if (in_section) {
                    ss << line << "\n";
                    // Stop after the two eye data rows (header + 2 data lines = 3 lines after title)
                    if (in_section && line.size() > 0 && line[0] == '1') break;
                }
            }
            intrinsics_text_ = ss.str();
            if (intrinsics_text_.empty())
                intrinsics_text_ = "Values available after first sim run.";
        } else {
            intrinsics_text_ = "Values available after first sim run.";
        }
    }

    ImGui::SeparatorText("Headset intrinsics");
    ImGui::TextDisabled("Read from PSVR2 driver at last session start.");
    ImGui::Spacing();
    ImGui::TextUnformatted(intrinsics_text_.c_str());
}


}  // namespace psvr2pt
