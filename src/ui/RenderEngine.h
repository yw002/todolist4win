#pragma once

#include <functional>
#include <memory>

// The DXGI/D3D/DWrite headers below expect the core Win32 definitions, and DXGI error codes
// (DXGI_ERROR_DEVICE_*) live in winerror.h pulled in by windows.h.
#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "ui/RenderResources.h"

namespace todo {

class RenderEngine {
public:
    explicit RenderEngine(HWND hwnd = nullptr);
    void Attach(HWND hwnd) noexcept;
    bool CreateDeviceIndependentResources();
    bool CreateDeviceResources(UINT width, UINT height, UINT dpi);
    void Render(const std::function<void(ID2D1DeviceContext&, RenderResources&)>& scene);
    bool ResizeBuffers(UINT width, UINT height, UINT dpi);
    // Scales the whole composited content around the swap chain's top-left corner. The window-rect
    // animation shrinks the HWND while the buffers keep their final size, so this is what makes the
    // content shrink with the window instead of being clipped. 1.0 restores the identity transform.
    void SetVisualScale(float scale);
    // Independent X/Y scale used by maximize/restore, where the window aspect ratio changes. The
    // uniform SetVisualScale stays for dock transitions that preserve the aspect ratio.
    void SetVisualStretch(float scaleX, float scaleY);
    [[nodiscard]] float VisualScale() const noexcept { return visualScaleX_; }
    // Whole-surface opacity, used together with the scale so a dock/minimize transition can fade the
    // last few pixels to nothing instead of ending on a visible thumbnail.
    void SetVisualOpacity(float opacity);
    [[nodiscard]] float VisualOpacity() const noexcept { return visualOpacity_; }
    bool RecoverDeviceResources(UINT width, UINT height, UINT dpi);
    void HandleDeviceLost();
    void ReleaseDeviceResources() noexcept;
    [[nodiscard]] bool HasDeviceResources() const noexcept;
    [[nodiscard]] ID2D1DeviceContext* Context() const noexcept;
    [[nodiscard]] IDWriteFactory* DWrite() const noexcept;
    [[nodiscard]] RenderResources& Resources() noexcept;
    [[nodiscard]] UINT Dpi() const noexcept;
    [[nodiscard]] D2D1_SIZE_F Size() const noexcept;
    [[nodiscard]] UINT PixelWidth() const noexcept { return width_; }
    [[nodiscard]] UINT PixelHeight() const noexcept { return height_; }

private:
    bool BindSwapChainTarget();
    void ApplyVisualTransform();
    HWND hwnd_{nullptr};
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> compositionDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> compositionVisual_;
    // Whole-surface opacity is applied through an effect group: IDCompositionVisual has no SetOpacity,
    // while IDCompositionEffectGroup does and is available on every Windows 10 build.
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> opacityEffect_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_;
    std::unique_ptr<RenderResources> resources_;
    UINT dpi_{96};
    UINT width_{1};
    UINT height_{1};
    float visualScaleX_{1.0F};
    float visualScaleY_{1.0F};
    float visualOpacity_{1.0F};
    int rebuildFailures_{0};
};

}  // namespace todo
