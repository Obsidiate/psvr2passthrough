// PSVR2PassthroughOverlay.exe entry point.
//
// Windowless background process: brings up an OpenVR overlay that composites the
// PSVR2 passthrough over any SteamVR game, including native OpenVR titles the
// OpenXR layer cannot attach to.

#include <windows.h>   // WIN32_LEAN_AND_MEAN / NOMINMAX come from the target defs

#include <string>
#include <vector>

#include "overlay_app.h"
#include "logging.h"

using namespace psvr2pt;

namespace {

// CLI flags. --register / --unregister are wired up in Commit 4 (manifest +
// autolaunch); recognised here now so the surface is stable.
enum class CliAction { Run, Register, Unregister };

CliAction parse_cli(const std::wstring& cmdline) {
    if (cmdline.find(L"--unregister") != std::wstring::npos) return CliAction::Unregister;
    if (cmdline.find(L"--register")   != std::wstring::npos) return CliAction::Register;
    return CliAction::Run;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    init_logging();

    const CliAction action = parse_cli(lpCmdLine ? lpCmdLine : L"");
    switch (action) {
        case CliAction::Register:
            PT_LOG_INFO("--register requested (implemented in Commit 4).");
            return 0;
        case CliAction::Unregister:
            PT_LOG_INFO("--unregister requested (implemented in Commit 4).");
            return 0;
        case CliAction::Run:
            break;
    }

    OverlayApp app;
    if (!app.initialise()) {
        PT_LOG_ERROR("Overlay failed to initialise; exiting.");
        return 1;
    }
    app.run();
    app.shutdown();
    return 0;
}
