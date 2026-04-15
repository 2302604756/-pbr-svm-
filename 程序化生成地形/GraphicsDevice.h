#pragma once
#include <d3d11.h>
#include <wrl/client.h> // 引入 ComPtr

using Microsoft::WRL::ComPtr;

class GraphicsDevice {
public:
    // 熟悉的命名，套上了 ComPtr 安全防护服
    ComPtr<ID3D11Device> pd3dDevice;
    ComPtr<ID3D11DeviceContext> pImmediateContext;
    ComPtr<IDXGISwapChain> pSwapChain;
    ComPtr<ID3D11RenderTargetView> pRenderTargetView;
    ComPtr<ID3D11DepthStencilView> pDepthStencilView;

    bool Initialize(HWND hWnd, int width, int height) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = width;
        sd.BufferDesc.Height = height;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;

        // 创建设备和交换链
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &sd, pSwapChain.GetAddressOf(), pd3dDevice.GetAddressOf(), nullptr, pImmediateContext.GetAddressOf()))) {
            return false;
        }

        // 设置主渲染目标 (BackBuffer)
        ComPtr<ID3D11Texture2D> pBackBuffer;
        pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)pBackBuffer.GetAddressOf());
        pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, pRenderTargetView.GetAddressOf());

        // 创建深度缓冲区 (Z-Buffer)
        D3D11_TEXTURE2D_DESC descDepth = {};
        descDepth.Width = width;
        descDepth.Height = height;
        descDepth.MipLevels = 1;
        descDepth.ArraySize = 1;
        descDepth.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        descDepth.SampleDesc.Count = 1;
        descDepth.Usage = D3D11_USAGE_DEFAULT;
        descDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        ComPtr<ID3D11Texture2D> pDepthStencil;
        pd3dDevice->CreateTexture2D(&descDepth, nullptr, pDepthStencil.GetAddressOf());

        D3D11_DEPTH_STENCIL_VIEW_DESC descDSV = {};
        descDSV.Format = descDepth.Format;
        descDSV.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        pd3dDevice->CreateDepthStencilView(pDepthStencil.Get(), &descDSV, pDepthStencilView.GetAddressOf());

        return true;
    }

    void SetMainRenderTarget(float width, float height) {
        pImmediateContext->OMSetRenderTargets(1, pRenderTargetView.GetAddressOf(), pDepthStencilView.Get());
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, width, height, 0.0f, 1.0f };
        pImmediateContext->RSSetViewports(1, &vp);
    }

    void ClearMainRTV(float r, float g, float b, float a) {
        float color[4] = { r, g, b, a };
        pImmediateContext->ClearRenderTargetView(pRenderTargetView.Get(), color);
        pImmediateContext->ClearDepthStencilView(pDepthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    }

    void Present() {
        pSwapChain->Present(0, 0);
    }
};