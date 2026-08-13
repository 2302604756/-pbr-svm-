#pragma once
#include "RenderGraph.h"
#include "Camera.h"
#include "Environment.h"
#include "RenderData.h"
#include "TerrainInteropManager.h"
#include "stb_image.h"
#include <d3dcompiler.h>

class RenderPipeline {
public:
    // =====================================
    // 1. 成员变量声明 
    // =====================================
    ComPtr<ID3D11VertexShader> pVertexShader;
    ComPtr<ID3D11PixelShader> pPixelShader;
    ComPtr<ID3D11PixelShader> pShadowPixelShader;
    ComPtr<ID3D11VertexShader> pSkyVertexShader;
    ComPtr<ID3D11PixelShader> pSkyPixelShader;
    ComPtr<ID3D11PixelShader> pBlurPixelShader;
    ComPtr<ID3D11PixelShader> pEdgePixelShader; 

    ComPtr<ID3D11InputLayout> pVertexLayout;
    ComPtr<ID3D11Buffer> pConstantBuffer;
    ComPtr<ID3D11Buffer> pBlurConstantBuffer;
    ComPtr<ID3D11DepthStencilState> pSkyDepthState;
    ComPtr<ID3D11RasterizerState> pRasterState;

    ComPtr<ID3D11SamplerState> pShadowSampler;
    ComPtr<ID3D11SamplerState> pWrapSampler;

    // 材质与云层 
    ComPtr<ID3D11ShaderResourceView> pRockAlbedoSRV;
    ComPtr<ID3D11ShaderResourceView> pRockNormalSRV;
    ComPtr<ID3D11ShaderResourceView> pGrassAlbedoSRV;
    ComPtr<ID3D11ShaderResourceView> pGrassNormalSRV;
    ComPtr<ID3D11ShaderResourceView> pSnowAlbedoSRV;
    ComPtr<ID3D11ShaderResourceView> pSnowNormalSRV;

    ComPtr<ID3D11Texture3D> pCloudVolumeTex;
    ComPtr<ID3D11ShaderResourceView> pCloudVolumeSRV;
    cudaGraphicsResource_t cudaCloudVolumeRes = nullptr;

    // =====================================
    //将局部结构体改为类的成员变量，彻底解决 Lambda 捕获和生命周期问题！
    // =====================================
    RGHandle m_hBackBuffer;
    RGHandle m_hRawVSM;
    RGHandle m_hShadowDepth;
    RGHandle m_hBlurX;
    RGHandle m_hFinalVSM;
    RGHandle m_hSceneColor;  // 【新增】：用于保存整个场景颜色的虚拟画布

    // =====================================
    // 2. 贴图加载工具
    // =====================================
    bool LoadTextureToDX11(GraphicsDevice& gfx, const char* filename, ComPtr<ID3D11ShaderResourceView>& outSRV) {
        int width, height, channels;
        unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
        if (!data) return false;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height; desc.MipLevels = 0; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

        ComPtr<ID3D11Texture2D> pTexture;
        gfx.pd3dDevice->CreateTexture2D(&desc, nullptr, pTexture.GetAddressOf());
        gfx.pImmediateContext->UpdateSubresource(pTexture.Get(), 0, nullptr, data, width * 4, 0);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0; srvDesc.Texture2D.MipLevels = -1;
        gfx.pd3dDevice->CreateShaderResourceView(pTexture.Get(), &srvDesc, outSRV.GetAddressOf());

        gfx.pImmediateContext->GenerateMips(outSRV.Get());
        stbi_image_free(data);
        return true;
    }

