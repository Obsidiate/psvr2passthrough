#include "compositor.h"
#include "distortion.h"
#include "logging.h"

#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>

namespace psvr2pt {

namespace {

constexpr const char* kVertexShader = R"HLSL(
struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut main(VSIn input)
{
    VSOut o;
    o.pos = float4(input.pos, 1.0);
    o.uv  = input.uv;
    return o;
}
)HLSL";

// Full-frame passthrough with brightness, contrast, and unsharp masking.
// cb0: params  = {global_alpha, brightness, contrast, unsharp_amount}
//      params2 = {unsharp_step (radius/tex_width in UV space), 0, 0, 0}
constexpr const char* kPixelShader = R"HLSL(
Texture2D    src     : register(t0);
SamplerState smp     : register(s0);

cbuffer Params : register(b0) {
    float4 params;   // x=alpha, y=brightness, z=contrast, w=unsharp_amount
    float4 params2;  // x=unsharp_step
};

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    float raw = src.Sample(smp, uv).r;

    // 3x3 Gaussian blur for unsharp mask (8 neighbours + weighted centre).
    float s = params2.x;
    float blurred =
        src.Sample(smp, uv + float2(-s, -s)).r * (1.0/16.0) +
        src.Sample(smp, uv + float2( 0, -s)).r * (2.0/16.0) +
        src.Sample(smp, uv + float2( s, -s)).r * (1.0/16.0) +
        src.Sample(smp, uv + float2(-s,  0)).r * (2.0/16.0) +
        raw                                    * (4.0/16.0) +
        src.Sample(smp, uv + float2( s,  0)).r * (2.0/16.0) +
        src.Sample(smp, uv + float2(-s,  s)).r * (1.0/16.0) +
        src.Sample(smp, uv + float2( 0,  s)).r * (2.0/16.0) +
        src.Sample(smp, uv + float2( s,  s)).r * (1.0/16.0);

    // Unsharp mask: amplify detail above the blur baseline.
    float g = saturate(raw + params.w * (raw - blurred));

    // Brightness then contrast around midpoint.
    g = saturate(g * params.y);
    g = saturate(0.5 + (g - 0.5) * params.z);

    float a = params.x;
    return float4(g * a, g * a, g * a, a);
}
)HLSL";

ComPtr<ID3DBlob> compile(const char* src, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, profile, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "(no log)";
        PT_LOG_ERROR("Shader compile failed ({}): {}", profile, msg);
        throw std::runtime_error("Shader compile failed");
    }
    return blob;
}

struct CBParams {
    DirectX::XMFLOAT4 params;   // x=alpha, y=brightness, z=contrast, w=unsharp_amount
    DirectX::XMFLOAT4 params2;  // x=unsharp_step
};
static_assert(sizeof(CBParams) == 32, "CBParams must be 32 bytes");

}  // namespace

Compositor::Compositor()  = default;
Compositor::~Compositor() = default;

bool Compositor::initialise(ID3D11Device* device,
                            UINT eye_width, UINT eye_height,
                            const CameraIntrinsics intrinsics[2],
                            const CameraParameters params[2]) {
    if (!device) return false;
    device_ = device;
    device_->GetImmediateContext(&ctx_);
    eye_w_ = eye_width;
    eye_h_ = eye_height;
    for (int i = 0; i < 2; ++i) {
        intrinsics_[i] = intrinsics[i];
        params_[i]     = params[i];
    }

    try {
        create_pipeline_();
        create_camera_textures_();
        create_eye_targets_(eye_w_, eye_h_);
        rebuild_mesh_(0, 1.0f, true, 0.f, 0.f, 0.f);
        rebuild_mesh_(1, 1.0f, true, 0.f, 0.f, 0.f);
    } catch (const std::exception& e) {
        PT_LOG_ERROR("Compositor::initialise failed: {}", e.what());
        return false;
    }
    ready_ = true;
    PT_LOG_INFO("Compositor ready ({}x{} per eye)", eye_w_, eye_h_);
    return true;
}

