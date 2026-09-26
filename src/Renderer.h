#pragma once
#include "Model.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <winrt/base.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <optional>

namespace viewer {
class Renderer {
public:
    void initialize(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel, bool uv);
    void resize(float width, float height, float scale);
    void setModel(Model const* model, Settings const& settings);
    void updateColors(Settings const& settings);
    void fit();
    void orbit(float dx,float dy);
    void pan(float dx,float dy);
    void zoom(float delta);
    void draw(Settings const& settings,float time);
    std::optional<size_t> pick(float x,float y,Settings const& settings) const;
private:
    DirectX::XMMATRIX matrix() const;
    Vec3 eye() const;
    Model const* model_{};
    bool uv_{};
    float width_{1},height_{1},scale_{1},yaw_{0.65f},pitch_{0.3f},distance_{3.2f},uvZoom_{1};
    Vec3 target_{}; Vec2 uvCenter_{0.5f,0.5f};
    uint32_t indexCount_{},lineCount_{};
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<IDXGISwapChain1> swap_;
    winrt::com_ptr<ID3D11RenderTargetView> targetView_;
    winrt::com_ptr<ID3D11DepthStencilView> depthView_;
    winrt::com_ptr<ID3D11VertexShader> vertexShader_;
    winrt::com_ptr<ID3D11PixelShader> pixelShader_;
    winrt::com_ptr<ID3D11GeometryShader> pointShader_;
    winrt::com_ptr<ID3D11InputLayout> layout_;
    winrt::com_ptr<ID3D11Buffer> vertices_,indices_,lines_,constants_;
    winrt::com_ptr<ID3D11RasterizerState> raster_;
    winrt::com_ptr<ID3D11DepthStencilState> depthOn_,depthOff_;
    winrt::com_ptr<ID3D11BlendState> blend_;
};
}
