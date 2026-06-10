#pragma once

#include <spdlog/spdlog.h>
#include <filesystem>
#include <memory>

namespace psvr2pt {

// Initialise once at layer load. Writes to
// %LOCALAPPDATA%\PSVR2PassthroughLayer\<log_filename> plus, in debug builds,
// OutputDebugString. The overlay passes its own filename so it doesn't truncate
// the layer's log when both run.
void init_logging(const char* log_filename = "layer.log");

// Returns %LOCALAPPDATA%\PSVR2PassthroughLayer\ (created if absent).
std::filesystem::path get_layer_data_dir();

inline auto& log() {
    static auto logger = spdlog::default_logger();
    return *logger;
}

}  // namespace psvr2pt

#define PT_LOG_TRACE(...)  ::psvr2pt::log().trace(__VA_ARGS__)
#define PT_LOG_DEBUG(...)  ::psvr2pt::log().debug(__VA_ARGS__)
#define PT_LOG_INFO(...)   ::psvr2pt::log().info(__VA_ARGS__)
#define PT_LOG_WARN(...)   ::psvr2pt::log().warn(__VA_ARGS__)
#define PT_LOG_ERROR(...)  ::psvr2pt::log().error(__VA_ARGS__)
