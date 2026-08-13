#pragma once
#include "GraphicsDevice.h"
#include "d3dx12lite.h"
#include "dx12RenderGraph.h"
#include "Camera.h"
#include "Environment.h"
#include "RenderData.h"
#include "TerrainInteropManager.h"
#include "stb_image.h"
#include <d3dcompiler.h>
#include <vector>

// ============================================================
// D3D12 渲染管线：Root Signature / PSO / 纹理上传 / Render Graph 构建
//
// 描述符堆布局（与 GraphicsDevice 一起规划）：
//   RTV 堆 : [0..1] backbuffer  [2..] RenderGraph 内部纹理
//   DSV 堆 : [0] 主深度          [1..] RenderGraph 内部深度
//   SRV 堆 : [0..5] 材质贴图     [6] 云层 3D 纹理    [7..] RenderGraph 内部纹理
//
// Root Signature：
//   b0 = 每帧常量缓冲（阴影/主相机各自版本）
//   b1 = Blur 参数
//   t0 = 动态 VSM 纹理（blurX 读 RawVSM / blurY 读 BlurX / main 读 FinalVSM）
//   t1..t7 = 材质贴图 + 云层
//   s0/s1 = 静态采样器
// ============================================================
class RenderPipeline {
public:
    // ============ Root Signature / PSO ============
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pShadowPSO;
    ComPtr<ID3D12PipelineState> pMainPSO;
    ComPtr<ID3D12PipelineState> pBlurPSO;
    ComPtr<ID3D12PipelineState> pSkyPSO;

    // ============ 材质纹理资源（SRV 堆偏移 0..5） ============
    ComPtr<ID3D12Resource> pRockAlbedoTex, pRockNormalTex, pGrassAlbedoTex, pGrassNormalTex, pSnowAlbedoTex, pSnowNormalTex;
    std::vector<ComPtr<ID3D12Resource>> m_uploadStaging;   // 初始化用一次性上传缓冲（保留防提前释放）

    // ============ 云层 3D 纹理 + CUDA 外部内存互操作（SRV 堆偏移 6） ============
    ComPtr<ID3D12Resource> pCloudVolumeTex;
    HANDLE cloudSharedHandle = nullptr;
    UINT64 cloudAllocationSize = 0;
    cudaExternalMemory_t cudaCloudExtMem = nullptr;
    cudaMipmappedArray_t cudaCloudMipArray = nullptr;
    cudaArray_t cudaCloudArray = nullptr;
    cudaSurfaceObject_t cudaCloudSurface = 0;

    // ============ 每帧常量缓冲上传（256B 对齐槽位） ============
    // 布局：offset 0 = 阴影 CB，512 = 主相机 CB，768 = Blur 参数
    ComPtr<ID3D12Resource> uploadBuffers[GraphicsDevice::NUM_FRAMES];
    unsigned char* m_uploadCPU[GraphicsDevice::NUM_FRAMES] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_shadowCBAddr[GraphicsDevice::NUM_FRAMES] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_mainCBAddr[GraphicsDevice::NUM_FRAMES] = {};
    D3D12_GPU_VIRTUAL_ADDRESS m_blurCBAddr[GraphicsDevice::NUM_FRAMES] = {};

    // ============ Render Graph 句柄 ============
    RGHandle m_hBackBuffer = RG_INVALID_HANDLE;
    RGHandle m_hMainDepth = RG_INVALID_HANDLE;
    RGHandle m_hRawVSM = RG_INVALID_HANDLE;
    RGHandle m_hShadowDepth = RG_INVALID_HANDLE;
    RGHandle m_hBlurX = RG_INVALID_HANDLE;
    RGHandle m_hFinalVSM = RG_INVALID_HANDLE;

