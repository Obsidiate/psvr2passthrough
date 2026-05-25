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
    float brightness           = 1.0f;   // luminance multiplier [0.5..4.0]

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

    // Camera stereo geometry corrections (see handoff_handtracking.md for details).
    float camera_toe_out_rad   = 0.32f;
    float camera_tilt_down_rad = 0.48f;
    float camera_roll_rad      = -0.1745f;


};

// JSON serialisation.
std::string config_to_json(const Config& c);
bool         config_from_json(const std::string& json, Config& out);

// On-disk location: %LOCALAPPDATA%\PSVR2PassthroughLayer\config.json
std::filesystem::path config_file_path();

Config load_config();
bool   save_config(const Config& c);

}  // namespace psvr2pt
