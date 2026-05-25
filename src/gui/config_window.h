#pragma once

#include "config.h"
#include "input_binding.h"

#include <d3d11.h>
#include <memory>
#include <string>

namespace psvr2pt {

class ConfigWindow {
public:
    ConfigWindow();
    ~ConfigWindow();

    void initialise(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void shutdown();

    // Called once per frame between ImGui::NewFrame() and ImGui::Render().
    void draw();

private:
    void draw_main_panel();
    void draw_about_panel();
    void draw_binding_capture_button();

    Config working_;
    Config on_disk_;
    bool   dirty_         = false;
    bool   saved_recently_= false;
    double saved_at_      = 0.0;

    // Binding capture state.
    bool               capturing_    = false;
    std::string        capture_label_;          // "Listening..." countdown label
    BindingCapturer    capturer_;

    ID3D11Device*        device_ = nullptr;
    ID3D11DeviceContext* ctx_    = nullptr;
};

}  // namespace psvr2pt
