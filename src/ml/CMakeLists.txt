find_package(onnxruntime CONFIG REQUIRED)   # vcpkg: onnxruntime
find_package(Stb REQUIRED)                  # vcpkg: stb (for stb_image_write in FrameRecorder)

add_library(ml STATIC
    FrameRecorder.h
    FrameRecorder.cpp
    HandDetection.h
    BespokeDetector.h
    BespokeDetector.cpp
)

target_include_directories(ml
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${Stb_INCLUDE_DIR}
)

target_link_libraries(ml
    PUBLIC  psvr2pt_core
    PRIVATE onnxruntime::onnxruntime
            spdlog::spdlog
)

target_compile_features(ml PUBLIC cxx_std_20)