    // =====================================
    // 3. 完整的初始化 
    // =====================================
    bool Initialize(GraphicsDevice& gfx) {
        ComPtr<ID3DBlob> pVSBlob, pPSBlob, pShadowPSBlob, pBlurPSBlob, pSkyVSBlob, pSkyPSBlob, errorBlob, pEdgePSBlob;

        // A. 编译 Shader
        D3DCompileFromFile(L"Shader.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0, pVSBlob.GetAddressOf(), errorBlob.GetAddressOf());
        D3DCompileFromFile(L"Shader.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0, pPSBlob.GetAddressOf(), errorBlob.GetAddressOf());
        D3DCompileFromFile(L"Shader.hlsl", nullptr, nullptr, "ShadowPS", "ps_5_0", 0, 0, pShadowPSBlob.GetAddressOf(), errorBlob.GetAddressOf());
        D3DCompileFromFile(L"Shader.hlsl", nullptr, nullptr, "BlurPS", "ps_5_0", 0, 0, pBlurPSBlob.GetAddressOf(), errorBlob.GetAddressOf());
        D3DCompileFromFile(L"SkyShader.hlsl", nullptr, nullptr, "VS", "vs_5_0", 0, 0, pSkyVSBlob.GetAddressOf(), errorBlob.GetAddressOf());
        D3DCompileFromFile(L"SkyShader.hlsl", nullptr, nullptr, "PS", "ps_5_0", 0, 0, pSkyPSBlob.GetAddressOf(), errorBlob.GetAddressOf());
        D3DCompileFromFile(L"EdgeShader.hlsl", nullptr, nullptr, "EdgePS", "ps_5_0", 0, 0, pEdgePSBlob.GetAddressOf(), errorBlob.GetAddressOf());

        gfx.pd3dDevice->CreateVertexShader(pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), nullptr, pVertexShader.GetAddressOf());
        gfx.pd3dDevice->CreatePixelShader(pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize(), nullptr, pPixelShader.GetAddressOf());
        gfx.pd3dDevice->CreatePixelShader(pShadowPSBlob->GetBufferPointer(), pShadowPSBlob->GetBufferSize(), nullptr, pShadowPixelShader.GetAddressOf());
        gfx.pd3dDevice->CreatePixelShader(pBlurPSBlob->GetBufferPointer(), pBlurPSBlob->GetBufferSize(), nullptr, pBlurPixelShader.GetAddressOf());
        gfx.pd3dDevice->CreateVertexShader(pSkyVSBlob->GetBufferPointer(), pSkyVSBlob->GetBufferSize(), nullptr, pSkyVertexShader.GetAddressOf());
        gfx.pd3dDevice->CreatePixelShader(pSkyPSBlob->GetBufferPointer(), pSkyPSBlob->GetBufferSize(), nullptr, pSkyPixelShader.GetAddressOf());
        gfx.pd3dDevice->CreatePixelShader(pEdgePSBlob->GetBufferPointer(), pEdgePSBlob->GetBufferSize(), nullptr, pEdgePixelShader.GetAddressOf());

        // B. 创建布局和光栅化状态
        D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        gfx.pd3dDevice->CreateInputLayout(layout, 3, pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), pVertexLayout.GetAddressOf());

        D3D11_RASTERIZER_DESC rsDesc = {};
        rsDesc.FillMode = D3D11_FILL_SOLID;
        rsDesc.CullMode = D3D11_CULL_NONE;
        gfx.pd3dDevice->CreateRasterizerState(&rsDesc, pRasterState.GetAddressOf());

        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.DepthEnable = TRUE;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; // 天空盒专用
        dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        gfx.pd3dDevice->CreateDepthStencilState(&dsDesc, pSkyDepthState.GetAddressOf());

        // C. 创建常量缓冲区
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DEFAULT; bd.ByteWidth = sizeof(ConstantBufferStruct); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        gfx.pd3dDevice->CreateBuffer(&bd, nullptr, pConstantBuffer.GetAddressOf());

        D3D11_BUFFER_DESC blurBd = {};
        blurBd.Usage = D3D11_USAGE_DEFAULT; blurBd.ByteWidth = sizeof(BlurParams); blurBd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        gfx.pd3dDevice->CreateBuffer(&blurBd, nullptr, pBlurConstantBuffer.GetAddressOf());

        // D. 创建采样器
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        gfx.pd3dDevice->CreateSamplerState(&sampDesc, pShadowSampler.GetAddressOf());

        D3D11_SAMPLER_DESC wrapDesc = {};
        wrapDesc.Filter = D3D11_FILTER_ANISOTROPIC; wrapDesc.MaxAnisotropy = 16;
        wrapDesc.AddressU = wrapDesc.AddressV = wrapDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        wrapDesc.ComparisonFunc = D3D11_COMPARISON_NEVER; wrapDesc.MaxLOD = D3D11_FLOAT32_MAX;
        gfx.pd3dDevice->CreateSamplerState(&wrapDesc, pWrapSampler.GetAddressOf());

        // E. 加载 PBR 贴图
        LoadTextureToDX11(gfx, "rock_albedo.jpg", pRockAlbedoSRV);
        LoadTextureToDX11(gfx, "rock_normal.jpg", pRockNormalSRV);
        LoadTextureToDX11(gfx, "grass_albedo.jpg", pGrassAlbedoSRV);
        LoadTextureToDX11(gfx, "grass_normal.jpg", pGrassNormalSRV);
        LoadTextureToDX11(gfx, "snow_albedo.jpg", pSnowAlbedoSRV);
        LoadTextureToDX11(gfx, "snow_normal.jpg", pSnowNormalSRV);

        // F. 云层 3D 纹理与 CUDA 映射
        D3D11_TEXTURE3D_DESC tex3DDesc = {};
        tex3DDesc.Width = 128; tex3DDesc.Height = 128; tex3DDesc.Depth = 128;
        tex3DDesc.MipLevels = 1; tex3DDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex3DDesc.Usage = D3D11_USAGE_DEFAULT; tex3DDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        gfx.pd3dDevice->CreateTexture3D(&tex3DDesc, nullptr, pCloudVolumeTex.GetAddressOf());

        D3D11_SHADER_RESOURCE_VIEW_DESC srv3DDesc = {};
        srv3DDesc.Format = tex3DDesc.Format; srv3DDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        srv3DDesc.Texture3D.MostDetailedMip = 0; srv3DDesc.Texture3D.MipLevels = 1;
        gfx.pd3dDevice->CreateShaderResourceView(pCloudVolumeTex.Get(), &srv3DDesc, pCloudVolumeSRV.GetAddressOf());

        cudaGraphicsD3D11RegisterResource(&cudaCloudVolumeRes, pCloudVolumeTex.Get(), cudaGraphicsRegisterFlagsSurfaceLoadStore);

        return true;
    }

