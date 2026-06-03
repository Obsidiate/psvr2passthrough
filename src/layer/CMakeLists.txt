add_library(PSVR2PassthroughLayer SHARED
    layer_main.cpp
    layer_dispatch.cpp
    layer_dispatch.h
    layer_session.cpp
    layer_session.h
    dllmain.cpp
    PSVR2PassthroughLayer.def
)

target_link_libraries(PSVR2PassthroughLayer
    PRIVATE
        psvr2pt_core
        psvr2pt_config
        psvr2pt_render
        ml
        OpenXR::headers
        OpenXR::openxr_loader
        d3d11
)

target_compile_definitions(PSVR2PassthroughLayer
    PRIVATE
        XR_USE_GRAPHICS_API_D3D11
        XR_USE_PLATFORM_WIN32
        NOMINMAX
)

set_target_properties(PSVR2PassthroughLayer PROPERTIES
    OUTPUT_NAME "PSVR2PassthroughLayer"
    PREFIX ""
)
# -----------------------------------------------------------------------------
# Post-build steps for the layer DLL.
#
# Both must live here, not in the top-level CMakeLists, because:
#   1. add_custom_command(TARGET ...) is directory-scoped to where the target
#      was defined (i.e. this file).
#   2. We need $<CONFIG> and $<TARGET_FILE:...> generator expressions, which
#      only get resolved at build time — configure_file() runs too early.
# -----------------------------------------------------------------------------

# 0. Copy the ONNX Runtime DLL next to the layer DLL so it is found at runtime.
find_package(onnxruntime CONFIG REQUIRED)
add_custom_command(TARGET PSVR2PassthroughLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:onnxruntime::onnxruntime>
        $<TARGET_FILE_DIR:PSVR2PassthroughLayer>
    COMMENT "Copying onnxruntime DLL alongside layer DLL"
)

# 1. Generate the OpenXR API layer manifest next to the DLL, with the absolute
#    DLL path baked into the library_path field.
add_custom_command(
    TARGET PSVR2PassthroughLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE=${CMAKE_SOURCE_DIR}/cmake/PSVR2PassthroughLayer.json.in
        -DDEST=$<TARGET_FILE_DIR:PSVR2PassthroughLayer>/PSVR2PassthroughLayer-051a.json
        -DLIBRARY_PATH=$<TARGET_FILE:PSVR2PassthroughLayer>
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateManifest.cmake
    COMMENT "Generating OpenXR API layer manifest"
    VERBATIM
)
