#include "logging.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#include <filesystem>
#include <shlobj.h>
#include <windows.h>

namespace psvr2pt {

std::filesystem::path get_layer_data_dir() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        std::filesystem::path dir = path;
        CoTaskMemFree(path);
        dir /= "PSVR2PassthroughLayer";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    return std::filesystem::temp_directory_path();
}

void init_logging() {
    static bool initialised = false;
    if (initialised) return;
    initialised = true;

    try {
        const auto log_path = get_layer_data_dir() / "layer.log";

        std::vector<spdlog::sink_ptr> sinks;
        sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            log_path.string(), /*truncate=*/true));
#ifdef _DEBUG
        sinks.emplace_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
#endif
        auto logger = std::make_shared<spdlog::logger>("psvr2pt", sinks.begin(), sinks.end());
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
    } catch (...) {
        // Logging must never throw out of layer code.
    }
}

}  // namespace psvr2pt