void Compositor::create_pipeline_() {
    auto vs_blob = compile(kVertexShader, "main", "vs_5_0");
    auto ps_blob = compile(kPixelShader,  "main", "ps_5_0");

    check_hr(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                         nullptr, &vs_), "CreateVertexShader");
    check_hr(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                        nullptr, &ps_), "CreatePixelShader");

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    check_hr(device_->CreateInputLayout(layout, 2, vs_blob->GetBufferPointer(),
                                        vs_blob->GetBufferSize(), &layout_), "CreateInputLayout");

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    check_hr(device_->CreateSamplerState(&sd, &sampler_), "CreateSamplerState");

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable    = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check_hr(device_->CreateBlendState(&bd, &blend_), "CreateBlendState");

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    check_hr(device_->CreateRasterizerState(&rd, &raster_), "CreateRasterizerState");

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth      = sizeof(CBParams);
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hr(device_->CreateBuffer(&cb, nullptr, &cb_per_eye_), "CreateBuffer(cb)");
}

void Compositor::create_camera_textures_() {
    for (int eye = 0; eye < 2; ++eye) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width  = kCameraWidth;
        td.Height = kCameraHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_BC4_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        check_hr(device_->CreateTexture2D(&td, nullptr, &cam_tex_[eye]), "CreateTexture2D(cam)");
        check_hr(device_->CreateShaderResourceView(cam_tex_[eye].Get(), nullptr, &cam_srv_[eye]),
                 "CreateSRV(cam)");
    }
}

void Compositor::create_eye_targets_(UINT w, UINT h) {
    for (int eye = 0; eye < 2; ++eye) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width  = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        check_hr(device_->CreateTexture2D(&td, nullptr, &eyes_[eye].texture),
                 "CreateTexture2D(eye-out)");
        check_hr(device_->CreateShaderResourceView(eyes_[eye].texture.Get(), nullptr,
                                                   &eyes_[eye].srv), "CreateSRV(eye-out)");
        check_hr(device_->CreateRenderTargetView(eyes_[eye].texture.Get(), nullptr,
                                                 &eyes_[eye].rtv), "CreateRTV(eye-out)");
        eyes_[eye].width  = w;
        eyes_[eye].height = h;
    }
}

void Compositor::rebuild_mesh_(int eye, float zoom, bool apply,
                               float signed_toe_out_rad, float tilt_down_rad,
                               float signed_roll_rad) {
    std::vector<UndistortVertex> verts;
    std::vector<unsigned long>   idx;
    if (apply) {
        create_undistortion_mesh(kCameraWidth, kCameraHeight, zoom,
                                 signed_toe_out_rad, tilt_down_rad, signed_roll_rad,
                                 intrinsics_[eye], params_[eye], verts, idx);
    } else {
        create_default_mesh(kCameraWidth, kCameraHeight, kCameraWidth, kCameraHeight,
                            verts, idx);
    }

    D3D11_BUFFER_DESC vbd{};
    vbd.Usage     = D3D11_USAGE_IMMUTABLE;
    vbd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(UndistortVertex));
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd{ verts.data(), 0, 0 };
    meshes_[eye].vb.Reset();
    check_hr(device_->CreateBuffer(&vbd, &vsd, &meshes_[eye].vb), "CreateBuffer(vb)");

    D3D11_BUFFER_DESC ibd{};
    ibd.Usage     = D3D11_USAGE_IMMUTABLE;
    ibd.ByteWidth = static_cast<UINT>(idx.size() * sizeof(unsigned long));
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd{ idx.data(), 0, 0 };
    meshes_[eye].ib.Reset();
    check_hr(device_->CreateBuffer(&ibd, &isd, &meshes_[eye].ib), "CreateBuffer(ib)");
    meshes_[eye].index_count = static_cast<UINT>(idx.size());
}

void Compositor::upload_frame(const StereoFrame& frame) {
    if (!ready_ || !frame.valid()) return;
    const uint8_t* sources[2] = { frame.left.data(), frame.right.data() };
    // BC4 row pitch: bytes per row of 4×4 blocks = (width/4) * 8 bytes/block.
    const UINT block_row_pitch = (kCameraWidth / 4) * 8;
    for (int eye = 0; eye < 2; ++eye) {
        ctx_->UpdateSubresource(cam_tex_[eye].Get(), 0, nullptr,
                                sources[eye], block_row_pitch, 0);
    }
}

