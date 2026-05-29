find_package(onnxruntime CONFIG REQUIRED)

add_library(psvr2pt_handtrack STATIC
    hand_tracker.h
    hand_tracker.cpp
    bc4_decode.h
    palm_post.h
)

target_include_directories(psvr2pt_handtrack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(psvr2pt_handtrack
    PUBLIC
        psvr2pt_core
    PRIVATE
        onnxruntime::onnxruntime
        spdlog::spdlog
)

target_compile_definitions(psvr2pt_handtrack PRIVATE NOMINMAX)