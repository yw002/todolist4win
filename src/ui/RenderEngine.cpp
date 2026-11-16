#include "ui/RenderEngine.h"

#include <algorithm>
#include <string>


#include <d2d1_1helper.h>
#include <functional>
#include <memory>

namespace todo {
namespace {
// Device-loss HRESULTs that can be recovered by rebuilding the D3D/D2D resources. The optional
// codes are guarded because older Windows SDKs do not define all of them.
bool IsRecoverablePresentError(HRESULT value) noexcept {
    switch (value) {
        case DXGI_ERROR_DEVICE_REMOVED:
        case DXGI_ERROR_DEVICE_RESET:
#ifdef DXGI_ERROR_DEVICE_HUNG
        case DXGI_ERROR_DEVICE_HUNG:
#endif
#ifdef DXGI_ERROR_DRIVER_INTERNAL_ERROR
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
#endif
            return true;
        default:
            return false;
    }
}
}  // namespace

RenderEngine::RenderEngine(HWND hwnd) : hwnd_(hwnd), resources_(std::make_unique<RenderResources>()) {}
void RenderEngine::Attach(HWND hwnd) noexcept { hwnd_ = hwnd; }

bool RenderEngine::CreateDeviceIndependentResources() {
    if (d2dFactory_ && dwrite_ && wic_) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &d2dFactory_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) return false;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)));
}

bool RenderEngine::CreateDeviceResources(UINT width, UINT height, UINT dpi) {
    if (!hwnd_ || !CreateDeviceIndependentResources()) return false;
    width_ = std::max(1U, width);
    height_ = std::max(1U, height);
    dpi_ = dpi;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3dDevice_, nullptr, &d3dContext_))) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3dDevice_, nullptr, &d3dContext_))) return false;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(d3dDevice_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    if (FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) || FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_))) return false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(factory->CreateSwapChainForComposition(dxgiDevice.Get(), &desc, nullptr, &swapChain_))) return false;
    if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&compositionDevice_))) || FAILED(compositionDevice_->CreateTargetForHwnd(hwnd_, TRUE, &compositionTarget_)) || FAILED(compositionDevice_->CreateVisual(&compositionVisual_)) || FAILED(compositionDevice_->CreateEffectGroup(&opacityEffect_))) return false;
    if (FAILED(compositionVisual_->SetEffect(opacityEffect_.Get()))) return false;
    if (FAILED(compositionVisual_->SetContent(swapChain_.Get())) || FAILED(compositionTarget_->SetRoot(compositionVisual_.Get())) || FAILED(compositionDevice_->Commit())) return false;
    // A recreated visual starts with an identity transform; restore the scale the window animation
    // had applied before the device was lost.
    ApplyVisualTransform();
    if (!BindSwapChainTarget()) return false;
    // Never replace resources_: controls cache a RenderResources pointer across frames (TextBox).
    // Rebuild in place so those pointers stay valid for the whole window lifetime.
    if (!resources_) return false;
    resources_->Release();
    if (!resources_->Create(context_.Get(), dwrite_.Get(), static_cast<float>(dpi_))) return false;
    rebuildFailures_ = 0;
    return true;
}

bool RenderEngine::BindSwapChainTarget() {
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(context_->CreateBitmapFromDxgiSurface(surface.Get(), properties, &targetBitmap_))) return false;
    context_->SetTarget(targetBitmap_.Get());
    context_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    context_->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    return true;
}

void RenderEngine::Render(const std::function<void(ID2D1DeviceContext&, RenderResources&)>& scene) {
    if (!context_ || !targetBitmap_ || !resources_) return;
    context_->SetTarget(targetBitmap_.Get());
    context_->BeginDraw();
    context_->SetTransform(D2D1::Matrix3x2F::Identity());
    context_->Clear(D2D1::ColorF(0, 0.0F));
    scene(*context_.Get(), *resources_);
    const HRESULT end = context_->EndDraw();
    HRESULT present = end;
    if (SUCCEEDED(end)) present = swapChain_->Present(1, 0);
    const bool recreateTarget = end == D2DERR_RECREATE_TARGET;
    // present holds the Present() result when EndDraw succeeded, otherwise the EndDraw failure.
    if (recreateTarget || IsRecoverablePresentError(present)) HandleDeviceLost();
}