    // ============================================================
    // 初始化
    // ============================================================
    bool Initialize(GraphicsDevice& gfx) {
        // A. 编译 Shader（DX12 使用 SM5.1 着色器模型）
        auto vsBlob       = CompileShader(L"shader.hlsl", "VS", "vs_5_1");
        auto psBlob       = CompileShader(L"shader.hlsl", "PS", "ps_5_1");
        auto shadowPSBlob = CompileShader(L"shader.hlsl", "ShadowPS", "ps_5_1");
        auto blurPSBlob   = CompileShader(L"shader.hlsl", "BlurPS", "ps_5_1");
        auto skyVSBlob    = CompileShader(L"SkyShader.hlsl", "VS", "vs_5_1");
        auto skyPSBlob    = CompileShader(L"SkyShader.hlsl", "PS", "ps_5_1");

        // B. Root Signature（手填 D3D12 结构，不依赖 d3dx12.h）
        D3D12_ROOT_PARAMETER params[4] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;            // b0: 主/阴影常量
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;            // b1: Blur 参数
        params[1].Descriptor.ShaderRegister = 1;
        params[1].Descriptor.RegisterSpace = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE rangeT0 = {};                                // t0: 动态 VSM
        rangeT0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rangeT0.NumDescriptors = 1;
        rangeT0.BaseShaderRegister = 0;
        rangeT0.RegisterSpace = 0;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &rangeT0;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_DESCRIPTOR_RANGE rangeT1 = {};                                // t1..t7: 材质+云层
        rangeT1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rangeT1.NumDescriptors = 7;
        rangeT1.BaseShaderRegister = 1;
        rangeT1.RegisterSpace = 0;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges = &rangeT1;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].Filter = D3D12_FILTER_ANISOTROPIC;
        samplers[1].MaxAnisotropy = 16;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 4;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 2;
        rsDesc.pStaticSamplers = samplers;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> sigBlob, sigErr;
        DX12_CHECK(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr));
        DX12_CHECK(gfx.device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSig)));

        // C. 顶点输入布局
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        // D. PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = rootSig.Get();
        pso.InputLayout = { layout, 3 };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.RasterizerState = d3dx12lite::DefaultRasterizer();
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.DepthStencilState = d3dx12lite::DefaultDepthStencil();
        pso.BlendState = d3dx12lite::DefaultBlend();
        pso.SampleMask = UINT_MAX;
        pso.SampleDesc.Count = 1;
        pso.NumRenderTargets = 1;

        // Shadow PSO：地形 VS + ShadowPS → R32G32_FLOAT + 深度
        pso.VS = d3dx12lite::ShaderBytecode(vsBlob.Get());
        pso.PS = d3dx12lite::ShaderBytecode(shadowPSBlob.Get());
        pso.RTVFormats[0] = DXGI_FORMAT_R32G32_FLOAT;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        DX12_CHECK(gfx.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pShadowPSO)));

        // Main PSO：地形 VS + PBR PS → R8G8B8A8_UNORM + 深度
        pso.VS = d3dx12lite::ShaderBytecode(vsBlob.Get());
        pso.PS = d3dx12lite::ShaderBytecode(psBlob.Get());
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        DX12_CHECK(gfx.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pMainPSO)));

        // Blur PSO：全屏三角形（SkyVS）+ BlurPS → R32G32_FLOAT，无深度无输入布局
        pso.VS = d3dx12lite::ShaderBytecode(skyVSBlob.Get());
        pso.PS = d3dx12lite::ShaderBytecode(blurPSBlob.Get());
        pso.InputLayout = { nullptr, 0 };
        pso.RTVFormats[0] = DXGI_FORMAT_R32G32_FLOAT;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        D3D12_DEPTH_STENCIL_DESC dsOff = {};
        dsOff.DepthEnable = FALSE; dsOff.StencilEnable = FALSE;
        pso.DepthStencilState = dsOff;
        DX12_CHECK(gfx.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pBlurPSO)));

        // Sky PSO：SkyVS + SkyPS → R8G8B8A8_UNORM + 只读深度（不写、LESS_EQUAL）
        pso.VS = d3dx12lite::ShaderBytecode(skyVSBlob.Get());
        pso.PS = d3dx12lite::ShaderBytecode(skyPSBlob.Get());
        pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        D3D12_DEPTH_STENCIL_DESC dsSky = {};
        dsSky.DepthEnable = TRUE;
        dsSky.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        dsSky.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        dsSky.StencilEnable = FALSE;
        pso.DepthStencilState = dsSky;
        DX12_CHECK(gfx.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pSkyPSO)));

        // E. 加载材质贴图（SRV 堆偏移 0..5，上传后 barrier 到 PIXEL_SHADER_RESOURCE）
        D3D12_CPU_DESCRIPTOR_HANDLE srvBase = gfx.srvHeap->GetCPUDescriptorHandleForHeapStart();
        auto srvAt = [&](UINT offset) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = srvBase;
            h.ptr += (UINT64)offset * gfx.srvDescriptorSize;
            return h;
        };
        CreateTexture2DFromFile(gfx, "rock_albedo.jpg", pRockAlbedoTex, srvAt(0));
        CreateTexture2DFromFile(gfx, "rock_normal.jpg", pRockNormalTex, srvAt(1));
        CreateTexture2DFromFile(gfx, "grass_albedo.jpg", pGrassAlbedoTex, srvAt(2));
        CreateTexture2DFromFile(gfx, "grass_normal.jpg", pGrassNormalTex, srvAt(3));
        CreateTexture2DFromFile(gfx, "snow_albedo.jpg", pSnowAlbedoTex, srvAt(4));
        CreateTexture2DFromFile(gfx, "snow_normal.jpg", pSnowNormalTex, srvAt(5));

        // F. 云层 3D 纹理 + CUDA 外部内存互操作（SRV 堆偏移 6）
        //    CUDA 13 移除了 cuda_d3d12_interop.h，改用 cudaImportExternalMemory 导入共享资源
        D3D12_RESOURCE_DESC cloudDesc = {};
        cloudDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        cloudDesc.Width = 128; cloudDesc.Height = 128; cloudDesc.DepthOrArraySize = 128;
        cloudDesc.MipLevels = 1;
        cloudDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cloudDesc.SampleDesc.Count = 1;
        cloudDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;  // CUDA 可写纹理要求
        D3D12_HEAP_PROPERTIES cloudHeap = {};
        cloudHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        DX12_CHECK(gfx.device->CreateCommittedResource(&cloudHeap, D3D12_HEAP_FLAG_SHARED, &cloudDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pCloudVolumeTex)));
        D3D12_RESOURCE_ALLOCATION_INFO ai = gfx.device->GetResourceAllocationInfo(0, 1, &cloudDesc);
        cloudAllocationSize = ai.SizeInBytes;
        DX12_CHECK(gfx.device->CreateSharedHandle(pCloudVolumeTex.Get(), nullptr, GENERIC_ALL, nullptr, &cloudSharedHandle));

        D3D12_SHADER_RESOURCE_VIEW_DESC cloudSRV = {};
        cloudSRV.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cloudSRV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        cloudSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cloudSRV.Texture3D.MipLevels = 1;
        gfx.device->CreateShaderResourceView(pCloudVolumeTex.Get(), &cloudSRV, srvAt(6));

        cudaExternalMemoryHandleDesc hd = {};
        hd.type = cudaExternalMemoryHandleTypeD3D12Resource;
        hd.handle.win32.handle = (void*)cloudSharedHandle;
        hd.size = cloudAllocationSize;
        hd.flags = cudaExternalMemoryDedicated;   // D3D12Resource 类型必须设置（CUDA 13 要求）
        if (cudaImportExternalMemory(&cudaCloudExtMem, &hd) == cudaSuccess) {
            cudaExternalMemoryMipmappedArrayDesc mad = {};
            mad.extent = make_cudaExtent(128, 128, 128);
            mad.formatDesc = cudaCreateChannelDesc<uchar4>();
            mad.numLevels = 1;
            mad.flags = 0;
            if (cudaExternalMemoryGetMappedMipmappedArray(&cudaCloudMipArray, cudaCloudExtMem, &mad) == cudaSuccess) {
                cudaGetMipmappedArrayLevel(&cudaCloudArray, cudaCloudMipArray, 0);
                cudaResourceDesc rd = {};
                rd.resType = cudaResourceTypeArray;
                rd.res.array.array = cudaCloudArray;
                cudaCreateSurfaceObject(&cudaCloudSurface, &rd);
            }
        }

        // G. 每帧常量缓冲上传堆（Map 常驻，每帧直接 memcpy）
        for (UINT i = 0; i < GraphicsDevice::NUM_FRAMES; ++i) {
            D3D12_RESOURCE_DESC upDesc = {};
            upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            upDesc.Width = 1024;   // 3 个 256B 对齐槽位
            upDesc.Height = 1; upDesc.DepthOrArraySize = 1; upDesc.MipLevels = 1;
            upDesc.SampleDesc.Count = 1; upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES upHeap = {};
            upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            DX12_CHECK(gfx.device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffers[i])));
            DX12_CHECK(uploadBuffers[i]->Map(0, nullptr, (void**)&m_uploadCPU[i]));
            D3D12_GPU_VIRTUAL_ADDRESS base = uploadBuffers[i]->GetGPUVirtualAddress();
            m_shadowCBAddr[i] = base;
            m_mainCBAddr[i] = base + 512;
            m_blurCBAddr[i] = base + 768;
        }
        return true;
    }

    // ============================================================
    // 渲染图构建（与 D3D11 版相同的 5 个 Pass）
    // ============================================================
    void BuildGraph(RenderGraph& graph, GraphicsDevice& gfx, TerrainInteropManager& terrain,
                    Camera& cam, EnvironmentSystem& env) {
        m_hBackBuffer = graph.ImportRenderTarget("BackBuffer",
            gfx.backBuffers[gfx.backBufferIndex].Get(), gfx.backBufferRTV[gfx.backBufferIndex],
            D3D12_RESOURCE_STATE_PRESENT);
        // 主深度也交给渲染图管理：MainPass 写(DEPTH_WRITE)、SkyPass 只读(DEPTH_READ)，
        // 中间的转换 barrier 由渲染图自动推导
        m_hMainDepth = graph.ImportDepthTarget("MainDepth",
            gfx.depthBuffer.Get(), gfx.depthDSV, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // --- 1. Shadow Pass：地形画到太阳视角，输出 VSM 原始矩 + 深度 ---
        graph.AddPass("ShadowPass",
            [&](RenderGraphBuilder& builder) {
                m_hRawVSM = builder.CreateTexture("VSM_Raw", { 2048, 2048, DXGI_FORMAT_R32G32_FLOAT, false });
                m_hShadowDepth = builder.CreateTexture("ShadowMap", { 2048, 2048, DXGI_FORMAT_UNKNOWN, true });
                builder.Write(m_hRawVSM);
                builder.Write(m_hShadowDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            },
            [this, &gfx, &terrain](ID3D12GraphicsCommandList* cl, RenderGraphRegistry& registry) {
                cl->SetGraphicsRootSignature(rootSig.Get());
                cl->SetPipelineState(pShadowPSO.Get());
                cl->SetGraphicsRootConstantBufferView(0, m_shadowCBAddr[gfx.frameIndex]);

                D3D12_CPU_DESCRIPTOR_HANDLE rtv = registry.GetRTV(m_hRawVSM);
                D3D12_CPU_DESCRIPTOR_HANDLE dsv = registry.GetDSV(m_hShadowDepth);
                float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
                cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                D3D12_VIEWPORT vp = { 0, 0, 2048, 2048, 0, 1 };
                D3D12_RECT scissor = { 0, 0, 2048, 2048 };
                cl->RSSetViewports(1, &vp);
                cl->RSSetScissorRects(1, &scissor);
                terrain.Render(cl);
            }
        );

        // --- 2. Blur X Pass：VSM 水平高斯模糊 ---
        graph.AddPass("BlurXPass",
            [&](RenderGraphBuilder& builder) {
                m_hBlurX = builder.CreateTexture("VSM_BlurX", { 2048, 2048, DXGI_FORMAT_R32G32_FLOAT, false });
                builder.Read(m_hRawVSM);
                builder.Write(m_hBlurX);
            },
            [this, &gfx](ID3D12GraphicsCommandList* cl, RenderGraphRegistry& registry) {
                cl->SetGraphicsRootSignature(rootSig.Get());
                cl->SetPipelineState(pBlurPSO.Get());
                cl->SetGraphicsRootConstantBufferView(1, m_blurCBAddr[gfx.frameIndex]);
                cl->SetGraphicsRootDescriptorTable(2, registry.GetSRVGPU(m_hRawVSM));

                D3D12_CPU_DESCRIPTOR_HANDLE rtv = registry.GetRTV(m_hBlurX);
                cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

                D3D12_VIEWPORT vp = { 0, 0, 2048, 2048, 0, 1 };
                D3D12_RECT scissor = { 0, 0, 2048, 2048 };
                cl->RSSetViewports(1, &vp);
                cl->RSSetScissorRects(1, &scissor);
                cl->DrawInstanced(3, 1, 0, 0);
            }
        );

        // --- 3. Blur Y Pass：VSM 垂直高斯模糊 ---
        graph.AddPass("BlurYPass",
            [&](RenderGraphBuilder& builder) {
                m_hFinalVSM = builder.CreateTexture("VSM_Final", { 2048, 2048, DXGI_FORMAT_R32G32_FLOAT, false });
                builder.Read(m_hBlurX);
                builder.Write(m_hFinalVSM);
            },
            [this, &gfx](ID3D12GraphicsCommandList* cl, RenderGraphRegistry& registry) {
                // Y 方向模糊参数（同一帧内覆盖上传缓冲，GPU 尚未读取）
                BlurParams bpY = { 0.0f, 1.0f / 2048.0f, 0.0f, 0.0f };
                memcpy(m_uploadCPU[gfx.frameIndex] + 768, &bpY, sizeof(bpY));

                cl->SetGraphicsRootSignature(rootSig.Get());
                cl->SetPipelineState(pBlurPSO.Get());
                cl->SetGraphicsRootConstantBufferView(1, m_blurCBAddr[gfx.frameIndex]);
                cl->SetGraphicsRootDescriptorTable(2, registry.GetSRVGPU(m_hBlurX));

                D3D12_CPU_DESCRIPTOR_HANDLE rtv = registry.GetRTV(m_hFinalVSM);
                cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

                D3D12_VIEWPORT vp = { 0, 0, 2048, 2048, 0, 1 };
                D3D12_RECT scissor = { 0, 0, 2048, 2048 };
                cl->RSSetViewports(1, &vp);
                cl->RSSetScissorRects(1, &scissor);
                cl->DrawInstanced(3, 1, 0, 0);
            }
        );

        // --- 4. Main Pass：地形 PBR + 贴图混合 + VSM 阴影 ---
        graph.AddPass("MainPass",
            [&](RenderGraphBuilder& builder) {
                builder.Read(m_hFinalVSM);
                builder.Write(m_hBackBuffer);
                builder.Write(m_hMainDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            },
            [this, &gfx, &terrain](ID3D12GraphicsCommandList* cl, RenderGraphRegistry& registry) {
                cl->SetGraphicsRootSignature(rootSig.Get());
                cl->SetPipelineState(pMainPSO.Get());
                cl->SetGraphicsRootConstantBufferView(0, m_mainCBAddr[gfx.frameIndex]);
                cl->SetGraphicsRootDescriptorTable(2, registry.GetSRVGPU(m_hFinalVSM));
                cl->SetGraphicsRootDescriptorTable(3, gfx.SRVHeapStartGPU());

                D3D12_CPU_DESCRIPTOR_HANDLE rtv = registry.GetRTV(m_hBackBuffer);
                D3D12_CPU_DESCRIPTOR_HANDLE dsv = registry.GetDSV(m_hMainDepth);
                float clearColor[4] = { 0.05f, 1.0f, 1.0f, 1.0f };
                cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                cl->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
                cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                D3D12_VIEWPORT vp = { 0, 0, 800, 600, 0, 1 };
                D3D12_RECT scissor = { 0, 0, 800, 600 };
                cl->RSSetViewports(1, &vp);
                cl->RSSetScissorRects(1, &scissor);
                terrain.Render(cl);
            }
        );

        // --- 5. Sky Pass：大气散射天空盒（全屏三角形） ---
        graph.AddPass("SkyPass",
            [&](RenderGraphBuilder& builder) {
                builder.Write(m_hBackBuffer);
                builder.Write(m_hMainDepth, D3D12_RESOURCE_STATE_DEPTH_READ);
            },
            [this, &gfx](ID3D12GraphicsCommandList* cl, RenderGraphRegistry& registry) {
                cl->SetGraphicsRootSignature(rootSig.Get());
                cl->SetPipelineState(pSkyPSO.Get());
                cl->SetGraphicsRootConstantBufferView(0, m_mainCBAddr[gfx.frameIndex]);

                D3D12_CPU_DESCRIPTOR_HANDLE rtv = registry.GetRTV(m_hBackBuffer);
                D3D12_CPU_DESCRIPTOR_HANDLE dsv = registry.GetDSV(m_hMainDepth);
                cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

                D3D12_VIEWPORT vp = { 0, 0, 800, 600, 0, 1 };
                D3D12_RECT scissor = { 0, 0, 800, 600 };
                cl->RSSetViewports(1, &vp);
                cl->RSSetScissorRects(1, &scissor);
                cl->DrawInstanced(3, 1, 0, 0);
            }
        );
    }

    // ============================================================
    // 每帧更新常量缓冲（在 BeginFrame 之后调用）
    // ============================================================
    void UpdateFrameCB(GraphicsDevice& gfx, Camera& cam, EnvironmentSystem& env) {
        UINT f = gfx.frameIndex;

        // 阴影 pass 用：太阳光空间矩阵
        ConstantBufferStruct cbShadow = {};
        cbShadow.WorldViewProj = XMMatrixTranspose(env.sunMatrix);
        cbShadow.LightViewProj = XMMatrixTranspose(env.sunMatrix);
        cbShadow.InvViewProj = XMMatrixTranspose(XMMatrixInverse(nullptr, env.sunMatrix));
        cbShadow.LightDirToSun = env.lightDir;
        cbShadow.CameraPos = XMFLOAT4(cam.camPos.x, cam.camPos.y, cam.camPos.z, 1.0f);
        cbShadow.WeatherParams = env.WeatherParams;
        cbShadow.Time = env.TotalTime;
        cbShadow.SkyParams = env.SkyParams;
        memcpy(m_uploadCPU[f] + 0, &cbShadow, sizeof(cbShadow));

        // 主 pass / 天空用：相机空间矩阵
        XMMATRIX viewProj = cam.GetViewMatrix() * cam.GetProjMatrix(800.0f / 600.0f);
        ConstantBufferStruct cbMain = {};
        cbMain.WorldViewProj = XMMatrixTranspose(viewProj);
        cbMain.LightViewProj = XMMatrixTranspose(env.sunMatrix);
        cbMain.InvViewProj = XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj));
        cbMain.LightDirToSun = env.lightDir;
        cbMain.CameraPos = XMFLOAT4(cam.camPos.x, cam.camPos.y, cam.camPos.z, 1.0f);
        cbMain.WeatherParams = env.WeatherParams;
        cbMain.Time = env.TotalTime;
        cbMain.SkyParams = env.SkyParams;
        memcpy(m_uploadCPU[f] + 512, &cbMain, sizeof(cbMain));

        // Blur 参数（X 方向；Y 方向在 BlurYPass 内覆盖）
        BlurParams bp = { 1.0f / 2048.0f, 0.0f, 0.0f, 0.0f };
        memcpy(m_uploadCPU[f] + 768, &bp, sizeof(bp));
    }

    // 云层更新（当前主循环默认不调用，与 D3D11 版一致；启用需配合 barrier 同步）
    void UpdateCloudVolume() {
        if (!cudaCloudSurface) return;
        LaunchCloudGenKernel(cudaCloudSurface, 128);
        cudaStreamSynchronize(0);
    }

    void Cleanup() {
        for (UINT i = 0; i < GraphicsDevice::NUM_FRAMES; ++i) {
            if (uploadBuffers[i] && m_uploadCPU[i]) { uploadBuffers[i]->Unmap(0, nullptr); m_uploadCPU[i] = nullptr; }
        }
        if (cudaCloudSurface) { cudaDestroySurfaceObject(cudaCloudSurface); cudaCloudSurface = 0; }
        if (cudaCloudExtMem) { cudaDestroyExternalMemory(cudaCloudExtMem); cudaCloudExtMem = nullptr; }
        if (cloudSharedHandle) { CloseHandle(cloudSharedHandle); cloudSharedHandle = nullptr; }
        cudaCloudArray = nullptr; cudaCloudMipArray = nullptr;
    }

private:
    // ============================================================
    // Shader 编译辅助
    // ============================================================
    static ComPtr<ID3DBlob> CompileShader(const wchar_t* file, const char* entry, const char* profile) {
        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3DCompileFromFile(file, nullptr, nullptr, entry, profile, 0, 0, &blob, &err);
        if (FAILED(hr)) {
            if (err) {
                OutputDebugStringA((char*)err->GetBufferPointer());
                MessageBoxA(nullptr, (char*)err->GetBufferPointer(), "Shader Compile Error", MB_ICONERROR);
            }
            DX12_CHECK(hr);
        }
        return blob;
    }

    // ============================================================
    // 贴图加载：stb → upload heap → CopyTextureRegion → barrier 到 PS 采样
    // ============================================================
    bool CreateTexture2DFromFile(GraphicsDevice& gfx, const char* filename,
                                 ComPtr<ID3D12Resource>& outTex, D3D12_CPU_DESCRIPTOR_HANDLE srvHandle) {
        int w, h, ch;
        unsigned char* data = stbi_load(filename, &w, &h, &ch, 4);
        if (!data) return false;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        DX12_CHECK(gfx.device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTex)));

        // 上传缓冲（行 pitch 按 256B 对齐）
        const UINT64 uploadSize = d3dx12lite::AlignUp((UINT64)w * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * h;
        ComPtr<ID3D12Resource> upload;
        D3D12_RESOURCE_DESC upDesc = {};
        upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upDesc.Width = uploadSize;
        upDesc.Height = 1; upDesc.DepthOrArraySize = 1; upDesc.MipLevels = 1;
        upDesc.SampleDesc.Count = 1; upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES upHeap = {};
        upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        DX12_CHECK(gfx.device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
        m_uploadStaging.push_back(upload);

        gfx.ExecuteImmediate([&](ID3D12GraphicsCommandList* cl) {
            d3dx12lite::UploadTexture2D(gfx.device.Get(), cl, outTex.Get(), upload.Get(),
                                        data, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = outTex.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl->ResourceBarrier(1, &b);
        });

        stbi_image_free(data);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        gfx.device->CreateShaderResourceView(outTex.Get(), &srvDesc, srvHandle);
        return true;
    }
};