    // =====================================
     // 5. 渲染图构建 (Render Graph)
     // =====================================
    void BuildGraph(RenderGraph& graph, GraphicsDevice& gfx, TerrainInteropManager& terrain, Camera& cam, EnvironmentSystem& env) {

        m_hBackBuffer = graph.ImportRenderTarget("BackBuffer", gfx.pRenderTargetView.Get(), gfx.pDepthStencilView.Get());

        // --- 1. Shadow Pass ---
        graph.AddPass("ShadowPass",
            [&](RenderGraphBuilder& builder) {
                m_hRawVSM = builder.CreateTexture("VSM_Raw", { 2048, 2048, DXGI_FORMAT_R32G32_FLOAT, false });
                m_hShadowDepth = builder.CreateTexture("ShadowMap", { 2048, 2048, DXGI_FORMAT_UNKNOWN, true });
                builder.Write(m_hRawVSM);
                builder.Write(m_hShadowDepth);
            },
            [this, &terrain, &env, &cam](GraphicsDevice& gfx, RenderGraphRegistry& registry) {
                ConstantBufferStruct cbShadow = {};
                cbShadow.WorldViewProj = XMMatrixTranspose(env.sunMatrix);
                cbShadow.LightViewProj = XMMatrixTranspose(env.sunMatrix);
                cbShadow.InvViewProj = XMMatrixTranspose(XMMatrixInverse(nullptr, env.sunMatrix));
                cbShadow.LightDirToSun = env.lightDir;
                cbShadow.CameraPos = XMFLOAT4(cam.camPos.x, cam.camPos.y, cam.camPos.z, 1.0f);
                cbShadow.WeatherParams = env.WeatherParams;
                cbShadow.Time = env.TotalTime;
                cbShadow.SkyParams = env.SkyParams;
                gfx.pImmediateContext->UpdateSubresource(pConstantBuffer.Get(), 0, nullptr, &cbShadow, 0, 0);

                gfx.pImmediateContext->VSSetConstantBuffers(0, 1, pConstantBuffer.GetAddressOf());
                gfx.pImmediateContext->PSSetConstantBuffers(0, 1, pConstantBuffer.GetAddressOf());

                auto rtv = registry.GetRTV(m_hRawVSM);
                auto dsv = registry.GetDSV(m_hShadowDepth);

                float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                gfx.pImmediateContext->ClearRenderTargetView(rtv, clearColor);
                gfx.pImmediateContext->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
                gfx.pImmediateContext->OMSetRenderTargets(1, &rtv, dsv);

                D3D11_VIEWPORT vp = { 0.0f, 0.0f, 2048.0f, 2048.0f, 0.0f, 1.0f };
                gfx.pImmediateContext->RSSetViewports(1, &vp);
                gfx.pImmediateContext->RSSetState(pRasterState.Get());

                gfx.pImmediateContext->IASetInputLayout(pVertexLayout.Get());
                gfx.pImmediateContext->VSSetShader(pVertexShader.Get(), nullptr, 0);
                gfx.pImmediateContext->PSSetShader(pShadowPixelShader.Get(), nullptr, 0);

                terrain.Render(gfx.pImmediateContext.Get());
            }
        );

        // --- 2. Blur X Pass ---
        graph.AddPass("BlurXPass",
            [&](RenderGraphBuilder& builder) {
                m_hBlurX = builder.CreateTexture("VSM_BlurX", { 2048, 2048, DXGI_FORMAT_R32G32_FLOAT, false });
                builder.Read(m_hRawVSM);
                builder.Write(m_hBlurX);
            },
            [this](GraphicsDevice& gfx, RenderGraphRegistry& registry) {
                auto rtv = registry.GetRTV(m_hBlurX);
                auto srv = registry.GetSRV(m_hRawVSM);

                gfx.pImmediateContext->OMSetRenderTargets(1, &rtv, nullptr);
                gfx.pImmediateContext->PSSetShaderResources(0, 1, &srv);

                gfx.pImmediateContext->IASetInputLayout(nullptr);
                gfx.pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                gfx.pImmediateContext->VSSetShader(pSkyVertexShader.Get(), nullptr, 0);
                gfx.pImmediateContext->PSSetShader(pBlurPixelShader.Get(), nullptr, 0);
                gfx.pImmediateContext->PSSetSamplers(0, 1, pShadowSampler.GetAddressOf());

                BlurParams bp = { 1.0f / 2048.0f, 0.0f, 0.0f, 0.0f };
                gfx.pImmediateContext->UpdateSubresource(pBlurConstantBuffer.Get(), 0, nullptr, &bp, 0, 0);
                gfx.pImmediateContext->PSSetConstantBuffers(1, 1, pBlurConstantBuffer.GetAddressOf());
                gfx.pImmediateContext->Draw(3, 0);
            }
        );

        // --- 3. Blur Y Pass ---
        graph.AddPass("BlurYPass",
            [&](RenderGraphBuilder& builder) {
                m_hFinalVSM = builder.CreateTexture("VSM_Final", { 2048, 2048, DXGI_FORMAT_R32G32_FLOAT, false });
                builder.Read(m_hBlurX);
                builder.Write(m_hFinalVSM);
            },
            [this](GraphicsDevice& gfx, RenderGraphRegistry& registry) {
                auto rtv = registry.GetRTV(m_hFinalVSM);
                auto srv = registry.GetSRV(m_hBlurX);

                gfx.pImmediateContext->OMSetRenderTargets(1, &rtv, nullptr);
                gfx.pImmediateContext->PSSetShaderResources(0, 1, &srv);

                BlurParams bp = { 0.0f, 1.0f / 2048.0f, 0.0f, 0.0f };
                gfx.pImmediateContext->UpdateSubresource(pBlurConstantBuffer.Get(), 0, nullptr, &bp, 0, 0);
                gfx.pImmediateContext->Draw(3, 0);
            }
        );

        // --- 4. Main PBR Pass ---
        graph.AddPass("MainPass",
            [&](RenderGraphBuilder& builder) {
                // 【核心修改】：在这里向系统申请一张和屏幕一样大的干净画布
                m_hSceneColor = builder.CreateTexture("SceneColor", { 800, 600, DXGI_FORMAT_R8G8B8A8_UNORM, false });
                builder.Read(m_hFinalVSM);

                builder.Write(m_hSceneColor); // 画在虚拟画布上
                builder.Write(m_hBackBuffer); // 因为要用到主屏幕的 DepthBuffer
            },
            [this, &terrain, &cam, &env](GraphicsDevice& gfx, RenderGraphRegistry& registry) {
                ConstantBufferStruct cbMain = {};
                cbMain.WorldViewProj = XMMatrixTranspose(cam.GetViewMatrix() * cam.GetProjMatrix(800.0f / 600.0f));
                cbMain.LightViewProj = XMMatrixTranspose(env.sunMatrix);
                cbMain.InvViewProj = XMMatrixTranspose(XMMatrixInverse(nullptr, cam.GetViewMatrix() * cam.GetProjMatrix(800.0f / 600.0f)));
                cbMain.LightDirToSun = env.lightDir;
                cbMain.CameraPos = XMFLOAT4(cam.camPos.x, cam.camPos.y, cam.camPos.z, 1.0f);
                cbMain.WeatherParams = env.WeatherParams;
                cbMain.Time = env.TotalTime;
                cbMain.SkyParams = env.SkyParams;
                gfx.pImmediateContext->UpdateSubresource(pConstantBuffer.Get(), 0, nullptr, &cbMain, 0, 0);

                gfx.pImmediateContext->VSSetConstantBuffers(0, 1, pConstantBuffer.GetAddressOf());
                gfx.pImmediateContext->PSSetConstantBuffers(0, 1, pConstantBuffer.GetAddressOf());

                // 【核心修改】：拿出刚才申请的虚拟画布
                auto rtv = registry.GetRTV(m_hBackBuffer);
                auto dsv = registry.GetDSV(m_hBackBuffer); // 借用主屏幕的深度图

                float clearColor[4] = { 0.05f, 1.0f, 1.0f, 1.0f };
                gfx.pImmediateContext->ClearRenderTargetView(rtv, clearColor);
                gfx.pImmediateContext->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
                gfx.pImmediateContext->OMSetRenderTargets(1, &rtv, dsv);

                D3D11_VIEWPORT vp = { 0.0f, 0.0f, 800.0f, 600.0f, 0.0f, 1.0f };
                gfx.pImmediateContext->RSSetViewports(1, &vp);
                gfx.pImmediateContext->RSSetState(pRasterState.Get());

                gfx.pImmediateContext->IASetInputLayout(pVertexLayout.Get());
                gfx.pImmediateContext->VSSetShader(pVertexShader.Get(), nullptr, 0);
                gfx.pImmediateContext->PSSetShader(pPixelShader.Get(), nullptr, 0);

                auto shadowSRV = registry.GetSRV(m_hFinalVSM);
                ID3D11ShaderResourceView* srvs[] = {
                    shadowSRV, pRockAlbedoSRV.Get(), pRockNormalSRV.Get(),
                    pGrassAlbedoSRV.Get(), pGrassNormalSRV.Get(), pSnowAlbedoSRV.Get(), pSnowNormalSRV.Get(), pCloudVolumeSRV.Get()
                };
                gfx.pImmediateContext->PSSetShaderResources(0, 8, srvs);

                ID3D11SamplerState* samplers[] = { pShadowSampler.Get(), pWrapSampler.Get() };
                gfx.pImmediateContext->PSSetSamplers(0, 2, samplers);

                terrain.Render(gfx.pImmediateContext.Get());
            }
        );

        // --- 5. Sky Pass ---
        graph.AddPass("SkyPass",
            [&](RenderGraphBuilder& builder) {
                // 【核心修改】：天空也画在虚拟画布上
                builder.Write(m_hSceneColor);
                builder.Write(m_hBackBuffer);
            },
            [this](GraphicsDevice& gfx, RenderGraphRegistry& registry) {
                auto rtv = registry.GetRTV(m_hBackBuffer);
                auto dsv = registry.GetDSV(m_hBackBuffer);
                gfx.pImmediateContext->OMSetRenderTargets(1, &rtv, dsv);

                gfx.pImmediateContext->OMSetDepthStencilState(pSkyDepthState.Get(), 0);
                gfx.pImmediateContext->IASetInputLayout(nullptr);
                gfx.pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                gfx.pImmediateContext->VSSetShader(pSkyVertexShader.Get(), nullptr, 0);
                gfx.pImmediateContext->PSSetShader(pSkyPixelShader.Get(), nullptr, 0);
                gfx.pImmediateContext->PSSetConstantBuffers(0, 1, pConstantBuffer.GetAddressOf());

                gfx.pImmediateContext->Draw(3, 0);
                gfx.pImmediateContext->OMSetDepthStencilState(nullptr, 0);
            }
        );

        
    }
};