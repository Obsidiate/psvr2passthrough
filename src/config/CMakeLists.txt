add_library(psvr2pt_config STATIC
    config.cpp
    config.h
)

target_include_directories(psvr2pt_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(psvr2pt_config PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(psvr2pt_config PUBLIC
    psvr2pt_core
    Shlwapi.lib
)