void Compositor::render(const CompositorConfig& cfg) {
    if (!ready_) return;

    if (mesh_dirty_         ||
        last_zoom_   != cfg.zoom_factor          ||
        last_undist_ != cfg.apply_undistortion   ||
        last_toe_l_  != cfg.camera_toe_out_rad_l || last_tilt_l_ != cfg.camera_tilt_down_rad_l || last_roll_l_ != cfg.camera_roll_rad_l ||
        last_toe_r_  != cfg.camera_toe_out_rad_r || last_tilt_r_ != cfg.camera_tilt_down_rad_r || last_roll_r_ != cfg.camera_roll_rad_r) {
        rebuild_mesh_(0, cfg.zoom_factor, cfg.apply_undistortion,
                      cfg.camera_toe_out_rad_l, cfg.camera_tilt_down_rad_l, cfg.camera_roll_rad_l);
        rebuild_mesh_(1, cfg.zoom_factor, cfg.apply_undistortion,
                      cfg.camera_toe_out_rad_r, cfg.camera_tilt_down_rad_r, cfg.camera_roll_rad_r);
        mesh_dirty_  = false;
        last_zoom_   = cfg.zoom_factor;
        last_undist_ = cfg.apply_undistortion;
        last_toe_l_  = cfg.camera_toe_out_rad_l; last_tilt_l_ = cfg.camera_tilt_down_rad_l; last_roll_l_ = cfg.camera_roll_rad_l;
        last_toe_r_  = cfg.camera_toe_out_rad_r; last_tilt_r_ = cfg.camera_tilt_down_rad_r; last_roll_r_ = cfg.camera_roll_rad_r;
    }

    const float clear[4]{ 0, 0, 0, 0 };
    UINT stride = sizeof(UndistortVertex);
    UINT offset = 0;
    const float blend_factor[4] = {};

    ctx_->IASetInputLayout(layout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->OMSetBlendState(blend_.Get(), blend_factor, 0xffffffff);
    ctx_->RSSetState(raster_.Get());

    // Upload constant buffer once — all params are identical for both eyes.
    {
        const float eff_brightness = cfg.brightness_enabled   ? cfg.brightness   : 1.0f;
        const float eff_contrast   = cfg.contrast_enabled     ? cfg.contrast     : 1.0f;
        const float eff_unsharp    = cfg.enhancements_enabled ? cfg.unsharp_amount : 0.0f;
        const float unsharp_step   = cfg.unsharp_radius / static_cast<float>(kCameraWidth);
        CBParams cb{
            DirectX::XMFLOAT4{ cfg.global_alpha, eff_brightness, eff_contrast, eff_unsharp },
            DirectX::XMFLOAT4{ unsharp_step, 0.f, 0.f, 0.f }
        };
        D3D11_MAPPED_SUBRESOURCE map{};
        ctx_->Map(cb_per_eye_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
        std::memcpy(map.pData, &cb, sizeof(cb));
        ctx_->Unmap(cb_per_eye_.Get(), 0);
        ctx_->VSSetConstantBuffers(0, 1, cb_per_eye_.GetAddressOf());
        ctx_->PSSetConstantBuffers(0, 1, cb_per_eye_.GetAddressOf());
    }

    for (int eye = 0; eye < 2; ++eye) {
        D3D11_VIEWPORT vp{ 0, 0,
            static_cast<float>(eye_w_), static_cast<float>(eye_h_), 0.f, 1.f };
        ctx_->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtv = eyes_[eye].rtv.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->ClearRenderTargetView(rtv, clear);

        ctx_->IASetVertexBuffers(0, 1, meshes_[eye].vb.GetAddressOf(), &stride, &offset);
        ctx_->IASetIndexBuffer(meshes_[eye].ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->PSSetShaderResources(0, 1, cam_srv_[eye].GetAddressOf());
        ctx_->DrawIndexed(meshes_[eye].index_count, 0, 0);

        ID3D11ShaderResourceView* null_srv = nullptr;
        ctx_->PSSetShaderResources(0, 1, &null_srv);
    }

    // Unbind only what we touched — avoids the full ClearState() overhead which
    // disrupts the host game's D3D11 bind state and causes unnecessary re-binding.
    ID3D11RenderTargetView* null_rtv  = nullptr;
    ID3D11Buffer*           null_buf  = nullptr;
    ID3D11SamplerState*     null_samp = nullptr;
    UINT                    zero      = 0;
    ctx_->OMSetRenderTargets(1, &null_rtv, nullptr);
    ctx_->VSSetShader(nullptr, nullptr, 0);
    ctx_->PSSetShader(nullptr, nullptr, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetConstantBuffers(0, 1, &null_buf);
    ctx_->PSSetConstantBuffers(0, 1, &null_buf);
    ctx_->PSSetSamplers(0, 1, &null_samp);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->RSSetState(nullptr);
    ctx_->IASetVertexBuffers(0, 1, &null_buf, &zero, &zero);
    ctx_->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
}

}  // namespace psvr2pt
