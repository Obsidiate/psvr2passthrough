#include <windows.h>

#include "logging.h"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            psvr2pt::init_logging();
            PT_LOG_INFO("PSVR2 Passthrough Layer DLL_PROCESS_ATTACH");
            break;
        case DLL_PROCESS_DETACH:
            PT_LOG_INFO("PSVR2 Passthrough Layer DLL_PROCESS_DETACH");
            break;
    }
    return TRUE;
}
