#pragma once

#include "config.h"
#include "distortion.h"
#include "frame.h"
#include "d3d_helpers.h"

#include <DirectXMath.h>
#include <array>

namespace psvr2pt {

struct EyeOutput {
    ComPtr<ID3D11Texture2D>          texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView>   rtv;
    UINT width  = 0;
    UINT height = 0;
};

// Runtime-tunable compositor parameters.
struct CompositorConfig {
    float global_alpha           = 1.0f;
    float brightness             = 1.0f;
    bool  apply_undistortion      = true;
    float zoom_factor             = 1.0f;
    float camera_toe_out_rad_l   =  0.32f;
    float camera_tilt_down_rad_l =  0.48f;
    float camera_roll_rad_l      = -0.1745f;
    float camera_toe_out_rad_r   = -0.32f;
    float camera_tilt_down_rad_r =  0.48f;
    float camera_roll_rad_r      =  0.1745f;
};

class Compositor {
public:
    Compositor();
    ~Compositor();

    bool initialise(ID3D11Device* device,
                    UINT eye_width, UINT eye_height,
                    const CameraIntrinsics intrinsics[2],
                    const CameraParameters params[2]);

    void upload_frame(const StereoFrame& frame);
    void render(const CompositorConfig& cfg);

    const EyeOutput& eye(int i) const { return eyes_[i]; }

private:
    void create_pipeline_();
    void create_camera_textures_();
    void create_eye_targets_(UINT w, UINT h);
    void rebuild_mesh_(int eye, float zoom, bool apply_undistortion,
                       float signed_toe_out_rad, float tilt_down_rad, float signed_roll_rad);

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> ctx_;

    ComPtr<ID3D11VertexShader>  vs_;
    ComPtr<ID3D11PixelShader>   ps_;
    ComPtr<ID3D11InputLayout>   layout_;
    ComPtr<ID3D11SamplerState>  sampler_;
    ComPtr<ID3D11BlendState>    blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Buffer>        cb_per_eye_;

    struct EyeMesh {
        ComPtr<ID3D11Buffer> vb;
        ComPtr<ID3D11Buffer> ib;
        UINT index_count = 0;
    };
    EyeMesh meshes_[2];

    ComPtr<ID3D11Texture2D>          cam_tex_[2];
    ComPtr<ID3D11ShaderResourceView> cam_srv_[2];

    EyeOutput eyes_[2];

    CameraIntrinsics intrinsics_[2]{};
    CameraParameters params_[2]{};
    UINT eye_w_ = 0;
    UINT eye_h_ = 0;
    bool ready_ = false;
};

}  // namespace psvr2pt
