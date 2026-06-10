#include "config.h"

#include <fstream>
#include <sstream>
#include <cctype>

#include <windows.h>
#include <shlobj.h>

namespace psvr2pt {

// ---------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------

std::filesystem::path config_file_path() {
    PWSTR p = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        dir = p;
        CoTaskMemFree(p);
        dir /= "PSVR2PassthroughLayer";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    } else {
        dir = std::filesystem::temp_directory_path();
    }
    return dir / "config.json";
}

// ---------------------------------------------------------------------------
// Hand-rolled JSON serialisation (flat object, no nested structures).
// PassthroughBinding is stored as flat fields prefixed "pt_".
// ---------------------------------------------------------------------------

namespace {

std::string fmt_float(float v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

const char* skip_ws(const char* s) {
    while (*s && std::isspace(static_cast<unsigned char>(*s))) ++s;
    return s;
}

bool parse_kv(const char* s, const char*& after, std::string& key, std::string& value_raw) {
    s = skip_ws(s);
    if (*s != '"') return false;
    ++s;
    key.clear();
    while (*s && *s != '"') key.push_back(*s++);
    if (*s != '"') return false;
    ++s;
    s = skip_ws(s);
    if (*s != ':') return false;
    ++s;
    s = skip_ws(s);
    value_raw.clear();
    if (*s == '"') {
        ++s;
        while (*s && *s != '"') value_raw.push_back(*s++);
        if (*s != '"') return false;
        ++s;
    } else {
        while (*s && *s != ',' && *s != '}' && !std::isspace(static_cast<unsigned char>(*s))) {
            value_raw.push_back(*s++);
        }
    }
    after = s;
    return true;
}

}  // namespace

std::string config_to_json(const Config& c) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"enabled\":              " << (c.enabled ? "true" : "false") << ",\n";
    os << "  \"force_passthrough_on\": " << (c.force_passthrough_on ? "true" : "false") << ",\n";
    os << "  \"global_alpha\":         " << fmt_float(c.global_alpha) << ",\n";
    os << "  \"brightness_enabled\":   " << (c.brightness_enabled   ? "true" : "false") << ",\n";
    os << "  \"brightness\":           " << fmt_float(c.brightness) << ",\n";
    os << "  \"contrast_enabled\":     " << (c.contrast_enabled     ? "true" : "false") << ",\n";
    os << "  \"contrast\":             " << fmt_float(c.contrast) << ",\n";
    os << "  \"enhancements_enabled\": " << (c.enhancements_enabled ? "true" : "false") << ",\n";
    os << "  \"unsharp_amount\":       " << fmt_float(c.unsharp_amount) << ",\n";
    os << "  \"unsharp_radius\":       " << fmt_float(c.unsharp_radius) << ",\n";
    os << "  \"toggle_mode\":          " << (c.toggle_mode ? "true" : "false") << ",\n";
    // PassthroughBinding as flat fields.
    os << "  \"pt_type\":              " << static_cast<int>(c.passthrough_binding.type) << ",\n";
    os << "  \"pt_vk\":               " << c.passthrough_binding.vk_code << ",\n";
    os << "  \"pt_xi_ctrl\":          " << c.passthrough_binding.xinput_controller << ",\n";
    os << "  \"pt_xi_btn\":           " << c.passthrough_binding.xinput_button_mask << ",\n";
    os << "  \"pt_di_guid\":          \"" << c.passthrough_binding.dinput_device_guid << "\",\n";
    os << "  \"pt_di_btn\":           " << c.passthrough_binding.dinput_button_index << ",\n";
    os << "  \"pt_di_name\":          \"" << c.passthrough_binding.dinput_device_name << "\",\n";
    os << "  \"apply_undistortion\":    " << (c.apply_undistortion ? "true" : "false") << ",\n";
    os << "  \"zoom_factor\":           " << fmt_float(c.zoom_factor) << ",\n";
    os << "  \"overlay_distance_m\":    " << fmt_float(c.overlay_distance_m) << ",\n";
    os << "  \"overlay_alpha\":         " << fmt_float(c.overlay_alpha) << ",\n";
    os << "  \"reprojection_enabled\":     " << (c.reprojection_enabled ? "true" : "false") << ",\n";
    os << "  \"camera_latency_offset_ns\": " << c.camera_latency_offset_ns << ",\n";
    os << "  \"debug_reprojection_stats\": " << (c.debug_reprojection_stats ? "true" : "false") << ",\n";
    os << "  \"ipd_correction_enabled\":   " << (c.ipd_correction_enabled ? "true" : "false") << ",\n";
    os << "  \"camera_separation_mm\":     " << fmt_float(c.camera_separation_mm) << ",\n";
    os << "  \"camera_eyes_linked\":       " << (c.camera_eyes_linked ? "true" : "false") << ",\n";
    os << "  \"camera_toe_out_rad_l\":  " << fmt_float(c.camera_toe_out_rad_l) << ",\n";
    os << "  \"camera_tilt_down_rad_l\":" << fmt_float(c.camera_tilt_down_rad_l) << ",\n";
    os << "  \"camera_roll_rad_l\":     " << fmt_float(c.camera_roll_rad_l) << ",\n";
    os << "  \"camera_toe_out_rad_r\":  " << fmt_float(c.camera_toe_out_rad_r) << ",\n";
    os << "  \"camera_tilt_down_rad_r\":" << fmt_float(c.camera_tilt_down_rad_r) << ",\n";
    os << "  \"camera_roll_rad_r\":     " << fmt_float(c.camera_roll_rad_r) << "\n";
    os << "}\n";
    return os.str();
}

