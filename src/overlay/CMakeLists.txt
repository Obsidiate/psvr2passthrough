# PSVR2PassthroughOverlay.exe — standalone OpenVR overlay app.
#
# Reuses the runtime-agnostic core/render/config libs (no OpenXR). Renders the
# undistorted stereo passthrough into a single side-by-side texture and submits
# it to SteamVR as a head-locked overlay, so it composites over native OpenVR
# games (Half-Life: Alyx, No Man's Sky) that an OpenXR layer can never attach to.

# OpenVR ships from vcpkg as plain headers + import lib (no CMake config
# package), so locate them directly. Resolve the release and debug import libs
# separately and expose them through a config-aware IMPORTED target, so a Release
# build links the release lib and a Debug build links the debug lib (vcpkg lays
# them out under lib/ and debug/lib/ respectively).
find_path(OPENVR_INCLUDE_DIR NAMES openvr.h)

# vcpkg keeps release libs under <triplet>/lib and debug libs under
# <triplet>/debug/lib. find_library's default search treats both as candidates,
# so anchor each lookup to its exact root (NO_DEFAULT_PATH) to avoid a Release
# build picking up the debug import lib.
set(_vcpkg_root "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_library(OPENVR_LIBRARY_RELEASE NAMES openvr_api
    PATHS "${_vcpkg_root}/lib"       NO_DEFAULT_PATH)
find_library(OPENVR_LIBRARY_DEBUG   NAMES openvr_api
    PATHS "${_vcpkg_root}/debug/lib" NO_DEFAULT_PATH)

if(NOT OPENVR_INCLUDE_DIR OR NOT OPENVR_LIBRARY_RELEASE)
    message(FATAL_ERROR
        "OpenVR not found. Ensure the 'openvr' vcpkg port is installed "
        "(it is listed in vcpkg.json).")
endif()

add_library(openvr::openvr UNKNOWN IMPORTED)
set_target_properties(openvr::openvr PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${OPENVR_INCLUDE_DIR}"
    IMPORTED_LOCATION "${OPENVR_LIBRARY_RELEASE}")
if(OPENVR_LIBRARY_DEBUG)
    set_target_properties(openvr::openvr PROPERTIES
        IMPORTED_LOCATION_DEBUG "${OPENVR_LIBRARY_DEBUG}")
endif()

add_executable(PSVR2PassthroughOverlay WIN32
    main.cpp
    overlay_app.cpp
    overlay_app.h
)

target_link_libraries(PSVR2PassthroughOverlay
    PRIVATE
        psvr2pt_core
        psvr2pt_config
        psvr2pt_render
        openvr::openvr
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

# Ship the SteamVR app manifest + IVRInput action/binding JSON next to the exe.
# --register passes SteamVR an absolute path resolved from the running exe, so
# the manifests must live alongside it (build tree and release zip alike).
set(OVERLAY_MANIFESTS
    ${CMAKE_CURRENT_SOURCE_DIR}/manifests/manifest.vrmanifest
    ${CMAKE_CURRENT_SOURCE_DIR}/manifests/actions.json
    ${CMAKE_CURRENT_SOURCE_DIR}/manifests/bindings_psvr2.json
)
foreach(_m ${OVERLAY_MANIFESTS})
    add_custom_command(TARGET PSVR2PassthroughOverlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_m} $<TARGET_FILE_DIR:PSVR2PassthroughOverlay>
        COMMENT "Copying overlay manifest: ${_m}"
        VERBATIM)
endforeach()
