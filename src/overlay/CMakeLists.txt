# PSVR2PassthroughOverlay.exe — standalone OpenVR overlay app.
#
# Reuses the runtime-agnostic core/render/config libs (no OpenXR). Renders the
# undistorted stereo passthrough into a single side-by-side texture and submits
# it to SteamVR as a head-locked overlay, so it composites over native OpenVR
# games (Half-Life: Alyx, No Man's Sky) that an OpenXR layer can never attach to.

# OpenVR ships from vcpkg as plain headers + import lib (no CMake config
# package), so locate them directly.
find_path(OPENVR_INCLUDE_DIR NAMES openvr.h)
find_library(OPENVR_LIBRARY NAMES openvr_api)

if(NOT OPENVR_INCLUDE_DIR OR NOT OPENVR_LIBRARY)
    message(FATAL_ERROR
        "OpenVR not found. Ensure the 'openvr' vcpkg port is installed "
        "(it is listed in vcpkg.json).")
endif()

add_executable(PSVR2PassthroughOverlay WIN32
    main.cpp
    overlay_app.cpp
    overlay_app.h
)

target_include_directories(PSVR2PassthroughOverlay PRIVATE ${OPENVR_INCLUDE_DIR})

target_link_libraries(PSVR2PassthroughOverlay
    PRIVATE
        psvr2pt_core
        psvr2pt_config
        psvr2pt_render
        ${OPENVR_LIBRARY}
        d3d11
        dxgi
)

target_compile_definitions(PSVR2PassthroughOverlay
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

set_target_properties(PSVR2PassthroughOverlay PROPERTIES
    OUTPUT_NAME "PSVR2PassthroughOverlay"
)
