add_executable(PSVR2PassthroughConfig WIN32
    main.cpp
    config_window.cpp
    config_window.h
    update_checker.cpp
    update_checker.h
    version.h
)

target_link_libraries(PSVR2PassthroughConfig PRIVATE
    psvr2pt_config
    psvr2pt_core
    imgui::imgui
    d3d11
    dxgi
    d3dcompiler
    winhttp
    shell32
)

target_compile_definitions(PSVR2PassthroughConfig PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

set_target_properties(PSVR2PassthroughConfig PROPERTIES
    OUTPUT_NAME "PSVR2PassthroughConfig"
)
