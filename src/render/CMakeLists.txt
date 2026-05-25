add_library(psvr2pt_render STATIC
    compositor.cpp
    compositor.h
    d3d_helpers.h
)

target_include_directories(psvr2pt_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(psvr2pt_render
    PUBLIC
        psvr2pt_core
        psvr2pt_config
    PRIVATE
        Microsoft::DirectXTK
        d3d11
        dxgi
        d3dcompiler
)
