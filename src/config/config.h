#pragma once

#include "input_binding.h"

#include <string>
#include <filesystem>

namespace psvr2pt {

struct Config {
    // Master switch — if false the layer goes inert without unregistering.
    bool  enabled              = true;

    // Passthrough appearance.
    float global_alpha         = 1.0f;   // transparency [0..1] — 1 = fully opaque
    bool  brightness_enabled   = true;
    float brightness           = 1.3f;   // luminance multiplier [0.5..4.0]
    bool  contrast_enabled     = true;
    float contrast             = 1.1f;   // tonal contrast around midpoint [0.5..3.0]
    bool  enhancements_enabled = true;
    float unsharp_amount       = 0.3f;   // unsharp mask strength [0.0..1.0]
    float unsharp_radius       = 1.5f;   // unsharp mask blur radius in camera pixels [0.5..4.0]

    // Debug override — ignores binding entirely, passthrough always visible.
    bool force_passthrough_on = false;

    // Passthrough button binding.
    // toggle_mode = true:  press once to show, press again to hide.
    // toggle_mode = false: visible only while button is held.
    // No binding set + force_passthrough_on false = passthrough hidden (safe default).
    bool               toggle_mode          = false;
    PassthroughBinding passthrough_binding  = {};

    // Undistortion.
    bool  apply_undistortion   = true;
    float zoom_factor          = 1.0f;

    // Camera stereo geometry corrections.
    // When camera_eyes_linked is true the right-eye values are derived from the
    // left-eye values (toe/roll negated, tilt copied) and are not saved separately.
    bool  camera_eyes_linked    = true;
    float camera_toe_out_rad_l  =  0.32f;
    float camera_tilt_down_rad_l =  0.48f;
    float camera_roll_rad_l     = -0.1745f;
    float camera_toe_out_rad_r  = -0.32f;
    float camera_tilt_down_rad_r =  0.48f;
    float camera_roll_rad_r     =  0.1745f;
};

// JSON serialisation.
std::string config_to_json(const Config& c);
bool         config_from_json(const std::string& json, Config& out);

// On-disk location: %LOCALAPPDATA%\PSVR2PassthroughLayer\config.json
std::filesystem::path config_file_path();

Config load_config();
bool   save_config(const Config& c);

}  // namespace psvr2pt
