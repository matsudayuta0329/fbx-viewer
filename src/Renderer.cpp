#include "Renderer.h"
#include "Shaders.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace DirectX;
using winrt::check_hresult;
namespace viewer {
namespace {
struct GPUVertex {
    Vec3 position, normal, uv;
    Color color;
    float hasUV;
};
struct alignas(16) Constants {
    XMFLOAT4X4 transform;
    XMFLOAT4 centerRadius, eyeMode, viewport, scroll, options, pointColor;
};

winrt::com_ptr<ID3DBlob> compile(char const *entry, char const *target) {
    winrt::com_ptr<ID3DBlob> code, error;
    HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, code.put(), error.put());
    if (FAILED(hr))
        throw std::runtime_error(error ? static_cast<char const *>(error->GetBufferPointer())
                                       : "Shader compilation failed");
    return code;
}
template <class T>
winrt::com_ptr<ID3D11Buffer> buffer(ID3D11Device *device, std::vector<T> const &values, UINT bind) {
    winrt::com_ptr<ID3D11Buffer> result;
    if (values.empty())
        return result;
    if (values.size() > UINT_MAX / sizeof(T))
        throw std::runtime_error("GPU buffer size exceeds limits.");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(values.size() * sizeof(T));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
    check_hresult(device->CreateBuffer(&desc, &data, result.put()));
    return result;
}
} // namespace
void Renderer::initialize(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const &panel, bool uv) {
    uv_ = uv;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, 1, D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
    if (FAILED(hr))
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
                                        device_.put(), nullptr, context_.put()));
    auto dxgi = device_.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    check_hresult(dxgi->GetAdapter(adapter.put()));
    winrt::com_ptr<IDXGIFactory2> factory;
    check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    check_hresult(factory->CreateSwapChainForComposition(device_.get(), &desc, nullptr, swap_.put()));
    auto native = panel.as<::ISwapChainPanelNative>();
    check_hresult(native->SetSwapChain(swap_.get()));
    auto vs = compile("VS", "vs_5_0"), ps = compile("PS", "ps_5_0"), gs = compile("GS", "gs_5_0");
    check_hresult(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                              vertexShader_.put()));
    check_hresult(
        device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pixelShader_.put()));
    check_hresult(device_->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr,
                                                pointShader_.put()));
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    check_hresult(
        device_->CreateInputLayout(elements, 5, vs->GetBufferPointer(), vs->GetBufferSize(), layout_.put()));
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.Usage = D3D11_USAGE_DEFAULT;
    check_hresult(device_->CreateBuffer(&cb, nullptr, constants_.put()));
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = true;
    check_hresult(device_->CreateRasterizerState(&raster, raster_.put()));
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = true;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    check_hresult(device_->CreateDepthStencilState(&depth, depthOn_.put()));
    depth.DepthEnable = false;
    check_hresult(device_->CreateDepthStencilState(&depth, depthOff_.put()));
    D3D11_BLEND_DESC blend{};
    auto &rt = blend.RenderTarget[0];
    rt.BlendEnable = true;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check_hresult(device_->CreateBlendState(&blend, blend_.put()));
    resize(1, 1, 1);
}
void Renderer::resize(float width, float height, float scale) {
    width_ = std::max(1.f, width);
    height_ = std::max(1.f, height);
    scale_ = std::max(0.5f, scale);
    if (!swap_)
        return;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    targetView_ = nullptr;
    depthView_ = nullptr;
    context_->Flush();
    UINT w = static_cast<UINT>(std::max(1.f, width_ * scale_)),
         h = static_cast<UINT>(std::max(1.f, height_ * scale_));
    check_hresult(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0));
    DXGI_MATRIX_3X2_F inverseScale{1 / scale_, 0, 0, 1 / scale_, 0, 0};
    check_hresult(swap_.as<IDXGISwapChain2>()->SetMatrixTransform(&inverseScale));
    winrt::com_ptr<ID3D11Texture2D> back;
    check_hresult(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()));
    check_hresult(device_->CreateRenderTargetView(back.get(), nullptr, targetView_.put()));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    winrt::com_ptr<ID3D11Texture2D> depth;
    check_hresult(device_->CreateTexture2D(&desc, nullptr, depth.put()));
    check_hresult(device_->CreateDepthStencilView(depth.get(), nullptr, depthView_.put()));
}
void Renderer::setModel(Model const *model, Settings const &settings) {
    if (!model) {
        model_ = nullptr;
        indexCount_ = 0;
        vertices_ = nullptr;
        indices_ = nullptr;
        return;
    }
    auto indices = buffer(device_.get(), model->indices, D3D11_BIND_INDEX_BUFFER);
    std::vector<GPUVertex> data;
    data.reserve(model->vertices.size());
    for (size_t i = 0; i < model->vertices.size(); ++i) {
        auto const &v = model->vertices[i];
        data.push_back({v.position, v.normal, v.uv[settings.uvChannel], vertexColor(v, i, *model, settings),
                        (v.uvMask & (1u << settings.uvChannel)) ? 1.f : 0.f});
    }
    auto vertices = buffer(device_.get(), data, D3D11_BIND_VERTEX_BUFFER);
    model_ = model;
    indices_ = std::move(indices);
    vertices_ = std::move(vertices);
    indexCount_ = static_cast<uint32_t>(model->indices.size());
    fit();
}
void Renderer::updateColors(Settings const &settings) {
    if (!model_)
        return;
    std::vector<GPUVertex> data;
    data.reserve(model_->vertices.size());
    for (size_t i = 0; i < model_->vertices.size(); ++i) {
        auto const &v = model_->vertices[i];
        data.push_back({v.position, v.normal, v.uv[settings.uvChannel], vertexColor(v, i, *model_, settings),
                        (v.uvMask & (1u << settings.uvChannel)) ? 1.f : 0.f});
    }
    auto next = buffer(device_.get(), data, D3D11_BIND_VERTEX_BUFFER);
    vertices_ = std::move(next);
}
void Renderer::fit() {
    yaw_ = 0.65f;
    pitch_ = 0.3f;
    distance_ =
        1.08f / std::sin(std::min(XM_PIDIV4 / 2, std::atan(std::tan(XM_PIDIV4 / 2) * width_ / height_)));
    target_ = {};
    uvCenter_ = {0.5f, 0.5f};
    uvZoom_ = 1;
}
Vec3 Renderer::eye() const {
    return target_ +
           Vec3{std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), std::cos(yaw_) * std::cos(pitch_)} *
               distance_;
}
XMMATRIX Renderer::matrix() const {
    float aspect = width_ / height_;
    if (uv_) {
        auto view = XMMatrixTranslation(-uvCenter_.x, -uvCenter_.y, 0);
        return view * XMMatrixOrthographicRH(1.3f * std::max(1.f, aspect) / uvZoom_,
                                             1.3f * std::max(1.f, 1 / aspect) / uvZoom_, -1, 1);
    }
    auto e = eye();
    auto view = XMMatrixLookAtRH(XMVectorSet(e.x, e.y, e.z, 1),
                                 XMVectorSet(target_.x, target_.y, target_.z, 1), XMVectorSet(0, 1, 0, 0));
    return view * XMMatrixPerspectiveFovRH(XM_PIDIV4, aspect, 0.001f, 10000.f);
}
void Renderer::orbit(float dx, float dy) {
    if (uv_) {
        pan(dx, dy);
        return;
    }
    yaw_ -= dx * 0.008f;
    pitch_ = std::clamp(pitch_ + dy * 0.008f, -1.55f, 1.55f);
}
void Renderer::pan(float dx, float dy) {
    if (uv_) {
        float aspect = width_ / height_;
        uvCenter_.x -= dx / width_ * 1.3f * std::max(1.f, aspect) / uvZoom_;
        uvCenter_.y += dy / height_ * 1.3f * std::max(1.f, 1 / aspect) / uvZoom_;
        return;
    }
    auto forward = normalized(target_ - eye());
    auto right = normalized(cross(forward, {0, 1, 0}));
    auto up = cross(right, forward);
    float factor = 2 * distance_ * std::tan(XM_PIDIV4 / 2) / height_;
    target_ = target_ + right * (-dx * factor) + up * (dy * factor);
}
void Renderer::zoom(float delta) {
    if (uv_)
        uvZoom_ = std::clamp(uvZoom_ * std::exp(delta * 0.001f), 0.01f, 1000.f);
    else
        distance_ = std::clamp(distance_ * std::exp(-delta * 0.001f), 0.02f, 1000.f);
}
void Renderer::draw(Settings const &s, float time, std::optional<Vec3> shadingEye) {
    if (!targetView_)
        return;
    float background[] = {0.075f, 0.085f, 0.105f, 1};
    context_->ClearRenderTargetView(targetView_.get(), background);
    context_->ClearDepthStencilView(depthView_.get(), D3D11_CLEAR_DEPTH, 1, 0);
    auto rtv = targetView_.get();
    context_->OMSetRenderTargets(1, &rtv, depthView_.get());
    D3D11_VIEWPORT vp{0, 0, width_ * scale_, height_ * scale_, 0, 1};
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_.get());
    if (model_ && vertices_) {
        Constants cb{};
        XMStoreFloat4x4(&cb.transform, matrix());
        auto c = model_->center;
        auto e = shadingEye.value_or(eye());
        cb.centerRadius = {c.x, c.y, c.z, model_->radius};
        cb.eyeMode = {e.x, e.y, e.z, static_cast<float>(s.mode)};
        cb.viewport = {width_ * scale_, height_ * scale_, uv_ ? 1.f : 0.f, 0};
        cb.scroll = {s.direction.x * s.speed, s.direction.y * s.speed, time, s.grid};
        bool transparent = s.mode == Mode::Surface && s.transparent;
        cb.options = {transparent ? 0.35f : 1.f, 0, s.pointSize * scale_, 0};
        cb.pointColor = {s.pointColor.r, s.pointColor.g, s.pointColor.b, 1};
        auto constant = constants_.get();
        context_->VSSetConstantBuffers(0, 1, &constant);
        context_->PSSetConstantBuffers(0, 1, &constant);
        context_->GSSetConstantBuffers(0, 1, &constant);
        context_->VSSetShader(vertexShader_.get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.get(), nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->IASetInputLayout(layout_.get());
        auto vertex = vertices_.get();
        UINT stride = sizeof(GPUVertex), offset = 0;
        context_->IASetVertexBuffers(0, 1, &vertex, &stride, &offset);
        context_->OMSetBlendState(blend_.get(), nullptr, UINT_MAX);
        context_->OMSetDepthStencilState(transparent || uv_ ? depthOff_.get() : depthOn_.get(), 0);
        context_->UpdateSubresource(constants_.get(), 0, nullptr, &cb, 0, 0);
        // Rasterize the same triangles and interpolants in either world or UV space.
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetIndexBuffer(indices_.get(), DXGI_FORMAT_R32_UINT, 0);
        context_->DrawIndexed(indexCount_, 0, 0);
        if (s.showVertices) {
            cb.options.x = 1;
            cb.options.w = 2.f;
            context_->UpdateSubresource(constants_.get(), 0, nullptr, &cb, 0, 0);
            context_->GSSetShader(pointShader_.get(), nullptr, 0);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context_->Draw(static_cast<UINT>(model_->vertices.size()), 0);
            context_->GSSetShader(nullptr, nullptr, 0);
        }
    }
    if (probe_) {
        auto clip = XMVector4Transform(XMVectorSet(probe_->x, probe_->y, 0, 1), matrix());
        XMFLOAT3 point;
        XMStoreFloat3(&point, clip / XMVectorGetW(clip));
        auto x = static_cast<UINT>(std::clamp((point.x + 1) * width_ * scale_ / 2, 0.f, width_ * scale_ - 1));
        auto y =
            static_cast<UINT>(std::clamp((1 - point.y) * height_ * scale_ / 2, 0.f, height_ * scale_ - 1));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> pixel;
        check_hresult(device_->CreateTexture2D(&desc, nullptr, pixel.put()));
        winrt::com_ptr<ID3D11Resource> source;
        targetView_->GetResource(source.put());
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
        context_->CopySubresourceRegion(pixel.get(), 0, 0, 0, 0, source.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(context_->Map(pixel.get(), 0, D3D11_MAP_READ, 0, &mapped));
        auto bytes = static_cast<uint8_t const *>(mapped.pData);
        probeColor_ = {bytes[2] / 255.f, bytes[1] / 255.f, bytes[0] / 255.f, bytes[3] / 255.f};
        context_->Unmap(pixel.get(), 0);
    }
    check_hresult(swap_->Present(1, 0));
}
Color Renderer::sampleUvColor(Vec2 uv, Settings const &settings, Vec3 shadingEye) {
    if (!uv_)
        throw std::logic_error("UV probe requires a UV renderer");
    probe_ = uv;
    try {
        draw(settings, 0, shadingEye);
    } catch (...) {
        probe_.reset();
        throw;
    }
    probe_.reset();
    return probeColor_;
}
std::optional<size_t> Renderer::pick(float x, float y, Settings const &s) const {
    if (!model_)
        return {};
    auto m = matrix();
    float closest = std::max(7.f, s.pointSize / 2 + 3);
    closest *= closest;
    float nearestDepth = std::numeric_limits<float>::max();
    std::optional<size_t> found;
    for (size_t i = 0; i < model_->vertices.size(); ++i) {
        auto const &v = model_->vertices[i];
        if (uv_ && !(v.uvMask & (1u << s.uvChannel)))
            continue;
        auto p = uv_ ? Vec3{v.uv[s.uvChannel].x, v.uv[s.uvChannel].y, 0}
                     : (v.position - model_->center) * (1 / model_->radius);
        auto clip = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), m);
        float w = XMVectorGetW(clip);
        if (w <= 0)
            continue;
        XMFLOAT3 ndc;
        XMStoreFloat3(&ndc, clip / w);
        if (ndc.z < 0 || ndc.z > 1)
            continue;
        float dx = (ndc.x + 1) * width_ / 2 - x, dy = (1 - ndc.y) * height_ / 2 - y, d = dx * dx + dy * dy;
        if (d < closest - 0.25f || (d <= closest + 0.25f && ndc.z < nearestDepth)) {
            closest = d;
            nearestDepth = ndc.z;
            found = i;
        }
    }
    if (!found || uv_ || (s.mode == Mode::Surface && s.transparent))
        return found;
    // Reject hidden vertices using a ray to the candidate, rather than reading back the GPU.
    auto origin = eye();
    auto point = (model_->vertices[*found].position - model_->center) * (1 / model_->radius);
    auto ray = point - origin;
    for (size_t i = 0; i < model_->indices.size(); i += 3) {
        auto a = (model_->vertices[model_->indices[i]].position - model_->center) * (1 / model_->radius);
        auto b = (model_->vertices[model_->indices[i + 1]].position - model_->center) * (1 / model_->radius);
        auto c = (model_->vertices[model_->indices[i + 2]].position - model_->center) * (1 / model_->radius);
        auto ab = b - a, ac = c - a, h = cross(ray, ac);
        float determinant = dot(ab, h);
        if (std::abs(determinant) < 1e-8f)
            continue;
        float inverse = 1 / determinant;
        auto t = origin - a;
        float u = inverse * dot(t, h);
        if (u < 0 || u > 1)
            continue;
        auto q = cross(t, ab);
        float v = inverse * dot(ray, q);
        if (v < 0 || u + v > 1)
            continue;
        float distance = inverse * dot(ac, q);
        if (distance > 0 && distance < 0.9999f)
            return {};
    }
    return found;
}
} // namespace viewer