bool config_from_json(const std::string& json, Config& out) {
    out = Config{};
    const char* s = json.c_str();
    s = skip_ws(s);
    if (*s != '{') return false;
    ++s;
    while (*s) {
        s = skip_ws(s);
        if (*s == '}') { ++s; break; }
        std::string k, v;
        const char* after = nullptr;
        if (!parse_kv(s, after, k, v)) return false;
        s = after;

        try {
            if      (k == "enabled")               out.enabled               = (v == "true");
            else if (k == "force_passthrough_on") out.force_passthrough_on  = (v == "true");
            else if (k == "global_alpha")         out.global_alpha          = std::stof(v);
            else if (k == "brightness_enabled")   out.brightness_enabled    = (v == "true");
            else if (k == "brightness")           out.brightness            = std::stof(v);
            else if (k == "contrast_enabled")     out.contrast_enabled      = (v == "true");
            else if (k == "contrast")             out.contrast              = std::stof(v);
            else if (k == "enhancements_enabled") out.enhancements_enabled  = (v == "true");
            else if (k == "unsharp_amount")       out.unsharp_amount        = std::stof(v);
            else if (k == "unsharp_radius")       out.unsharp_radius        = std::stof(v);
            else if (k == "toggle_mode")         out.toggle_mode           = (v == "true");
            else if (k == "pt_type")             out.passthrough_binding.type
                                                     = static_cast<BindingType>(std::stoi(v));
            else if (k == "pt_vk")              out.passthrough_binding.vk_code              = std::stoi(v);
            else if (k == "pt_xi_ctrl")         out.passthrough_binding.xinput_controller    = std::stoi(v);
            else if (k == "pt_xi_btn")          out.passthrough_binding.xinput_button_mask   = static_cast<unsigned>(std::stoul(v));
            else if (k == "pt_di_guid")         out.passthrough_binding.dinput_device_guid   = v;
            else if (k == "pt_di_btn")          out.passthrough_binding.dinput_button_index  = std::stoi(v);
            else if (k == "pt_di_name")         out.passthrough_binding.dinput_device_name   = v;
            else if (k == "apply_undistortion")   out.apply_undistortion     = (v == "true");
            else if (k == "zoom_factor")          out.zoom_factor            = std::stof(v);
            else if (k == "overlay_distance_m")   out.overlay_distance_m     = std::stof(v);
            else if (k == "overlay_alpha")        out.overlay_alpha          = std::stof(v);
            else if (k == "reprojection_enabled")   out.reprojection_enabled   = (v == "true");
            else if (k == "ipd_correction_enabled") out.ipd_correction_enabled = (v == "true");
            else if (k == "camera_separation_mm")   out.camera_separation_mm   = std::stof(v);
            else if (k == "camera_eyes_linked")     out.camera_eyes_linked     = (v == "true");
            else if (k == "camera_toe_out_rad_l") out.camera_toe_out_rad_l   = std::stof(v);
            else if (k == "camera_tilt_down_rad_l") out.camera_tilt_down_rad_l = std::stof(v);
            else if (k == "camera_roll_rad_l")    out.camera_roll_rad_l      = std::stof(v);
            else if (k == "camera_toe_out_rad_r") out.camera_toe_out_rad_r   = std::stof(v);
            else if (k == "camera_tilt_down_rad_r") out.camera_tilt_down_rad_r = std::stof(v);
            else if (k == "camera_roll_rad_r")      out.camera_roll_rad_r         = std::stof(v);
            else if (k == "camera_latency_offset_ns") out.camera_latency_offset_ns = std::stoll(v);
            else if (k == "debug_reprojection_stats") out.debug_reprojection_stats = (v == "true");
            // Legacy single-eye keys — load into left eye; right eye mirrors on next save.
            else if (k == "camera_toe_out_rad")   { out.camera_toe_out_rad_l  =  std::stof(v);
                                                    out.camera_toe_out_rad_r  = -std::stof(v); }
            else if (k == "camera_tilt_down_rad") { out.camera_tilt_down_rad_l = std::stof(v);
                                                    out.camera_tilt_down_rad_r = std::stof(v); }
            else if (k == "camera_roll_rad")      { out.camera_roll_rad_l     =  std::stof(v);
                                                    out.camera_roll_rad_r     = -std::stof(v); }
            // Silently ignore unknown/removed keys (e.g. old detection/recenter fields).
        } catch (...) {}

        s = skip_ws(s);
        if (*s == ',') ++s;
    }

    if (out.global_alpha < 0.f)  out.global_alpha = 0.f;
    if (out.global_alpha > 1.f)  out.global_alpha = 1.f;
    if (out.brightness     < 0.5f) out.brightness     = 0.5f;
    if (out.brightness     > 4.0f) out.brightness     = 4.0f;
    if (out.contrast       < 0.5f) out.contrast       = 0.5f;
    if (out.contrast       > 3.0f) out.contrast       = 3.0f;
    if (out.unsharp_amount < 0.0f) out.unsharp_amount = 0.0f;
    if (out.unsharp_amount > 1.0f) out.unsharp_amount = 1.0f;
    if (out.unsharp_radius < 0.5f) out.unsharp_radius = 0.5f;
    if (out.unsharp_radius > 4.0f) out.unsharp_radius = 4.0f;
    if (out.zoom_factor    < 0.5f) out.zoom_factor    = 0.5f;
    if (out.zoom_factor    > 4.0f) out.zoom_factor    = 4.0f;
    if (out.overlay_distance_m < 0.3f) out.overlay_distance_m = 0.3f;
    if (out.overlay_distance_m > 5.0f) out.overlay_distance_m = 5.0f;
    if (out.overlay_alpha      < 0.1f) out.overlay_alpha      = 0.1f;
    if (out.overlay_alpha      > 1.0f) out.overlay_alpha      = 1.0f;
    if (out.camera_separation_mm < 40.f)  out.camera_separation_mm = 40.f;
    if (out.camera_separation_mm > 120.f) out.camera_separation_mm = 120.f;
    if (out.camera_latency_offset_ns < 0)           out.camera_latency_offset_ns = 0;
    if (out.camera_latency_offset_ns > 100'000'000) out.camera_latency_offset_ns = 100'000'000;
    return true;
}

Config load_config() {
    Config c;
    std::ifstream f(config_file_path(), std::ios::binary);
    if (!f) return c;
    std::stringstream ss;
    ss << f.rdbuf();
    config_from_json(ss.str(), c);
    return c;
}

bool save_config(const Config& c) {
    const auto final_path = config_file_path();
    const auto tmp_path   = final_path.string() + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << config_to_json(c);
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(final_path, ec);
        std::filesystem::rename(tmp_path, final_path, ec);
    }
    return !ec;
}

}  // namespace psvr2pt
