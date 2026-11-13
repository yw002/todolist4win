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
    bool RecoverDeviceResources(UINT width, UINT height, UINT dpi);
    void HandleDeviceLost();
    void ReleaseDeviceResources() noexcept;
    [[nodiscard]] bool HasDeviceResources() const noexcept;
    [[nodiscard]] ID2D1DeviceContext* Context() const noexcept;
    [[nodiscard]] IDWriteFactory* DWrite() const noexcept;
    [[nodiscard]] RenderResources& Resources() noexcept;
    [[nodiscard]] UINT Dpi() const noexcept;
    [[nodiscard]] D2D1_SIZE_F Size() const noexcept;

private:
    bool BindSwapChainTarget();
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
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_;
    std::unique_ptr<RenderResources> resources_;
    UINT dpi_{96};
    UINT width_{1};
    UINT height_{1};
    int rebuildFailures_{0};
};

}  // namespace todo