void RenderEngine::SetVisualScale(float scale) { SetVisualStretch(scale, scale); }

void RenderEngine::SetVisualStretch(float scaleX, float scaleY) {
    visualScaleX_ = std::clamp(scaleX, 0.05F, 4.0F);
    visualScaleY_ = std::clamp(scaleY, 0.05F, 4.0F);
    ApplyVisualTransform();
}

void RenderEngine::SetVisualOpacity(float opacity) {
    visualOpacity_ = std::clamp(opacity, 0.0F, 1.0F);
    ApplyVisualTransform();
}

void RenderEngine::ApplyVisualTransform() {
    if (!compositionVisual_) return;
    compositionVisual_->SetTransform(D2D1::Matrix3x2F::Scale(visualScaleX_, visualScaleY_));
    if (opacityEffect_) opacityEffect_->SetOpacity(visualOpacity_);
    if (compositionDevice_) compositionDevice_->Commit();
}

bool RenderEngine::ResizeBuffers(UINT width, UINT height, UINT dpi) {
    if (!swapChain_ || !context_) return CreateDeviceResources(width, height, dpi);
    width_ = std::max(1U, width);
    height_ = std::max(1U, height);
    dpi_ = dpi;
    context_->SetTarget(nullptr);
    targetBitmap_.Reset();
    HRESULT resizeResult = swapChain_->ResizeBuffers(2, width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(resizeResult)) { HandleDeviceLost(); return false; }
    // Reuse the same RenderResources object; Create() releases its device-bound brushes first.
    return resources_->Create(context_.Get(), dwrite_.Get(), static_cast<float>(dpi_));
}

bool RenderEngine::RecoverDeviceResources(UINT width, UINT height, UINT dpi) {
    // Idempotent recovery: safe to call repeatedly from the render loop after a device-loss storm.
    // ReleaseDeviceResources() only releases the object's resources, it never destroys the object,
    // so the address cached by controls stays valid.
    ReleaseDeviceResources();
    width_ = std::max(1U, width);
    height_ = std::max(1U, height);
    dpi_ = dpi;
    if (CreateDeviceResources(width_, height_, dpi_)) {
        rebuildFailures_ = 0;
        return true;
    }
    ++rebuildFailures_;
    return false;
}

void RenderEngine::HandleDeviceLost() {
    // One immediate attempt; the caller retries with backoff when consecutive rebuilds keep failing.
    if (rebuildFailures_ >= 3) { ReleaseDeviceResources(); return; }
    RecoverDeviceResources(width_, height_, dpi_);
}

void RenderEngine::ReleaseDeviceResources() noexcept {
    if (resources_) resources_->Release();
    if (context_) context_->SetTarget(nullptr);
    targetBitmap_.Reset();
    context_.Reset();
    d2dDevice_.Reset();
    opacityEffect_.Reset();
    compositionVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    swapChain_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
}

bool RenderEngine::HasDeviceResources() const noexcept { return context_ && targetBitmap_; }
ID2D1DeviceContext* RenderEngine::Context() const noexcept { return context_.Get(); }
IDWriteFactory* RenderEngine::DWrite() const noexcept { return dwrite_.Get(); }
RenderResources& RenderEngine::Resources() noexcept { return *resources_; }
UINT RenderEngine::Dpi() const noexcept { return dpi_; }
D2D1_SIZE_F RenderEngine::Size() const noexcept { return {static_cast<float>(width_) * 96.0F / dpi_, static_cast<float>(height_) * 96.0F / dpi_}; }

}  // namespace todo
