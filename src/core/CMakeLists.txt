add_library(psvr2pt_core STATIC
    shared_memory.cpp
    shared_memory.h
    distortion.cpp
    distortion.h
    fov.cpp
    fov.h
    camera_source.cpp
    camera_source.h
    frame.h
    logging.cpp
    logging.h
    input_binding.cpp
    input_binding.h
)

target_include_directories(psvr2pt_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(psvr2pt_core PUBLIC
    spdlog::spdlog
    fmt::fmt
    dinput8
    dxguid
    xinput
    rpcrt4
)

target_compile_definitions(psvr2pt_core PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
